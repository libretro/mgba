/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "log.h"

#include "core/thread.h"

#define MAX_CATEGORY 64

static struct mLogger* _defaultLogger = NULL;

struct mLogger* mLogGetContext(void) {
	struct mLogger* logger = NULL;
#ifndef DISABLE_LOGGING
	logger = mCoreThreadLogger();
#endif
	if (logger) {
		return logger;
	}
	return _defaultLogger;
}

void mLogSetDefaultLogger(struct mLogger* logger) {
	_defaultLogger = logger;
}

static int _category = 0;
static const char* _categoryNames[MAX_CATEGORY];

int mLogGenerateCategory(const char* name) {
	++_category;
	if (_category < MAX_CATEGORY) {
		_categoryNames[_category] = name;
	}
	return _category;
}

const char* mLogCategoryName(int category) {
	if (category < MAX_CATEGORY) {
		return _categoryNames[category];
	}
	return 0;
}

mLOG_DEFINE_CATEGORY(STATUS, "Status")
