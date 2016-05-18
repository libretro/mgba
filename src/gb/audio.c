/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "audio.h"

#include "core/interface.h"
#include "core/sync.h"
#include "gb/gb.h"
#include "gb/io.h"

#ifdef _3DS
#define blip_add_delta blip_add_delta_fast
#endif

#define FRAME_CYCLES (DMG_LR35902_FREQUENCY >> 9)

const uint32_t DMG_LR35902_FREQUENCY = 0x400000;
static const int CLOCKS_PER_BLIP_FRAME = 0x1000;
static const unsigned BLIP_BUFFER_SIZE = 0x4000;
const int GB_AUDIO_VOLUME_MAX = 0x100;

static void _writeDuty(struct GBAudioEnvelope* envelope, uint8_t value);
static bool _writeSweep(struct GBAudioEnvelope* envelope, uint8_t value);
static int32_t _updateSquareChannel(struct GBAudioSquareControl* envelope, int duty);
static void _updateEnvelope(struct GBAudioEnvelope* envelope);
static bool _updateSweep(struct GBAudioChannel1* ch, bool initial);
static int32_t _updateChannel1(struct GBAudioChannel1* ch);
static int32_t _updateChannel2(struct GBAudioChannel2* ch);
static int32_t _updateChannel3(struct GBAudioChannel3* ch, enum GBAudioStyle style);
static int32_t _updateChannel4(struct GBAudioChannel4* ch);
static void _sample(struct GBAudio* audio, int32_t cycles);
static void _scheduleEvent(struct GBAudio* audio);

void GBAudioInit(struct GBAudio* audio, size_t samples, uint8_t* nr52, enum GBAudioStyle style) {
	audio->samples = samples;
	audio->left = blip_new(BLIP_BUFFER_SIZE);
	audio->right = blip_new(BLIP_BUFFER_SIZE);
	audio->clockRate = DMG_LR35902_FREQUENCY;
	// Guess too large; we hang producing extra samples if we guess too low
	blip_set_rates(audio->left, DMG_LR35902_FREQUENCY, 96000);
	blip_set_rates(audio->right, DMG_LR35902_FREQUENCY, 96000);
	audio->forceDisableCh[0] = false;
	audio->forceDisableCh[1] = false;
	audio->forceDisableCh[2] = false;
	audio->forceDisableCh[3] = false;
	audio->masterVolume = GB_AUDIO_VOLUME_MAX;
	audio->nr52 = nr52;
	audio->style = style;
}

void GBAudioDeinit(struct GBAudio* audio) {
	blip_delete(audio->left);
	blip_delete(audio->right);
}

void GBAudioReset(struct GBAudio* audio) {
	audio->nextEvent = 0;
	audio->nextCh1 = 0;
	audio->nextCh2 = 0;
	audio->nextCh3 = 0;
	audio->fadeCh3 = 0;
	audio->nextCh4 = 0;
	audio->ch1 = (struct GBAudioChannel1) { .envelope = { .dead = 2 } };
	audio->ch2 = (struct GBAudioChannel2) { .envelope = { .dead = 2 } };
	audio->ch3 = (struct GBAudioChannel3) { .bank = 0 };
	audio->ch4 = (struct GBAudioChannel4) { .envelope = { .dead = 2 } };
	audio->eventDiff = 0;
	audio->nextFrame = 0;
	audio->frame = 0;
	audio->nextSample = 0;
	audio->sampleInterval = 128;
	audio->lastLeft = 0;
	audio->lastRight = 0;
	audio->clock = 0;
	audio->volumeRight = 0;
	audio->volumeLeft = 0;
	audio->ch1Right = false;
	audio->ch2Right = false;
	audio->ch3Right = false;
	audio->ch4Right = false;
	audio->ch1Left = false;
	audio->ch2Left = false;
	audio->ch3Left = false;
	audio->ch4Left = false;
	audio->playingCh1 = false;
	audio->playingCh2 = false;
	audio->playingCh3 = false;
	audio->playingCh4 = false;
}

void GBAudioResizeBuffer(struct GBAudio* audio, size_t samples) {
	mCoreSyncLockAudio(audio->p->sync);
	audio->samples = samples;
	blip_clear(audio->left);
	blip_clear(audio->right);
	audio->clock = 0;
	mCoreSyncConsumeAudio(audio->p->sync);
}

void GBAudioWriteNR10(struct GBAudio* audio, uint8_t value) {
	audio->ch1.shift = GBAudioRegisterSquareSweepGetShift(value);
	bool oldDirection = audio->ch1.direction;
	audio->ch1.direction = GBAudioRegisterSquareSweepGetDirection(value);
	if (audio->ch1.sweepOccurred && oldDirection && !audio->ch1.direction) {
		audio->playingCh1 = false;
		*audio->nr52 &= ~0x0001;
	}
	audio->ch1.sweepOccurred = false;
	audio->ch1.time = GBAudioRegisterSquareSweepGetTime(value);
	if (!audio->ch1.time) {
		audio->ch1.time = 8;
	}
}

void GBAudioWriteNR11(struct GBAudio* audio, uint8_t value) {
	_writeDuty(&audio->ch1.envelope, value);
	audio->ch1.control.length = 64 - audio->ch1.envelope.length;
}

void GBAudioWriteNR12(struct GBAudio* audio, uint8_t value) {
	if (!_writeSweep(&audio->ch1.envelope, value)) {
		audio->playingCh1 = false;
		*audio->nr52 &= ~0x0001;
	}
}

void GBAudioWriteNR13(struct GBAudio* audio, uint8_t value) {
	audio->ch1.control.frequency &= 0x700;
	audio->ch1.control.frequency |= GBAudioRegisterControlGetFrequency(value);
}

void GBAudioWriteNR14(struct GBAudio* audio, uint8_t value) {
	audio->ch1.control.frequency &= 0xFF;
	audio->ch1.control.frequency |= GBAudioRegisterControlGetFrequency(value << 8);
	bool wasStop = audio->ch1.control.stop;
	audio->ch1.control.stop = GBAudioRegisterControlGetStop(value << 8);
	if (!wasStop && audio->ch1.control.stop && audio->ch1.control.length && !(audio->frame & 1)) {
		--audio->ch1.control.length;
		if (audio->ch1.control.length == 0) {
			audio->playingCh1 = false;
		}
	}
	if (GBAudioRegisterControlIsRestart(value << 8)) {
		if (audio->nextEvent == INT_MAX) {
			audio->eventDiff = 0;
		}
		if (audio->playingCh1) {
			audio->ch1.control.hi = !audio->ch1.control.hi;
		}
		audio->nextCh1 = audio->eventDiff;
		audio->playingCh1 = audio->ch1.envelope.initialVolume || audio->ch1.envelope.direction;
		audio->ch1.envelope.currentVolume = audio->ch1.envelope.initialVolume;
		if (audio->ch1.envelope.currentVolume > 0) {
			audio->ch1.envelope.dead = audio->ch1.envelope.stepTime ? 0 : 1;
		} else {
			audio->ch1.envelope.dead = audio->ch1.envelope.stepTime ? 0 : 2;
		}
		audio->ch1.realFrequency = audio->ch1.control.frequency;
		audio->ch1.sweepStep = audio->ch1.time;
		audio->ch1.sweepEnable = (audio->ch1.sweepStep != 8) || audio->ch1.shift;
		audio->ch1.sweepOccurred = false;
		if (audio->playingCh1 && audio->ch1.shift) {
			audio->playingCh1 = _updateSweep(&audio->ch1, true);
		}
		if (!audio->ch1.control.length) {
			audio->ch1.control.length = 64;
			if (audio->ch1.control.stop && !(audio->frame & 1)) {
				--audio->ch1.control.length;
			}
		}
		_scheduleEvent(audio);
	}
	*audio->nr52 &= ~0x0001;
	*audio->nr52 |= audio->playingCh1;
}

void GBAudioWriteNR21(struct GBAudio* audio, uint8_t value) {
	_writeDuty(&audio->ch2.envelope, value);
	audio->ch2.control.length = 64 - audio->ch2.envelope.length;
}

void GBAudioWriteNR22(struct GBAudio* audio, uint8_t value) {
	if (!_writeSweep(&audio->ch2.envelope, value)) {
		audio->playingCh2 = false;
		*audio->nr52 &= ~0x0002;
	}
}

void GBAudioWriteNR23(struct GBAudio* audio, uint8_t value) {
	audio->ch2.control.frequency &= 0x700;
	audio->ch2.control.frequency |= GBAudioRegisterControlGetFrequency(value);
}

void GBAudioWriteNR24(struct GBAudio* audio, uint8_t value) {
	audio->ch2.control.frequency &= 0xFF;
	audio->ch2.control.frequency |= GBAudioRegisterControlGetFrequency(value << 8);
	bool wasStop = audio->ch2.control.stop;
	audio->ch2.control.stop = GBAudioRegisterControlGetStop(value << 8);
	if (!wasStop && audio->ch2.control.stop && audio->ch2.control.length && !(audio->frame & 1)) {
		--audio->ch2.control.length;
		if (audio->ch2.control.length == 0) {
			audio->playingCh2 = false;
		}
	}
	if (GBAudioRegisterControlIsRestart(value << 8)) {
		audio->playingCh2 = audio->ch2.envelope.initialVolume || audio->ch2.envelope.direction;
		audio->ch2.envelope.currentVolume = audio->ch2.envelope.initialVolume;
		if (audio->ch2.envelope.currentVolume > 0) {
			audio->ch2.envelope.dead = audio->ch2.envelope.stepTime ? 0 : 1;
		} else {
			audio->ch2.envelope.dead = audio->ch2.envelope.stepTime ? 0 : 2;
		}
		if (audio->nextEvent == INT_MAX) {
			audio->eventDiff = 0;
		}
		if (audio->playingCh2) {
			audio->ch2.control.hi = !audio->ch2.control.hi;
		}
		audio->nextCh2 = audio->eventDiff;
		if (!audio->ch2.control.length) {
			audio->ch2.control.length = 64;
			if (audio->ch2.control.stop && !(audio->frame & 1)) {
				--audio->ch2.control.length;
			}
		}
		_scheduleEvent(audio);
	}
	*audio->nr52 &= ~0x0002;
	*audio->nr52 |= audio->playingCh2 << 1;
}

void GBAudioWriteNR30(struct GBAudio* audio, uint8_t value) {
	audio->ch3.enable = GBAudioRegisterBankGetEnable(value);
	if (!audio->ch3.enable) {
		audio->playingCh3 = false;
		*audio->nr52 &= ~0x0004;
	}
}

void GBAudioWriteNR31(struct GBAudio* audio, uint8_t value) {
	audio->ch3.length = 256 - value;
}

void GBAudioWriteNR32(struct GBAudio* audio, uint8_t value) {
	audio->ch3.volume = GBAudioRegisterBankVolumeGetVolumeGB(value);
}

void GBAudioWriteNR33(struct GBAudio* audio, uint8_t value) {
	audio->ch3.rate &= 0x700;
	audio->ch3.rate |= GBAudioRegisterControlGetRate(value);
}

void GBAudioWriteNR34(struct GBAudio* audio, uint8_t value) {
	audio->ch3.rate &= 0xFF;
	audio->ch3.rate |= GBAudioRegisterControlGetRate(value << 8);
	bool wasStop = audio->ch3.stop;
	audio->ch3.stop = GBAudioRegisterControlGetStop(value << 8);
	if (!wasStop && audio->ch3.stop && audio->ch3.length && !(audio->frame & 1)) {
		--audio->ch3.length;
		if (audio->ch3.length == 0) {
			audio->playingCh3 = false;
		}
	}
	bool wasEnable = audio->playingCh3;
	if (GBAudioRegisterControlIsRestart(value << 8)) {
		audio->playingCh3 = audio->ch3.enable;
		if (!audio->ch3.length) {
			audio->ch3.length = 256;
			if (audio->ch3.stop && !(audio->frame & 1)) {
				--audio->ch3.length;
			}
		}

		if (audio->style == GB_AUDIO_DMG && wasEnable && audio->playingCh3 && audio->ch3.readable) {
			if (audio->ch3.window < 8) {
				audio->ch3.wavedata8[0] = audio->ch3.wavedata8[audio->ch3.window >> 1];
			} else {
				audio->ch3.wavedata8[0] = audio->ch3.wavedata8[((audio->ch3.window >> 1) & ~3)];
				audio->ch3.wavedata8[1] = audio->ch3.wavedata8[((audio->ch3.window >> 1) & ~3) + 1];
				audio->ch3.wavedata8[2] = audio->ch3.wavedata8[((audio->ch3.window >> 1) & ~3) + 2];
				audio->ch3.wavedata8[3] = audio->ch3.wavedata8[((audio->ch3.window >> 1) & ~3) + 3];
			}
		}
		audio->ch3.window = 0;
	}
	if (audio->playingCh3) {
		if (audio->nextEvent == INT_MAX) {
			audio->eventDiff = 0;
		}
		audio->ch3.readable = audio->style != GB_AUDIO_DMG;
		_scheduleEvent(audio);
		// TODO: Where does this cycle delay come from?
		audio->nextCh3 = audio->eventDiff + audio->nextEvent + 4 + 2 * (2048 - audio->ch3.rate);
	}
	*audio->nr52 &= ~0x0004;
	*audio->nr52 |= audio->playingCh3 << 2;
}

void GBAudioWriteNR41(struct GBAudio* audio, uint8_t value) {
	_writeDuty(&audio->ch4.envelope, value);
	audio->ch4.length = 64 - audio->ch4.envelope.length;
}

void GBAudioWriteNR42(struct GBAudio* audio, uint8_t value) {
	if (!_writeSweep(&audio->ch4.envelope, value)) {
		audio->playingCh4 = false;
		*audio->nr52 &= ~0x0008;
	}
}

void GBAudioWriteNR43(struct GBAudio* audio, uint8_t value) {
	audio->ch4.ratio = GBAudioRegisterNoiseFeedbackGetRatio(value);
	audio->ch4.frequency = GBAudioRegisterNoiseFeedbackGetFrequency(value);
	audio->ch4.power = GBAudioRegisterNoiseFeedbackGetPower(value);
}

void GBAudioWriteNR44(struct GBAudio* audio, uint8_t value) {
	bool wasStop = audio->ch4.stop;
	audio->ch4.stop = GBAudioRegisterNoiseControlGetStop(value);
	if (!wasStop && audio->ch4.stop && audio->ch4.length && !(audio->frame & 1)) {
		--audio->ch4.length;
		if (audio->ch4.length == 0) {
			audio->playingCh4 = false;
		}
	}
	if (GBAudioRegisterNoiseControlIsRestart(value)) {
		audio->playingCh4 = audio->ch4.envelope.initialVolume || audio->ch4.envelope.direction;
		audio->ch4.envelope.currentVolume = audio->ch4.envelope.initialVolume;
		if (audio->ch4.envelope.currentVolume > 0) {
			audio->ch4.envelope.dead = audio->ch4.envelope.stepTime ? 0 : 1;
		} else {
			audio->ch4.envelope.dead = audio->ch4.envelope.stepTime ? 0 : 2;
		}
		if (audio->ch4.power) {
			audio->ch4.lfsr = 0x40;
		} else {
			audio->ch4.lfsr = 0x4000;
		}
		if (audio->nextEvent == INT_MAX) {
			audio->eventDiff = 0;
		}
		audio->nextCh4 = audio->eventDiff;
		if (!audio->ch4.length) {
			audio->ch4.length = 64;
			if (audio->ch4.stop && !(audio->frame & 1)) {
				--audio->ch4.length;
			}
		}
		_scheduleEvent(audio);
	}
	*audio->nr52 &= ~0x0008;
	*audio->nr52 |= audio->playingCh4 << 3;
}

void GBAudioWriteNR50(struct GBAudio* audio, uint8_t value) {
	audio->volumeRight = GBRegisterNR50GetVolumeRight(value);
	audio->volumeLeft = GBRegisterNR50GetVolumeLeft(value);
}

void GBAudioWriteNR51(struct GBAudio* audio, uint8_t value) {
	audio->ch1Right = GBRegisterNR51GetCh1Right(value);
	audio->ch2Right = GBRegisterNR51GetCh2Right(value);
	audio->ch3Right = GBRegisterNR51GetCh3Right(value);
	audio->ch4Right = GBRegisterNR51GetCh4Right(value);
	audio->ch1Left = GBRegisterNR51GetCh1Left(value);
	audio->ch2Left = GBRegisterNR51GetCh2Left(value);
	audio->ch3Left = GBRegisterNR51GetCh3Left(value);
	audio->ch4Left = GBRegisterNR51GetCh4Left(value);
}

void GBAudioWriteNR52(struct GBAudio* audio, uint8_t value) {
	bool wasEnable = audio->enable;
	audio->enable = GBAudioEnableGetEnable(value);
	if (!audio->enable) {
		audio->playingCh1 = 0;
		audio->playingCh2 = 0;
		audio->playingCh3 = 0;
		audio->playingCh4 = 0;
		GBAudioWriteNR10(audio, 0);
		GBAudioWriteNR12(audio, 0);
		GBAudioWriteNR13(audio, 0);
		GBAudioWriteNR14(audio, 0);
		GBAudioWriteNR22(audio, 0);
		GBAudioWriteNR23(audio, 0);
		GBAudioWriteNR24(audio, 0);
		GBAudioWriteNR30(audio, 0);
		GBAudioWriteNR32(audio, 0);
		GBAudioWriteNR33(audio, 0);
		GBAudioWriteNR34(audio, 0);
		GBAudioWriteNR42(audio, 0);
		GBAudioWriteNR43(audio, 0);
		GBAudioWriteNR44(audio, 0);
		GBAudioWriteNR50(audio, 0);
		GBAudioWriteNR51(audio, 0);
		if (audio->style != GB_AUDIO_DMG) {
			GBAudioWriteNR11(audio, 0);
			GBAudioWriteNR21(audio, 0);
			GBAudioWriteNR31(audio, 0);
			GBAudioWriteNR41(audio, 0);
		}

		if (audio->p) {
			audio->p->memory.io[REG_NR10] = 0;
			audio->p->memory.io[REG_NR11] = 0;
			audio->p->memory.io[REG_NR12] = 0;
			audio->p->memory.io[REG_NR13] = 0;
			audio->p->memory.io[REG_NR14] = 0;
			audio->p->memory.io[REG_NR21] = 0;
			audio->p->memory.io[REG_NR22] = 0;
			audio->p->memory.io[REG_NR23] = 0;
			audio->p->memory.io[REG_NR24] = 0;
			audio->p->memory.io[REG_NR30] = 0;
			audio->p->memory.io[REG_NR31] = 0;
			audio->p->memory.io[REG_NR32] = 0;
			audio->p->memory.io[REG_NR33] = 0;
			audio->p->memory.io[REG_NR34] = 0;
			audio->p->memory.io[REG_NR42] = 0;
			audio->p->memory.io[REG_NR43] = 0;
			audio->p->memory.io[REG_NR44] = 0;
			audio->p->memory.io[REG_NR50] = 0;
			audio->p->memory.io[REG_NR51] = 0;
			if (audio->style != GB_AUDIO_DMG) {
				audio->p->memory.io[REG_NR11] = 0;
				audio->p->memory.io[REG_NR21] = 0;
				audio->p->memory.io[REG_NR31] = 0;
				audio->p->memory.io[REG_NR41] = 0;
			}
		}
		*audio->nr52 &= ~0x000F;
	} else if (!wasEnable) {
		audio->frame = 7;
	}
}

int32_t GBAudioProcessEvents(struct GBAudio* audio, int32_t cycles) {
	if (audio->nextEvent == INT_MAX) {
		return INT_MAX;
	}
	audio->nextEvent -= cycles;
	audio->eventDiff += cycles;
	while (audio->nextEvent <= 0) {
		audio->nextEvent = INT_MAX;
		if (audio->enable) {
			audio->nextFrame -= audio->eventDiff;
			int frame = -1;
			if (audio->nextFrame <= 0) {
				frame = (audio->frame + 1) & 7;
				audio->frame = frame;
				audio->nextFrame += FRAME_CYCLES;
				if (audio->nextFrame < audio->nextEvent) {
					audio->nextEvent = audio->nextFrame;
				}
			}

			if (audio->playingCh1) {
				audio->nextCh1 -= audio->eventDiff;
				if (!audio->ch1.envelope.dead && frame == 7) {
					--audio->ch1.envelope.nextStep;
					if (audio->ch1.envelope.nextStep == 0) {
						int8_t sample = audio->ch1.control.hi * 0x10 - 0x8;
						_updateEnvelope(&audio->ch1.envelope);
						audio->ch1.sample = sample * audio->ch1.envelope.currentVolume;
					}
				}

				if (audio->ch1.sweepEnable && (frame & 3) == 2) {
					--audio->ch1.sweepStep;
					if (audio->ch1.sweepStep == 0) {
						audio->playingCh1 = _updateSweep(&audio->ch1, false);
					}
				}

				if (audio->ch1.envelope.dead != 2) {
					if (audio->nextCh1 <= 0) {
						audio->nextCh1 += _updateChannel1(&audio->ch1);
					}
					if (audio->nextCh1 < audio->nextEvent) {
						audio->nextEvent = audio->nextCh1;
					}
				}
			}

			if (audio->ch1.control.length && audio->ch1.control.stop && !(frame & 1)) {
				--audio->ch1.control.length;
				if (audio->ch1.control.length == 0) {
					audio->playingCh1 = 0;
				}
			}

			if (audio->playingCh2) {
				audio->nextCh2 -= audio->eventDiff;
				if (!audio->ch2.envelope.dead && frame == 7) {
					--audio->ch2.envelope.nextStep;
					if (audio->ch2.envelope.nextStep == 0) {
						int8_t sample = audio->ch2.control.hi * 0x10 - 0x8;
						_updateEnvelope(&audio->ch2.envelope);
						audio->ch2.sample = sample * audio->ch2.envelope.currentVolume;
					}
				}

				if (audio->ch2.envelope.dead != 2) {
					if (audio->nextCh2 <= 0) {
						audio->nextCh2 += _updateChannel2(&audio->ch2);
					}
					if (audio->nextCh2 < audio->nextEvent) {
						audio->nextEvent = audio->nextCh2;
					}
				}
			}

			if (audio->ch2.control.length && audio->ch2.control.stop && !(frame & 1)) {
				--audio->ch2.control.length;
				if (audio->ch2.control.length == 0) {
					audio->playingCh2 = 0;
				}
			}

			if (audio->playingCh3) {
				audio->nextCh3 -= audio->eventDiff;
				audio->fadeCh3 -= audio->eventDiff;
				if (audio->fadeCh3 <= 0) {
					audio->ch3.readable = false;
					audio->fadeCh3 = INT_MAX;
				}
				if (audio->nextCh3 <= 0) {
					if (audio->style == GB_AUDIO_DMG) {
						audio->fadeCh3 = audio->nextCh3 + 2;
					}
					audio->nextCh3 += _updateChannel3(&audio->ch3, audio->style);
					audio->ch3.readable = true;
				}
				if (audio->fadeCh3 < audio->nextEvent) {
					audio->nextEvent = audio->fadeCh3;
				}
				if (audio->nextCh3 < audio->nextEvent) {
					audio->nextEvent = audio->nextCh3;
				}
			}

			if (audio->ch3.length && audio->ch3.stop && !(frame & 1)) {
				--audio->ch3.length;
				if (audio->ch3.length == 0) {
					audio->playingCh3 = 0;
				}
			}

			if (audio->playingCh4) {
				audio->nextCh4 -= audio->eventDiff;
				if (!audio->ch4.envelope.dead && frame == 7) {
					--audio->ch4.envelope.nextStep;
					if (audio->ch4.envelope.nextStep == 0) {
						int8_t sample = (audio->ch4.sample >> 31) * 0x8;
						_updateEnvelope(&audio->ch4.envelope);
						audio->ch4.sample = sample * audio->ch4.envelope.currentVolume;
					}
				}
			}

			if (audio->ch4.length && audio->ch4.stop && !(frame & 1)) {
				--audio->ch4.length;
				if (audio->ch4.length == 0) {
					audio->playingCh4 = 0;
				}
			}
		}

		*audio->nr52 &= ~0x000F;
		*audio->nr52 |= audio->playingCh1;
		*audio->nr52 |= audio->playingCh2 << 1;
		*audio->nr52 |= audio->playingCh3 << 2;
		*audio->nr52 |= audio->playingCh4 << 3;

		if (audio->p) {
			audio->nextSample -= audio->eventDiff;
			if (audio->nextSample <= 0) {
				_sample(audio, audio->sampleInterval);
				audio->nextSample += audio->sampleInterval;
			}

			if (audio->nextSample < audio->nextEvent) {
				audio->nextEvent = audio->nextSample;
			}
		}
		audio->eventDiff = 0;
	}
	return audio->nextEvent;
}

void GBAudioSamplePSG(struct GBAudio* audio, int16_t* left, int16_t* right) {
	int sampleLeft = 0;
	int sampleRight = 0;

	if (audio->ch4.envelope.dead != 2) {
		while (audio->nextCh4 <= 0) {
			audio->nextCh4 += _updateChannel4(&audio->ch4);
		}
		if (audio->nextCh4 < audio->nextEvent) {
			audio->nextEvent = audio->nextCh4;
		}
	}

	if (audio->playingCh1 && !audio->forceDisableCh[0]) {
		if (audio->ch1Left) {
			sampleLeft += audio->ch1.sample;
		}

		if (audio->ch1Right) {
			sampleRight += audio->ch1.sample;
		}
	}

	if (audio->playingCh2 && !audio->forceDisableCh[1]) {
		if (audio->ch2Left) {
			sampleLeft += audio->ch2.sample;
		}

		if (audio->ch2Right) {
			sampleRight += audio->ch2.sample;
		}
	}

	if (audio->playingCh3 && !audio->forceDisableCh[2]) {
		if (audio->ch3Left) {
			sampleLeft += audio->ch3.sample;
		}

		if (audio->ch3Right) {
			sampleRight += audio->ch3.sample;
		}
	}

	if (audio->playingCh4 && !audio->forceDisableCh[3]) {
		if (audio->ch4Left) {
			sampleLeft += audio->ch4.sample;
		}

		if (audio->ch4Right) {
			sampleRight += audio->ch4.sample;
		}
	}

	*left = sampleLeft * (1 + audio->volumeLeft);
	*right = sampleRight * (1 + audio->volumeRight);
}

void _sample(struct GBAudio* audio, int32_t cycles) {
	int16_t sampleLeft = 0;
	int16_t sampleRight = 0;
	GBAudioSamplePSG(audio, &sampleLeft, &sampleRight);
	sampleLeft = (sampleLeft * audio->masterVolume) >> 6;
	sampleRight = (sampleRight * audio->masterVolume) >> 6;

	mCoreSyncLockAudio(audio->p->sync);
	unsigned produced;
	if ((size_t) blip_samples_avail(audio->left) < audio->samples) {
		blip_add_delta(audio->left, audio->clock, sampleLeft - audio->lastLeft);
		blip_add_delta(audio->right, audio->clock, sampleRight - audio->lastRight);
		audio->lastLeft = sampleLeft;
		audio->lastRight = sampleRight;
		audio->clock += cycles;
		if (audio->clock >= CLOCKS_PER_BLIP_FRAME) {
			blip_end_frame(audio->left, audio->clock);
			blip_end_frame(audio->right, audio->clock);
			audio->clock -= CLOCKS_PER_BLIP_FRAME;
		}
	}
	produced = blip_samples_avail(audio->left);
	if (audio->p->stream && audio->p->stream->postAudioFrame) {
		audio->p->stream->postAudioFrame(audio->p->stream, sampleLeft, sampleRight);
	}
	bool wait = produced >= audio->samples;
	mCoreSyncProduceAudio(audio->p->sync, wait);

	if (wait && audio->p->stream && audio->p->stream->postAudioBuffer) {
		audio->p->stream->postAudioBuffer(audio->p->stream, audio->left, audio->right);
	}
}

void _writeDuty(struct GBAudioEnvelope* envelope, uint8_t value) {
	envelope->length = GBAudioRegisterDutyGetLength(value);
	envelope->duty = GBAudioRegisterDutyGetDuty(value);
}

bool _writeSweep(struct GBAudioEnvelope* envelope, uint8_t value) {
	envelope->stepTime = GBAudioRegisterSweepGetStepTime(value);
	envelope->direction = GBAudioRegisterSweepGetDirection(value);
	envelope->initialVolume = GBAudioRegisterSweepGetInitialVolume(value);
	if (envelope->stepTime == 0) {
		envelope->dead = envelope->currentVolume ? 1 : 2;
	} else if (!envelope->direction && !envelope->currentVolume) {
		envelope->dead = 2;
	} else if (envelope->direction && envelope->currentVolume == 0xF) {
		envelope->dead = 1;
	} else {
		envelope->dead = 0;
	}
	envelope->nextStep = envelope->stepTime;
	return envelope->initialVolume || envelope->direction;
}

static int32_t _updateSquareChannel(struct GBAudioSquareControl* control, int duty) {
	control->hi = !control->hi;
	int period = 4 * (2048 - control->frequency);
	switch (duty) {
	case 0:
		return control->hi ? period : period * 7;
	case 1:
		return control->hi ? period * 2 : period * 6;
	case 2:
		return period * 4;
	case 3:
		return control->hi ? period * 6 : period * 2;
	default:
		// This should never be hit
		return period * 4;
	}
}

static void _updateEnvelope(struct GBAudioEnvelope* envelope) {
	if (envelope->direction) {
		++envelope->currentVolume;
	} else {
		--envelope->currentVolume;
	}
	if (envelope->currentVolume >= 15) {
		envelope->currentVolume = 15;
		envelope->dead = 1;
	} else if (envelope->currentVolume <= 0) {
		envelope->currentVolume = 0;
		envelope->dead = 2;
	} else {
		envelope->nextStep = envelope->stepTime;
	}
}

static bool _updateSweep(struct GBAudioChannel1* ch, bool initial) {
	if (initial || ch->time != 8) {
		int frequency = ch->realFrequency;
		if (ch->direction) {
			frequency -= frequency >> ch->shift;
			if (!initial && frequency >= 0) {
				ch->control.frequency = frequency;
				ch->realFrequency = frequency;
			}
		} else {
			frequency += frequency >> ch->shift;
			if (frequency < 2048) {
				if (!initial && ch->shift) {
					ch->control.frequency = frequency;
					ch->realFrequency = frequency;
					if (!_updateSweep(ch, true)) {
						return false;
					}
				}
			} else {
				return false;
			}
		}
		ch->sweepOccurred = true;
	}
	ch->sweepStep = ch->time;
	return true;
}

static int32_t _updateChannel1(struct GBAudioChannel1* ch) {
	int timing = _updateSquareChannel(&ch->control, ch->envelope.duty);
	ch->sample = ch->control.hi * 0x10 - 0x8;
	ch->sample *= ch->envelope.currentVolume;
	return timing;
}

static int32_t _updateChannel2(struct GBAudioChannel2* ch) {
	int timing = _updateSquareChannel(&ch->control, ch->envelope.duty);
	ch->sample = ch->control.hi * 0x10 - 0x8;
	ch->sample *= ch->envelope.currentVolume;
	return timing;
}

static int32_t _updateChannel3(struct GBAudioChannel3* ch, enum GBAudioStyle style) {
	int i;
	int volume;
	switch (ch->volume) {
	case 0:
		volume = 0;
		break;
	case 1:
		volume = 4;
		break;
	case 2:
		volume = 2;
		break;
	case 3:
		volume = 1;
		break;
	default:
		volume = 3;
		break;
	}
	switch (style) {
		int start;
		int end;
	case GB_AUDIO_DMG:
	default:
		++ch->window;
		ch->window &= 0x1F;
		ch->sample = ch->wavedata8[ch->window >> 1];
		if (!(ch->window & 1)) {
			ch->sample >>= 4;
		}
		ch->sample &= 0xF;
		break;
	case GB_AUDIO_GBA:
		if (ch->size) {
			start = 7;
			end = 0;
		} else if (ch->bank) {
			start = 7;
			end = 4;
		} else {
			start = 3;
			end = 0;
		}
		uint32_t bitsCarry = ch->wavedata32[end] & 0x000000F0;
		uint32_t bits;
		for (i = start; i >= end; --i) {
			bits = ch->wavedata32[i] & 0x000000F0;
			ch->wavedata32[i] = ((ch->wavedata32[i] & 0x0F0F0F0F) << 4) | ((ch->wavedata32[i] & 0xF0F0F000) >> 12);
			ch->wavedata32[i] |= bitsCarry << 20;
			bitsCarry = bits;
		}
		ch->sample = bitsCarry >> 4;
		break;
	}
	ch->sample -= 8;
	ch->sample *= volume * 4;
	return 2 * (2048 - ch->rate);
}

static int32_t _updateChannel4(struct GBAudioChannel4* ch) {
	int lsb = ch->lfsr & 1;
	ch->sample = lsb * 0x10 - 0x8;
	ch->sample *= ch->envelope.currentVolume;
	ch->lfsr >>= 1;
	ch->lfsr ^= (lsb * 0x60) << (ch->power ? 0 : 8);
	int timing = ch->ratio ? 2 * ch->ratio : 1;
	timing <<= ch->frequency;
	timing *= 8;
	return timing;
}

void _scheduleEvent(struct GBAudio* audio) {
	// TODO: Don't need p
	if (audio->p) {
		audio->nextEvent = audio->p->cpu->cycles >> audio->p->doubleSpeed;
		audio->p->cpu->nextEvent = audio->nextEvent;
	} else {
		audio->nextEvent = 0;
	}
}
