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

static size_t _compositePixels(unsigned maxVideoWidth, unsigned maxVideoHeight) {
	return (size_t) maxVideoWidth * 2 * maxVideoHeight * 2;
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

	if (sMultiplayer->compositeBuffer) {
		free(sMultiplayer->compositeBuffer);
		sMultiplayer->compositeBuffer = NULL;
		sMultiplayer->compositeBufferPixels = 0;
	}
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

		sMultiplayer->outputBuffers[i] = malloc((size_t) sMultiplayer->maxVideoWidth * sMultiplayer->maxVideoHeight * VIDEO_BYTES_PER_PIXEL);
		if (!sMultiplayer->outputBuffers[i]) {
			return false;
		}
		memset(sMultiplayer->outputBuffers[i], 0xFF, (size_t) sMultiplayer->maxVideoWidth * sMultiplayer->maxVideoHeight * VIDEO_BYTES_PER_PIXEL);
		sMultiplayer->cores[i]->setVideoBuffer(sMultiplayer->cores[i], sMultiplayer->outputBuffers[i], sMultiplayer->maxVideoWidth);

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

	return true;
}

void mLibretroMultiplayerInit(struct mLibretroMultiplayer* multiplayer, unsigned maxVideoWidth, unsigned maxVideoHeight) {
	memset(multiplayer, 0, sizeof(*multiplayer));
	multiplayer->maxVideoWidth = maxVideoWidth;
	multiplayer->maxVideoHeight = maxVideoHeight;
	multiplayer->mode = mLIBRETRO_SPLITSCREEN_OFF;
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
	sMultiplayer->mode = mLIBRETRO_SPLITSCREEN_OFF;
}

void mLibretroMultiplayerUpdateMode(retro_environment_t environCallback) {
	mASSERT(sMultiplayer);
	sMultiplayer->mode = _parseMode(environCallback);
}

bool mLibretroMultiplayerApplyMode(const void* romData, size_t romSize, const char* romPath, retro_log_printf_t logCallback) {
	mASSERT(sMultiplayer);
	int desiredPlayers = _modePlayerCount(sMultiplayer->mode);

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
		if (logCallback) {
			logCallback(RETRO_LOG_WARN, "libretro: failed to start multiplayer splitscreen session; continuing in single-player mode\n");
		}
		sMultiplayer->mode = mLIBRETRO_SPLITSCREEN_OFF;
		return false;
	}

	if (logCallback) {
		logCallback(RETRO_LOG_INFO, "libretro: started %d-player splitscreen multiplayer session\n", sMultiplayer->numPlayers);
	}

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
	if (sMultiplayer->numPlayers < 2 || !geometry) {
		return;
	}

	switch (sMultiplayer->mode) {
	case mLIBRETRO_SPLITSCREEN_2P_VERTICAL:
		geometry->base_width *= 2;
		geometry->max_width *= 2;
		geometry->aspect_ratio *= 2.0;
		break;
	case mLIBRETRO_SPLITSCREEN_2P_HORIZONTAL:
		geometry->base_height *= 2;
		geometry->max_height *= 2;
		geometry->aspect_ratio *= 0.5;
		break;
	case mLIBRETRO_SPLITSCREEN_4P_GRID:
		geometry->base_width *= 2;
		geometry->max_width *= 2;
		geometry->base_height *= 2;
		geometry->max_height *= 2;
		break;
	default:
		break;
	}
}

static const mColor* _compose2PVertical(const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	unsigned secWidth, secHeight;
	sMultiplayer->cores[1]->currentVideoSize(sMultiplayer->cores[1], &secWidth, &secHeight);

	*outWidth = primaryWidth + secWidth;
	*outHeight = primaryHeight > secHeight ? primaryHeight : secHeight;
	*outPitch = (size_t) *outWidth * VIDEO_BYTES_PER_PIXEL;
	memset(sMultiplayer->compositeBuffer, 0xFF, (size_t) *outWidth * *outHeight * VIDEO_BYTES_PER_PIXEL);

	size_t y;
	for (y = 0; y < primaryHeight; ++y) {
		mColor* row = &sMultiplayer->compositeBuffer[y * (*outWidth)];
		memcpy(row, &primaryFrame[y * sMultiplayer->maxVideoWidth], (size_t) primaryWidth * VIDEO_BYTES_PER_PIXEL);
	}
	for (y = 0; y < secHeight; ++y) {
		mColor* row = &sMultiplayer->compositeBuffer[y * (*outWidth) + primaryWidth];
		memcpy(row, &sMultiplayer->outputBuffers[1][y * sMultiplayer->maxVideoWidth], (size_t) secWidth * VIDEO_BYTES_PER_PIXEL);
	}
	return sMultiplayer->compositeBuffer;
}

static const mColor* _compose2PHorizontal(const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	unsigned secWidth, secHeight;
	sMultiplayer->cores[1]->currentVideoSize(sMultiplayer->cores[1], &secWidth, &secHeight);

	*outWidth = primaryWidth > secWidth ? primaryWidth : secWidth;
	*outHeight = primaryHeight + secHeight;
	*outPitch = (size_t) *outWidth * VIDEO_BYTES_PER_PIXEL;
	memset(sMultiplayer->compositeBuffer, 0xFF, (size_t) *outWidth * *outHeight * VIDEO_BYTES_PER_PIXEL);

	size_t y;
	for (y = 0; y < primaryHeight; ++y) {
		mColor* row = &sMultiplayer->compositeBuffer[y * (*outWidth)];
		memcpy(row, &primaryFrame[y * sMultiplayer->maxVideoWidth], (size_t) primaryWidth * VIDEO_BYTES_PER_PIXEL);
	}
	for (y = 0; y < secHeight; ++y) {
		mColor* row = &sMultiplayer->compositeBuffer[(y + primaryHeight) * (*outWidth)];
		memcpy(row, &sMultiplayer->outputBuffers[1][y * sMultiplayer->maxVideoWidth], (size_t) secWidth * VIDEO_BYTES_PER_PIXEL);
	}
	return sMultiplayer->compositeBuffer;
}

static const mColor* _compose4PGrid(const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	unsigned widths[MAX_GBAS], heights[MAX_GBAS];
	widths[0] = primaryWidth;
	heights[0] = primaryHeight;

	int i;
	for (i = 1; i < sMultiplayer->numPlayers; ++i) {
		sMultiplayer->cores[i]->currentVideoSize(sMultiplayer->cores[i], &widths[i], &heights[i]);
	}
	/* Fill any unused slots with the primary size for layout calculation. */
	for (i = sMultiplayer->numPlayers; i < MAX_GBAS; ++i) {
		widths[i] = primaryWidth;
		heights[i] = primaryHeight;
	}

	unsigned topWidth = widths[0] + widths[1];
	unsigned botWidth = widths[2] + widths[3];
	*outWidth = topWidth > botWidth ? topWidth : botWidth;

	unsigned topHeight = heights[0] > heights[1] ? heights[0] : heights[1];
	unsigned botHeight = heights[2] > heights[3] ? heights[2] : heights[3];
	*outHeight = topHeight + botHeight;
	*outPitch = (size_t) *outWidth * VIDEO_BYTES_PER_PIXEL;

	memset(sMultiplayer->compositeBuffer, 0xFF, (size_t) *outWidth * *outHeight * VIDEO_BYTES_PER_PIXEL);

	/* Quadrant positions: [0]=top-left, [1]=top-right, [2]=bottom-left, [3]=bottom-right */
	unsigned offX[4] = { 0, widths[0], 0, widths[2] };
	unsigned offY[4] = { 0, 0, topHeight, topHeight };

	const mColor* framePtrs[MAX_GBAS];
	framePtrs[0] = primaryFrame;
	for (i = 1; i < sMultiplayer->numPlayers; ++i) {
		framePtrs[i] = sMultiplayer->outputBuffers[i];
	}

	for (i = 0; i < sMultiplayer->numPlayers && i < MAX_GBAS; ++i) {
		size_t y;
		for (y = 0; y < heights[i]; ++y) {
			mColor* dst = &sMultiplayer->compositeBuffer[(offY[i] + y) * (*outWidth) + offX[i]];
			const mColor* src = &framePtrs[i][y * sMultiplayer->maxVideoWidth];
			memcpy(dst, src, (size_t) widths[i] * VIDEO_BYTES_PER_PIXEL);
		}
	}

	return sMultiplayer->compositeBuffer;
}

const mColor* mLibretroMultiplayerComposeFrame(const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	mASSERT(sMultiplayer);
	if (sMultiplayer->numPlayers < 2 || !sMultiplayer->compositeBuffer) {
		*outPitch = (size_t) primaryWidth * VIDEO_BYTES_PER_PIXEL;
		*outWidth = primaryWidth;
		*outHeight = primaryHeight;
		return primaryFrame;
	}

	switch (sMultiplayer->mode) {
	case mLIBRETRO_SPLITSCREEN_2P_VERTICAL:
		return _compose2PVertical(primaryFrame, primaryWidth, primaryHeight, outPitch, outWidth, outHeight);
	case mLIBRETRO_SPLITSCREEN_2P_HORIZONTAL:
		return _compose2PHorizontal(primaryFrame, primaryWidth, primaryHeight, outPitch, outWidth, outHeight);
	case mLIBRETRO_SPLITSCREEN_4P_GRID:
		return _compose4PGrid(primaryFrame, primaryWidth, primaryHeight, outPitch, outWidth, outHeight);
	default:
		break;
	}

	*outPitch = (size_t) primaryWidth * VIDEO_BYTES_PER_PIXEL;
	*outWidth = primaryWidth;
	*outHeight = primaryHeight;
	return primaryFrame;
}

bool mLibretroMultiplayerStateActive(void) {
	mASSERT(sMultiplayer);
	return sMultiplayer->numPlayers > 1;
}

size_t mLibretroMultiplayerSerializeSize(void) {
	mASSERT(sMultiplayer);
	if (!mLibretroMultiplayerStateActive()) {
		return 0;
	}

	size_t total = 0;
	int i;
	for (i = 1; i < sMultiplayer->numPlayers; ++i) {
		struct VFile* vfm = VFileMemChunk(NULL, 0);
		if (!vfm) {
			return 0;
		}
		mCoreSaveStateNamed(sMultiplayer->cores[i], vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
		total += vfm->size(vfm);
		vfm->close(vfm);
	}
	return total;
}

bool mLibretroMultiplayerSerialize(void* data, size_t size) {
	mASSERT(sMultiplayer);
	if (!mLibretroMultiplayerStateActive() || !data) {
		return false;
	}

	uint8_t* cursor = data;
	size_t remaining = size;
	int i;
	for (i = 1; i < sMultiplayer->numPlayers; ++i) {
		struct VFile* vfm = VFileMemChunk(NULL, 0);
		if (!vfm) {
			return false;
		}
		mCoreSaveStateNamed(sMultiplayer->cores[i], vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
		ssize_t stateSize = vfm->size(vfm);
		if (stateSize <= 0 || (size_t) stateSize > remaining) {
			vfm->close(vfm);
			return false;
		}
		vfm->seek(vfm, 0, SEEK_SET);
		vfm->read(vfm, cursor, (size_t) stateSize);
		vfm->close(vfm);
		cursor += stateSize;
		remaining -= stateSize;
	}
	return true;
}

bool mLibretroMultiplayerUnserialize(const void* data, size_t size) {
	mASSERT(sMultiplayer);
	if (!mLibretroMultiplayerStateActive() || !data) {
		return false;
	}

	int numSecondaries = sMultiplayer->numPlayers - 1;

	if (numSecondaries == 1) {
		struct VFile* vfm = VFileFromConstMemory(data, size);
		if (!vfm) {
			return false;
		}
		bool success = mCoreLoadStateNamed(sMultiplayer->cores[1], vfm, SAVESTATE_RTC);
		vfm->close(vfm);
		return success;
	}

	size_t perCore = size / numSecondaries;
	if (perCore * numSecondaries != size) {
		return false;
	}

	const uint8_t* cursor = data;
	int i;
	for (i = 1; i < sMultiplayer->numPlayers; ++i) {
		struct VFile* vfm = VFileFromConstMemory(cursor, perCore);
		if (!vfm) {
			return false;
		}
		if (!mCoreLoadStateNamed(sMultiplayer->cores[i], vfm, SAVESTATE_RTC)) {
			vfm->close(vfm);
			return false;
		}
		vfm->close(vfm);
		cursor += perCore;
	}
	return true;
}
