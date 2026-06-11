#include <ft/fighter.h>
#include <lb/library.h>
#include <sc/scene.h>
#include <sys/video.h>
#ifdef PORT
#include <sys/audio.h>
#include <sys/dma.h>
#endif

// // // // // // // // // // // //
//                               //
//           FUNCTIONS           //
//                               //
// // // // // // // // // // // //

// 0x800D4520
s32 lbBackupCreateChecksum(LBBackupData *backup)
{
    s32 i, checksum = 0;
    u8 *bytes = (u8*)backup;

    for (i = 0; i < (sizeof(LBBackupData) - sizeof(gSCManagerBackupData.checksum)); i++)
    {
        checksum += *bytes++ * (i + 1);
    }
    return checksum;
}

// 0x800D45A4
sb32 lbBackupIsChecksumValid(void)
{
    if ((lbBackupCreateChecksum(&gSCManagerBackupData) == gSCManagerBackupData.checksum) && (gSCManagerBackupData.signature == 666)) // :otstare:
    {
        return TRUE;
    }
    else return FALSE;
}

// 0x800D45F4
void lbBackupWrite(void)
{
    gSCManagerBackupData.checksum = lbBackupCreateChecksum(&gSCManagerBackupData);
    syDmaWriteSram(&gSCManagerBackupData, ALIGN(sizeof(LBBackupData),  0x0), sizeof(LBBackupData));
    syDmaWriteSram(&gSCManagerBackupData, ALIGN(sizeof(LBBackupData), 0x10), sizeof(LBBackupData));
}

// 0x800D4644
sb32 lbBackupIsSramValid(void)
{
    syDmaReadSram(ALIGN(sizeof(LBBackupData), 0x0), &gSCManagerBackupData, sizeof(LBBackupData));

    if (lbBackupIsChecksumValid() == FALSE)
    {
        syDmaReadSram(ALIGN(sizeof(LBBackupData), 0x10), &gSCManagerBackupData, sizeof(LBBackupData));

        if (lbBackupIsChecksumValid() == FALSE)
        {
            gSCManagerBackupData = dSCManagerDefaultBackupData;

            lbBackupWrite();

            return FALSE;
        }
        lbBackupWrite();
    }
    return TRUE;
}

// 0x800D46F4
void lbBackupApplyOptions(void)
{
    syAudioSetQuality(gSCManagerBackupData.sound_mono_or_stereo);
    syVideoSetCenterOffsets
    (
        gSCManagerBackupData.screen_adjust_h, gSCManagerBackupData.screen_adjust_h, 
        gSCManagerBackupData.screen_adjust_v, gSCManagerBackupData.screen_adjust_v
    );
}

#ifdef PORT
/* synth fkinds (>= 32 with enough mods) make the unlock-mask shift UB, and a
 * synth is never in the u16 mask anyway; treat anything past the vanilla
 * playables as not locked so synth selections survive uncorrected */
static sb32 lbBackupFighterIsLocked(u32 fkind)
{
    return (fkind < GMCOMMON_FIGHTERS_PLAYABLE_NUM) &&
           !((gSCManagerBackupData.fighter_mask | LBBACKUP_CHARACTER_MASK_STARTER) & (1 << fkind));
}
#endif

// 0x800D473C
void lbBackupCorrectErrors(void)
{
    s32 i;

#ifdef PORT
    if (lbBackupFighterIsLocked(gSCManagerBackupData.characters_fkind))
#else
    if (!((gSCManagerBackupData.fighter_mask | LBBACKUP_CHARACTER_MASK_STARTER) & (1 << gSCManagerBackupData.characters_fkind)))
#endif
    {
        gSCManagerBackupData.characters_fkind = dSCManagerDefaultBackupData.characters_fkind;
    }
#ifdef PORT
    if (lbBackupFighterIsLocked(gSCManagerSceneData.fkind))
#else
    if (!((gSCManagerBackupData.fighter_mask | LBBACKUP_CHARACTER_MASK_STARTER) & (1 << gSCManagerSceneData.fkind)))
#endif
    {
        gSCManagerSceneData.fkind = nFTKindNull;
    }
#ifdef PORT
    if (lbBackupFighterIsLocked(gSCManagerSceneData.training_man_fkind))
#else
    if (!((gSCManagerBackupData.fighter_mask | LBBACKUP_CHARACTER_MASK_STARTER) & (1 << gSCManagerSceneData.training_man_fkind)))
#endif
    {
        gSCManagerSceneData.training_man_fkind = nFTKindNull;
    }
#ifdef PORT
    if (lbBackupFighterIsLocked(gSCManagerSceneData.training_com_fkind))
#else
    if (!((gSCManagerBackupData.fighter_mask | LBBACKUP_CHARACTER_MASK_STARTER) & (1 << gSCManagerSceneData.training_com_fkind)))
#endif
    {
        gSCManagerSceneData.training_com_fkind = nFTKindNull;
    }
    for (i = 0; i < ARRAY_COUNT(gSCManagerTransferBattleState.players); i++)
    {
#ifdef PORT
        if (lbBackupFighterIsLocked(gSCManagerTransferBattleState.players[i].fkind))
#else
        if (!((1 << gSCManagerTransferBattleState.players[i].fkind) & (gSCManagerBackupData.fighter_mask | LBBACKUP_CHARACTER_MASK_STARTER)))
#endif
        {
            gSCManagerTransferBattleState.players[i].fkind = nFTKindNull;
            gSCManagerTransferBattleState.players[i].pkind = nFTPlayerKindMan;
        }
    }
    if (!(gSCManagerBackupData.unlock_mask & LBBACKUP_UNLOCK_MASK_INISHIE))
    {
        if (gSCManagerSceneData.maps_vsmode_gkind == nGRKindInishie)
        {
            gSCManagerSceneData.maps_vsmode_gkind = dSCManagerDefaultSceneData.maps_vsmode_gkind;
        }
        if (gSCManagerSceneData.maps_training_gkind == nGRKindInishie)
        {
            gSCManagerSceneData.maps_training_gkind = dSCManagerDefaultSceneData.maps_training_gkind;
        }
    }
#if defined(REGION_US)
    if (!(gSCManagerBackupData.unlock_mask & LBBACKUP_UNLOCK_MASK_ITEMSWITCH))
    {
        gSCManagerTransferBattleState.item_toggles = dSCManagerDefaultBattleState.item_toggles;
        gSCManagerTransferBattleState.item_appearance_rate  = dSCManagerDefaultBattleState.item_appearance_rate;
    }
#endif
}

// 0x800D48E0
void lbBackupClearNewcomers(void)
{
    gSCManagerBackupData.unlock_mask &= ~LBBACKUP_UNLOCK_MASK_NEWCOMERS;
    gSCManagerBackupData.unlock_mask |= dSCManagerDefaultBackupData.unlock_mask;

    gSCManagerBackupData.fighter_mask = dSCManagerDefaultBackupData.fighter_mask;
}

// 0x800D4914
void lbBackupClear1PHighScore(void)
{
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gSCManagerBackupData.spgame_records); i++)
    {
        gSCManagerBackupData.spgame_records[i].spgame_hiscore         = dSCManagerDefaultBackupData.spgame_records[i].spgame_hiscore;
        gSCManagerBackupData.spgame_records[i].spgame_continues       = dSCManagerDefaultBackupData.spgame_records[i].spgame_continues;
        gSCManagerBackupData.spgame_records[i].spgame_total_bonuses         = dSCManagerDefaultBackupData.spgame_records[i].spgame_total_bonuses;
        gSCManagerBackupData.spgame_records[i].spgame_best_difficulty = dSCManagerDefaultBackupData.spgame_records[i].spgame_best_difficulty;
        gSCManagerBackupData.spgame_records[i].is_spgame_complete     = dSCManagerDefaultBackupData.spgame_records[i].is_spgame_complete;
    }
}

//0x800D49E0
void lbBackupClearVSRecord(void)
{
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gSCManagerBackupData.vs_records); i++)
    {
        gSCManagerBackupData.vs_records[i] = dSCManagerDefaultBackupData.vs_records[i];
    }
    gSCManagerBackupData.vs_total_battles = dSCManagerDefaultBackupData.vs_total_battles;
}

// 0x800D4B60
void lbBackupClearBonusStageTime(void)
{
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gSCManagerBackupData.spgame_records); i++)
    {
        gSCManagerBackupData.spgame_records[i].bonus1_time       = dSCManagerDefaultBackupData.spgame_records[i].bonus1_time;
        gSCManagerBackupData.spgame_records[i].bonus1_task_count = dSCManagerDefaultBackupData.spgame_records[i].bonus1_task_count;
        gSCManagerBackupData.spgame_records[i].bonus2_time       = dSCManagerDefaultBackupData.spgame_records[i].bonus2_time;
        gSCManagerBackupData.spgame_records[i].bonus2_task_count = dSCManagerDefaultBackupData.spgame_records[i].bonus2_task_count;
    }
}

// 0x800D4C0C
void lbBackupClearPrize(void)
{
    gSCManagerBackupData.unlock_mask &= ~LBBACKUP_UNLOCK_MASK_PRIZE;
    gSCManagerBackupData.unlock_mask |= dSCManagerDefaultBackupData.unlock_mask;

    gSCManagerBackupData.ground_mask = dSCManagerDefaultBackupData.ground_mask;
    gSCManagerBackupData.vs_itemswitch_battles = dSCManagerDefaultBackupData.vs_itemswitch_battles;
}

// 0x800D4C48
void lbBackupClearAllData(void)
{
    gSCManagerBackupData = dSCManagerDefaultBackupData;
}

// 0x800D4C90 - unused
void func_ovl0_800D4C90(void)
{
    return;
}

extern int port_cheat_unlock_all(void);
extern int port_cheat_unlock_luigi(void);
extern int port_cheat_unlock_ness(void);
extern int port_cheat_unlock_captain(void);
extern int port_cheat_unlock_purin(void);
extern int port_cheat_unlock_inishie(void);
extern int port_cheat_unlock_soundtest(void);
extern int port_cheat_unlock_itemswitch(void);

void lbBackupApplyCheats(void) {
    // If the master cheat is checked, unlock everything instantly
    if (port_cheat_unlock_all()) {
        gSCManagerBackupData.fighter_mask |= LBBACKUP_CHARACTER_MASK_ALL;
        gSCManagerBackupData.unlock_mask  |= LBBACKUP_UNLOCK_MASK_ALL;
    } else {
        // Otherwise, check them individually
        if (port_cheat_unlock_luigi())   gSCManagerBackupData.fighter_mask |= LBBACKUP_MASK_FIGHTER(nFTKindLuigi);
        if (port_cheat_unlock_ness())    gSCManagerBackupData.fighter_mask |= LBBACKUP_MASK_FIGHTER(nFTKindNess);
        if (port_cheat_unlock_captain()) gSCManagerBackupData.fighter_mask |= LBBACKUP_MASK_FIGHTER(nFTKindCaptain);
        if (port_cheat_unlock_purin())   gSCManagerBackupData.fighter_mask |= LBBACKUP_MASK_FIGHTER(nFTKindPurin);

        if (port_cheat_unlock_inishie())    gSCManagerBackupData.unlock_mask |= LBBACKUP_UNLOCK_MASK_INISHIE;
        if (port_cheat_unlock_soundtest())  gSCManagerBackupData.unlock_mask |= LBBACKUP_UNLOCK_MASK_SOUNDTEST;
        if (port_cheat_unlock_itemswitch()) gSCManagerBackupData.unlock_mask |= LBBACKUP_UNLOCK_MASK_ITEMSWITCH;
    }
}
