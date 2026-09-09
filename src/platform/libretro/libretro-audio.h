/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef LIBRETRO_AUDIO_H
#define LIBRETRO_AUDIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define GBA_OUTPUT_RATE 65536

enum LibretroAudioMode {
	CONV_PASSTHROUGH,
	CONV_UPSAMPLE,
	CONV_DOWNSAMPLE,
	CONV_RESAMPLE
};

struct LibretroAudioConverter {
	unsigned inputRate;
	enum LibretroAudioMode mode;
	// 2x upsample + generic resample state
	int16_t prevLeft;
	int16_t prevRight;
	bool havePrev;
	// 2:1/4:1 downsample state
	int32_t accLeft;
	int32_t accRight;
	unsigned accCount;
	unsigned factor; // downsample ratio (2 or 4)
	// generic resample state
	uint32_t phase;
};

void audioConverterReset(struct LibretroAudioConverter* conv, unsigned inputRate);
size_t audioConverterProcess(struct LibretroAudioConverter* conv, const int16_t* in, size_t nFrames, int16_t* out);

#endif
