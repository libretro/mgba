#include "libretro_savestate.h"

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

size_t mLibretroSerializeSize(struct mCore* core) {
	if (!core) {
		return 0;
	}

	struct VFile* vfm = VFileMemChunk(NULL, 0);
	if (!vfm) {
		return 0;
	}
	mCoreSaveStateNamed(core, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
	size_t size = vfm->size(vfm);
	vfm->close(vfm);

	if (!mLibretroMultiplayerStateActive()) {
		return size;
	}

	size_t secondarySize = mLibretroMultiplayerSerializeSize();
	if (!secondarySize) {
		return size;
	}

	return sizeof(struct mLibretroSerializedMultiplayerHeader) + size + secondarySize;
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
	ssize_t primarySize = vfm->size(vfm);
	if (primarySize <= 0) {
		vfm->close(vfm);
		return false;
	}

	if (!mLibretroMultiplayerStateActive()) {
		if ((ssize_t) size > primarySize) {
			size = primarySize;
		} else if ((ssize_t) size < primarySize) {
			vfm->close(vfm);
			return false;
		}
		vfm->seek(vfm, 0, SEEK_SET);
		vfm->read(vfm, data, size);
		vfm->close(vfm);
		return true;
	}

	size_t secondarySize = mLibretroMultiplayerSerializeSize();
	if (!secondarySize) {
		vfm->close(vfm);
		return false;
	}

	size_t totalSize = sizeof(struct mLibretroSerializedMultiplayerHeader) + (size_t) primarySize + secondarySize;
	if (size != totalSize) {
		vfm->close(vfm);
		return false;
	}

	struct mLibretroSerializedMultiplayerHeader header = {
		.magic = LIBRETRO_MULTIPLAYER_STATE_MAGIC,
		.version = LIBRETRO_MULTIPLAYER_STATE_VERSION,
		.primarySize = (uint32_t) primarySize,
		.secondarySize = (uint32_t) secondarySize,
	};

	memcpy(data, &header, sizeof(header));
	vfm->seek(vfm, 0, SEEK_SET);
	vfm->read(vfm, (uint8_t*) data + sizeof(header), (size_t) primarySize);
	vfm->close(vfm);

	return mLibretroMultiplayerSerialize((uint8_t*) data + sizeof(header) + (size_t) primarySize, secondarySize);
}

bool mLibretroUnserialize(struct mCore* core, const void* data, size_t size, retro_log_printf_t logCallback) {
	if (!core || !data) {
		return false;
	}

	if (size >= sizeof(struct mLibretroSerializedMultiplayerHeader)) {
		const struct mLibretroSerializedMultiplayerHeader* header = data;
		if (header->magic == LIBRETRO_MULTIPLAYER_STATE_MAGIC && header->version == LIBRETRO_MULTIPLAYER_STATE_VERSION) {
			size_t totalSize = sizeof(*header) + (size_t) header->primarySize + (size_t) header->secondarySize;
			if (size != totalSize || !header->primarySize || !header->secondarySize) {
				return false;
			}

			if (!mLibretroMultiplayerStateActive()) {
				if (logCallback) {
					logCallback(RETRO_LOG_WARN, "libretro: multiplayer savestate requires active splitscreen session\n");
				}
				return false;
			}

			const uint8_t* payload = (const uint8_t*) data + sizeof(*header);
			const void* primaryData = payload;
			const void* secondaryData = payload + header->primarySize;

			if (!mLibretroMultiplayerUnserialize(secondaryData, header->secondarySize)) {
				return false;
			}

			struct VFile* primary = VFileFromConstMemory(primaryData, header->primarySize);
			if (!primary) {
				return false;
			}
			bool success = mCoreLoadStateNamed(core, primary, SAVESTATE_RTC);
			primary->close(primary);
			return success;
		}
	}

	struct VFile* vfm = VFileFromConstMemory(data, size);
	if (!vfm) {
		return false;
	}
	bool success = mCoreLoadStateNamed(core, vfm, SAVESTATE_RTC);
	vfm->close(vfm);
	return success;
}
