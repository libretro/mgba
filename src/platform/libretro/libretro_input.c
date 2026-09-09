#include "libretro_input.h"

#include <string.h>

static const int keymap[] = {
	RETRO_DEVICE_ID_JOYPAD_A,
	RETRO_DEVICE_ID_JOYPAD_B,
	RETRO_DEVICE_ID_JOYPAD_SELECT,
	RETRO_DEVICE_ID_JOYPAD_START,
	RETRO_DEVICE_ID_JOYPAD_RIGHT,
	RETRO_DEVICE_ID_JOYPAD_LEFT,
	RETRO_DEVICE_ID_JOYPAD_UP,
	RETRO_DEVICE_ID_JOYPAD_DOWN,
	RETRO_DEVICE_ID_JOYPAD_R,
	RETRO_DEVICE_ID_JOYPAD_L,
};

void mLibretroTurboStateInit(struct mLibretroTurboState* state) {
	memset(state, 0, sizeof(*state));
	state->downState[0] = true;
	state->downState[1] = true;
}

static int16_t _cycleTurbo(unsigned port, bool a, bool b, bool l, bool r, struct mLibretroTurboState* turboState) {
	int16_t buttons = 0;
	if (port > 1) {
		port = 0;
	}
	turboState->clock[port]++;
	if (turboState->clock[port] >= 2) {
		turboState->clock[port] = 0;
		turboState->downState[port] = !turboState->downState[port];
	}

	if (a) {
		buttons |= turboState->downState[port] << 0;
	}

	if (b) {
		buttons |= turboState->downState[port] << 1;
	}

	if (l) {
		buttons |= turboState->downState[port] << 9;
	}

	if (r) {
		buttons |= turboState->downState[port] << 8;
	}

	return buttons;
}

uint16_t mLibretroInputReadKeys(unsigned port, retro_input_state_t inputCallback, bool useBitmasks, struct mLibretroTurboState* turboState) {
	uint16_t keys = 0;
	size_t i;

	if (useBitmasks) {
		int16_t joypadMask = inputCallback(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
		for (i = 0; i < sizeof(keymap) / sizeof(*keymap); ++i) {
			keys |= ((joypadMask >> keymap[i]) & 1) << i;
		}
#define JOYPAD_BIT(BUTTON) (1 << RETRO_DEVICE_ID_JOYPAD_ ## BUTTON)
		keys |= _cycleTurbo(port, joypadMask & JOYPAD_BIT(X), joypadMask & JOYPAD_BIT(Y), joypadMask & JOYPAD_BIT(L2), joypadMask & JOYPAD_BIT(R2), turboState);
#undef JOYPAD_BIT
	} else {
		for (i = 0; i < sizeof(keymap) / sizeof(*keymap); ++i) {
			keys |= (!!inputCallback(port, RETRO_DEVICE_JOYPAD, 0, keymap[i])) << i;
		}
		keys |= _cycleTurbo(
			port,
			inputCallback(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X),
			inputCallback(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y),
			inputCallback(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2),
			inputCallback(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2),
			turboState
		);
	}

	return keys;
}
