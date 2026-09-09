#include "libretro_multiplayer_display.h"

#include <stdlib.h>
#include <string.h>

#define VIDEO_BYTES_PER_PIXEL sizeof(mColor)

static size_t _compositePixels(unsigned maxVideoWidth, unsigned maxVideoHeight) {
	return (size_t) maxVideoWidth * 2 * maxVideoHeight * 2;
}

void mLibretroMultiplayerDisplayInit(struct mLibretroMultiplayer* multiplayer, unsigned maxVideoWidth, unsigned maxVideoHeight) {
	multiplayer->video.maxVideoWidth = maxVideoWidth;
	multiplayer->video.maxVideoHeight = maxVideoHeight;
	multiplayer->video.mode = mLIBRETRO_SPLITSCREEN_OFF;
	multiplayer->video.displayPlayers = mLIBRETRO_DISPLAY_ALL;
	multiplayer->video.localPlayerIndex = 0;
}

void mLibretroMultiplayerDisplayReset(struct mLibretroMultiplayer* multiplayer) {
	multiplayer->video.mode = mLIBRETRO_SPLITSCREEN_OFF;
	multiplayer->video.displayPlayers = mLIBRETRO_DISPLAY_ALL;
	multiplayer->video.localPlayerIndex = 0;
}

bool mLibretroMultiplayerDisplayCreateCompositeBuffer(struct mLibretroMultiplayer* multiplayer) {
	multiplayer->video.compositeBufferPixels = _compositePixels(multiplayer->video.maxVideoWidth, multiplayer->video.maxVideoHeight);
	multiplayer->video.compositeBuffer = malloc(multiplayer->video.compositeBufferPixels * VIDEO_BYTES_PER_PIXEL);
	if (!multiplayer->video.compositeBuffer) {
		multiplayer->video.compositeBufferPixels = 0;
		return false;
	}
	memset(multiplayer->video.compositeBuffer, 0xFF, multiplayer->video.compositeBufferPixels * VIDEO_BYTES_PER_PIXEL);
	return true;
}

void mLibretroMultiplayerDisplayDestroyCompositeBuffer(struct mLibretroMultiplayer* multiplayer) {
	if (multiplayer->video.compositeBuffer) {
		free(multiplayer->video.compositeBuffer);
		multiplayer->video.compositeBuffer = NULL;
		multiplayer->video.compositeBufferPixels = 0;
	}
}

struct mComposePlayerPlacement {
	unsigned id;
	unsigned row;
	unsigned col;
	unsigned width;
	unsigned height;
	const mColor* frame;
};

struct mComposeLayout {
	struct mComposePlayerPlacement players[MAX_GBAS];
	unsigned count;
	unsigned rowHeights[2];
	unsigned colWidths[2];
};

static void _reserveLayoutCell(struct mComposeLayout* layout, unsigned row, unsigned col, unsigned width, unsigned height) {
	if (row > 1 || col > 1) {
		return;
	}
	if (layout->rowHeights[row] < height) {
		layout->rowHeights[row] = height;
	}
	if (layout->colWidths[col] < width) {
		layout->colWidths[col] = width;
	}
}

static void _addPlayer(struct mComposeLayout* layout, unsigned id, unsigned row, unsigned col, const mColor* frame, unsigned width, unsigned height) {
	_reserveLayoutCell(layout, row, col, width, height);
	if (layout->count >= MAX_GBAS) {
		return;
	}
	layout->players[layout->count].id = id;
	layout->players[layout->count].row = row;
	layout->players[layout->count].col = col;
	layout->players[layout->count].width = width;
	layout->players[layout->count].height = height;
	layout->players[layout->count].frame = frame;
	++layout->count;
}

static const mColor* _composeLayout(const struct mLibretroMultiplayer* multiplayer, const struct mComposeLayout* layout, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	const struct mLibretroMultiplayerDisplay* video = &multiplayer->video;
	unsigned xOffset[2] = { 0, layout->colWidths[0] };
	unsigned yOffset[2] = { 0, layout->rowHeights[0] };

	*outWidth = layout->colWidths[0] + layout->colWidths[1];
	*outHeight = layout->rowHeights[0] + layout->rowHeights[1];
	*outPitch = (size_t) *outWidth * VIDEO_BYTES_PER_PIXEL;

	memset(video->compositeBuffer, 0xFF, (size_t) *outWidth * *outHeight * VIDEO_BYTES_PER_PIXEL);

	unsigned i;
	for (i = 0; i < layout->count; ++i) {
		const struct mComposePlayerPlacement* placement = &layout->players[i];
		size_t y;
		for (y = 0; y < placement->height; ++y) {
			mColor* dst = &video->compositeBuffer[(yOffset[placement->row] + y) * (*outWidth) + xOffset[placement->col]];
			const mColor* src = &placement->frame[y * video->maxVideoWidth];
			memcpy(dst, src, (size_t) placement->width * VIDEO_BYTES_PER_PIXEL);
		}
	}

	return video->compositeBuffer;
}

static const mColor* _compose2PVertical(const struct mLibretroMultiplayer* multiplayer, const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	unsigned secWidth, secHeight;
	multiplayer->cores[1]->currentVideoSize(multiplayer->cores[1], &secWidth, &secHeight);

	struct mComposeLayout layout = { 0 };
	_addPlayer(&layout, 0, 0, 0, primaryFrame, primaryWidth, primaryHeight);
	_addPlayer(&layout, 1, 0, 1, multiplayer->outputBuffers[1], secWidth, secHeight);
	return _composeLayout(multiplayer, &layout, outPitch, outWidth, outHeight);
}

static const mColor* _compose2PHorizontal(const struct mLibretroMultiplayer* multiplayer, const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	unsigned secWidth, secHeight;
	multiplayer->cores[1]->currentVideoSize(multiplayer->cores[1], &secWidth, &secHeight);

	struct mComposeLayout layout = { 0 };
	_addPlayer(&layout, 0, 0, 0, primaryFrame, primaryWidth, primaryHeight);
	_addPlayer(&layout, 1, 1, 0, multiplayer->outputBuffers[1], secWidth, secHeight);
	return _composeLayout(multiplayer, &layout, outPitch, outWidth, outHeight);
}

static const mColor* _compose4PGrid(const struct mLibretroMultiplayer* multiplayer, const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	struct mComposeLayout layout = { 0 };
	_addPlayer(&layout, 0, 0, 0, primaryFrame, primaryWidth, primaryHeight);

	int i;
	for (i = 1; i < multiplayer->numPlayers && i < MAX_GBAS; ++i) {
		unsigned width, height;
		multiplayer->cores[i]->currentVideoSize(multiplayer->cores[i], &width, &height);
		_addPlayer(&layout, (unsigned) i, (unsigned) (i / 2), (unsigned) (i % 2), multiplayer->outputBuffers[i], width, height);
	}

	for (i = multiplayer->numPlayers; i < MAX_GBAS; ++i) {
		_reserveLayoutCell(&layout, (unsigned) (i / 2), (unsigned) (i % 2), primaryWidth, primaryHeight);
	}

	return _composeLayout(multiplayer, &layout, outPitch, outWidth, outHeight);
}

static const mColor* _returnFrame(const struct mLibretroMultiplayerDisplay* video, const mColor* frame, unsigned width, unsigned height, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	*outPitch = (size_t) video->maxVideoWidth * VIDEO_BYTES_PER_PIXEL;
	*outWidth = width;
	*outHeight = height;
	return frame;
}

static const mColor* _composeSinglePlayerView(const struct mLibretroMultiplayer* multiplayer, const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	const struct mLibretroMultiplayerDisplay* video = &multiplayer->video;
	if (video->displayPlayers == mLIBRETRO_DISPLAY_SELF && video->localPlayerIndex > 0 && video->localPlayerIndex < multiplayer->numPlayers) {
		int idx = video->localPlayerIndex;
		unsigned width, height;
		multiplayer->cores[idx]->currentVideoSize(multiplayer->cores[idx], &width, &height);
		return _returnFrame(video, multiplayer->outputBuffers[idx], width, height, outPitch, outWidth, outHeight);
	}

	return _returnFrame(video, primaryFrame, primaryWidth, primaryHeight, outPitch, outWidth, outHeight);
}

void mLibretroMultiplayerDisplayAdjustGeometry(const struct mLibretroMultiplayer* multiplayer, struct retro_game_geometry* geometry) {
	const struct mLibretroMultiplayerDisplay* video = &multiplayer->video;
	if (multiplayer->numPlayers < 2 || !geometry) {
		return;
	}

	if (video->displayPlayers == mLIBRETRO_DISPLAY_SELF) {
		return;
	}

	switch (video->mode) {
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

const mColor* mLibretroMultiplayerDisplayComposeFrame(const struct mLibretroMultiplayer* multiplayer, const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight) {
	const struct mLibretroMultiplayerDisplay* video = &multiplayer->video;
	if (video->displayPlayers == mLIBRETRO_DISPLAY_SELF) {
		return _composeSinglePlayerView(multiplayer, primaryFrame, primaryWidth, primaryHeight, outPitch, outWidth, outHeight);
	}

	if (multiplayer->numPlayers < 2 || !video->compositeBuffer) {
		return _returnFrame(video, primaryFrame, primaryWidth, primaryHeight, outPitch, outWidth, outHeight);
	}

	enum mLibretroSplitscreenMode mode = video->mode;
	if (mode == mLIBRETRO_SPLITSCREEN_OFF) {
		return _composeSinglePlayerView(multiplayer, primaryFrame, primaryWidth, primaryHeight, outPitch, outWidth, outHeight);
	}

	switch (mode) {
	case mLIBRETRO_SPLITSCREEN_2P_VERTICAL:
		return _compose2PVertical(multiplayer, primaryFrame, primaryWidth, primaryHeight, outPitch, outWidth, outHeight);
	case mLIBRETRO_SPLITSCREEN_2P_HORIZONTAL:
		return _compose2PHorizontal(multiplayer, primaryFrame, primaryWidth, primaryHeight, outPitch, outWidth, outHeight);
	case mLIBRETRO_SPLITSCREEN_4P_GRID:
		return _compose4PGrid(multiplayer, primaryFrame, primaryWidth, primaryHeight, outPitch, outWidth, outHeight);
	default:
		break;
	}

	return _returnFrame(video, primaryFrame, primaryWidth, primaryHeight, outPitch, outWidth, outHeight);
}
