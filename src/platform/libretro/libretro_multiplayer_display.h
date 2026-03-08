#ifndef MGBA_LIBRETRO_MULTIPLAYER_DISPLAY_H
#define MGBA_LIBRETRO_MULTIPLAYER_DISPLAY_H

#include <mgba-util/common.h>

#include "libretro.h"
#include "libretro_multiplayer.h"

CXX_GUARD_START

void mLibretroMultiplayerDisplayInit(struct mLibretroMultiplayer* multiplayer, unsigned maxVideoWidth, unsigned maxVideoHeight);
void mLibretroMultiplayerDisplayReset(struct mLibretroMultiplayer* multiplayer);
bool mLibretroMultiplayerDisplayCreateCompositeBuffer(struct mLibretroMultiplayer* multiplayer);
void mLibretroMultiplayerDisplayDestroyCompositeBuffer(struct mLibretroMultiplayer* multiplayer);
void mLibretroMultiplayerDisplayAdjustGeometry(const struct mLibretroMultiplayer* multiplayer, struct retro_game_geometry* geometry);
const mColor* mLibretroMultiplayerDisplayComposeFrame(const struct mLibretroMultiplayer* multiplayer, const mColor* primaryFrame, unsigned primaryWidth, unsigned primaryHeight, size_t* outPitch, unsigned* outWidth, unsigned* outHeight);

CXX_GUARD_END

#endif
