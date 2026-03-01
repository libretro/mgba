#include "libretro_multiplayer.h"

#include <mgba/core/config.h>
#include <mgba/core/serialize.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba-util/memory.h>
#include <mgba-util/vfs.h>

#include <fcntl.h>
#include <string.h>

#define VIDEO_BYTES_PER_PIXEL sizeof(mColor)
#define LOCKSTEP_PUMP_WATCHDOG 2000000

static struct mLibretroMultiplayer* sMultiplayer;

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
	if (!lockstepUser->multiplayer || !lockstepUser->multiplayer->active || !lockstepUser->multiplayer->secondaryCore || !lockstepUser->multiplayer->primaryCore) {
		return;
	}

	if (lockstepUser->stepping || lockstepUser->multiplayer->pumping) {
		return;
	}

	lockstepUser->blocked = true;

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
	if (watchdog <= 0) {
		mLOG(GBA_SIO, FATAL, "Lockstep single-thread watchdog expired while waiting for peer wake");
		mASSERT(false);
	}
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

static void _detachLockstep(void) {
	struct mCore* primaryCore = sMultiplayer->primaryCore;
	if (!sMultiplayer->coordinatorInitialized) {
		return;
	}

	if (primaryCore && primaryCore->platform(primaryCore) == mPLATFORM_GBA) {
		GBASIOLockstepCoordinatorDetach(&sMultiplayer->coordinator, &sMultiplayer->drivers[0]);
	}

	if (sMultiplayer->secondaryCore && sMultiplayer->secondaryCore->platform(sMultiplayer->secondaryCore) == mPLATFORM_GBA) {
		sMultiplayer->secondaryCore->setPeripheral(sMultiplayer->secondaryCore, mPERIPH_GBA_LINK_PORT, NULL);
		GBASIOLockstepCoordinatorDetach(&sMultiplayer->coordinator, &sMultiplayer->drivers[1]);
	}

	GBASIOLockstepCoordinatorDeinit(&sMultiplayer->coordinator);
	sMultiplayer->coordinatorInitialized = false;
}

static void _destroySecondaryCore(void) {
	if (!sMultiplayer->secondaryCore) {
		return;
	}

	mCoreConfigDeinit(&sMultiplayer->secondaryCore->config);
	sMultiplayer->secondaryCore->deinit(sMultiplayer->secondaryCore);
	sMultiplayer->secondaryCore = NULL;
}

static void _destroyBuffers(void) {
	if (sMultiplayer->secondaryOutputBuffer) {
		free(sMultiplayer->secondaryOutputBuffer);
		sMultiplayer->secondaryOutputBuffer = NULL;
	}

	if (sMultiplayer->compositeBuffer) {
		free(sMultiplayer->compositeBuffer);
		sMultiplayer->compositeBuffer = NULL;
		sMultiplayer->compositeBufferPixels = 0;
	}
}

static void _destroySecondaryRom(void) {
	if (sMultiplayer->secondaryRomData) {
		mappedMemoryFree(sMultiplayer->secondaryRomData, sMultiplayer->secondaryRomSize);
		sMultiplayer->secondaryRomData = NULL;
		sMultiplayer->secondaryRomSize = 0;
	}
}

static void _stopSession(void) {
	struct mCore* primaryCore = sMultiplayer->primaryCore;
	_clearPrimaryLinkPeripheral(primaryCore);
	_detachLockstep();
	_destroySecondaryCore();
	_destroyBuffers();
	_destroySecondaryRom();
	sMultiplayer->primaryCore = NULL;
	sMultiplayer->active = false;
}

static bool _initSecondaryCore(const void* romData, size_t romSize, const char* romPath) {
	struct VFile* rom;

	if (romData && romSize) {
		sMultiplayer->secondaryRomData = anonymousMemoryMap(romSize);
		if (!sMultiplayer->secondaryRomData) {
			return false;
		}
		sMultiplayer->secondaryRomSize = romSize;
		memcpy(sMultiplayer->secondaryRomData, romData, romSize);

		rom = VFileFromMemory(sMultiplayer->secondaryRomData, romSize);
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

	sMultiplayer->secondaryCore = mCoreFindVF(rom);
	if (!sMultiplayer->secondaryCore) {
		rom->close(rom);
		return false;
	}

	mCoreInitConfig(sMultiplayer->secondaryCore, NULL);
	sMultiplayer->secondaryCore->init(sMultiplayer->secondaryCore);

	sMultiplayer->secondaryOutputBuffer = malloc((size_t) sMultiplayer->maxVideoWidth * sMultiplayer->maxVideoHeight * VIDEO_BYTES_PER_PIXEL);
	if (!sMultiplayer->secondaryOutputBuffer) {
		rom->close(rom);
		return false;
	}
	memset(sMultiplayer->secondaryOutputBuffer, 0xFF, (size_t) sMultiplayer->maxVideoWidth * sMultiplayer->maxVideoHeight * VIDEO_BYTES_PER_PIXEL);
	sMultiplayer->secondaryCore->setVideoBuffer(sMultiplayer->secondaryCore, sMultiplayer->secondaryOutputBuffer, sMultiplayer->maxVideoWidth);

	memset(&sMultiplayer->secondaryStream, 0, sizeof(sMultiplayer->secondaryStream));
	sMultiplayer->secondaryCore->setAVStream(sMultiplayer->secondaryCore, &sMultiplayer->secondaryStream);

	if (!sMultiplayer->secondaryCore->loadROM(sMultiplayer->secondaryCore, rom)) {
		rom->close(rom);
		return false;
	}

	sMultiplayer->secondaryCore->reset(sMultiplayer->secondaryCore);
	return true;
}

static bool _attachLockstep(void) {
	struct mCore* primaryCore = sMultiplayer->primaryCore;
	if (!primaryCore || primaryCore->platform(primaryCore) != mPLATFORM_GBA) {
		return false;
	}
	if (!sMultiplayer->secondaryCore || sMultiplayer->secondaryCore->platform(sMultiplayer->secondaryCore) != mPLATFORM_GBA) {
		return false;
	}

	GBASIOLockstepCoordinatorInit(&sMultiplayer->coordinator);
	sMultiplayer->coordinatorInitialized = true;

	sMultiplayer->users[0].d.sleep = _lockstepSleep;
	sMultiplayer->users[0].d.wake = _lockstepWake;
	sMultiplayer->users[0].d.requestedId = _requestedId;
	sMultiplayer->users[0].d.playerIdChanged = NULL;
	sMultiplayer->users[0].requestedId = 0;
	sMultiplayer->users[0].multiplayer = sMultiplayer;
	sMultiplayer->users[0].playerIndex = 0;
	sMultiplayer->users[0].blocked = false;
	sMultiplayer->users[0].stepping = false;

	sMultiplayer->users[1].d.sleep = _lockstepSleep;
	sMultiplayer->users[1].d.wake = _lockstepWake;
	sMultiplayer->users[1].d.requestedId = _requestedId;
	sMultiplayer->users[1].d.playerIdChanged = NULL;
	sMultiplayer->users[1].requestedId = 1;
	sMultiplayer->users[1].multiplayer = sMultiplayer;
	sMultiplayer->users[1].playerIndex = 1;
	sMultiplayer->users[1].blocked = false;
	sMultiplayer->users[1].stepping = false;

	GBASIOLockstepDriverCreate(&sMultiplayer->drivers[0], &sMultiplayer->users[0].d);
	GBASIOLockstepDriverCreate(&sMultiplayer->drivers[1], &sMultiplayer->users[1].d);

	GBASIOLockstepCoordinatorAttach(&sMultiplayer->coordinator, &sMultiplayer->drivers[0]);
	GBASIOLockstepCoordinatorAttach(&sMultiplayer->coordinator, &sMultiplayer->drivers[1]);

	primaryCore->setPeripheral(primaryCore, mPERIPH_GBA_LINK_PORT, &sMultiplayer->drivers[0].d);
	sMultiplayer->secondaryCore->setPeripheral(sMultiplayer->secondaryCore, mPERIPH_GBA_LINK_PORT, &sMultiplayer->drivers[1].d);
	return true;
}

static bool _startSession(const void* romData, size_t romSize, const char* romPath) {
	struct mCore* primaryCore = sMultiplayer->primaryCore;
	if (!primaryCore || primaryCore->platform(primaryCore) != mPLATFORM_GBA || !romData || !romSize) {
		if (!(romPath && *romPath)) {
			return false;
		}
	}

	if (!_initSecondaryCore(romData, romSize, romPath)) {
		_stopSession();
		return false;
	}

	sMultiplayer->compositeBufferPixels = _compositePixels(sMultiplayer->maxVideoWidth, sMultiplayer->maxVideoHeight);
	sMultiplayer->compositeBuffer = malloc(sMultiplayer->compositeBufferPixels * VIDEO_BYTES_PER_PIXEL);
	if (!sMultiplayer->compositeBuffer) {
		_stopSession();
		return false;
	}
	memset(sMultiplayer->compositeBuffer, 0xFF, sMultiplayer->compositeBufferPixels * VIDEO_BYTES_PER_PIXEL);

	if (!_attachLockstep()) {
		_stopSession();
		return false;
	}
	sMultiplayer->primaryCore = primaryCore;

	sMultiplayer->active = true;
	return true;
}

void mLibretroMultiplayerInit(struct mLibretroMultiplayer* multiplayer, unsigned maxVideoWidth, unsigned maxVideoHeight) {
	memset(multiplayer, 0, sizeof(*multiplayer));
	multiplayer->maxVideoWidth = maxVideoWidth;
	multiplayer->maxVideoHeight = maxVideoHeight;
	multiplayer->mode = mLIBRETRO_SPLITSCREEN_OFF;
	multiplayer->primaryCore = NULL;
	sMultiplayer = multiplayer;
}

void mLibretroMultiplayerSetPrimaryCore(struct mCore* primaryCore) {
	mASSERT(sMultiplayer);
	sMultiplayer->primaryCore = primaryCore;
}

void mLibretroMultiplayerDeinit(void) {
	mASSERT(sMultiplayer);
	_stopSession();
	sMultiplayer->mode = mLIBRETRO_SPLITSCREEN_OFF;
}

void mLibretroMultiplayerUpdateMode(retro_environment_t environCallback) {
	mASSERT(sMultiplayer);
	sMultiplayer->mode = _parseMode(environCallback);
}

bool mLibretroMultiplayerApplyMode(const void* romData, size_t romSize, const char* romPath, retro_log_printf_t logCallback) {
	mASSERT(sMultiplayer);
	if (sMultiplayer->mode == mLIBRETRO_SPLITSCREEN_OFF) {
		if (sMultiplayer->active) {
			_stopSession();
		}
		return true;
	}

	if (sMultiplayer->active) {
		return true;
	}

	if (!_startSession(romData, romSize, romPath)) {
		if (logCallback) {
			logCallback(RETRO_LOG_WARN, "libretro: failed to start multiplayer splitscreen session; continuing in single-player mode\n");
		}
		sMultiplayer->mode = mLIBRETRO_SPLITSCREEN_OFF;
		return false;
	}

	if (logCallback) {
		logCallback(RETRO_LOG_INFO, "libretro: started 2-player splitscreen multiplayer session\n");
	}

	return true;
}

void mLibretroMultiplayerReset(void) {
	mASSERT(sMultiplayer);
	struct mCore* primaryCore = sMultiplayer->primaryCore;
	if (!primaryCore) {
		return;
	}

	if (sMultiplayer->active && sMultiplayer->secondaryCore) {
		sMultiplayer->secondaryCore->reset(sMultiplayer->secondaryCore);
	}

	primaryCore->reset(primaryCore);
}

void mLibretroMultiplayerSetKeys(uint16_t player1Keys, uint16_t player2Keys) {
	mASSERT(sMultiplayer);
	struct mCore* primaryCore = sMultiplayer->primaryCore;
	if (!primaryCore) {
		return;
	}

	primaryCore->setKeys(primaryCore, player1Keys);

	if (!sMultiplayer->active || !sMultiplayer->secondaryCore) {
		return;
	}

	sMultiplayer->secondaryCore->setKeys(sMultiplayer->secondaryCore, player2Keys);
}

void mLibretroMultiplayerRunFrame(void) {
	mASSERT(sMultiplayer);
	struct mCore* primaryCore = sMultiplayer->primaryCore;
	if (!primaryCore) {
		return;
	}
	sMultiplayer->pumpedThisFrame[0] = false;
	sMultiplayer->pumpedThisFrame[1] = false;

	primaryCore->runFrame(primaryCore);

	if (sMultiplayer->active && sMultiplayer->secondaryCore && !sMultiplayer->pumpedThisFrame[1]) {
		sMultiplayer->secondaryCore->runFrame(sMultiplayer->secondaryCore);
	}
}

void mLibretroMultiplayerAdjustGeometry(struct retro_game_geometry* geometry) {
	mASSERT(sMultiplayer);
	if (!sMultiplayer->active || !geometry) {
		return;
	}

	if (sMultiplayer->mode == mLIBRETRO_SPLITSCREEN_2P_VERTICAL) {
		geometry->base_width *= 2;
		geometry->max_width *= 2;
		geometry->aspect_ratio *= 2.0;
		return;
	}

	if (sMultiplayer->mode == mLIBRETRO_SPLITSCREEN_2P_HORIZONTAL) {
		geometry->base_height *= 2;
		geometry->max_height *= 2;
		geometry->aspect_ratio *= 0.5;
	}
}

const mColor* mLibretroMultiplayerComposeFrame(const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	mASSERT(sMultiplayer);
	if (!sMultiplayer->active || !sMultiplayer->secondaryCore || !sMultiplayer->compositeBuffer || !sMultiplayer->secondaryOutputBuffer) {
		*outPitch = (size_t) primaryWidth * VIDEO_BYTES_PER_PIXEL;
		*outWidth = primaryWidth;
		*outHeight = primaryHeight;
		return primaryFrame;
	}

	unsigned secondaryWidth, secondaryHeight;
	sMultiplayer->secondaryCore->currentVideoSize(sMultiplayer->secondaryCore, &secondaryWidth, &secondaryHeight);

	if (sMultiplayer->mode == mLIBRETRO_SPLITSCREEN_2P_VERTICAL) {
		size_t y;
		*outWidth = primaryWidth + secondaryWidth;
		*outHeight = primaryHeight > secondaryHeight ? primaryHeight : secondaryHeight;
		*outPitch = (size_t) *outWidth * VIDEO_BYTES_PER_PIXEL;
		memset(sMultiplayer->compositeBuffer, 0xFF, (size_t) *outWidth * *outHeight * VIDEO_BYTES_PER_PIXEL);
		for (y = 0; y < primaryHeight; ++y) {
			mColor* row = &sMultiplayer->compositeBuffer[y * (*outWidth)];
			memcpy(row, &primaryFrame[y * sMultiplayer->maxVideoWidth], (size_t) primaryWidth * VIDEO_BYTES_PER_PIXEL);
		}
		for (y = 0; y < secondaryHeight; ++y) {
			mColor* row = &sMultiplayer->compositeBuffer[y * (*outWidth) + primaryWidth];
			memcpy(row, &sMultiplayer->secondaryOutputBuffer[y * sMultiplayer->maxVideoWidth], (size_t) secondaryWidth * VIDEO_BYTES_PER_PIXEL);
		}
		return sMultiplayer->compositeBuffer;
	}

	if (sMultiplayer->mode == mLIBRETRO_SPLITSCREEN_2P_HORIZONTAL) {
		size_t y;
		*outWidth = primaryWidth > secondaryWidth ? primaryWidth : secondaryWidth;
		*outHeight = primaryHeight + secondaryHeight;
		*outPitch = (size_t) *outWidth * VIDEO_BYTES_PER_PIXEL;
		memset(sMultiplayer->compositeBuffer, 0xFF, (size_t) *outWidth * *outHeight * VIDEO_BYTES_PER_PIXEL);
		for (y = 0; y < primaryHeight; ++y) {
			mColor* row = &sMultiplayer->compositeBuffer[y * (*outWidth)];
			memcpy(row, &primaryFrame[y * sMultiplayer->maxVideoWidth], (size_t) primaryWidth * VIDEO_BYTES_PER_PIXEL);
		}
		for (y = 0; y < secondaryHeight; ++y) {
			mColor* row = &sMultiplayer->compositeBuffer[(y + primaryHeight) * (*outWidth)];
			memcpy(row, &sMultiplayer->secondaryOutputBuffer[y * sMultiplayer->maxVideoWidth], (size_t) secondaryWidth * VIDEO_BYTES_PER_PIXEL);
		}
		return sMultiplayer->compositeBuffer;
	}

	*outPitch = (size_t) primaryWidth * VIDEO_BYTES_PER_PIXEL;
	*outWidth = primaryWidth;
	*outHeight = primaryHeight;
	return primaryFrame;
}

bool mLibretroMultiplayerStateActive(void) {
	mASSERT(sMultiplayer);
	return sMultiplayer->active && sMultiplayer->secondaryCore;
}

size_t mLibretroMultiplayerSerializeSize(void) {
	mASSERT(sMultiplayer);
	if (!mLibretroMultiplayerStateActive()) {
		return 0;
	}

	struct VFile* vfm = VFileMemChunk(NULL, 0);
	if (!vfm) {
		return 0;
	}

	mCoreSaveStateNamed(sMultiplayer->secondaryCore, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
	size_t size = vfm->size(vfm);
	vfm->close(vfm);
	return size;
}

bool mLibretroMultiplayerSerialize(void* data, size_t size) {
	mASSERT(sMultiplayer);
	if (!mLibretroMultiplayerStateActive() || !data) {
		return false;
	}

	struct VFile* vfm = VFileMemChunk(NULL, 0);
	if (!vfm) {
		return false;
	}

	mCoreSaveStateNamed(sMultiplayer->secondaryCore, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
	ssize_t stateSize = vfm->size(vfm);
	if ((ssize_t) size != stateSize) {
		vfm->close(vfm);
		return false;
	}

	vfm->seek(vfm, 0, SEEK_SET);
	vfm->read(vfm, data, size);
	vfm->close(vfm);
	return true;
}

bool mLibretroMultiplayerUnserialize(const void* data, size_t size) {
	mASSERT(sMultiplayer);
	if (!mLibretroMultiplayerStateActive() || !data) {
		return false;
	}

	struct VFile* vfm = VFileFromConstMemory(data, size);
	if (!vfm) {
		return false;
	}

	bool success = mCoreLoadStateNamed(sMultiplayer->secondaryCore, vfm, SAVESTATE_RTC);
	vfm->close(vfm);
	return success;
}
