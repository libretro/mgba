#include "libretro_log.h"

#include <stdarg.h>
#include <stdio.h>

static retro_log_printf_t sLogCallback;

void mLibretroSetLogCallback(retro_log_printf_t callback) {
	sLogCallback = callback;
}

retro_log_printf_t mLibretroGetLogCallback(void) {
	return sLogCallback;
}

void mLibretroLog(enum retro_log_level level, const char* format, ...) {
	if (!sLogCallback) {
		return;
	}

	char buffer[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	sLogCallback(level, "%s", buffer);
}
