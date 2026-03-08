#ifndef MGBA_LIBRETRO_NETPLAY_H
#define MGBA_LIBRETRO_NETPLAY_H

#include <mgba-util/common.h>

#include "libretro.h"

CXX_GUARD_START

struct mLibretroNetplayState {
	bool contextKnown;
	bool rollbackContextActive;
};

void mLibretroNetplayInit(struct mLibretroNetplayState* state);
bool mLibretroNetplayRefresh(struct mLibretroNetplayState* state, retro_environment_t environCallback);
bool mLibretroNetplayRollbackContextActive(const struct mLibretroNetplayState* state);

CXX_GUARD_END

#endif
