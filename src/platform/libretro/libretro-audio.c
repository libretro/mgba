/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "libretro-audio.h"

#include <string.h>

void audioConverterReset(struct LibretroAudioConverter* conv, unsigned inputRate) {
	conv->inputRate = inputRate;
	conv->prevLeft = 0;
	conv->prevRight = 0;
	conv->havePrev = false;
	conv->accLeft = 0;
	conv->accRight = 0;
	conv->accCount = 0;
	conv->factor = 1;
	conv->phase = 0;
	switch (inputRate) {
	case GBA_OUTPUT_RATE / 2:
		conv->mode = CONV_UPSAMPLE;
		break;
	case GBA_OUTPUT_RATE * 2:
		conv->mode = CONV_DOWNSAMPLE;
		conv->factor = 2;
		break;
	case GBA_OUTPUT_RATE * 4:
		conv->mode = CONV_DOWNSAMPLE;
		conv->factor = 4;
		break;
	case GBA_OUTPUT_RATE:
	case 0: // degenerate; passthrough at least terminates
		conv->mode = CONV_PASSTHROUGH;
		break;
	default:
		conv->mode = CONV_RESAMPLE;
		break;
	}
}

size_t audioConverterProcess(struct LibretroAudioConverter* conv, const int16_t* in, size_t nFrames, int16_t* out) {
	size_t i;
	size_t outFrames = 0;
	switch (conv->mode) {
	case CONV_PASSTHROUGH:
		memcpy(out, in, nFrames * 2 * sizeof(int16_t));
		return nFrames;
	case CONV_UPSAMPLE: // 2x linear interpolation
		for (i = 0; i < nFrames; ++i) {
			int16_t left = in[i * 2];
			int16_t right = in[i * 2 + 1];
			if (!conv->havePrev) {
				conv->prevLeft = left;
				conv->prevRight = right;
				conv->havePrev = true;
			}
			out[outFrames * 2] = (int16_t) (((int32_t) conv->prevLeft + left) / 2);
			out[outFrames * 2 + 1] = (int16_t) (((int32_t) conv->prevRight + right) / 2);
			++outFrames;
			out[outFrames * 2] = left;
			out[outFrames * 2 + 1] = right;
			++outFrames;
			conv->prevLeft = left;
			conv->prevRight = right;
		}
		return outFrames;
	case CONV_DOWNSAMPLE: // 2:1 or 4:1 averaging
		for (i = 0; i < nFrames; ++i) {
			conv->accLeft += in[i * 2];
			conv->accRight += in[i * 2 + 1];
			++conv->accCount;
			if (conv->accCount == conv->factor) {
				out[outFrames * 2] = (int16_t) (conv->accLeft / (int32_t) conv->factor);
				out[outFrames * 2 + 1] = (int16_t) (conv->accRight / (int32_t) conv->factor);
				++outFrames;
				conv->accLeft = 0;
				conv->accRight = 0;
				conv->accCount = 0;
			}
		}
		return outFrames;
	case CONV_RESAMPLE:
	default: {
		// Generic linear resample to GBA_OUTPUT_RATE
		size_t outCapacity = nFrames * 2;
		for (i = 0; i < nFrames; ++i) {
			int16_t left = in[i * 2];
			int16_t right = in[i * 2 + 1];
			if (!conv->havePrev) {
				conv->prevLeft = left;
				conv->prevRight = right;
				conv->havePrev = true;
			}
			while (conv->phase < 0x10000) {
				if (outFrames < outCapacity) {
					out[outFrames * 2] = (int16_t) (conv->prevLeft + (((int64_t) (left - conv->prevLeft) * conv->phase) >> 16));
					out[outFrames * 2 + 1] = (int16_t) (conv->prevRight + (((int64_t) (right - conv->prevRight) * conv->phase) >> 16));
					++outFrames;
				}
				conv->phase += conv->inputRate;
			}
			conv->phase -= 0x10000;
			conv->prevLeft = left;
			conv->prevRight = right;
		}
		return outFrames;
	}
	}
}
