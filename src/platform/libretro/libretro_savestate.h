#ifndef MGBA_LIBRETRO_SAVESTATE_H
#define MGBA_LIBRETRO_SAVESTATE_H

#include <mgba-util/common.h>

#include <mgba/core/core.h>

#include "libretro.h"

CXX_GUARD_START

size_t mLibretroSerializeSize(struct mCore* core);
bool mLibretroSerialize(struct mCore* core, void* data, size_t size);
bool mLibretroUnserialize(struct mCore* core, const void* data, size_t size, retro_log_printf_t logCallback);

CXX_GUARD_END

#endif
