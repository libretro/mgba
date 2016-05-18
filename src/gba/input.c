/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "input.h"

#include "gba/interface.h"

const struct mInputPlatformInfo GBAInputInfo = {
	.platformName = "gba",
	.keyId = (const char*[]) {
		"A",
		"B",
		"Select",
		"Start",
		"Right",
		"Left",
		"Up",
		"Down",
		"R",
		"L"
	},
	.nKeys = GBA_KEY_MAX
};
