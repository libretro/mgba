#ifndef MGBA_LIBRETRO_MULTIPLAYER_H
#define MGBA_LIBRETRO_MULTIPLAYER_H

#include <mgba-util/common.h>

#include <mgba/core/core.h>
#include <mgba/core/lockstep.h>
#include <mgba/internal/gba/sio/lockstep.h>

#include "libretro.h"

CXX_GUARD_START

enum mLibretroSplitscreenMode {
	mLIBRETRO_SPLITSCREEN_OFF = 0,
	mLIBRETRO_SPLITSCREEN_2P_VERTICAL,
	mLIBRETRO_SPLITSCREEN_2P_HORIZONTAL,
};

struct mLibretroLockstepUser {
	struct mLockstepUser d;
	int requestedId;
	struct mLibretroMultiplayer* multiplayer;
	unsigned playerIndex;
	bool blocked;
	bool stepping;
};

struct mLibretroMultiplayer {
	enum mLibretroSplitscreenMode mode;
	bool active;
	unsigned maxVideoWidth;
	unsigned maxVideoHeight;
	struct mCore* primaryCore;

	struct mCore* secondaryCore;
	mColor* secondaryOutputBuffer;
	mColor* compositeBuffer;
	size_t compositeBufferPixels;

	void* secondaryRomData;
	size_t secondaryRomSize;

	struct mAVStream secondaryStream;
	bool pumping;
	bool pumpedThisFrame[2];

	struct mLibretroLockstepUser users[2];
	struct GBASIOLockstepCoordinator coordinator;
	struct GBASIOLockstepDriver drivers[2];
	bool coordinatorInitialized;
};

void mLibretroMultiplayerInit(struct mLibretroMultiplayer* multiplayer, unsigned maxVideoWidth, unsigned maxVideoHeight);
void mLibretroMultiplayerDeinit(struct mLibretroMultiplayer* multiplayer, struct mCore* primaryCore);
void mLibretroMultiplayerUpdateMode(struct mLibretroMultiplayer* multiplayer, retro_environment_t environCallback);
bool mLibretroMultiplayerApplyMode(struct mLibretroMultiplayer* multiplayer, struct mCore* primaryCore, const void* romData, size_t romSize, const char* romPath, retro_log_printf_t logCallback);
void mLibretroMultiplayerReset(struct mLibretroMultiplayer* multiplayer, struct mCore* primaryCore);
void mLibretroMultiplayerSetKeys(struct mLibretroMultiplayer* multiplayer, struct mCore* primaryCore, uint16_t player1Keys, uint16_t player2Keys);
void mLibretroMultiplayerRunFrame(struct mLibretroMultiplayer* multiplayer, struct mCore* primaryCore);
void mLibretroMultiplayerAdjustGeometry(const struct mLibretroMultiplayer* multiplayer, unsigned* baseWidth, unsigned* baseHeight, unsigned* maxWidth, unsigned* maxHeight, float* aspectRatio);
const mColor* mLibretroMultiplayerComposeFrame(struct mLibretroMultiplayer* multiplayer, const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight);

CXX_GUARD_END

#endif
