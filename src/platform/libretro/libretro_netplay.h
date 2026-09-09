#ifndef MGBA_LIBRETRO_NETPLAY_H
#define MGBA_LIBRETRO_NETPLAY_H

#include <mgba-util/common.h>

#include "libretro.h"

CXX_GUARD_START

struct mLibretroNetplayState {
	bool rollbackContextActive;
};

void mLibretroNetplayInit(struct mLibretroNetplayState* state);
bool mLibretroNetplayRefresh(struct mLibretroNetplayState* state, retro_environment_t environCallback);

CXX_GUARD_END

#endif
