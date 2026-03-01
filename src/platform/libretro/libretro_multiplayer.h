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

	struct mLibretroLockstepUser users[2];
	struct GBASIOLockstepCoordinator coordinator;
	struct GBASIOLockstepDriver drivers[2];
	bool coordinatorInitialized;
};

void mLibretroMultiplayerInit(struct mLibretroMultiplayer* multiplayer, unsigned maxVideoWidth, unsigned maxVideoHeight);
void mLibretroMultiplayerSetPrimaryCore(struct mCore* primaryCore);
void mLibretroMultiplayerDeinit(void);
void mLibretroMultiplayerUpdateMode(retro_environment_t environCallback);
bool mLibretroMultiplayerApplyMode(const void* romData, size_t romSize, const char* romPath, retro_log_printf_t logCallback);
void mLibretroMultiplayerReset(void);
void mLibretroMultiplayerSetKeys(uint16_t player1Keys, uint16_t player2Keys);
void mLibretroMultiplayerRunFrame(void);
void mLibretroMultiplayerAdjustGeometry(struct retro_game_geometry* geometry);
const mColor* mLibretroMultiplayerComposeFrame(const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight);
bool mLibretroMultiplayerStateActive(void);
size_t mLibretroMultiplayerSerializeSize(void);
bool mLibretroMultiplayerSerialize(void* data, size_t size);
bool mLibretroMultiplayerUnserialize(const void* data, size_t size);

CXX_GUARD_END

#endif
