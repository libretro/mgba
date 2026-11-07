#ifndef MGBA_LIBRETRO_LOG_H
#define MGBA_LIBRETRO_LOG_H

#include <mgba-util/common.h>

#include "libretro.h"

CXX_GUARD_START

void mLibretroSetLogCallback(retro_log_printf_t callback);
retro_log_printf_t mLibretroGetLogCallback(void);
void mLibretroLog(enum retro_log_level level, const char* format, ...);

CXX_GUARD_END

#endif
