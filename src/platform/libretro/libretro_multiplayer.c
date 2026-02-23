#include "libretro_multiplayer.h"

#include <mgba/core/config.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba-util/memory.h>
#include <mgba-util/vfs.h>

#include <fcntl.h>
#include <string.h>

#define VIDEO_BYTES_PER_PIXEL sizeof(mColor)
#define LOCKSTEP_PUMP_WATCHDOG 2000000

static void _stepRunnerCore(struct mCore* core) {
	if (!core) {
		return;
	}
	if (core->runLoop) {
		core->runLoop(core);
		return;
	}
	if (core->step) {
		core->step(core);
		return;
	}
	if (core->runFrame) {
		core->runFrame(core);
	}
}

static void _lockstepSleep(struct mLockstepUser* user) {
	struct mLibretroLockstepUser* lockstepUser = (struct mLibretroLockstepUser*) user;
	lockstepUser->blocked = true;

	if (!lockstepUser->multiplayer || !lockstepUser->multiplayer->active || !lockstepUser->multiplayer->secondaryCore || !lockstepUser->multiplayer->primaryCore) {
		return;
	}

	if (lockstepUser->stepping || lockstepUser->multiplayer->pumping) {
		return;
	}

	struct mCore* runner = lockstepUser->playerIndex == 0 ?
			lockstepUser->multiplayer->secondaryCore :
			lockstepUser->multiplayer->primaryCore;
	unsigned runnerIndex = lockstepUser->playerIndex == 0 ? 1 : 0;
	if (!runner || (!runner->step && !runner->runLoop && !runner->runFrame)) {
		return;
	}

	lockstepUser->multiplayer->pumping = true;
	lockstepUser->stepping = true;
	int watchdog = LOCKSTEP_PUMP_WATCHDOG;
	while (lockstepUser->blocked && watchdog-- > 0) {
		if (!lockstepUser->multiplayer->active) {
			break;
		}
		lockstepUser->multiplayer->pumpedThisFrame[runnerIndex] = true;
		_stepRunnerCore(runner);
	}
	lockstepUser->stepping = false;
	lockstepUser->multiplayer->pumping = false;
	mASSERT_LOG(GBA_SIO, !lockstepUser->blocked, "Lockstep single-thread watchdog expired while waiting for peer wake");
}

static void _lockstepWake(struct mLockstepUser* user) {
	struct mLibretroLockstepUser* lockstepUser = (struct mLibretroLockstepUser*) user;
	lockstepUser->blocked = false;
}

static int _requestedId(struct mLockstepUser* user) {
	struct mLibretroLockstepUser* lockstepUser = (struct mLibretroLockstepUser*) user;
	return lockstepUser->requestedId;
}

static enum mLibretroSplitscreenMode _parseMode(retro_environment_t environCallback) {
	struct retro_variable var = {
		.key = "mgba_multiplayer_splitscreen",
		.value = 0,
	};

	if (!environCallback || !environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value) {
		return mLIBRETRO_SPLITSCREEN_OFF;
	}

	if (strcmp(var.value, "Side by Side") == 0) {
		return mLIBRETRO_SPLITSCREEN_2P_VERTICAL;
	}

	if (strcmp(var.value, "Top/Bottom") == 0) {
		return mLIBRETRO_SPLITSCREEN_2P_HORIZONTAL;
	}

	return mLIBRETRO_SPLITSCREEN_OFF;
}

static size_t _compositePixels(unsigned maxVideoWidth, unsigned maxVideoHeight) {
	return (size_t) maxVideoWidth * 2 * maxVideoHeight * 2;
}

static void _clearPrimaryLinkPeripheral(struct mCore* primaryCore) {
	if (!primaryCore || primaryCore->platform(primaryCore) != mPLATFORM_GBA) {
		return;
	}
	primaryCore->setPeripheral(primaryCore, mPERIPH_GBA_LINK_PORT, NULL);
}

static void _detachLockstep(struct mLibretroMultiplayer* multiplayer, struct mCore* primaryCore) {
	if (!multiplayer->coordinatorInitialized) {
		return;
	}

	if (primaryCore && primaryCore->platform(primaryCore) == mPLATFORM_GBA) {
		GBASIOLockstepCoordinatorDetach(&multiplayer->coordinator, &multiplayer->drivers[0]);
	}

	if (multiplayer->secondaryCore && multiplayer->secondaryCore->platform(multiplayer->secondaryCore) == mPLATFORM_GBA) {
		multiplayer->secondaryCore->setPeripheral(multiplayer->secondaryCore, mPERIPH_GBA_LINK_PORT, NULL);
		GBASIOLockstepCoordinatorDetach(&multiplayer->coordinator, &multiplayer->drivers[1]);
	}

	GBASIOLockstepCoordinatorDeinit(&multiplayer->coordinator);
	multiplayer->coordinatorInitialized = false;
}

static void _destroySecondaryCore(struct mLibretroMultiplayer* multiplayer) {
	if (!multiplayer->secondaryCore) {
		return;
	}

	mCoreConfigDeinit(&multiplayer->secondaryCore->config);
	multiplayer->secondaryCore->deinit(multiplayer->secondaryCore);
	multiplayer->secondaryCore = NULL;
}

static void _destroyBuffers(struct mLibretroMultiplayer* multiplayer) {
	if (multiplayer->secondaryOutputBuffer) {
		free(multiplayer->secondaryOutputBuffer);
		multiplayer->secondaryOutputBuffer = NULL;
	}

	if (multiplayer->compositeBuffer) {
		free(multiplayer->compositeBuffer);
		multiplayer->compositeBuffer = NULL;
		multiplayer->compositeBufferPixels = 0;
	}
}

static void _destroySecondaryRom(struct mLibretroMultiplayer* multiplayer) {
	if (multiplayer->secondaryRomData) {
		mappedMemoryFree(multiplayer->secondaryRomData, multiplayer->secondaryRomSize);
		multiplayer->secondaryRomData = NULL;
		multiplayer->secondaryRomSize = 0;
	}
}

static void _stopSession(struct mLibretroMultiplayer* multiplayer, struct mCore* primaryCore) {
	_clearPrimaryLinkPeripheral(primaryCore);
	_detachLockstep(multiplayer, primaryCore);
	_destroySecondaryCore(multiplayer);
	_destroyBuffers(multiplayer);
	_destroySecondaryRom(multiplayer);
	multiplayer->primaryCore = NULL;
	multiplayer->active = false;
}

static bool _initSecondaryCore(struct mLibretroMultiplayer* multiplayer, const void* romData, size_t romSize, const char* romPath) {
	struct VFile* rom;

	if (romData && romSize) {
		multiplayer->secondaryRomData = anonymousMemoryMap(romSize);
		if (!multiplayer->secondaryRomData) {
			return false;
		}
		multiplayer->secondaryRomSize = romSize;
		memcpy(multiplayer->secondaryRomData, romData, romSize);

		rom = VFileFromMemory(multiplayer->secondaryRomData, romSize);
		if (!rom) {
			return false;
		}
	} else if (romPath && *romPath) {
		rom = VFileOpen(romPath, O_RDONLY);
		if (!rom) {
			return false;
		}
	} else {
		return false;
	}

	multiplayer->secondaryCore = mCoreFindVF(rom);
	if (!multiplayer->secondaryCore) {
		rom->close(rom);
		return false;
	}

	mCoreInitConfig(multiplayer->secondaryCore, NULL);
	multiplayer->secondaryCore->init(multiplayer->secondaryCore);

	multiplayer->secondaryOutputBuffer = malloc((size_t) multiplayer->maxVideoWidth * multiplayer->maxVideoHeight * VIDEO_BYTES_PER_PIXEL);
	if (!multiplayer->secondaryOutputBuffer) {
		rom->close(rom);
		return false;
	}
	memset(multiplayer->secondaryOutputBuffer, 0xFF, (size_t) multiplayer->maxVideoWidth * multiplayer->maxVideoHeight * VIDEO_BYTES_PER_PIXEL);
	multiplayer->secondaryCore->setVideoBuffer(multiplayer->secondaryCore, multiplayer->secondaryOutputBuffer, multiplayer->maxVideoWidth);

	memset(&multiplayer->secondaryStream, 0, sizeof(multiplayer->secondaryStream));
	multiplayer->secondaryCore->setAVStream(multiplayer->secondaryCore, &multiplayer->secondaryStream);

	if (!multiplayer->secondaryCore->loadROM(multiplayer->secondaryCore, rom)) {
		rom->close(rom);
		return false;
	}

	multiplayer->secondaryCore->reset(multiplayer->secondaryCore);
	return true;
}

static bool _attachLockstep(struct mLibretroMultiplayer* multiplayer, struct mCore* primaryCore) {
	if (!primaryCore || primaryCore->platform(primaryCore) != mPLATFORM_GBA) {
		return false;
	}
	if (!multiplayer->secondaryCore || multiplayer->secondaryCore->platform(multiplayer->secondaryCore) != mPLATFORM_GBA) {
		return false;
	}

	GBASIOLockstepCoordinatorInit(&multiplayer->coordinator);
	multiplayer->coordinatorInitialized = true;

	multiplayer->users[0].d.sleep = _lockstepSleep;
	multiplayer->users[0].d.wake = _lockstepWake;
	multiplayer->users[0].d.requestedId = _requestedId;
	multiplayer->users[0].d.playerIdChanged = NULL;
	multiplayer->users[0].requestedId = 0;
	multiplayer->users[0].multiplayer = multiplayer;
	multiplayer->users[0].playerIndex = 0;
	multiplayer->users[0].blocked = false;
	multiplayer->users[0].stepping = false;

	multiplayer->users[1].d.sleep = _lockstepSleep;
	multiplayer->users[1].d.wake = _lockstepWake;
	multiplayer->users[1].d.requestedId = _requestedId;
	multiplayer->users[1].d.playerIdChanged = NULL;
	multiplayer->users[1].requestedId = 1;
	multiplayer->users[1].multiplayer = multiplayer;
	multiplayer->users[1].playerIndex = 1;
	multiplayer->users[1].blocked = false;
	multiplayer->users[1].stepping = false;

	GBASIOLockstepDriverCreate(&multiplayer->drivers[0], &multiplayer->users[0].d);
	GBASIOLockstepDriverCreate(&multiplayer->drivers[1], &multiplayer->users[1].d);

	GBASIOLockstepCoordinatorAttach(&multiplayer->coordinator, &multiplayer->drivers[0]);
	GBASIOLockstepCoordinatorAttach(&multiplayer->coordinator, &multiplayer->drivers[1]);

	primaryCore->setPeripheral(primaryCore, mPERIPH_GBA_LINK_PORT, &multiplayer->drivers[0].d);
	multiplayer->secondaryCore->setPeripheral(multiplayer->secondaryCore, mPERIPH_GBA_LINK_PORT, &multiplayer->drivers[1].d);
	return true;
}

static bool _startSession(struct mLibretroMultiplayer* multiplayer, struct mCore* primaryCore, const void* romData, size_t romSize, const char* romPath) {
	if (!primaryCore || primaryCore->platform(primaryCore) != mPLATFORM_GBA || !romData || !romSize) {
		if (!(romPath && *romPath)) {
			return false;
		}
	}

	if (!_initSecondaryCore(multiplayer, romData, romSize, romPath)) {
		_stopSession(multiplayer, primaryCore);
		return false;
	}

	multiplayer->compositeBufferPixels = _compositePixels(multiplayer->maxVideoWidth, multiplayer->maxVideoHeight);
	multiplayer->compositeBuffer = malloc(multiplayer->compositeBufferPixels * VIDEO_BYTES_PER_PIXEL);
	if (!multiplayer->compositeBuffer) {
		_stopSession(multiplayer, primaryCore);
		return false;
	}
	memset(multiplayer->compositeBuffer, 0xFF, multiplayer->compositeBufferPixels * VIDEO_BYTES_PER_PIXEL);

	if (!_attachLockstep(multiplayer, primaryCore)) {
		_stopSession(multiplayer, primaryCore);
		return false;
	}
	multiplayer->primaryCore = primaryCore;

	multiplayer->active = true;
	return true;
}

void mLibretroMultiplayerInit(struct mLibretroMultiplayer* multiplayer, unsigned maxVideoWidth, unsigned maxVideoHeight) {
	memset(multiplayer, 0, sizeof(*multiplayer));
	multiplayer->maxVideoWidth = maxVideoWidth;
	multiplayer->maxVideoHeight = maxVideoHeight;
	multiplayer->mode = mLIBRETRO_SPLITSCREEN_OFF;
	multiplayer->primaryCore = NULL;
}

void mLibretroMultiplayerDeinit(struct mLibretroMultiplayer* multiplayer, struct mCore* primaryCore) {
	_stopSession(multiplayer, primaryCore);
	multiplayer->mode = mLIBRETRO_SPLITSCREEN_OFF;
}

void mLibretroMultiplayerUpdateMode(struct mLibretroMultiplayer* multiplayer, retro_environment_t environCallback) {
	multiplayer->mode = _parseMode(environCallback);
}

bool mLibretroMultiplayerApplyMode(struct mLibretroMultiplayer* multiplayer, struct mCore* primaryCore, const void* romData, size_t romSize, const char* romPath, retro_log_printf_t logCallback) {
	if (multiplayer->mode == mLIBRETRO_SPLITSCREEN_OFF) {
		if (multiplayer->active) {
			_stopSession(multiplayer, primaryCore);
		}
		return true;
	}

	if (multiplayer->active) {
		return true;
	}

	if (!_startSession(multiplayer, primaryCore, romData, romSize, romPath)) {
		if (logCallback) {
			logCallback(RETRO_LOG_WARN, "libretro: failed to start multiplayer splitscreen session; continuing in single-player mode\n");
		}
		multiplayer->mode = mLIBRETRO_SPLITSCREEN_OFF;
		return false;
	}

	if (logCallback) {
		logCallback(RETRO_LOG_INFO, "libretro: started 2-player splitscreen multiplayer session\n");
	}

	return true;
}

void mLibretroMultiplayerSetKeys(struct mLibretroMultiplayer* multiplayer, struct mCore* primaryCore, uint16_t player1Keys, uint16_t player2Keys) {
	if (!primaryCore) {
		return;
	}

	primaryCore->setKeys(primaryCore, player1Keys);

	if (!multiplayer->active || !multiplayer->secondaryCore) {
		return;
	}

	multiplayer->secondaryCore->setKeys(multiplayer->secondaryCore, player2Keys);
}

void mLibretroMultiplayerRunFrame(struct mLibretroMultiplayer* multiplayer, struct mCore* primaryCore) {
	if (!primaryCore) {
		return;
	}
	multiplayer->primaryCore = primaryCore;
	multiplayer->pumpedThisFrame[0] = false;
	multiplayer->pumpedThisFrame[1] = false;

	primaryCore->runFrame(primaryCore);

	if (multiplayer->active && multiplayer->secondaryCore && !multiplayer->pumpedThisFrame[1]) {
		multiplayer->secondaryCore->runFrame(multiplayer->secondaryCore);
	}
}

void mLibretroMultiplayerAdjustGeometry(const struct mLibretroMultiplayer* multiplayer, unsigned* baseWidth, unsigned* baseHeight, unsigned* maxWidth, unsigned* maxHeight, float* aspectRatio) {
	if (!multiplayer->active) {
		return;
	}

	if (multiplayer->mode == mLIBRETRO_SPLITSCREEN_2P_VERTICAL) {
		*baseWidth *= 2;
		*maxWidth *= 2;
		*aspectRatio *= 2.0;
		return;
	}

	if (multiplayer->mode == mLIBRETRO_SPLITSCREEN_2P_HORIZONTAL) {
		*baseHeight *= 2;
		*maxHeight *= 2;
		*aspectRatio *= 0.5;
	}
}

const mColor* mLibretroMultiplayerComposeFrame(struct mLibretroMultiplayer* multiplayer, const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	if (!multiplayer->active || !multiplayer->secondaryCore || !multiplayer->compositeBuffer || !multiplayer->secondaryOutputBuffer) {
		*outPitch = (size_t) primaryWidth * VIDEO_BYTES_PER_PIXEL;
		*outWidth = primaryWidth;
		*outHeight = primaryHeight;
		return primaryFrame;
	}

	unsigned secondaryWidth, secondaryHeight;
	multiplayer->secondaryCore->currentVideoSize(multiplayer->secondaryCore, &secondaryWidth, &secondaryHeight);

	if (multiplayer->mode == mLIBRETRO_SPLITSCREEN_2P_VERTICAL) {
		size_t y;
		*outWidth = primaryWidth + secondaryWidth;
		*outHeight = primaryHeight > secondaryHeight ? primaryHeight : secondaryHeight;
		*outPitch = (size_t) *outWidth * VIDEO_BYTES_PER_PIXEL;
		memset(multiplayer->compositeBuffer, 0xFF, (size_t) *outWidth * *outHeight * VIDEO_BYTES_PER_PIXEL);
		for (y = 0; y < primaryHeight; ++y) {
			mColor* row = &multiplayer->compositeBuffer[y * (*outWidth)];
			memcpy(row, &primaryFrame[y * multiplayer->maxVideoWidth], (size_t) primaryWidth * VIDEO_BYTES_PER_PIXEL);
		}
		for (y = 0; y < secondaryHeight; ++y) {
			mColor* row = &multiplayer->compositeBuffer[y * (*outWidth) + primaryWidth];
			memcpy(row, &multiplayer->secondaryOutputBuffer[y * multiplayer->maxVideoWidth], (size_t) secondaryWidth * VIDEO_BYTES_PER_PIXEL);
		}
		return multiplayer->compositeBuffer;
	}

	if (multiplayer->mode == mLIBRETRO_SPLITSCREEN_2P_HORIZONTAL) {
		size_t y;
		*outWidth = primaryWidth > secondaryWidth ? primaryWidth : secondaryWidth;
		*outHeight = primaryHeight + secondaryHeight;
		*outPitch = (size_t) *outWidth * VIDEO_BYTES_PER_PIXEL;
		memset(multiplayer->compositeBuffer, 0xFF, (size_t) *outWidth * *outHeight * VIDEO_BYTES_PER_PIXEL);
		for (y = 0; y < primaryHeight; ++y) {
			mColor* row = &multiplayer->compositeBuffer[y * (*outWidth)];
			memcpy(row, &primaryFrame[y * multiplayer->maxVideoWidth], (size_t) primaryWidth * VIDEO_BYTES_PER_PIXEL);
		}
		for (y = 0; y < secondaryHeight; ++y) {
			mColor* row = &multiplayer->compositeBuffer[(y + primaryHeight) * (*outWidth)];
			memcpy(row, &multiplayer->secondaryOutputBuffer[y * multiplayer->maxVideoWidth], (size_t) secondaryWidth * VIDEO_BYTES_PER_PIXEL);
		}
		return multiplayer->compositeBuffer;
	}

	*outPitch = (size_t) primaryWidth * VIDEO_BYTES_PER_PIXEL;
	*outWidth = primaryWidth;
	*outHeight = primaryHeight;
	return primaryFrame;
}
