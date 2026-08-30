#ifndef _SYNETSYNC_H_
#define _SYNETSYNC_H_

#include <PR/ultratypes.h>

/* Columns of the per-tick state trace. full..objman are gated (folded into
 * full and counted as divergences); input, joints and camera are recorded for
 * diagnosis only. Order is the file column order - append, never reorder. */
enum SYNetSyncColumn
{
	nSYNetSyncColumnFull,
	nSYNetSyncColumnRNG,
	nSYNetSyncColumnBattle,
	nSYNetSyncColumnFighters,
	nSYNetSyncColumnItems,
	nSYNetSyncColumnWeapons,
	nSYNetSyncColumnStage,
	nSYNetSyncColumnObjMan,
	nSYNetSyncColumnInput,
	nSYNetSyncColumnJoints,
	nSYNetSyncColumnCamera,
	nSYNetSyncColumnVars,
	SYNETSYNC_COLUMN_NUM
};

#define SYNETSYNC_GATED_COLUMN_MASK ((1U << (nSYNetSyncColumnObjMan + 1)) - 1U)

typedef struct SYNetSyncTickHash
{
	u32 column[SYNETSYNC_COLUMN_NUM];

} SYNetSyncTickHash;

enum SYNetSyncVerifyResult
{
	nSYNetSyncVerifyNotRequested = -1,
	nSYNetSyncVerifyPass = 0,
	nSYNetSyncVerifyDiverged = 1,
	nSYNetSyncVerifyNotLoaded = 2,
	nSYNetSyncVerifyShort = 3
};

extern u32 syNetSyncHashBattleFighters(void);
extern u32 syNetSyncNextSpawnSerial(void);

extern u32 syNetSyncHashFighters(void);
extern u32 syNetSyncHashJoints(void);
extern u32 syNetSyncHashItems(void);
extern u32 syNetSyncHashWeapons(void);
extern u32 syNetSyncHashStage(void);
extern u32 syNetSyncHashBattleState(void);
extern u32 syNetSyncHashRNG(void);
extern u32 syNetSyncHashObjectManager(void);
extern u32 syNetSyncHashCamera(void);
extern u32 syNetSyncHashVars(void);
extern void syNetSyncHashTick(SYNetSyncTickHash *out);
extern const char *syNetSyncGetColumnName(s32 column);

extern void syNetSyncInitDebugEnv(void);
extern void syNetSyncStartVSSession(void);
extern void syNetSyncRecordTick(void);
extern void syNetSyncFinishVSSession(void);
extern s32 syNetSyncGetVerifyResult(void);
extern u32 syNetSyncGetVerifyComparedTicks(void);

#endif /* _SYNETSYNC_H_ */
