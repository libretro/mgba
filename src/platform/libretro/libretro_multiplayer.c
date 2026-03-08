#include "libretro_multiplayer.h"
#include "libretro_lockstep.h"

#include "libretro_multiplayer_display.h"

#include "libretro_log.h"

#include <mgba/core/config.h>
#include <mgba/gba/interface.h>
#include <mgba-util/memory.h>
#include <mgba-util/vfs.h>

#include <fcntl.h>
#include <string.h>

#define VIDEO_BYTES_PER_PIXEL sizeof(mColor)
#define COOPERATIVE_WATCHDOG 4000000

static struct mLibretroMultiplayer* sMultiplayer;

static void _lockstepSleep(struct mLockstepUser* user) {
	struct mLibretroLockstepUser* lockstepUser = (struct mLibretroLockstepUser*) user;
	if (!lockstepUser->multiplayer || lockstepUser->multiplayer->numPlayers < 2) {
		return;
	}

	lockstepUser->blocked = true;
}

static void _lockstepWake(struct mLockstepUser* user) {
	struct mLibretroLockstepUser* lockstepUser = (struct mLibretroLockstepUser*) user;
	lockstepUser->blocked = false;
}

static int _requestedId(struct mLockstepUser* user) {
	struct mLibretroLockstepUser* lockstepUser = (struct mLibretroLockstepUser*) user;
	return lockstepUser->requestedId;
}

static int _modePlayerCount(enum mLibretroSplitscreenMode mode) {
	switch (mode) {
	case mLIBRETRO_SPLITSCREEN_2P_VERTICAL:
	case mLIBRETRO_SPLITSCREEN_2P_HORIZONTAL:
		return 2;
	case mLIBRETRO_SPLITSCREEN_4P_GRID:
		return 4;
	default:
		return 1;
	}
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

	if (strcmp(var.value, "4-Player Grid") == 0) {
		return mLIBRETRO_SPLITSCREEN_4P_GRID;
	}

	return mLIBRETRO_SPLITSCREEN_OFF;
}

static enum mLibretroDisplayPlayers _parseDisplayPlayers(retro_environment_t environCallback) {
	struct retro_variable var = {
		.key = "mgba_multiplayer_av",
		.value = 0,
	};

	if (!environCallback || !environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value) {
		return mLIBRETRO_DISPLAY_ALL;
	}

	if (strcmp(var.value, "Self") == 0) {
		return mLIBRETRO_DISPLAY_SELF;
	}

	return mLIBRETRO_DISPLAY_ALL;
}

static void _clearPrimaryLinkPeripheral(void) {
	struct mCore* primary = sMultiplayer->cores[0];
	if (!primary || primary->platform(primary) != mPLATFORM_GBA) {
		return;
	}
	primary->setPeripheral(primary, mPERIPH_GBA_LINK_PORT, NULL);
}

static void _detachLockstep(void) {
	if (!sMultiplayer->coordinatorInitialized) {
		return;
	}

	int i;
	for (i = 0; i < sMultiplayer->numPlayers; ++i) {
		struct mCore* core = sMultiplayer->cores[i];
		if (core && core->platform(core) == mPLATFORM_GBA) {
			if (i > 0) {
				core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, NULL);
			}
			GBASIOLockstepCoordinatorDetach(&sMultiplayer->coordinator, &sMultiplayer->drivers[i]);
		}
	}

	GBASIOLockstepCoordinatorDeinit(&sMultiplayer->coordinator);
	sMultiplayer->coordinatorInitialized = false;
}

static void _destroySecondaryCores(void) {
	int i;
	for (i = 1; i < sMultiplayer->numPlayers; ++i) {
		if (sMultiplayer->cores[i]) {
			mCoreConfigDeinit(&sMultiplayer->cores[i]->config);
			sMultiplayer->cores[i]->deinit(sMultiplayer->cores[i]);
			sMultiplayer->cores[i] = NULL;
		}
	}
}

static void _destroyBuffers(void) {
	int i;
	for (i = 1; i < MAX_GBAS; ++i) {
		if (sMultiplayer->outputBuffers[i]) {
			free(sMultiplayer->outputBuffers[i]);
			sMultiplayer->outputBuffers[i] = NULL;
		}
	}

	mLibretroMultiplayerDisplayDestroyCompositeBuffer(sMultiplayer);
}

static void _destroyRomData(void) {
	if (sMultiplayer->romData) {
		mappedMemoryFree(sMultiplayer->romData, sMultiplayer->romSize);
		sMultiplayer->romData = NULL;
		sMultiplayer->romSize = 0;
	}
}

static void _stopSession(void) {
	_clearPrimaryLinkPeripheral();
	_detachLockstep();
	_destroySecondaryCores();
	_destroyBuffers();
	_destroyRomData();
	sMultiplayer->numPlayers = sMultiplayer->cores[0] ? 1 : 0;
}

static bool _initSecondaryCores(int numPlayers, const void* romData, size_t romSize, const char* romPath) {
	if (numPlayers < 2 || numPlayers > MAX_GBAS) {
		return false;
	}

	if (romData && romSize) {
		sMultiplayer->romData = anonymousMemoryMap(romSize);
		if (!sMultiplayer->romData) {
			return false;
		}
		sMultiplayer->romSize = romSize;
		memcpy(sMultiplayer->romData, romData, romSize);
	} else if (!(romPath && *romPath)) {
		return false;
	}

	int i;
	for (i = 1; i < numPlayers; ++i) {
		struct VFile* rom;
		if (sMultiplayer->romData) {
			rom = VFileFromMemory(sMultiplayer->romData, sMultiplayer->romSize);
		} else {
			rom = VFileOpen(romPath, O_RDONLY);
		}
		if (!rom) {
			return false;
		}

		sMultiplayer->cores[i] = mCoreFindVF(rom);
		if (!sMultiplayer->cores[i]) {
			rom->close(rom);
			return false;
		}

		mCoreInitConfig(sMultiplayer->cores[i], NULL);
		sMultiplayer->cores[i]->init(sMultiplayer->cores[i]);

		sMultiplayer->outputBuffers[i] = malloc((size_t) sMultiplayer->video.maxVideoWidth * sMultiplayer->video.maxVideoHeight * VIDEO_BYTES_PER_PIXEL);
		if (!sMultiplayer->outputBuffers[i]) {
			return false;
		}
		memset(sMultiplayer->outputBuffers[i], 0xFF, (size_t) sMultiplayer->video.maxVideoWidth * sMultiplayer->video.maxVideoHeight * VIDEO_BYTES_PER_PIXEL);
		sMultiplayer->cores[i]->setVideoBuffer(sMultiplayer->cores[i], sMultiplayer->outputBuffers[i], sMultiplayer->video.maxVideoWidth);

		memset(&sMultiplayer->streams[i], 0, sizeof(sMultiplayer->streams[i]));
		sMultiplayer->cores[i]->setAVStream(sMultiplayer->cores[i], &sMultiplayer->streams[i]);

		if (!sMultiplayer->cores[i]->loadROM(sMultiplayer->cores[i], rom)) {
			rom->close(rom);
			return false;
		}

		sMultiplayer->cores[i]->reset(sMultiplayer->cores[i]);
		sMultiplayer->numPlayers = i + 1;
	}

	return true;
}

static bool _attachLockstep(void) {
	struct mCore* primary = sMultiplayer->cores[0];
	if (!primary || primary->platform(primary) != mPLATFORM_GBA) {
		return false;
	}

	int i;
	for (i = 1; i < sMultiplayer->numPlayers; ++i) {
		if (!sMultiplayer->cores[i] || sMultiplayer->cores[i]->platform(sMultiplayer->cores[i]) != mPLATFORM_GBA) {
			return false;
		}
	}

	GBASIOLockstepCoordinatorInit(&sMultiplayer->coordinator);
	sMultiplayer->coordinatorInitialized = true;

	for (i = 0; i < sMultiplayer->numPlayers; ++i) {
		sMultiplayer->users[i].d.sleep = _lockstepSleep;
		sMultiplayer->users[i].d.wake = _lockstepWake;
		sMultiplayer->users[i].d.requestedId = _requestedId;
		sMultiplayer->users[i].d.playerIdChanged = NULL;
		sMultiplayer->users[i].requestedId = i;
		sMultiplayer->users[i].multiplayer = sMultiplayer;
		sMultiplayer->users[i].playerIndex = i;
		sMultiplayer->users[i].blocked = false;

		GBASIOLockstepDriverCreate(&sMultiplayer->drivers[i], &sMultiplayer->users[i].d);
		GBASIOLockstepCoordinatorAttach(&sMultiplayer->coordinator, &sMultiplayer->drivers[i]);
	}

	for (i = 0; i < sMultiplayer->numPlayers; ++i) {
		sMultiplayer->cores[i]->setPeripheral(sMultiplayer->cores[i], mPERIPH_GBA_LINK_PORT, &sMultiplayer->drivers[i].d);
	}
	return true;
}

static bool _startSession(int numPlayers, const void* romData, size_t romSize, const char* romPath) {
	struct mCore* primary = sMultiplayer->cores[0];
	if (!primary || primary->platform(primary) != mPLATFORM_GBA || !romData || !romSize) {
		if (!(romPath && *romPath)) {
			return false;
		}
	}

	if (!_initSecondaryCores(numPlayers, romData, romSize, romPath)) {
		_stopSession();
		return false;
	}

	if (!mLibretroMultiplayerDisplayCreateCompositeBuffer(sMultiplayer)) {
		_stopSession();
		return false;
	}

	if (!_attachLockstep()) {
		_stopSession();
		return false;
	}

	return true;
}

void mLibretroMultiplayerInit(struct mLibretroMultiplayer* multiplayer, unsigned maxVideoWidth, unsigned maxVideoHeight) {
	memset(multiplayer, 0, sizeof(*multiplayer));
	mLibretroMultiplayerDisplayInit(multiplayer, maxVideoWidth, maxVideoHeight);
	sMultiplayer = multiplayer;
}

void mLibretroMultiplayerSetPrimaryCore(struct mCore* primaryCore) {
	mASSERT(sMultiplayer);
	sMultiplayer->cores[0] = primaryCore;
	if (!primaryCore) {
		sMultiplayer->numPlayers = 0;
	} else if (sMultiplayer->numPlayers < 1) {
		sMultiplayer->numPlayers = 1;
	}
}

void mLibretroMultiplayerDeinit(void) {
	mASSERT(sMultiplayer);
	_stopSession();
	mLibretroMultiplayerDisplayReset(sMultiplayer);
}

void mLibretroMultiplayerUpdateMode(retro_environment_t environCallback) {
	mASSERT(sMultiplayer);
	sMultiplayer->video.mode = _parseMode(environCallback);
}

void mLibretroMultiplayerUpdateDisplayPlayers(retro_environment_t environCallback) {
	mASSERT(sMultiplayer);
	sMultiplayer->video.displayPlayers = _parseDisplayPlayers(environCallback);
}

bool mLibretroMultiplayerApplyMode(const void* romData, size_t romSize, const char* romPath) {
	mASSERT(sMultiplayer);
	bool wantsSession = sMultiplayer->video.mode != mLIBRETRO_SPLITSCREEN_OFF;
	int desiredPlayers = wantsSession ? _modePlayerCount(sMultiplayer->video.mode) : 1;

	/* Tear down if the player count changed or if switching to single-player. */
	if (sMultiplayer->numPlayers > 1 && sMultiplayer->numPlayers != desiredPlayers) {
		_stopSession();
	}

	if (desiredPlayers <= 1) {
		return true;
	}

	if (sMultiplayer->numPlayers == desiredPlayers) {
		return true;
	}

	if (!_startSession(desiredPlayers, romData, romSize, romPath)) {
		mLibretroLog(RETRO_LOG_WARN, "libretro: failed to start multiplayer lockstep session; continuing in single-player mode\n");
		return false;
	}

	mLibretroLog(RETRO_LOG_INFO, "libretro: started %d-player splitscreen multiplayer session\n", sMultiplayer->numPlayers);

	return true;
}

void mLibretroMultiplayerReset(void) {
	mASSERT(sMultiplayer);
	if (!sMultiplayer->cores[0]) {
		return;
	}

	int i;
	for (i = 1; i < sMultiplayer->numPlayers; ++i) {
		if (sMultiplayer->cores[i]) {
			sMultiplayer->cores[i]->reset(sMultiplayer->cores[i]);
		}
	}

	sMultiplayer->cores[0]->reset(sMultiplayer->cores[0]);
}

void mLibretroMultiplayerSetKeys(uint16_t keys[MAX_GBAS]) {
	mASSERT(sMultiplayer);
	if (!sMultiplayer->cores[0]) {
		return;
	}

	int i;
	for (i = 0; i < sMultiplayer->numPlayers; ++i) {
		if (sMultiplayer->cores[i]) {
			sMultiplayer->cores[i]->setKeys(sMultiplayer->cores[i], keys[i]);
		}
	}
}

void mLibretroMultiplayerRunFrame(void) {
	mASSERT(sMultiplayer);
	struct mCore* primary = sMultiplayer->cores[0];
	if (!primary) {
		return;
	}

	if (sMultiplayer->numPlayers < 2) {
		primary->runFrame(primary);
		return;
	}

	uint32_t startFrames[MAX_GBAS];
	bool done[MAX_GBAS];

	int i;
	for (i = 0; i < sMultiplayer->numPlayers; ++i) {
		startFrames[i] = sMultiplayer->cores[i]->frameCounter(sMultiplayer->cores[i]);
		done[i] = false;
	}

	int watchdog = COOPERATIVE_WATCHDOG;
	bool allDone = false;
	while (!allDone && watchdog > 0) {
		bool ranAny = false;
		int blockedCount = 0;

		for (i = 0; i < sMultiplayer->numPlayers; ++i) {
			if (sMultiplayer->users[i].blocked) {
				++blockedCount;
			}
		}

		for (i = 0; i < sMultiplayer->numPlayers; ++i) {
			if (sMultiplayer->users[i].blocked) {
				continue;
			}
			if (done[i] && blockedCount == 0) {
				continue;
			}

			sMultiplayer->cores[i]->runLoop(sMultiplayer->cores[i]);
			--watchdog;
			ranAny = true;

			if (!done[i] && sMultiplayer->cores[i]->frameCounter(sMultiplayer->cores[i]) != startFrames[i]) {
				done[i] = true;
			}
		}

		if (!ranAny) {
			mLOG(GBA_SIO, FATAL, "All players blocked simultaneously -- deadlock");
			mASSERT(false);
			break;
		}

		allDone = true;
		for (i = 0; i < sMultiplayer->numPlayers; ++i) {
			if (!done[i]) {
				allDone = false;
				break;
			}
		}
	}
	if (watchdog <= 0) {
		mLOG(GBA_SIO, FATAL, "Cooperative scheduling watchdog expired");
		mASSERT(false);
	}
}

void mLibretroMultiplayerAdjustGeometry(struct retro_game_geometry* geometry) {
	mASSERT(sMultiplayer);
	mLibretroMultiplayerDisplayAdjustGeometry(sMultiplayer, geometry);
}

const mColor* mLibretroMultiplayerComposeFrame(const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	mASSERT(sMultiplayer);
	return mLibretroMultiplayerDisplayComposeFrame(sMultiplayer, primaryFrame, primaryWidth, primaryHeight, outPitch, outWidth, outHeight);
}

bool mLibretroMultiplayerStateActive(void) {
	mASSERT(sMultiplayer);
	return sMultiplayer->numPlayers > 1;
}

const struct mLibretroMultiplayer* mLibretroMultiplayerGet(void) {
	mASSERT(sMultiplayer);
	return sMultiplayer;
}

int mLibretroMultiplayerConfiguredPlayers(void) {
	mASSERT(sMultiplayer);
	int desired = _modePlayerCount(sMultiplayer->video.mode);
	if (sMultiplayer->numPlayers > desired) {
		desired = sMultiplayer->numPlayers;
	}
	return desired;
}
