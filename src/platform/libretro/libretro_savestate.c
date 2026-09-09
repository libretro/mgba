#include "libretro_savestate.h"

#include "libretro_log.h"

#include <mgba/core/serialize.h>
#include <mgba-util/vfs.h>

#include <string.h>

#include "libretro_multiplayer.h"

struct mLibretroSerializedMultiplayerHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t primarySize;
	uint32_t secondarySize;
};

#define LIBRETRO_MULTIPLAYER_STATE_MAGIC 0x314C504D
#define LIBRETRO_MULTIPLAYER_STATE_VERSION 1

static bool _serializeSecondaryStates(void* data, size_t size) {
	const struct mLibretroMultiplayer* multiplayer = mLibretroMultiplayerGet();
	if (!multiplayer || !data || !size) {
		return false;
	}

	int configured = mLibretroMultiplayerConfiguredPlayers();
	if (configured < 2) {
		return false;
	}

	int numSlots = configured - 1;
	size_t perSlot = size / numSlots;
	if (perSlot * numSlots != size) {
		return false;
	}

	memset(data, 0, size);

	uint8_t* cursor = data;
	int i;
	for (i = 1; i < multiplayer->numPlayers && i < configured; ++i) {
		if (!multiplayer->cores[i]) {
			cursor += perSlot;
			continue;
		}

		struct VFile* vfm = VFileMemChunk(NULL, 0);
		if (!vfm) {
			return false;
		}

		mCoreSaveStateNamed(multiplayer->cores[i], vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
		ssize_t stateSize = vfm->size(vfm);
		if (stateSize <= 0 || (size_t) stateSize > perSlot) {
			vfm->close(vfm);
			return false;
		}

		vfm->seek(vfm, 0, SEEK_SET);
		vfm->read(vfm, cursor, (size_t) stateSize);
		vfm->close(vfm);
		cursor += perSlot;
	}

	return true;
}

static bool _unserializeSecondaryStates(const void* data, size_t size) {
	const struct mLibretroMultiplayer* multiplayer = mLibretroMultiplayerGet();
	if (!multiplayer || !mLibretroMultiplayerStateActive() || !data || !size) {
		return false;
	}

	int configured = mLibretroMultiplayerConfiguredPlayers();
	if (configured < 2) {
		return false;
	}

	int numSlots = configured - 1;
	size_t perSlot = size / numSlots;
	if (perSlot * numSlots != size) {
		return false;
	}

	const uint8_t* cursor = data;
	int i;
	for (i = 1; i < multiplayer->numPlayers && i < configured; ++i) {
		if (!multiplayer->cores[i]) {
			cursor += perSlot;
			continue;
		}

		struct VFile* vfm = VFileFromConstMemory(cursor, perSlot);
		if (!vfm) {
			return false;
		}

		if (!mCoreLoadStateNamed(multiplayer->cores[i], vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC)) {
			vfm->close(vfm);
			return false;
		}

		vfm->close(vfm);
		cursor += perSlot;
	}

	return true;
}

size_t mLibretroSerializeSize(struct mCore* core) {
	if (!core) {
		return 0;
	}

	// mGBA states can vary slightly in size depending on RTC/SaveData. 
	// We MUST ensure the size returned here is stable.
	struct VFile* vfm = VFileMemChunk(NULL, 0);
	if (!vfm) {
		return 0;
	}
	mCoreSaveStateNamed(core, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
	size_t baseSize = vfm->size(vfm);
	vfm->close(vfm);

	int configured = mLibretroMultiplayerConfiguredPlayers();
	if (configured < 2) {
		return baseSize;
	}

	size_t secondarySize = (configured - 1) * baseSize;
	return sizeof(struct mLibretroSerializedMultiplayerHeader) + baseSize + secondarySize;
}

bool mLibretroSerialize(struct mCore* core, void* data, size_t size) {
	if (!core || !data) {
		return false;
	}

	struct VFile* vfm = VFileMemChunk(NULL, 0);
	if (!vfm) {
		return false;
	}
	
	mCoreSaveStateNamed(core, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
	size_t primarySize = (size_t)vfm->size(vfm);

	int configured = mLibretroMultiplayerConfiguredPlayers();
	
	// Handle Single Player / Standard case
	if (configured < 2) {
		if (size < primarySize) {
			vfm->close(vfm);
			return false;
		}
		vfm->seek(vfm, 0, SEEK_SET);
		vfm->read(vfm, data, primarySize);
		vfm->close(vfm);
		
		// Ensure remainder of buffer is clean for determinism
		if (size > primarySize) {
			memset((uint8_t*)data + primarySize, 0, size - primarySize);
		}
		return true;
	}

	// Handle Multiplayer case
	size_t secondarySize = (configured - 1) * primarySize;
	size_t totalRequired = sizeof(struct mLibretroSerializedMultiplayerHeader) + primarySize + secondarySize;
	
	if (size < totalRequired) {
		vfm->close(vfm);
		return false;
	}

	struct mLibretroSerializedMultiplayerHeader header = {
		.magic = LIBRETRO_MULTIPLAYER_STATE_MAGIC,
		.version = LIBRETRO_MULTIPLAYER_STATE_VERSION,
		.primarySize = (uint32_t)primarySize,
		.secondarySize = (uint32_t)secondarySize,
	};

	// Use memcpy to avoid alignment faults on ARM/other architectures
	memcpy(data, &header, sizeof(header));
	
	vfm->seek(vfm, 0, SEEK_SET);
	vfm->read(vfm, (uint8_t*)data + sizeof(header), primarySize);
	vfm->close(vfm);

	uint8_t* secondaryPtr = (uint8_t*)data + sizeof(header) + primarySize;
	if (mLibretroMultiplayerStateActive()) {
		if (!_serializeSecondaryStates(secondaryPtr, secondarySize)) {
			return false;
		}
	} else {
		memset(secondaryPtr, 0, secondarySize);
	}

	// Pad out any remaining space provided by RetroArch
	if (size > totalRequired) {
		memset((uint8_t*)data + totalRequired, 0, size - totalRequired);
	}

	return true;
}

bool mLibretroUnserialize(struct mCore* core, const void* data, size_t size) {
	if (!core || !data) {
		return false;
	}

	// Safely check for multiplayer header via memcpy to avoid alignment issues
	if (size >= sizeof(struct mLibretroSerializedMultiplayerHeader)) {
		struct mLibretroSerializedMultiplayerHeader header;
		memcpy(&header, data, sizeof(header));

		if (header.magic == LIBRETRO_MULTIPLAYER_STATE_MAGIC && header.version == LIBRETRO_MULTIPLAYER_STATE_VERSION) {
			size_t totalRequired = sizeof(header) + (size_t) header.primarySize + (size_t) header.secondarySize;
			if (size >= totalRequired && header.primarySize > 0) {
				const uint8_t* payload = (const uint8_t*) data + sizeof(header);
				const uint8_t* secondaryData = payload + header.primarySize;

				struct VFile* primary = VFileFromConstMemory(payload, header.primarySize);
				if (!primary) {
					return false;
				}

				bool success = mCoreLoadStateNamed(core, primary, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
				primary->close(primary);
				if (!success) {
					return false;
				}

				if (mLibretroMultiplayerStateActive() && header.secondarySize > 0 && !_unserializeSecondaryStates(secondaryData, header.secondarySize)) {
					mLibretroLog(RETRO_LOG_WARN, "libretro: failed secondary unserialize\n");
				}

				return true;
			}
		}
	}

	// Fallback to standard mGBA state load
	struct VFile* vfm = VFileFromConstMemory(data, size);
	if (!vfm) {
		return false;
	}
	bool success = mCoreLoadStateNamed(core, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
	vfm->close(vfm);
	return success;
}