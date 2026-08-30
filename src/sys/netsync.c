#include <sys/netsync.h>

#include <ft/fighter.h>
#include <it/item.h>
#include <wp/weapon.h>
#include <gm/gmdef.h>
#include <gm/gmcamera.h>
#include <gr/ground.h>
#include <mp/map.h>
#include <sc/sctypes.h>
#include <sc/scmanager.h>
#include <sys/objdef.h>
#include <sys/objman.h>
#include <sys/utils.h>
#include <sys/netinput.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PORT
extern void port_log(const char *fmt, ...);
#endif

extern s32 sSYUtilsRandomSeed;
extern s32 *sSYUtilsRandomSeedPtr;

/* Per-tick structured hash of the gameplay state (T1 state trace).
 *
 * Every sub-hash is order-independent where the underlying container is a
 * linked list (per-slot / per-key accumulation), hashes floats by bit pattern,
 * and never hashes an address: GObj pointers become a stable object key
 * (player slot for fighters, creation serial for items/weapons - gobj->id is
 * only the kind), other pointers become a non-null bit.
 *
 * Gated columns (folded into `full`): rng battle fighters items weapons stage
 * objman. Recorded but not gated: `input` (already verified by netreplay),
 * `joints` and `camera` (kept separate so a divergence there is attributed,
 * not folded into fighters; DObj poses are advanced update-side by gcRunAll,
 * never by draw - the same holds for the yakumono poses hashed in `stage`),
 * and `vars` (the per-kind unions hashed word-wise with pointer slots masked
 * from a generated table, netsyncvars.inc - a layout difference between two
 * builds shows up there first).
 *
 * Not covered: map vertex data, effects. Deliberately skipped as render or
 * audio only: FTAttackColl.attack_matrix, colanim, afterimage, magnify_pos,
 * is_magnify_show, arrow_gobj, sfx handles/ids, display_mode, fog/shade
 * colors. Rule: a field written from any *ProcDisplay is never hashed in a
 * gated column - syTaskmanRunTask drops the draw pass when no gfx context
 * is free, so such fields lag by a frame on a slow renderer. */

#define SYNETSYNC_FNV_BASIS 2166136261U
#define SYNETSYNC_FNV_PRIME 16777619U
#define SYNETSYNC_NULL_ID 0xFFFFFFFFU

static sb32 sSYNetSyncIsTraceEnabled;
static sb32 sSYNetSyncIsVerifyEnabled;
static sb32 sSYNetSyncIsSessionActive;
static sb32 sSYNetSyncIsVerifyLoaded;
static sb32 sSYNetSyncIsTickMismatchReported;
static sb32 sSYNetSyncIsSlotFallbackReported;
static const char *sSYNetSyncTracePath;
static const char *sSYNetSyncVerifyPath;
static u32 sSYNetSyncTickCount;
static u32 sSYNetSyncVerifyLoadedTicks;
static u32 sSYNetSyncVerifyComparedTicks;
static u32 sSYNetSyncVerifyDivergences;
static u32 sSYNetSyncVerifyFirstDivergenceTick;
static u32 sSYNetSyncVerifyFirstDivergenceMask;
static u32 sSYNetSyncVerifyColumnReported;
static SYNetSyncTickHash *sSYNetSyncVerifyRecords;
#ifdef PORT
static FILE *sSYNetSyncTraceFile;
#endif

/* Per-match creation serial for items and weapons. gobj->id is the object
 * KIND (nGCCommonKindItem etc.), not an instance id, so it cannot tell two
 * items apart; the managers stamp this into ITStruct/WPStruct.spawn_serial
 * at creation (PORT only). Reset at every VS session start. */
static u32 sSYNetSyncSpawnSerial;

/* SSB64_SYNC_DUMP_TICK=N: log the identity-bearing fighter fields for ticks
 * N..N+2 so a divergence found by the trace can be attributed to a field. */
static u32 sSYNetSyncDumpTick = 0xFFFFFFFFU;

u32 syNetSyncNextSpawnSerial(void)
{
	sSYNetSyncSpawnSerial++;

	return sSYNetSyncSpawnSerial;
}

static const char *sSYNetSyncColumnNames[SYNETSYNC_COLUMN_NUM] = {
	"full", "rng", "battle", "fighters", "items", "weapons", "stage", "objman", "input", "joints", "camera", "vars",
};

static u32 hu(u32 hash, u32 value)
{
	hash ^= value;
	hash *= SYNETSYNC_FNV_PRIME;

	return hash;
}

static u32 syNetSyncHashF32(f32 value)
{
	union SYNetSyncF32Reinterpret
	{
		f32 fv;
		u32 uv;

	} reinterpret;

	reinterpret.fv = value;

	return reinterpret.uv;
}

static u32 hs(u32 h, s32 value)
{
	return hu(h, (u32)value);
}

static u32 hf(u32 h, f32 value)
{
	return hu(h, syNetSyncHashF32(value));
}

static u32 hv3(u32 h, Vec3f *v)
{
	h = hf(h, v->x);
	h = hf(h, v->y);
	h = hf(h, v->z);

	return h;
}

static u32 hv2(u32 h, Vec2f *v)
{
	h = hf(h, v->x);
	h = hf(h, v->y);

	return h;
}

static u32 hv2b(u32 h, Vec2b *v)
{
	h = hs(h, v->x);
	h = hs(h, v->y);

	return h;
}

/* Stable, address-free identity of a game object. gobj->id is only the kind,
 * so fighters are keyed by player slot and items/weapons by their creation
 * serial; anything else falls back to kind + link. */
static u32 syNetSyncObjectKey(GObj *gobj)
{
	if (gobj == NULL)
	{
		return SYNETSYNC_NULL_ID;
	}
	if (gobj->user_data.p != NULL)
	{
		if (gobj->link_id == nGCCommonLinkIDFighter)
		{
			return 0x01000000U | (u32)ftGetStruct(gobj)->player;
		}
#ifdef PORT
		if (gobj->link_id == nGCCommonLinkIDItem)
		{
			return 0x02000000U | itGetStruct(gobj)->spawn_serial;
		}
		if (gobj->link_id == nGCCommonLinkIDWeapon)
		{
			return 0x03000000U | wpGetStruct(gobj)->spawn_serial;
		}
#endif
	}
	return 0x0F000000U | ((u32)gobj->link_id << 16) | (gobj->id & 0xFFFFU);
}

/* Live-object table, rebuilt once per tick from the gameplay links. Stored
 * GObj pointers in fighter/item/weapon fields (throw_gobj, attack records,
 * computer.target_gobj, ...) can outlive their target: the GObj goes back to
 * the free list and its memory is reused, so dereferencing it would hash
 * allocator history, not gameplay. Keys are therefore looked up by pointer
 * equality against the objects that are alive right now; a miss is "dead". */
#define SYNETSYNC_LIVE_MAX 256
#define SYNETSYNC_DEAD_KEY 0x0DEAD000U

static GObj *sSYNetSyncLiveGObjs[SYNETSYNC_LIVE_MAX];
static u32 sSYNetSyncLiveKeys[SYNETSYNC_LIVE_MAX];
static s32 sSYNetSyncLiveCount;
static sb32 sSYNetSyncIsLiveOverflowReported;

static void syNetSyncBuildLiveTable(void)
{
	static const s32 links[3] = { nGCCommonLinkIDFighter, nGCCommonLinkIDItem, nGCCommonLinkIDWeapon };
	s32 li;

	sSYNetSyncLiveCount = 0;

	for (li = 0; li < 3; li++)
	{
		GObj *gobj;

		for (gobj = gGCCommonLinks[links[li]]; gobj != NULL; gobj = gobj->link_next)
		{
			if (sSYNetSyncLiveCount >= SYNETSYNC_LIVE_MAX)
			{
#ifdef PORT
				if (sSYNetSyncIsLiveOverflowReported == FALSE)
				{
					sSYNetSyncIsLiveOverflowReported = TRUE;
					port_log("SSB64 SyncTrace: more than %d live gameplay objects; keys beyond that hash as dead\n",
					         SYNETSYNC_LIVE_MAX);
				}
#endif
				return;
			}
			sSYNetSyncLiveGObjs[sSYNetSyncLiveCount] = gobj;
			sSYNetSyncLiveKeys[sSYNetSyncLiveCount] = syNetSyncObjectKey(gobj);
			sSYNetSyncLiveCount++;
		}
	}
}

static u32 syNetSyncLookupKey(GObj *gobj)
{
	s32 i;

	if (gobj == NULL)
	{
		return SYNETSYNC_NULL_ID;
	}
	for (i = 0; i < sSYNetSyncLiveCount; i++)
	{
		if (sSYNetSyncLiveGObjs[i] == gobj)
		{
			return sSYNetSyncLiveKeys[i];
		}
	}
	return SYNETSYNC_DEAD_KEY;
}

/* stored GObj pointer -> stable object key via the live table (never the address, never a deref) */
static u32 hid(u32 h, GObj *gobj)
{
	return hu(h, syNetSyncLookupKey(gobj));
}

/* any other pointer -> non-null bit */
static u32 hnn(u32 h, const void *p)
{
	return hu(h, (p != NULL) ? 1U : 0U);
}

static u32 syNetSyncHashStatFlags(u32 h, GMStatFlags *sf)
{
	return hu(h, (u32)sf->halfword);
}

static u32 syNetSyncHashAttackRecords(u32 h, GMAttackRecord *records, s32 count)
{
	s32 i;

	for (i = 0; i < count; i++)
	{
		h = hid(h, records[i].victim_gobj);
		h = hu(h, (u32)records[i].victim_flags.is_interact_hurt);
		h = hu(h, (u32)records[i].victim_flags.is_interact_shield);
		h = hu(h, (u32)records[i].victim_flags.is_interact_reflect);
		h = hu(h, (u32)records[i].victim_flags.is_interact_absorb);
		h = hu(h, (u32)records[i].victim_flags.group_id);
		h = hu(h, (u32)records[i].victim_flags.timer_rehit);
	}
	return h;
}

static u32 syNetSyncHashMPCollData(u32 h, MPCollData *cd)
{
	if (cd->p_translate != NULL)
	{
		h = hv3(h, cd->p_translate);
	}
	if (cd->p_lr != NULL)
	{
		h = hs(h, *cd->p_lr);
	}
	h = hv3(h, &cd->pos_prev);
	h = hv3(h, &cd->pos_diff);
	h = hv3(h, &cd->vel_speed);
	h = hv3(h, &cd->vel_push);
	h = hf(h, cd->map_coll.top);
	h = hf(h, cd->map_coll.center);
	h = hf(h, cd->map_coll.bottom);
	h = hf(h, cd->map_coll.width);
	h = hv2(h, &cd->cliffcatch_coll);
	h = hu(h, cd->mask_prev);
	h = hu(h, cd->mask_curr);
	h = hu(h, cd->mask_unk);
	h = hu(h, cd->mask_stat);
	h = hu(h, cd->update_tic);
	h = hs(h, cd->ewall_line_id);
	h = hs(h, cd->is_coll_end);
	h = hv3(h, &cd->line_coll_dist);
	h = hs(h, cd->floor_line_id);
	h = hf(h, cd->floor_dist);
	h = hu(h, cd->floor_flags);
	h = hv3(h, &cd->floor_angle);
	h = hs(h, cd->ceil_line_id);
	h = hu(h, cd->ceil_flags);
	h = hv3(h, &cd->ceil_angle);
	h = hs(h, cd->lwall_line_id);
	h = hu(h, cd->lwall_flags);
	h = hv3(h, &cd->lwall_angle);
	h = hs(h, cd->rwall_line_id);
	h = hu(h, cd->rwall_flags);
	h = hv3(h, &cd->rwall_angle);
	h = hs(h, cd->cliff_id);
	h = hs(h, cd->ignore_line_id);

	return h;
}

static u32 syNetSyncHashDObjPose(u32 h, DObj *dobj)
{
	if (dobj == NULL)
	{
		return hu(h, SYNETSYNC_NULL_ID);
	}
	h = hv3(h, &dobj->translate.vec.f);
	h = hf(h, dobj->rotate.a);
	h = hv3(h, &dobj->rotate.vec.f);
	h = hv3(h, &dobj->scale.vec.f);
	h = hf(h, dobj->anim_wait);
	h = hf(h, dobj->anim_speed);
	h = hf(h, dobj->anim_frame);
	h = hu(h, dobj->flags);
	h = hu(h, dobj->is_anim_root);

	return h;
}

static u32 syNetSyncHashFTComputer(u32 h, FTComputer *cp)
{
	h = hu(h, cp->objective);
	h = hu(h, cp->objective_base);
	h = hu(h, cp->input_kind);
	h = hu(h, cp->behavior);
	h = hu(h, cp->unk_ftcom_0x4);
	h = hu(h, cp->trait);
	h = hu(h, cp->unk_ftcom_0x6);
	h = hu(h, cp->input_wait);
	h = hnn(h, cp->p_command);
	h = hnn(h, (const void *)cp->proc_com);
	h = hu(h, cp->jump_wait);
	h = hu(h, cp->item_track_wait);
	h = hu(h, cp->behavior_change_wait);
	h = hu(h, cp->unk_ftcom_0x16);
	h = hu(h, cp->walk_stop_wait);
	h = hu(h, cp->fighter_follow_since);
	h = hu(h, cp->fighter_follow_wait);
	h = hu(h, cp->fighter_follow_end);
	h = hu(h, cp->unk_ftcom_0x20);
	h = hu(h, cp->target_find_wait);
	h = hu(h, cp->wiggle_wait);
	h = hu(h, cp->target_damage_percent);
	h = hu(h, cp->attack_count);
	h = hu(h, cp->appeal_attempt_frames);
	h = hu(h, cp->stand_stop_wait);
	h = hid(h, cp->target_gobj);
	h = hu(h, cp->item_throw_wait);
	h = hu(h, cp->unk_ftcom_0x35);
	h = hu(h, cp->unk_ftcom_0x36);
	h = hu(h, cp->input_repeat_count);
	h = hu(h, cp->unk_ftcom_0x38);
	h = hu(h, cp->stickn_button_a_count);
	h = hu(h, cp->sticktilts_button_a_count);
	h = hu(h, cp->sticksmashs_button_a_count);
	h = hu(h, cp->sticktilthi_button_a_count);
	h = hu(h, cp->sticksmashhi_button_a_count);
	h = hu(h, cp->sticktiltlw_button_a_count);
	h = hu(h, cp->sticksmashlw_button_a_count);
	h = hu(h, cp->sticksmashs_button_b_count);
	h = hu(h, cp->sticksmashhi_button_b_count);
	h = hu(h, cp->sticksmashlw_button_b_count);
	h = hu(h, cp->stickn_button_z_button_a_count);
	h = hu(h, cp->unk_ftcom_0x44);
	h = hu(h, cp->ftcom_flags_0x48_b0);
	h = hu(h, cp->ftcom_flags_0x48_b1);
	h = hu(h, cp->ftcom_flags_0x48_b2);
	h = hu(h, cp->ftcom_flags_0x48_b3);
	h = hu(h, cp->ftcom_flags_0x48_b4);
	h = hu(h, cp->ftcom_flags_0x48_b5);
	h = hu(h, cp->ftcom_flags_0x48_b6);
	h = hu(h, cp->ftcom_flags_0x48_b7);
	h = hu(h, cp->is_within_vertical_bounds);
	h = hu(h, cp->ftcom_flags_0x49_b1);
	h = hu(h, cp->ftcom_flags_0x49_b2);
	h = hu(h, cp->ftcom_flags_0x49_b3);
	h = hu(h, cp->is_counterattack);
	h = hu(h, cp->is_shield_item_weapon);
	h = hu(h, cp->is_opponent_ra);
	h = hu(h, cp->is_attempt_specialhi_recovery);
	h = hu(h, cp->ftcom_flags_0x4A_b0);
	h = hu(h, cp->ftcom_flags_0x4A_b1);
	h = hu(h, cp->is_stop_stand);
	h = hv2(h, &cp->cliff_left_pos);
	h = hv2(h, &cp->cliff_right_pos);
	h = hs(h, cp->target_line_id);
	h = hv2(h, &cp->target_pos);
	h = hf(h, cp->target_dist);
	h = hnn(h, cp->target_user);
	h = hv2(h, &cp->origin_pos);
	h = hv2(h, &cp->edge_pos);
	h = hv2(h, &cp->stand_pos);
	h = hs(h, cp->floor_line_id);
	h = hf(h, cp->dash_predict);
	h = hf(h, cp->jump_predict);

	return h;
}

static u32 syNetSyncHashFTAttackColl(u32 h, FTAttackColl *ac)
{
	h = hs(h, ac->attack_state);
	h = hu(h, ac->group_id);
	h = hs(h, ac->joint_id);
	h = hs(h, ac->damage);
	h = hs(h, ac->element);
	h = hnn(h, ac->joint);
	h = hv3(h, &ac->offset);
	h = hf(h, ac->size);
	h = hs(h, ac->angle);
	h = hs(h, ac->knockback_scale);
	h = hs(h, ac->knockback_weight);
	h = hs(h, ac->knockback_base);
	h = hs(h, ac->shield_damage);
	h = hu(h, ac->fgm_level);
	h = hu(h, ac->fgm_kind);
	h = hu(h, ac->is_hit_air);
	h = hu(h, ac->is_hit_ground);
	h = hu(h, ac->can_rebound);
	h = hu(h, ac->is_scale_pos);
	h = hu(h, ac->motion_attack_id);
	h = hu(h, ac->motion_count);
	h = hu(h, ac->stat_count);
	h = hv3(h, &ac->pos_curr);
	h = hv3(h, &ac->pos_prev);

	/* records are only cleared by the next attack, so while the coll is Off
	 * they are stale history the game never reads */
	if (ac->attack_state != nGMAttackStateOff)
	{
		h = syNetSyncHashAttackRecords(h, ac->attack_records, GMATTACKREC_NUM_MAX);
	}
	return h;
}

static u32 syNetSyncHashFTDamageColl(u32 h, FTDamageColl *dc)
{
	h = hs(h, dc->hitstatus);
	h = hs(h, dc->joint_id);
	h = hnn(h, dc->joint);
	h = hs(h, dc->placement);
	h = hs(h, dc->is_grabbable);
	h = hv3(h, &dc->offset);
	h = hv3(h, &dc->size);

	return h;
}

static u32 syNetSyncHashFTMotionScript(u32 h, FTMotionScript *ms)
{
	s32 i;

	h = hf(h, ms->script_wait);
	h = hnn(h, ms->p_script);
	h = hs(h, ms->script_id);

	for (i = 0; i < 4; i++)
	{
		h = hs(h, ms->loop_count[i]);
	}
	for (i = 0; i < (s32)(sizeof(ms->p_goto) / sizeof(ms->p_goto[0])); i++)
	{
		h = hnn(h, ms->p_goto[i]);
	}
	return h;
}

/* Everything in FTStruct that is simulation state and not inside a per-kind
 * union. Joint poses are hashed separately (see syNetSyncHashFighterJoints). */
static u32 syNetSyncHashFighter(FTStruct *fp)
{
	u32 h = SYNETSYNC_FNV_BASIS;
	s32 i;

	h = hs(h, fp->fkind);
	h = hu(h, fp->team);
	h = hu(h, fp->player);
	h = hu(h, fp->detail_curr);
	h = hu(h, fp->detail_base);
	h = hu(h, fp->costume);
	h = hu(h, fp->shade);
	h = hu(h, fp->handicap);
	h = hu(h, fp->level);
	h = hs(h, fp->stock_count);
	h = hu(h, fp->team_order);
	h = hs(h, fp->player_num);
	h = hu(h, fp->status_total_tics);
	h = hs(h, fp->pkind);
	h = hs(h, fp->status_id);
	h = hs(h, fp->motion_id);
	h = hs(h, fp->percent_damage);
	h = hs(h, fp->damage_resist);
	h = hs(h, fp->shield_health);
	h = hf(h, fp->unk_ft_0x38);
	h = hs(h, fp->unk_ft_0x3C);
	h = hu(h, fp->hitlag_tics);
	h = hs(h, fp->lr);

	h = hv3(h, &fp->physics.vel_air);
	h = hv3(h, &fp->physics.vel_damage_air);
	h = hv3(h, &fp->physics.vel_ground);
	h = hf(h, fp->physics.vel_damage_ground);
	h = hf(h, fp->physics.vel_jostle_x);
	h = hf(h, fp->physics.vel_jostle_z);

	h = syNetSyncHashMPCollData(h, &fp->coll_data);

	h = hu(h, fp->jumps_used);
	h = hu(h, fp->unk_ft_0x149);
	h = hs(h, fp->ga);
	h = hf(h, fp->attack1_followup_frames);
	h = hs(h, fp->attack1_status_id);
	h = hs(h, fp->attack1_input_count);
	h = hs(h, fp->cliffcatch_wait);
	h = hs(h, fp->tics_since_last_z);
	h = hs(h, fp->acid_wait);
	h = hs(h, fp->twister_wait);
	h = hs(h, fp->tarucann_wait);
	h = hs(h, fp->damagefloor_wait);
	h = hs(h, fp->playertag_wait);
	h = hs(h, fp->card_anim_frame_id);

	h = hu(h, fp->motion_vars.flags.flag0);
	h = hu(h, fp->motion_vars.flags.flag1);
	h = hu(h, fp->motion_vars.flags.flag2);
	h = hu(h, fp->motion_vars.flags.flag3);

	h = hu(h, fp->is_attack_active);
	h = hu(h, fp->is_hitstatus_nodamage);
	h = hu(h, fp->is_damage_coll_modify);
	h = hu(h, fp->is_modelpart_modify);
	h = hu(h, fp->is_texturepart_modify);
	h = hu(h, fp->is_reflect);
	h = hs(h, fp->reflect_lr);
	h = hu(h, fp->is_absorb);
	h = hs(h, fp->absorb_lr);
	h = hu(h, fp->is_goto_attack100);
	h = hu(h, fp->is_fastfall);
	/* is_magnify_show is written by ftDisplayMainProcDisplay() - the draw proc,
	 * which syTaskmanRunTask drops when no gfx context is free - so it is not
	 * simulation state; hashing it produced isolated single-tick divergences. */
	h = hu(h, fp->is_limit_map_bounds);
	h = hu(h, fp->is_invisible);
	h = hu(h, fp->is_shadow_hide);
	h = hu(h, fp->is_rebirth);
	h = hu(h, fp->is_magnify_ignore);
	h = hu(h, fp->is_playertag_hide);
	h = hu(h, fp->is_playertag_bossend);
	h = hu(h, fp->is_effect_skip);
	h = hu(h, fp->effect_joint_array_id);
	h = hu(h, fp->is_shield);
	h = hu(h, fp->is_effect_attach);
	h = hu(h, fp->is_jostle_ignore);
	h = hu(h, fp->is_have_translate_scale);
	h = hu(h, fp->is_control_disable);
	h = hu(h, fp->is_hitstun);
	h = hu(h, fp->slope_contour);
	h = hu(h, fp->is_use_animlocks);
	h = hu(h, fp->is_muted);
	h = hu(h, fp->unk_ft_0x190_b5);
	h = hu(h, fp->is_item_show);
	h = hu(h, fp->is_cliff_hold);
	h = hu(h, fp->is_events_forward);
	h = hu(h, fp->is_ghost);
	h = hu(h, fp->is_damage_resist);
	h = hu(h, fp->is_menu_ignore);
	h = hu(h, fp->camera_mode);
	h = hu(h, fp->is_special_interrupt);
	h = hu(h, fp->is_ignore_dead);
	h = hu(h, fp->is_catchstatus);
	h = hu(h, fp->is_catch_or_capture);
	h = hu(h, fp->is_use_fogcolor);
	h = hu(h, fp->is_shield_catch);
	h = hu(h, fp->is_knockback_paused);

	h = hu(h, fp->capture_immune_mask);
	h = hu(h, fp->catch_mask);
	h = hu(h, fp->anim_desc.word);
	h = hv3(h, &fp->anim_vel);

	h = hu(h, fp->input.button_mask_a);
	h = hu(h, fp->input.button_mask_b);
	h = hu(h, fp->input.button_mask_z);
	h = hu(h, fp->input.button_mask_l);
	h = hu(h, fp->input.pl.button_hold);
	h = hu(h, fp->input.pl.button_tap);
	h = hu(h, fp->input.pl.button_release);
	h = hv2b(h, &fp->input.pl.stick_range);
	h = hv2b(h, &fp->input.pl.stick_prev);
	h = hu(h, fp->input.cp.button_inputs);
	h = hv2b(h, &fp->input.cp.stick_range);

	h = syNetSyncHashFTComputer(h, &fp->computer);

	h = hv2(h, &fp->damage_coll_size);
	h = hu(h, fp->tap_stick_x);
	h = hu(h, fp->tap_stick_y);
	h = hu(h, fp->hold_stick_x);
	h = hu(h, fp->hold_stick_y);
	h = hs(h, fp->breakout_wait);
	h = hs(h, fp->breakout_lr);
	h = hs(h, fp->breakout_ud);
	h = hu(h, fp->shuffle_frame_index);
	h = hu(h, fp->shuffle_index_max);
	h = hu(h, fp->is_shuffle_electric);
	h = hu(h, fp->shuffle_tics);
	h = hid(h, fp->throw_gobj);
	h = hs(h, fp->throw_fkind);
	h = hu(h, fp->throw_team);
	h = hu(h, fp->throw_player);
	h = hs(h, fp->throw_player_num);
	h = hu(h, fp->motion_attack_id);
	h = hu(h, fp->motion_count);
	h = syNetSyncHashStatFlags(h, &fp->stat_flags);
	h = hu(h, fp->stat_count);

	for (i = 0; i < 4; i++)
	{
		h = syNetSyncHashFTAttackColl(h, &fp->attack_colls[i]);
	}
	h = hs(h, fp->invincible_tics);
	h = hs(h, fp->intangible_tics);
	h = hs(h, fp->special_hitstatus);
	h = hs(h, fp->star_invincible_tics);
	h = hs(h, fp->star_hitstatus);
	h = hs(h, fp->hitstatus);

	for (i = 0; i < 11; i++)
	{
		h = syNetSyncHashFTDamageColl(h, &fp->damage_colls[i]);
	}
	h = hf(h, fp->unk_ft_0x7A0);
	h = hf(h, fp->hitlag_mul);
	h = hf(h, fp->shield_heal_wait);
	h = hs(h, fp->unk_ft_0x7AC);
	h = hs(h, fp->attack_damage);
	h = hf(h, fp->attack_knockback);
	h = hu(h, fp->attack_count);
	h = hs(h, fp->attack_shield_push);
	h = hf(h, fp->attack_rebound);
	h = hs(h, fp->hit_lr);
	h = hs(h, fp->shield_damage);
	h = hs(h, fp->shield_damage_total);
	h = hs(h, fp->shield_lr);
	h = hs(h, fp->shield_player);
	h = hs(h, fp->reflect_damage);
	h = hs(h, fp->damage_lag);
	h = hf(h, fp->damage_knockback);
	h = hf(h, fp->knockback_resist_passive);
	h = hf(h, fp->knockback_resist_status);
	h = hf(h, fp->damage_knockback_stack);
	h = hs(h, fp->damage_queue);
	h = hs(h, fp->damage_angle);
	h = hs(h, fp->damage_element);
	h = hs(h, fp->damage_lr);
	h = hs(h, fp->damage_index);
	h = hs(h, fp->damage_joint_id);
	h = hs(h, fp->damage_player_num);
	h = hs(h, fp->damage_player);
	h = hu(h, fp->damage_count);
	h = hs(h, fp->damage_kind);
	h = hs(h, fp->damage_heal);
	h = hf(h, fp->damage_mul);
	h = hs(h, fp->damage_object_class);
	h = hs(h, fp->damage_object_kind);
	h = syNetSyncHashStatFlags(h, &fp->damage_stat_flags);
	h = hu(h, fp->damage_stat_count);
	h = hf(h, fp->public_knockback);

	h = hid(h, fp->search_gobj);
	h = hf(h, fp->search_gobj_dist);
	h = hnn(h, (const void *)fp->proc_catch);
	h = hnn(h, (const void *)fp->proc_capture);
	h = hid(h, fp->catch_gobj);
	h = hid(h, fp->capture_gobj);
	h = hnn(h, fp->throw_desc);
	h = hid(h, fp->item_gobj);

	if (fp->special_coll != NULL)
	{
		h = hs(h, fp->special_coll->kind);
		h = hs(h, fp->special_coll->joint_id);
		h = hv3(h, &fp->special_coll->offset);
		h = hv3(h, &fp->special_coll->size);
		h = hs(h, fp->special_coll->damage_resist);
	}
	else h = hu(h, SYNETSYNC_NULL_ID);

	h = hv3(h, &fp->entry_pos);
	h = hf(h, fp->camera_zoom_frame);
	h = hf(h, fp->camera_zoom_range);

	h = syNetSyncHashFTMotionScript(h, &fp->motion_scripts[0][0]);
	h = syNetSyncHashFTMotionScript(h, &fp->motion_scripts[0][1]);
	h = syNetSyncHashFTMotionScript(h, &fp->motion_scripts[1][0]);
	h = syNetSyncHashFTMotionScript(h, &fp->motion_scripts[1][1]);

	for (i = 0; i < (s32)(sizeof(fp->modelpart_status) / sizeof(fp->modelpart_status[0])); i++)
	{
		h = hs(h, fp->modelpart_status[i].modelpart_id_base);
		h = hs(h, fp->modelpart_status[i].modelpart_id_curr);
	}
	for (i = 0; i < 2; i++)
	{
		h = hs(h, fp->texturepart_status[i].texture_id_base);
		h = hs(h, fp->texturepart_status[i].texture_id_curr);
	}
	h = hnn(h, (const void *)fp->proc_update);
	h = hnn(h, (const void *)fp->proc_accessory);
	h = hnn(h, (const void *)fp->proc_interrupt);
	h = hnn(h, (const void *)fp->proc_physics);
	h = hnn(h, (const void *)fp->proc_map);
	h = hnn(h, (const void *)fp->proc_slope);
	h = hnn(h, (const void *)fp->proc_damage);
	h = hnn(h, (const void *)fp->proc_trap);
	h = hnn(h, (const void *)fp->proc_shield);
	h = hnn(h, (const void *)fp->proc_hit);
	h = hnn(h, (const void *)fp->proc_passive);
	h = hnn(h, (const void *)fp->proc_lagupdate);
	h = hnn(h, (const void *)fp->proc_lagstart);
	h = hnn(h, (const void *)fp->proc_lagend);
	h = hnn(h, (const void *)fp->proc_status);

	h = hs(h, fp->key.input_wait);
	h = hnn(h, fp->key.script);
	h = hs(h, fp->hammer_tics);

	return h;
}

static u32 syNetSyncHashFighterJoints(FTStruct *fp)
{
	u32 h = SYNETSYNC_FNV_BASIS;
	s32 i;

	for (i = 0; i < FTPARTS_JOINT_NUM_MAX; i++)
	{
		h = syNetSyncHashDObjPose(h, fp->joints[i]);
	}
	return h;
}

static u32 syNetSyncMergeSlots(u32 *slot_hash)
{
	u32 merged = SYNETSYNC_FNV_BASIS;
	s32 si;

	for (si = 0; si < GMCOMMON_PLAYERS_MAX; si++)
	{
		merged = hu(merged ^ slot_hash[si], (u32)si);
	}
	return merged;
}

static void syNetSyncAccumulateSlot(u32 *slot_hash, s32 slot, u32 contribution)
{
	if ((slot >= 0) && (slot < GMCOMMON_PLAYERS_MAX))
	{
		slot_hash[slot] = hu(slot_hash[slot] ^ contribution, (u32)slot ^ 0x9E3779B9U);
	}
	else
	{
		/* never happens in VS (player is 0..3); say so once instead of aliasing silently */
#ifdef PORT
		if (sSYNetSyncIsSlotFallbackReported == FALSE)
		{
			sSYNetSyncIsSlotFallbackReported = TRUE;
			port_log("SSB64 SyncTrace: fighter with out-of-range player slot %d folded into slot 0\n", slot);
		}
#endif
		slot_hash[0] = hu(slot_hash[0] ^ contribution, (u32)slot ^ 0x85EBCA77U);
	}
}

/* Pre-existing narrow fighter hash used by netpeer; kept as is. */
u32 syNetSyncHashBattleFighters(void)
{
	GObj *fighter_gobj;
	u32 slot_hash[GMCOMMON_PLAYERS_MAX];
	s32 si;

	for (si = 0; si < GMCOMMON_PLAYERS_MAX; si++)
	{
		slot_hash[si] = SYNETSYNC_FNV_BASIS;
	}

	for (fighter_gobj = gGCCommonLinks[nGCCommonLinkIDFighter]; fighter_gobj != NULL;
	     fighter_gobj = fighter_gobj->link_next)
	{
		FTStruct *fp;
		u32 contribution;

		fp = ftGetStruct(fighter_gobj);

		contribution = SYNETSYNC_FNV_BASIS;

		contribution = hu(contribution, (u32)fp->player);
		contribution = hu(contribution, (u32)fp->fkind);
		contribution = hu(contribution, (u32)fp->status_id);
		contribution = hu(contribution, (u32)fp->motion_id);
		contribution = hu(contribution, (u32)fp->percent_damage);
		contribution = hu(contribution, (u32)fp->stock_count);
		contribution = hu(contribution, (u32)fp->lr);
		contribution = hu(contribution, (u32)(fp->ga != FALSE));

		contribution = hu(contribution, syNetSyncHashF32(fp->physics.vel_air.x));
		contribution = hu(contribution, syNetSyncHashF32(fp->physics.vel_air.y));
		contribution = hu(contribution, syNetSyncHashF32(fp->physics.vel_air.z));
		contribution = hu(contribution, syNetSyncHashF32(fp->physics.vel_ground.x));
		contribution = hu(contribution, syNetSyncHashF32(fp->physics.vel_ground.z));
		contribution = hu(contribution, syNetSyncHashF32(fp->physics.vel_damage_ground));

		contribution = hu(contribution, syNetSyncHashF32(fp->coll_data.pos_prev.x));
		contribution = hu(contribution, syNetSyncHashF32(fp->coll_data.pos_prev.y));
		contribution = hu(contribution, syNetSyncHashF32(fp->coll_data.pos_prev.z));

		syNetSyncAccumulateSlot(slot_hash, fp->player, contribution);
	}
	return syNetSyncMergeSlots(slot_hash);
}

u32 syNetSyncHashFighters(void)
{
	GObj *fighter_gobj;
	u32 slot_hash[GMCOMMON_PLAYERS_MAX];
	s32 si;

	for (si = 0; si < GMCOMMON_PLAYERS_MAX; si++)
	{
		slot_hash[si] = SYNETSYNC_FNV_BASIS;
	}
	for (fighter_gobj = gGCCommonLinks[nGCCommonLinkIDFighter]; fighter_gobj != NULL;
	     fighter_gobj = fighter_gobj->link_next)
	{
		FTStruct *fp = ftGetStruct(fighter_gobj);

		if (fp == NULL)
		{
			continue;
		}
		syNetSyncAccumulateSlot(slot_hash, fp->player, hu(syNetSyncHashFighter(fp), fighter_gobj->id));
	}
	return syNetSyncMergeSlots(slot_hash);
}

u32 syNetSyncHashJoints(void)
{
	GObj *fighter_gobj;
	u32 slot_hash[GMCOMMON_PLAYERS_MAX];
	s32 si;

	for (si = 0; si < GMCOMMON_PLAYERS_MAX; si++)
	{
		slot_hash[si] = SYNETSYNC_FNV_BASIS;
	}
	for (fighter_gobj = gGCCommonLinks[nGCCommonLinkIDFighter]; fighter_gobj != NULL;
	     fighter_gobj = fighter_gobj->link_next)
	{
		FTStruct *fp = ftGetStruct(fighter_gobj);

		if (fp == NULL)
		{
			continue;
		}
		syNetSyncAccumulateSlot(slot_hash, fp->player, syNetSyncHashFighterJoints(fp));
	}
	return syNetSyncMergeSlots(slot_hash);
}

static u32 syNetSyncHashITAttackColl(u32 h, ITAttackColl *ac)
{
	s32 i;

	h = hs(h, ac->attack_state);
	h = hs(h, ac->damage);
	h = hf(h, ac->throw_mul);
	h = hf(h, ac->stale);
	h = hs(h, ac->element);

	for (i = 0; i < ITEM_ATKCOLL_NUM_MAX; i++)
	{
		h = hv3(h, &ac->offsets[i]);
	}
	h = hf(h, ac->size);
	h = hs(h, ac->angle);
	h = hu(h, ac->knockback_scale);
	h = hu(h, ac->knockback_weight);
	h = hu(h, ac->knockback_base);
	h = hs(h, ac->shield_damage);
	h = hs(h, ac->priority);
	h = hu(h, ac->interact_mask);
	h = hu(h, ac->fgm_id);
	h = hu(h, ac->can_setoff);
	h = hu(h, ac->can_rehit_item);
	h = hu(h, ac->can_rehit_fighter);
	h = hu(h, ac->can_rehit_shield);
	h = hu(h, ac->can_hop);
	h = hu(h, ac->can_reflect);
	h = hu(h, ac->can_shield);
	h = hu(h, ac->motion_attack_id);
	h = hu(h, ac->motion_count);
	h = syNetSyncHashStatFlags(h, &ac->stat_flags);
	h = hu(h, ac->stat_count);
	h = hs(h, ac->attack_count);

	for (i = 0; i < ITEM_ATKCOLL_NUM_MAX; i++)
	{
		h = hv3(h, &ac->attack_pos[i].pos_curr);
		h = hv3(h, &ac->attack_pos[i].pos_prev);
		h = hs(h, ac->attack_pos[i].unk_ithitpos_0x18);
	}
	if (ac->attack_state != nGMAttackStateOff)
	{
		h = syNetSyncHashAttackRecords(h, ac->attack_records, GMATTACKREC_NUM_MAX);
	}

	return h;
}

static u32 syNetSyncHashItem(ITStruct *ip)
{
	u32 h = SYNETSYNC_FNV_BASIS;

	h = hid(h, ip->owner_gobj);
	h = hs(h, ip->kind);
	h = hs(h, ip->type);
	h = hu(h, ip->team);
	h = hu(h, ip->player);
	h = hu(h, ip->handicap);
	h = hs(h, ip->player_num);
	h = hs(h, ip->percent_damage);
	h = hu(h, ip->hitlag_tics);
	h = hs(h, ip->lr);
	h = hf(h, ip->physics.vel_ground);
	h = hv3(h, &ip->physics.vel_air);
	h = syNetSyncHashMPCollData(h, &ip->coll_data);
	h = hs(h, ip->ga);
	h = syNetSyncHashITAttackColl(h, &ip->attack_coll);
	h = hu(h, ip->damage_coll.interact_mask);
	h = hs(h, ip->damage_coll.hitstatus);
	h = hv3(h, &ip->damage_coll.offset);
	h = hv3(h, &ip->damage_coll.size);
	h = hs(h, ip->hit_normal_damage);
	h = hs(h, ip->hit_lr);
	h = hs(h, ip->hit_refresh_damage);
	h = hs(h, ip->hit_attack_damage);
	h = hs(h, ip->hit_shield_damage);
	h = hf(h, ip->shield_collide_angle);
	h = hv3(h, &ip->shield_collide_dir);
	h = hid(h, ip->reflect_gobj);
#if defined(REGION_US)
	h = syNetSyncHashStatFlags(h, &ip->reflect_stat_flags);
	h = hu(h, ip->reflect_stat_count);
#endif
	h = hs(h, ip->damage_highest);
	h = hf(h, ip->damage_knockback);
	h = hs(h, ip->damage_queue);
	h = hs(h, ip->damage_angle);
	h = hs(h, ip->damage_element);
	h = hs(h, ip->damage_lr);
	h = hid(h, ip->damage_gobj);
	h = hu(h, ip->damage_team);
	h = hu(h, ip->damage_port);
	h = hs(h, ip->damage_player_num);
	h = hu(h, ip->damage_handicap);
	h = hs(h, ip->damage_lag);
	h = hs(h, ip->lifetime);
	h = hf(h, ip->vel_scale);
	h = hu(h, ip->is_allow_pickup);
	h = hu(h, ip->is_hold);
	h = hu(h, ip->times_landed);
	h = hu(h, ip->times_thrown);
	h = hu(h, ip->weight);
	h = hu(h, ip->is_damage_all);
	h = hu(h, ip->is_attach_surface);
	h = hu(h, ip->is_thrown);
	h = hu(h, ip->attach_line_id);
	h = hu(h, ip->pickup_wait);
	h = hu(h, ip->is_allow_knockback);
	h = hu(h, ip->is_unused_item_bool);
	h = hu(h, ip->is_static_damage);
	h = hu(h, ip->is_hitlag_victim);
	h = hu(h, ip->multi);
	h = hu(h, ip->event_id);
	h = hf(h, ip->spin_step);
	h = hu(h, ip->arrow_timer);

	return h;
}

u32 syNetSyncHashItems(void)
{
	GObj *item_gobj;
	u32 acc = SYNETSYNC_FNV_BASIS;
	u32 count = 0;

	for (item_gobj = gGCCommonLinks[nGCCommonLinkIDItem]; item_gobj != NULL; item_gobj = item_gobj->link_next)
	{
		ITStruct *ip = itGetStruct(item_gobj);

		if (ip == NULL)
		{
			continue;
		}
		acc ^= hu(syNetSyncHashItem(ip), syNetSyncObjectKey(item_gobj));
		count++;
	}
	acc = hu(acc, count);
	/* Poke Ball roll queue (consumed with syUtilsRandIntRange) */
	acc = hu(acc, gITManagerMonsterData.monster_curr);
	acc = hu(acc, gITManagerMonsterData.monster_prev);
	acc = hu(acc, gITManagerMonsterData.monsters_num);

	for (count = 0; count < (u32)(sizeof(gITManagerMonsterData.monster_id) / sizeof(gITManagerMonsterData.monster_id[0])); count++)
	{
		acc = hu(acc, gITManagerMonsterData.monster_id[count]);
	}
	acc = hu(acc, gITManagerAppearActor.mapobjs_num);
	acc = hu(acc, gITManagerAppearActor.spawn_wait);
	/* ITRandomWeights holds pointers (kinds, blocks): hash the scalars only */
	acc = hu(acc, gITManagerAppearActor.weights.valids_num);
	acc = hu(acc, gITManagerAppearActor.weights.weights_sum);
	acc = hu(acc, gITManagerRandomWeights.valids_num);
	acc = hu(acc, gITManagerRandomWeights.weights_sum);

	return acc;
}

static u32 syNetSyncHashWPAttackColl(u32 h, WPAttackColl *ac)
{
	s32 i;

	h = hs(h, ac->attack_state);
	h = hs(h, ac->damage);
	h = hf(h, ac->stale);
	h = hs(h, ac->element);

	for (i = 0; i < WEAPON_ATKCOLL_NUM_MAX; i++)
	{
		h = hv3(h, &ac->offsets[i]);
	}
	h = hf(h, ac->size);
	h = hs(h, ac->angle);
	h = hu(h, ac->knockback_scale);
	h = hu(h, ac->knockback_weight);
	h = hu(h, ac->knockback_base);
	h = hs(h, ac->shield_damage);
	h = hs(h, ac->priority);
	h = hu(h, ac->interact_mask);
	h = hu(h, ac->fgm_id);
	h = hu(h, ac->can_setoff);
	h = hu(h, ac->can_rehit_item);
	h = hu(h, ac->can_rehit_fighter);
	h = hu(h, ac->can_rehit_shield);
	h = hu(h, ac->can_hop);
	h = hu(h, ac->can_reflect);
	h = hu(h, ac->can_absorb);
	h = hu(h, ac->can_not_heal);
	h = hu(h, ac->can_shield);
	h = hu(h, ac->motion_attack_id);
	h = hu(h, ac->motion_count);
	h = syNetSyncHashStatFlags(h, &ac->stat_flags);
	h = hu(h, ac->stat_count);
	h = hs(h, ac->attack_count);

	for (i = 0; i < WEAPON_ATKCOLL_NUM_MAX; i++)
	{
		h = hv3(h, &ac->attack_pos[i].pos_curr);
		h = hv3(h, &ac->attack_pos[i].pos_prev);
		h = hs(h, ac->attack_pos[i].unk_wphitpos_0x18);
	}
	if (ac->attack_state != nGMAttackStateOff)
	{
		h = syNetSyncHashAttackRecords(h, ac->attack_records, GMATTACKREC_NUM_MAX);
	}

	return h;
}

static u32 syNetSyncHashWeapon(WPStruct *wp)
{
	u32 h = SYNETSYNC_FNV_BASIS;

	h = hid(h, wp->owner_gobj);
	h = hs(h, wp->kind);
	h = hu(h, wp->team);
	h = hu(h, wp->player);
	h = hu(h, wp->handicap);
	h = hs(h, wp->player_num);
	h = hs(h, wp->lr);
	h = hf(h, wp->physics.vel_ground);
	h = hv3(h, &wp->physics.vel_air);
	h = syNetSyncHashMPCollData(h, &wp->coll_data);
	h = hs(h, wp->ga);
	h = syNetSyncHashWPAttackColl(h, &wp->attack_coll);
	h = hs(h, wp->hit_normal_damage);
	h = hs(h, wp->hit_refresh_damage);
	h = hs(h, wp->hit_attack_damage);
	h = hs(h, wp->hit_shield_damage);
	h = hf(h, wp->shield_collide_angle);
	h = hv3(h, &wp->shield_collide_dir);
	h = hid(h, wp->reflect_gobj);
#if defined(REGION_US)
	h = syNetSyncHashStatFlags(h, &wp->reflect_stat_flags);
	h = hu(h, wp->reflect_stat_count);
#endif
	h = hid(h, wp->absorb_gobj);
	h = hu(h, wp->is_hitlag_victim);
	h = hu(h, wp->is_hitlag_weapon);
	h = hu(h, wp->group_id);
	h = hs(h, wp->lifetime);
	h = hu(h, wp->is_camera_follow);
	h = hu(h, wp->is_static_damage);

	return h;
}

u32 syNetSyncHashWeapons(void)
{
	GObj *weapon_gobj;
	u32 acc = SYNETSYNC_FNV_BASIS;
	u32 count = 0;

	for (weapon_gobj = gGCCommonLinks[nGCCommonLinkIDWeapon]; weapon_gobj != NULL;
	     weapon_gobj = weapon_gobj->link_next)
	{
		WPStruct *wp = wpGetStruct(weapon_gobj);

		if (wp == NULL)
		{
			continue;
		}
		acc ^= hu(syNetSyncHashWeapon(wp), syNetSyncObjectKey(weapon_gobj));
		count++;
	}
	return hu(acc, count);
}

u32 syNetSyncHashStage(void)
{
	u32 h = SYNETSYNC_FNV_BASIS;
	s32 i;

	{
		/* MPAllBounds = 4 x MPBounds = 16 f32, no padding */
		const f32 *bounds = (const f32 *)&gMPCollisionBounds;

		for (i = 0; i < (s32)(sizeof(gMPCollisionBounds) / sizeof(f32)); i++)
		{
			h = hf(h, bounds[i]);
		}
	}
	h = hu(h, gMPCollisionUpdateTic);
	h = hs(h, gMPCollisionLinesNum);
	h = hs(h, gMPCollisionYakumonosNum);

	for (i = 0; i < gMPCollisionYakumonosNum; i++)
	{
		if (gMPCollisionSpeeds != NULL)
		{
			h = hv3(h, &gMPCollisionSpeeds[i]);
		}
		if (gMPCollisionYakumonoDObjs != NULL)
		{
			h = syNetSyncHashDObjPose(h, gMPCollisionYakumonoDObjs->dobjs[i]);
		}
	}
	return h;
}

u32 syNetSyncHashBattleState(void)
{
	SCBattleState *bs = gSCManagerBattleState;
	u32 h = SYNETSYNC_FNV_BASIS;
	s32 i;
	s32 j;

	if (bs == NULL)
	{
		return hu(h, SYNETSYNC_NULL_ID);
	}
	h = hu(h, bs->game_type);
	h = hu(h, bs->gkind);
	h = hu(h, bs->is_team_battle);
	h = hu(h, bs->game_rules);
	h = hu(h, bs->pl_count);
	h = hu(h, bs->cp_count);
	h = hu(h, bs->time_limit);
	h = hu(h, bs->stocks);
	h = hu(h, bs->handicap);
	h = hu(h, bs->is_team_attack);
	h = hu(h, bs->is_stage_select);
	h = hu(h, bs->damage_ratio);
	h = hu(h, bs->item_toggles);
	h = hu(h, bs->is_reset_players);
	h = hu(h, bs->game_status);
	h = hu(h, bs->time_remain);
	h = hu(h, bs->time_passed);
	h = hu(h, bs->item_appearance_rate);
	h = hu(h, bs->is_show_score);
	h = hu(h, bs->is_not_teamshadows);

	for (i = 0; i < GMCOMMON_PLAYERS_MAX; i++)
	{
		SCPlayerData *pd = &bs->players[i];

		h = hu(h, pd->level);
		h = hu(h, pd->handicap);
		h = hu(h, pd->pkind);
		h = hu(h, pd->fkind);
		h = hu(h, pd->team);
		h = hu(h, pd->player);
		h = hu(h, pd->costume);
		h = hu(h, pd->shade);
		h = hu(h, pd->color);
		h = hu(h, pd->is_single_stockicon);
		h = hu(h, pd->tag);
		h = hs(h, pd->stock_count);
		h = hu(h, pd->is_spgame_enemy);
		h = hu(h, pd->place);
		h = hs(h, pd->falls);
		h = hs(h, pd->score);

		for (j = 0; j < GMCOMMON_PLAYERS_MAX; j++)
		{
			h = hs(h, pd->total_kos_players[j]);
		}
		h = hs(h, pd->unk_pblock_0x28);
		h = hs(h, pd->unk_pblock_0x2C);
		h = hs(h, pd->total_selfdestructs);
		h = hs(h, pd->total_damage_given);
		h = hs(h, pd->total_damage_all);

		for (j = 0; j < GMCOMMON_PLAYERS_MAX; j++)
		{
			h = hs(h, pd->total_damage_players[j]);
		}
		h = hs(h, pd->stock_damage_all);
		h = hs(h, pd->combo_damage_foe);
		h = hs(h, pd->combo_count_foe);
		h = hid(h, pd->fighter_gobj);
		h = hu(h, pd->stale_id);

		for (j = 0; j < 5; j++)
		{
			h = hu(h, pd->stale_info[j].attack_id);
			h = hu(h, pd->stale_info[j].motion_count);
		}
	}
	h = hu(h, gFTManagerPlayersNum);
	h = hu(h, gFTManagerMotionCount);
	h = hu(h, gFTManagerStatUpdateCount);

	return h;
}

u32 syNetSyncHashRNG(void)
{
	u32 h = SYNETSYNC_FNV_BASIS;

	h = hs(h, syUtilsRandSeed());
	h = hu(h, (sSYUtilsRandomSeedPtr == &sSYUtilsRandomSeed) ? 1U : 0U);

	return h;
}

/* Liveness of every gameplay link: object count and the XOR of ids. */
u32 syNetSyncHashObjectManager(void)
{
	u32 h = SYNETSYNC_FNV_BASIS;
	s32 link_id;

	for (link_id = nGCCommonLinkIDGround; link_id <= nGCCommonLinkIDWeapon; link_id++)
	{
		GObj *gobj;
		u32 count = 0;
		u32 ids = 0;

		for (gobj = gGCCommonLinks[link_id]; gobj != NULL; gobj = gobj->link_next)
		{
			count++;
			ids ^= hu(SYNETSYNC_FNV_BASIS, syNetSyncObjectKey(gobj));
		}
		h = hu(h, (u32)link_id);
		h = hu(h, count);
		h = hu(h, ids);
	}
	return h;
}

u32 syNetSyncHashCamera(void)
{
	GMCamera *cam = &gGMCameraStruct;
	u32 h = SYNETSYNC_FNV_BASIS;

	h = hs(h, cam->status_default);
	h = hs(h, cam->status_curr);
	h = hs(h, cam->status_prev);
	h = hnn(h, (const void *)cam->func_camera);
	h = hf(h, cam->target_dist);
	h = hv3(h, &cam->vel_at);
	h = hs(h, cam->viewport_ulx);
	h = hs(h, cam->viewport_uly);
	h = hs(h, cam->viewport_lrx);
	h = hs(h, cam->viewport_lry);
	h = hs(h, cam->viewport_center_x);
	h = hs(h, cam->viewport_center_y);
	h = hs(h, cam->viewport_width);
	h = hs(h, cam->viewport_height);
	h = hf(h, cam->fovy);
	h = hid(h, cam->pzoom_fighter_gobj);
	h = hf(h, cam->pzoom_eye_x);
	h = hf(h, cam->pzoom_eye_y);
	h = hf(h, cam->pzoom_dist);
	h = hf(h, cam->pzoom_pan_scale);
	h = hf(h, cam->pzoom_fov);
	h = hv3(h, &cam->zoom_origin_pos);
	h = hv3(h, &cam->zoom_target_pos);
	h = hid(h, cam->pfollow_fighter_gobj);
	h = hf(h, cam->pfollow_eye_x);
	h = hf(h, cam->pfollow_eye_y);
	h = hf(h, cam->pfollow_dist);
	h = hf(h, cam->pfollow_pan_scale);
	h = hf(h, cam->pfollow_fov);
	h = hv3(h, &cam->vel_all);
	h = hf(h, gGMCameraPauseCameraEyeX);
	h = hf(h, gGMCameraPauseCameraEyeY);

	return h;
}

/* ------------------------------------------------------------------------ */
/* per-kind unions: word-wise with pointer slots masked (generated table)    */

#ifdef PORT
/* offset/size of one pointer field; for pointers inside arrays (of pointers or
 * of structs) the slot repeats `count` times every `stride` bytes */
typedef struct SYNetSyncPtrSlot
{
	u16 offset;
	u16 size;
	u16 stride;
	u16 count;

} SYNetSyncPtrSlot;

static sb32 syNetSyncPtrSlotContains(const SYNetSyncPtrSlot *slot, size_t off, sb32 *is_start)
{
	u32 k;

	for (k = 0; k < slot->count; k++)
	{
		size_t start = (size_t)slot->offset + (size_t)k * slot->stride;

		if ((off >= start) && (off < start + slot->size))
		{
			*is_start = (off == start) ? TRUE : FALSE;
			return TRUE;
		}
	}
	return FALSE;
}

/* Hash `len` bytes of `base` as u32 words, except the pointer slots, which
 * contribute only a non-null bit. The slot table is generated from the
 * declared types (t1/gen_union_hashers.py) and computed with offsetof in
 * this build, so each compiler masks its own layout. Layout differences
 * between builds therefore show up as a `vars` divergence - deliberate. */
static u32 syNetSyncHashMaskedWords(u32 h, const void *base, size_t len, const SYNetSyncPtrSlot *slots, s32 slot_count)
{
	const u8 *bytes = (const u8 *)base;
	size_t off = 0;

	while (off + sizeof(u32) <= len)
	{
		s32 i;
		sb32 in_slot = FALSE;

		for (i = 0; i < slot_count; i++)
		{
			sb32 is_start;

			if (syNetSyncPtrSlotContains(&slots[i], off, &is_start) != FALSE)
			{
				if (is_start != FALSE)
				{
					const void *ptr;

					memcpy(&ptr, bytes + off, sizeof(ptr));
					h = hu(h, (ptr != NULL) ? 1U : 0U);
				}
				in_slot = TRUE;
				break;
			}
		}
		if (in_slot == FALSE)
		{
			u32 word;

			memcpy(&word, bytes + off, sizeof(word));
			h = hu(h, word);
		}
		off += sizeof(u32);
	}
	return hu(h, (u32)len);
}

#include "netsyncvars.inc"
#endif /* PORT: pointer-slot machinery + generated table */

u32 syNetSyncHashVars(void)
{
#ifdef PORT
	GObj *gobj;
	u32 slot_hash[GMCOMMON_PLAYERS_MAX];
	u32 acc;
	s32 si;

	for (si = 0; si < GMCOMMON_PLAYERS_MAX; si++)
	{
		slot_hash[si] = SYNETSYNC_FNV_BASIS;
	}
	for (gobj = gGCCommonLinks[nGCCommonLinkIDFighter]; gobj != NULL; gobj = gobj->link_next)
	{
		FTStruct *fp = ftGetStruct(gobj);
		u32 h = SYNETSYNC_FNV_BASIS;

		if (fp == NULL)
		{
			continue;
		}
		h = syNetSyncHashFTStatusVars(h, fp);
		h = syNetSyncHashFTPassiveVars(h, fp);
		syNetSyncAccumulateSlot(slot_hash, fp->player, h);
	}
	acc = syNetSyncMergeSlots(slot_hash);

	for (gobj = gGCCommonLinks[nGCCommonLinkIDItem]; gobj != NULL; gobj = gobj->link_next)
	{
		ITStruct *ip = itGetStruct(gobj);

		if (ip != NULL)
		{
			acc ^= hu(syNetSyncHashITStatusVars(SYNETSYNC_FNV_BASIS, ip), syNetSyncObjectKey(gobj));
		}
	}
	for (gobj = gGCCommonLinks[nGCCommonLinkIDWeapon]; gobj != NULL; gobj = gobj->link_next)
	{
		WPStruct *wp = wpGetStruct(gobj);

		if (wp != NULL)
		{
			acc ^= hu(syNetSyncHashWPStatusVars(SYNETSYNC_FNV_BASIS, wp), syNetSyncObjectKey(gobj));
		}
	}
	acc = syNetSyncHashGRStruct(acc, &gGRCommonStruct);

	return acc;
#else
	return SYNETSYNC_FNV_BASIS;
#endif
}

void syNetSyncHashTick(SYNetSyncTickHash *out)
{
	u32 full = SYNETSYNC_FNV_BASIS;
	s32 i;

	syNetSyncBuildLiveTable();
	out->column[nSYNetSyncColumnRNG] = syNetSyncHashRNG();
	out->column[nSYNetSyncColumnBattle] = syNetSyncHashBattleState();
	out->column[nSYNetSyncColumnFighters] = syNetSyncHashFighters();
	out->column[nSYNetSyncColumnItems] = syNetSyncHashItems();
	out->column[nSYNetSyncColumnWeapons] = syNetSyncHashWeapons();
	out->column[nSYNetSyncColumnStage] = syNetSyncHashStage();
	out->column[nSYNetSyncColumnObjMan] = syNetSyncHashObjectManager();
	out->column[nSYNetSyncColumnInput] = syNetInputGetPublishedInputChecksum();
	out->column[nSYNetSyncColumnJoints] = syNetSyncHashJoints();
	out->column[nSYNetSyncColumnCamera] = syNetSyncHashCamera();
	out->column[nSYNetSyncColumnVars] = syNetSyncHashVars();

	for (i = nSYNetSyncColumnRNG; i <= nSYNetSyncColumnObjMan; i++)
	{
		full = hu(full, out->column[i]);
	}
	out->column[nSYNetSyncColumnFull] = full;
}

/* ------------------------------------------------------------------------ */
/* trace file + verify                                                       */

const char *syNetSyncGetColumnName(s32 column)
{
	if ((column < 0) || (column >= SYNETSYNC_COLUMN_NUM))
	{
		return "?";
	}
	return sSYNetSyncColumnNames[column];
}

void syNetSyncInitDebugEnv(void)
{
#ifdef PORT
	sSYNetSyncTracePath = getenv("SSB64_SYNC_TRACE");
	sSYNetSyncVerifyPath = getenv("SSB64_SYNC_VERIFY");
	sSYNetSyncIsTraceEnabled = (sSYNetSyncTracePath != NULL) ? TRUE : FALSE;
	sSYNetSyncIsVerifyEnabled = (sSYNetSyncVerifyPath != NULL) ? TRUE : FALSE;
	{
		const char *dump_env = getenv("SSB64_SYNC_DUMP_TICK");

		sSYNetSyncDumpTick = (dump_env != NULL) ? (u32)strtoul(dump_env, NULL, 10) : 0xFFFFFFFFU;
	}
	if ((sSYNetSyncIsTraceEnabled != FALSE) || (sSYNetSyncIsVerifyEnabled != FALSE))
	{
		/* Layout probe: these must agree between the two builds being compared.
		 * (FTCommandVars is suspected to be 20 bytes on MSVC vs 16 on clang.) */
		port_log("SSB64 SyncTrace: sizeof FTStruct=%u ITStruct=%u WPStruct=%u MPCollData=%u FTCommandVars=%u "
		         "FTComputer=%u FTAttackColl=%u SCBattleState=%u GObj=%u DObj=%u\n",
		         (u32)sizeof(FTStruct), (u32)sizeof(ITStruct), (u32)sizeof(WPStruct), (u32)sizeof(MPCollData),
		         (u32)sizeof(union FTCommandVars), (u32)sizeof(FTComputer), (u32)sizeof(FTAttackColl),
		         (u32)sizeof(SCBattleState), (u32)sizeof(GObj), (u32)sizeof(DObj));
	}
#endif
}

#ifdef PORT
static sb32 syNetSyncLoadVerifyFile(const char *path)
{
	FILE *fp;
	char line[512];
	u32 capacity = 0;
	u32 count = 0;

	fp = fopen(path, "r");

	if (fp == NULL)
	{
		port_log("SSB64 SyncTrace: failed to open verify path=%s\n", path);
		return FALSE;
	}
	while (fgets(line, sizeof(line), fp) != NULL)
	{
		SYNetSyncTickHash rec;
		u32 tick;
		s32 n;

		if ((line[0] == '#') || (line[0] == '\n') || (line[0] == '\r'))
		{
			continue;
		}
		{
			/* "tick hex hex ..." - one token per column, count must match this build */
			char *cursor = line;
			char *end;

			tick = (u32)strtoul(cursor, &end, 10);
			n = (end != cursor) ? 1 : 0;
			cursor = end;

			while ((n >= 1) && (n < 1 + SYNETSYNC_COLUMN_NUM))
			{
				u32 value = (u32)strtoul(cursor, &end, 16);

				if (end == cursor)
				{
					break;
				}
				rec.column[n - 1] = value;
				cursor = end;
				n++;
			}
		}
		if (n != 1 + SYNETSYNC_COLUMN_NUM)
		{
			port_log("SSB64 SyncTrace: bad line %u in verify file (%d fields)\n", count, n);
			fclose(fp);
			free(sSYNetSyncVerifyRecords);
			sSYNetSyncVerifyRecords = NULL;
			return FALSE;
		}
		if (tick != count)
		{
			port_log("SSB64 SyncTrace: verify file tick %u out of order (expected %u)\n", tick, count);
			fclose(fp);
			free(sSYNetSyncVerifyRecords);
			sSYNetSyncVerifyRecords = NULL;
			return FALSE;
		}
		if (count == capacity)
		{
			SYNetSyncTickHash *grown;

			capacity = (capacity == 0) ? 2048 : capacity * 2;
			grown = (SYNetSyncTickHash *)realloc(sSYNetSyncVerifyRecords, capacity * sizeof(SYNetSyncTickHash));

			if (grown == NULL)
			{
				fclose(fp);
				free(sSYNetSyncVerifyRecords);
				sSYNetSyncVerifyRecords = NULL;
				return FALSE;
			}
			sSYNetSyncVerifyRecords = grown;
		}
		sSYNetSyncVerifyRecords[count] = rec;
		count++;
	}
	fclose(fp);
	sSYNetSyncVerifyLoadedTicks = count;

	return (count != 0) ? TRUE : FALSE;
}
#endif

#ifdef PORT
/* the decomp's include/string.h shadows the host header; keep this local */
static sb32 syNetSyncPathsEqual(const char *a, const char *b)
{
	while ((*a != 0) && (*a == *b))
	{
		a++;
		b++;
	}
	return (*a == *b) ? TRUE : FALSE;
}
#endif

void syNetSyncStartVSSession(void)
{
	sSYNetSyncTickCount = 0;
	sSYNetSyncVerifyComparedTicks = 0;
	sSYNetSyncVerifyDivergences = 0;
	sSYNetSyncVerifyFirstDivergenceTick = 0;
	sSYNetSyncVerifyFirstDivergenceMask = 0;
	sSYNetSyncVerifyColumnReported = 0;
	sSYNetSyncVerifyLoadedTicks = 0;
	sSYNetSyncIsVerifyLoaded = FALSE;
	sSYNetSyncIsTickMismatchReported = FALSE;
	sSYNetSyncIsSlotFallbackReported = FALSE;
	sSYNetSyncIsSessionActive = FALSE;
	sSYNetSyncSpawnSerial = 0;

#ifdef PORT
	/* a session that never finished (rematch, forced scene reset) must not leak */
	if (sSYNetSyncTraceFile != NULL)
	{
		fclose(sSYNetSyncTraceFile);
		sSYNetSyncTraceFile = NULL;
	}
	if (sSYNetSyncVerifyRecords != NULL)
	{
		free(sSYNetSyncVerifyRecords);
		sSYNetSyncVerifyRecords = NULL;
	}
	if ((sSYNetSyncIsTraceEnabled != FALSE) && (sSYNetSyncIsVerifyEnabled != FALSE) &&
		(syNetSyncPathsEqual(sSYNetSyncTracePath, sSYNetSyncVerifyPath) != FALSE))
	{
		/* opening the trace for writing would truncate the oracle before it is read */
		port_log("SSB64 SyncTrace: SSB64_SYNC_TRACE and SSB64_SYNC_VERIFY name the same file; trace disabled\n");
		sSYNetSyncIsTraceEnabled = FALSE;
	}
	if (sSYNetSyncIsTraceEnabled != FALSE)
	{
		sSYNetSyncTraceFile = fopen(sSYNetSyncTracePath, "w");

		if (sSYNetSyncTraceFile == NULL)
		{
			port_log("SSB64 SyncTrace: failed to open trace path=%s\n", sSYNetSyncTracePath);
		}
		else
		{
			s32 i;

			fprintf(sSYNetSyncTraceFile, "# ssb64h v1 tick");

			for (i = 0; i < SYNETSYNC_COLUMN_NUM; i++)
			{
				fprintf(sSYNetSyncTraceFile, " %s", sSYNetSyncColumnNames[i]);
			}
			fprintf(sSYNetSyncTraceFile, "\n");
			port_log("SSB64 SyncTrace: trace start path=%s\n", sSYNetSyncTracePath);
		}
	}
	if (sSYNetSyncIsVerifyEnabled != FALSE)
	{
		if (syNetSyncLoadVerifyFile(sSYNetSyncVerifyPath) != FALSE)
		{
			sSYNetSyncIsVerifyLoaded = TRUE;
			port_log("SSB64 SyncTrace: verify start path=%s ticks=%u\n", sSYNetSyncVerifyPath,
			         sSYNetSyncVerifyLoadedTicks);
		}
		else
		{
			port_log("SSB64 SyncTrace: verify requested but not loaded path=%s result=LOADFAIL\n",
			         sSYNetSyncVerifyPath);
		}
	}
	sSYNetSyncIsSessionActive = ((sSYNetSyncTraceFile != NULL) || (sSYNetSyncVerifyRecords != NULL)) ? TRUE : FALSE;
#endif
}

#ifdef PORT
/* Attribution aid for a `fighters` divergence: the group hashes and the
 * identity-bearing fields of every fighter at one tick. Compare the lines
 * from the two builds. */
static void syNetSyncDumpFighters(u32 tick)
{
	GObj *gobj;

	for (gobj = gGCCommonLinks[nGCCommonLinkIDFighter]; gobj != NULL; gobj = gobj->link_next)
	{
		FTStruct *fp = ftGetStruct(gobj);
		u32 g_coll;
		u32 g_computer;
		u32 g_attack[4];
		u32 g_damage;
		u32 g_scripts;
		s32 i;

		if (fp == NULL)
		{
			continue;
		}
		g_coll = syNetSyncHashMPCollData(SYNETSYNC_FNV_BASIS, &fp->coll_data);
		g_computer = syNetSyncHashFTComputer(SYNETSYNC_FNV_BASIS, &fp->computer);
		g_damage = SYNETSYNC_FNV_BASIS;

		for (i = 0; i < 4; i++)
		{
			g_attack[i] = syNetSyncHashFTAttackColl(SYNETSYNC_FNV_BASIS, &fp->attack_colls[i]);
		}
		for (i = 0; i < 11; i++)
		{
			g_damage = syNetSyncHashFTDamageColl(g_damage, &fp->damage_colls[i]);
		}
		g_scripts = SYNETSYNC_FNV_BASIS;
		g_scripts = syNetSyncHashFTMotionScript(g_scripts, &fp->motion_scripts[0][0]);
		g_scripts = syNetSyncHashFTMotionScript(g_scripts, &fp->motion_scripts[0][1]);
		g_scripts = syNetSyncHashFTMotionScript(g_scripts, &fp->motion_scripts[1][0]);
		g_scripts = syNetSyncHashFTMotionScript(g_scripts, &fp->motion_scripts[1][1]);

		port_log("SSB64 SyncDump: tick=%u p%d fkind=%d status=%d motion=%d pct=%d hit=%d dmgplayer=%d inv=%d "
		         "coll=%08X computer=%08X atk=%08X/%08X/%08X/%08X dmgcoll=%08X scripts=%08X "
		         "throw=%08X search=%08X catch=%08X capture=%08X item=%08X target=%08X\n",
		         tick, fp->player, fp->fkind, fp->status_id, fp->motion_id, fp->percent_damage, fp->hitstatus,
		         fp->damage_player, fp->invincible_tics, g_coll, g_computer, g_attack[0], g_attack[1], g_attack[2],
		         g_attack[3], g_damage, g_scripts, syNetSyncLookupKey(fp->throw_gobj),
		         syNetSyncLookupKey(fp->search_gobj), syNetSyncLookupKey(fp->catch_gobj),
		         syNetSyncLookupKey(fp->capture_gobj), syNetSyncLookupKey(fp->item_gobj),
		         syNetSyncLookupKey(fp->computer.target_gobj));

		for (i = 0; i < 4; i++)
		{
			s32 r;

			for (r = 0; r < GMATTACKREC_NUM_MAX; r++)
			{
				GMAttackRecord *rec = &fp->attack_colls[i].attack_records[r];

				if (rec->victim_gobj != NULL)
				{
					port_log("SSB64 SyncDump: tick=%u p%d atk%d rec%d victim=%08X state=%d hurt=%u shield=%u "
					         "reflect=%u absorb=%u group=%u rehit=%u\n",
					         tick, fp->player, i, r, syNetSyncLookupKey(rec->victim_gobj),
					         fp->attack_colls[i].attack_state, rec->victim_flags.is_interact_hurt,
					         rec->victim_flags.is_interact_shield, rec->victim_flags.is_interact_reflect,
					         rec->victim_flags.is_interact_absorb, rec->victim_flags.group_id,
					         rec->victim_flags.timer_rehit);
				}
			}
		}
	}
}
#endif

void syNetSyncRecordTick(void)
{
#ifdef PORT
	SYNetSyncTickHash rec;
	u32 tick;
	s32 i;

	if (sSYNetSyncIsSessionActive == FALSE)
	{
		return;
	}
	tick = sSYNetSyncTickCount;
	syNetSyncHashTick(&rec);

	if ((tick >= sSYNetSyncDumpTick) && (tick <= sSYNetSyncDumpTick + 2))
	{
		syNetSyncDumpFighters(tick);
	}

	/* The hook must run once per advanced input tick; if it ever drifts from
	 * netinput's count the two traces are offset and the diff blames the
	 * wrong tick - say so loudly, once. */
	if ((sSYNetSyncIsTickMismatchReported == FALSE) && (syNetInputGetPublishedTickCount() != tick + 1))
	{
		sSYNetSyncIsTickMismatchReported = TRUE;
		port_log("SSB64 SyncTrace: WARNING trace tick %u but netinput published %u ticks\n", tick,
		         syNetInputGetPublishedTickCount());
	}
	if (sSYNetSyncTraceFile != NULL)
	{
		fprintf(sSYNetSyncTraceFile, "%u", tick);

		for (i = 0; i < SYNETSYNC_COLUMN_NUM; i++)
		{
			fprintf(sSYNetSyncTraceFile, " %08X", rec.column[i]);
		}
		fprintf(sSYNetSyncTraceFile, "\n");
		/* the tick before a crash is the one that matters; ~110 bytes at 60 Hz is nothing */
		fflush(sSYNetSyncTraceFile);
	}
	if ((sSYNetSyncVerifyRecords != NULL) && (tick < sSYNetSyncVerifyLoadedTicks))
	{
		SYNetSyncTickHash *expected = &sSYNetSyncVerifyRecords[tick];
		u32 mask = 0;

		for (i = 0; i < SYNETSYNC_COLUMN_NUM; i++)
		{
			if (expected->column[i] != rec.column[i])
			{
				mask |= (1U << i);
			}
		}
		sSYNetSyncVerifyComparedTicks++;

		/* gated columns only: full..objman. input/joints/camera are reported, not counted */
		if ((mask & SYNETSYNC_GATED_COLUMN_MASK) != 0)
		{
			if (sSYNetSyncVerifyDivergences == 0)
			{
				sSYNetSyncVerifyFirstDivergenceTick = tick;
				sSYNetSyncVerifyFirstDivergenceMask = mask;
			}
			sSYNetSyncVerifyDivergences++;
		}
		for (i = 0; i < SYNETSYNC_COLUMN_NUM; i++)
		{
			if (((mask & (1U << i)) != 0) && ((sSYNetSyncVerifyColumnReported & (1U << i)) == 0))
			{
				sSYNetSyncVerifyColumnReported |= (1U << i);
				port_log("SSB64 SyncTrace: FIRST DIVERGENCE tick=%u column=%s expected=0x%08X actual=0x%08X%s\n", tick,
				         sSYNetSyncColumnNames[i], expected->column[i], rec.column[i],
				         ((SYNETSYNC_GATED_COLUMN_MASK & (1U << i)) != 0) ? "" : " (not gated)");
			}
		}
	}
	sSYNetSyncTickCount++;
#endif
}

void syNetSyncFinishVSSession(void)
{
#ifdef PORT
	if (sSYNetSyncTraceFile != NULL)
	{
		fclose(sSYNetSyncTraceFile);
		sSYNetSyncTraceFile = NULL;
		port_log("SSB64 SyncTrace: trace wrote path=%s ticks=%u\n", sSYNetSyncTracePath, sSYNetSyncTickCount);
	}
	if (sSYNetSyncVerifyRecords != NULL)
	{
		port_log("SSB64 SyncTrace: verify ticks=%u compared=%u divergences=%u first_tick=%u first_mask=0x%03X result=%s\n",
		         sSYNetSyncTickCount, sSYNetSyncVerifyComparedTicks, sSYNetSyncVerifyDivergences,
		         sSYNetSyncVerifyFirstDivergenceTick, sSYNetSyncVerifyFirstDivergenceMask,
		         (sSYNetSyncVerifyDivergences == 0) ? "PASS" : "FAIL");
		free(sSYNetSyncVerifyRecords);
		sSYNetSyncVerifyRecords = NULL;
	}
	sSYNetSyncIsSessionActive = FALSE;
#endif
}

/* nSYNetSyncVerifyNotRequested = no SSB64_SYNC_VERIFY; NotLoaded = requested
 * but the file failed to load (a missing oracle must never read as PASS);
 * Diverged = a gated column differed; Short = the loaded trace ended before
 * the ticks recorded so far (only partly verified); Pass otherwise. */
s32 syNetSyncGetVerifyResult(void)
{
#ifdef PORT
	if (sSYNetSyncIsVerifyEnabled == FALSE)
	{
		return nSYNetSyncVerifyNotRequested;
	}
	if (sSYNetSyncIsVerifyLoaded == FALSE)
	{
		return nSYNetSyncVerifyNotLoaded;
	}
	if (sSYNetSyncVerifyDivergences != 0)
	{
		return nSYNetSyncVerifyDiverged;
	}
	if (sSYNetSyncTickCount > sSYNetSyncVerifyLoadedTicks)
	{
		return nSYNetSyncVerifyShort;
	}
	return nSYNetSyncVerifyPass;
#else
	return nSYNetSyncVerifyNotRequested;
#endif
}

u32 syNetSyncGetVerifyComparedTicks(void)
{
	return sSYNetSyncVerifyComparedTicks;
}
