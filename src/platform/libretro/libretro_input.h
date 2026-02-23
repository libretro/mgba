#ifndef MGBA_LIBRETRO_INPUT_H
#define MGBA_LIBRETRO_INPUT_H

#include <mgba-util/common.h>

#include "libretro.h"

CXX_GUARD_START

struct mLibretroTurboState {
	int clock[2];
	bool downState[2];
};

void mLibretroTurboStateInit(struct mLibretroTurboState* state);
uint16_t mLibretroInputReadKeys(unsigned port, retro_input_state_t inputCallback, bool useBitmasks, struct mLibretroTurboState* turboState);

CXX_GUARD_END

#endif
