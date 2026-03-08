#ifndef MGBA_LIBRETRO_MULTIPLAYER_H
#define MGBA_LIBRETRO_MULTIPLAYER_H

#include <mgba-util/common.h>

#include <mgba/core/core.h>
#include <mgba/core/lockstep.h>
#include <mgba/internal/gba/sio.h>
#include "libretro_lockstep.h"

#include "libretro.h"

CXX_GUARD_START

enum mLibretroSplitscreenMode {
	mLIBRETRO_SPLITSCREEN_OFF = 0,
	mLIBRETRO_SPLITSCREEN_2P_VERTICAL,
	mLIBRETRO_SPLITSCREEN_2P_HORIZONTAL,
	mLIBRETRO_SPLITSCREEN_4P_GRID,
};

enum mLibretroDisplayPlayers {
	mLIBRETRO_DISPLAY_SELF = 0,
	mLIBRETRO_DISPLAY_ALL,
};

struct mLibretroLockstepUser {
	struct mLockstepUser d;
	int requestedId;
	struct mLibretroMultiplayer* multiplayer;
	unsigned playerIndex;
	bool blocked;
};

struct mLibretroMultiplayerDisplay {
	enum mLibretroSplitscreenMode mode;
	enum mLibretroDisplayPlayers displayPlayers;
	int localPlayerIndex;
	unsigned maxVideoWidth;
	unsigned maxVideoHeight;
	mColor* compositeBuffer;
	size_t compositeBufferPixels;
};

struct mLibretroMultiplayer {
	struct mLibretroMultiplayerDisplay video;

	int numPlayers;
	struct mCore* cores[MAX_GBAS];
	mColor* outputBuffers[MAX_GBAS];
	struct mAVStream streams[MAX_GBAS];

	void* romData;
	size_t romSize;

	struct mLibretroLockstepUser users[MAX_GBAS];
	struct GBASIOLockstepCoordinator coordinator;
	struct GBASIOLockstepDriver drivers[MAX_GBAS];
	bool coordinatorInitialized;
};

static inline struct mCore* mLibretroMultiplayerGetPrimaryCore(struct mLibretroMultiplayer* mp) {
	return mp->cores[0];
}

void mLibretroMultiplayerInit(struct mLibretroMultiplayer* multiplayer, unsigned maxVideoWidth, unsigned maxVideoHeight);
void mLibretroMultiplayerSetPrimaryCore(struct mCore* primaryCore);
void mLibretroMultiplayerDeinit(void);
void mLibretroMultiplayerUpdateMode(retro_environment_t environCallback);
void mLibretroMultiplayerUpdateDisplayPlayers(retro_environment_t environCallback);
bool mLibretroMultiplayerApplyMode(const void* romData, size_t romSize, const char* romPath);
void mLibretroMultiplayerReset(void);
void mLibretroMultiplayerSetKeys(uint16_t keys[MAX_GBAS]);
void mLibretroMultiplayerRunFrame(void);
void mLibretroMultiplayerAdjustGeometry(struct retro_game_geometry* geometry);
const mColor* mLibretroMultiplayerComposeFrame(const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight);
bool mLibretroMultiplayerStateActive(void);
int mLibretroMultiplayerConfiguredPlayers(void);
const struct mLibretroMultiplayer* mLibretroMultiplayerGet(void);

CXX_GUARD_END

#endif
