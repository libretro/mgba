/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "thread.h"

#include "core/core.h"
#include "util/patch.h"
#include "util/vfs.h"

#include "feature/commandline.h"

#include <signal.h>

#ifndef DISABLE_THREADING

static const float _defaultFPSTarget = 60.f;

#ifdef USE_PTHREADS
static pthread_key_t _contextKey;
static pthread_once_t _contextOnce = PTHREAD_ONCE_INIT;

static void _createTLS(void) {
	pthread_key_create(&_contextKey, 0);
}
#elif _WIN32
static DWORD _contextKey;
static INIT_ONCE _contextOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK _createTLS(PINIT_ONCE once, PVOID param, PVOID* context) {
	UNUSED(once);
	UNUSED(param);
	UNUSED(context);
	_contextKey = TlsAlloc();
	return TRUE;
}
#endif

static void _changeState(struct mCoreThread* threadContext, enum mCoreThreadState newState, bool broadcast) {
	MutexLock(&threadContext->stateMutex);
	threadContext->state = newState;
	if (broadcast) {
		ConditionWake(&threadContext->stateCond);
	}
	MutexUnlock(&threadContext->stateMutex);
}

static void _waitOnInterrupt(struct mCoreThread* threadContext) {
	while (threadContext->state == THREAD_INTERRUPTED) {
		ConditionWait(&threadContext->stateCond, &threadContext->stateMutex);
	}
}

static void _waitUntilNotState(struct mCoreThread* threadContext, enum mCoreThreadState oldState) {
	MutexLock(&threadContext->sync.videoFrameMutex);
	bool videoFrameWait = threadContext->sync.videoFrameWait;
	threadContext->sync.videoFrameWait = false;
	MutexUnlock(&threadContext->sync.videoFrameMutex);

	while (threadContext->state == oldState) {
		MutexUnlock(&threadContext->stateMutex);

		if (!MutexTryLock(&threadContext->sync.videoFrameMutex)) {
			ConditionWake(&threadContext->sync.videoFrameRequiredCond);
			MutexUnlock(&threadContext->sync.videoFrameMutex);
		}

		if (!MutexTryLock(&threadContext->sync.audioBufferMutex)) {
			ConditionWake(&threadContext->sync.audioRequiredCond);
			MutexUnlock(&threadContext->sync.audioBufferMutex);
		}

		MutexLock(&threadContext->stateMutex);
		ConditionWake(&threadContext->stateCond);
	}

	MutexLock(&threadContext->sync.videoFrameMutex);
	threadContext->sync.videoFrameWait = videoFrameWait;
	MutexUnlock(&threadContext->sync.videoFrameMutex);
}

static void _pauseThread(struct mCoreThread* threadContext, bool onThread) {
	threadContext->state = THREAD_PAUSING;
	if (!onThread) {
		_waitUntilNotState(threadContext, THREAD_PAUSING);
	}
}

static THREAD_ENTRY _mCoreThreadRun(void* context) {
	struct mCoreThread* threadContext = context;
#ifdef USE_PTHREADS
	pthread_once(&_contextOnce, _createTLS);
	pthread_setspecific(_contextKey, threadContext);
#elif _WIN32
	InitOnceExecuteOnce(&_contextOnce, _createTLS, NULL, 0);
	TlsSetValue(_contextKey, threadContext);
#endif

	ThreadSetName("CPU Thread");

#if !defined(_WIN32) && defined(USE_PTHREADS)
	sigset_t signals;
	sigemptyset(&signals);
	pthread_sigmask(SIG_SETMASK, &signals, 0);
#endif

	struct mCore* core = threadContext->core;
	core->setSync(core, &threadContext->sync);
	core->reset(core);

	if (threadContext->startCallback) {
		threadContext->startCallback(threadContext);
	}

	_changeState(threadContext, THREAD_RUNNING, true);

	while (threadContext->state < THREAD_EXITING) {
		struct mDebugger* debugger = core->debugger;
		if (debugger) {
			mDebuggerRun(debugger);
			if (debugger->state == DEBUGGER_SHUTDOWN) {
				_changeState(threadContext, THREAD_EXITING, false);
			}
		} else {
			while (threadContext->state == THREAD_RUNNING) {
				core->runLoop(core);
			}
		}

		int resetScheduled = 0;
		MutexLock(&threadContext->stateMutex);
		while (threadContext->state > THREAD_RUNNING && threadContext->state < THREAD_EXITING) {
			if (threadContext->state == THREAD_PAUSING) {
				threadContext->state = THREAD_PAUSED;
				ConditionWake(&threadContext->stateCond);
			}
			if (threadContext->state == THREAD_INTERRUPTING) {
				threadContext->state = THREAD_INTERRUPTED;
				ConditionWake(&threadContext->stateCond);
			}
			if (threadContext->state == THREAD_RUN_ON) {
				if (threadContext->run) {
					threadContext->run(threadContext);
				}
				threadContext->state = threadContext->savedState;
				ConditionWake(&threadContext->stateCond);
			}
			if (threadContext->state == THREAD_RESETING) {
				threadContext->state = THREAD_RUNNING;
				resetScheduled = 1;
			}
			while (threadContext->state == THREAD_PAUSED || threadContext->state == THREAD_INTERRUPTED) {
				ConditionWait(&threadContext->stateCond, &threadContext->stateMutex);
			}
		}
		MutexUnlock(&threadContext->stateMutex);
		if (resetScheduled) {
			core->reset(core);
		}
	}

	while (threadContext->state < THREAD_SHUTDOWN) {
		_changeState(threadContext, THREAD_SHUTDOWN, false);
	}

	if (threadContext->cleanCallback) {
		threadContext->cleanCallback(threadContext);
	}

	return 0;
}

bool mCoreThreadStart(struct mCoreThread* threadContext) {
	threadContext->state = THREAD_INITIALIZED;
	threadContext->logger.p = threadContext;
	threadContext->logLevel = threadContext->core->opts.logLevel;

	if (!threadContext->sync.fpsTarget) {
		threadContext->sync.fpsTarget = _defaultFPSTarget;
	}

	MutexInit(&threadContext->stateMutex);
	ConditionInit(&threadContext->stateCond);

	MutexInit(&threadContext->sync.videoFrameMutex);
	ConditionInit(&threadContext->sync.videoFrameAvailableCond);
	ConditionInit(&threadContext->sync.videoFrameRequiredCond);
	MutexInit(&threadContext->sync.audioBufferMutex);
	ConditionInit(&threadContext->sync.audioRequiredCond);

	threadContext->interruptDepth = 0;

#ifdef USE_PTHREADS
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGINT);
	sigaddset(&signals, SIGTRAP);
	pthread_sigmask(SIG_BLOCK, &signals, 0);
#endif

	threadContext->sync.audioWait = threadContext->core->opts.audioSync;
	threadContext->sync.videoFrameWait = threadContext->core->opts.videoSync;
	threadContext->sync.fpsTarget = threadContext->core->opts.fpsTarget;

	MutexLock(&threadContext->stateMutex);
	ThreadCreate(&threadContext->thread, _mCoreThreadRun, threadContext);
	while (threadContext->state < THREAD_RUNNING) {
		ConditionWait(&threadContext->stateCond, &threadContext->stateMutex);
	}
	MutexUnlock(&threadContext->stateMutex);

	return true;
}

bool mCoreThreadHasStarted(struct mCoreThread* threadContext) {
	bool hasStarted;
	MutexLock(&threadContext->stateMutex);
	hasStarted = threadContext->state > THREAD_INITIALIZED;
	MutexUnlock(&threadContext->stateMutex);
	return hasStarted;
}

bool mCoreThreadHasExited(struct mCoreThread* threadContext) {
	bool hasExited;
	MutexLock(&threadContext->stateMutex);
	hasExited = threadContext->state > THREAD_EXITING;
	MutexUnlock(&threadContext->stateMutex);
	return hasExited;
}

bool mCoreThreadHasCrashed(struct mCoreThread* threadContext) {
	bool hasExited;
	MutexLock(&threadContext->stateMutex);
	hasExited = threadContext->state == THREAD_CRASHED;
	MutexUnlock(&threadContext->stateMutex);
	return hasExited;
}

void mCoreThreadEnd(struct mCoreThread* threadContext) {
	MutexLock(&threadContext->stateMutex);
	_waitOnInterrupt(threadContext);
	threadContext->state = THREAD_EXITING;
	ConditionWake(&threadContext->stateCond);
	MutexUnlock(&threadContext->stateMutex);
	MutexLock(&threadContext->sync.audioBufferMutex);
	threadContext->sync.audioWait = 0;
	ConditionWake(&threadContext->sync.audioRequiredCond);
	MutexUnlock(&threadContext->sync.audioBufferMutex);

	MutexLock(&threadContext->sync.videoFrameMutex);
	threadContext->sync.videoFrameWait = false;
	threadContext->sync.videoFrameOn = false;
	ConditionWake(&threadContext->sync.videoFrameRequiredCond);
	ConditionWake(&threadContext->sync.videoFrameAvailableCond);
	MutexUnlock(&threadContext->sync.videoFrameMutex);
}

void mCoreThreadReset(struct mCoreThread* threadContext) {
	MutexLock(&threadContext->stateMutex);
	_waitOnInterrupt(threadContext);
	threadContext->state = THREAD_RESETING;
	ConditionWake(&threadContext->stateCond);
	MutexUnlock(&threadContext->stateMutex);
}

void mCoreThreadJoin(struct mCoreThread* threadContext) {
	ThreadJoin(threadContext->thread);

	MutexDeinit(&threadContext->stateMutex);
	ConditionDeinit(&threadContext->stateCond);

	MutexDeinit(&threadContext->sync.videoFrameMutex);
	ConditionWake(&threadContext->sync.videoFrameAvailableCond);
	ConditionDeinit(&threadContext->sync.videoFrameAvailableCond);
	ConditionWake(&threadContext->sync.videoFrameRequiredCond);
	ConditionDeinit(&threadContext->sync.videoFrameRequiredCond);

	ConditionWake(&threadContext->sync.audioRequiredCond);
	ConditionDeinit(&threadContext->sync.audioRequiredCond);
	MutexDeinit(&threadContext->sync.audioBufferMutex);
}

bool mCoreThreadIsActive(struct mCoreThread* threadContext) {
	return threadContext->state >= THREAD_RUNNING && threadContext->state < THREAD_EXITING;
}

void mCoreThreadInterrupt(struct mCoreThread* threadContext) {
	if (!threadContext) {
		return;
	}
	MutexLock(&threadContext->stateMutex);
	++threadContext->interruptDepth;
	if (threadContext->interruptDepth > 1 || !mCoreThreadIsActive(threadContext)) {
		MutexUnlock(&threadContext->stateMutex);
		return;
	}
	threadContext->savedState = threadContext->state;
	_waitOnInterrupt(threadContext);
	threadContext->state = THREAD_INTERRUPTING;
	ConditionWake(&threadContext->stateCond);
	_waitUntilNotState(threadContext, THREAD_INTERRUPTING);
	MutexUnlock(&threadContext->stateMutex);
}

void mCoreThreadContinue(struct mCoreThread* threadContext) {
	if (!threadContext) {
		return;
	}
	MutexLock(&threadContext->stateMutex);
	--threadContext->interruptDepth;
	if (threadContext->interruptDepth < 1 && mCoreThreadIsActive(threadContext)) {
		threadContext->state = threadContext->savedState;
		ConditionWake(&threadContext->stateCond);
	}
	MutexUnlock(&threadContext->stateMutex);
}

void mCoreThreadRunFunction(struct mCoreThread* threadContext, void (*run)(struct mCoreThread*)) {
	MutexLock(&threadContext->stateMutex);
	threadContext->run = run;
	_waitOnInterrupt(threadContext);
	threadContext->savedState = threadContext->state;
	threadContext->state = THREAD_RUN_ON;
	ConditionWake(&threadContext->stateCond);
	_waitUntilNotState(threadContext, THREAD_RUN_ON);
	MutexUnlock(&threadContext->stateMutex);
}

void mCoreThreadPause(struct mCoreThread* threadContext) {
	bool frameOn = threadContext->sync.videoFrameOn;
	MutexLock(&threadContext->stateMutex);
	_waitOnInterrupt(threadContext);
	if (threadContext->state == THREAD_RUNNING) {
		_pauseThread(threadContext, false);
		threadContext->frameWasOn = frameOn;
		frameOn = false;
	}
	MutexUnlock(&threadContext->stateMutex);

	mCoreSyncSetVideoSync(&threadContext->sync, frameOn);
}

void mCoreThreadUnpause(struct mCoreThread* threadContext) {
	bool frameOn = threadContext->sync.videoFrameOn;
	MutexLock(&threadContext->stateMutex);
	_waitOnInterrupt(threadContext);
	if (threadContext->state == THREAD_PAUSED || threadContext->state == THREAD_PAUSING) {
		threadContext->state = THREAD_RUNNING;
		ConditionWake(&threadContext->stateCond);
		frameOn = threadContext->frameWasOn;
	}
	MutexUnlock(&threadContext->stateMutex);

	mCoreSyncSetVideoSync(&threadContext->sync, frameOn);
}

bool mCoreThreadIsPaused(struct mCoreThread* threadContext) {
	bool isPaused;
	MutexLock(&threadContext->stateMutex);
	_waitOnInterrupt(threadContext);
	isPaused = threadContext->state == THREAD_PAUSED;
	MutexUnlock(&threadContext->stateMutex);
	return isPaused;
}

void mCoreThreadTogglePause(struct mCoreThread* threadContext) {
	bool frameOn = threadContext->sync.videoFrameOn;
	MutexLock(&threadContext->stateMutex);
	_waitOnInterrupt(threadContext);
	if (threadContext->state == THREAD_PAUSED || threadContext->state == THREAD_PAUSING) {
		threadContext->state = THREAD_RUNNING;
		ConditionWake(&threadContext->stateCond);
		frameOn = threadContext->frameWasOn;
	} else if (threadContext->state == THREAD_RUNNING) {
		_pauseThread(threadContext, false);
		threadContext->frameWasOn = frameOn;
		frameOn = false;
	}
	MutexUnlock(&threadContext->stateMutex);

	mCoreSyncSetVideoSync(&threadContext->sync, frameOn);
}

void mCoreThreadPauseFromThread(struct mCoreThread* threadContext) {
	bool frameOn = true;
	MutexLock(&threadContext->stateMutex);
	_waitOnInterrupt(threadContext);
	if (threadContext->state == THREAD_RUNNING) {
		_pauseThread(threadContext, true);
		frameOn = false;
	}
	MutexUnlock(&threadContext->stateMutex);

	mCoreSyncSetVideoSync(&threadContext->sync, frameOn);
}

#ifdef USE_PTHREADS
struct mCoreThread* mCoreThreadGet(void) {
	pthread_once(&_contextOnce, _createTLS);
	return pthread_getspecific(_contextKey);
}
#elif _WIN32
struct mCoreThread* mCoreThreadGet(void) {
	InitOnceExecuteOnce(&_contextOnce, _createTLS, NULL, 0);
	return TlsGetValue(_contextKey);
}
#else
struct mCoreThread* mCoreThreadGet(void) {
	return NULL;
}
#endif

#else
struct mCoreThread* mCoreThreadGet(void) {
	return NULL;
}
#endif

static void _mCoreLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	struct mCoreThread* thread = mCoreThreadGet();
	if (thread && !(thread->logLevel & level)) {
		return;
	}
	printf("%s: ", mLogCategoryName(category));
	vprintf(format, args);
	printf("\n");
}

struct mLogger* mCoreThreadLogger(void) {
	struct mCoreThread* thread = mCoreThreadGet();
	if (thread) {
		if (!thread->logger.d.log) {
			thread->logger.d.log = _mCoreLog;
		}
		return &thread->logger.d;
	}
	return NULL;
}

