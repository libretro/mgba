#include "libretro_netplay.h"

#include "libretro_log.h"

void mLibretroNetplayInit(struct mLibretroNetplayState* state) {
	if (!state) {
		return;
	}

	state->rollbackContextActive = false;
	state->contextKnown = false;
}

bool mLibretroNetplayRefresh(struct mLibretroNetplayState* state, retro_environment_t environCallback) {
	if (!state) {
		return false;
	}

	unsigned localClientIndex = 0;
	bool netplayActive = environCallback && environCallback(RETRO_ENVIRONMENT_GET_NETPLAY_CLIENT_INDEX, &localClientIndex);

	bool haveContext = false;
	bool rollbackActive = false;
	enum retro_savestate_context context = RETRO_SAVESTATE_CONTEXT_NORMAL;
	if (environCallback && environCallback(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT, &context)) {
		haveContext = true;
		rollbackActive = context == RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY;
	}

	if (!haveContext) {
		return false;
	}

	state->contextKnown = true;

	if (!netplayActive) {
		if (!state->rollbackContextActive) {
			return false;
		}
		state->rollbackContextActive = false;
		mLibretroLog(RETRO_LOG_INFO, "libretro: rollback netplay context ended\n");
		return true;
	}

	if (!rollbackActive || rollbackActive == state->rollbackContextActive) {
		return false;
	}

	state->rollbackContextActive = rollbackActive;
	mLibretroLog(RETRO_LOG_INFO, "libretro: rollback netplay context detected\n");

	return true;
}

bool mLibretroNetplayRollbackContextActive(const struct mLibretroNetplayState* state) {
	if (!state) {
		return false;
	}
	return state->rollbackContextActive;
}
