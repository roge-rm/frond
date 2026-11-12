#include "ExternalStorage.h"
#include <cstring>

#include "EmSPISlaveSD.h"
#include "savestate/ChunkHelper.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
#include "savestate/SavestateProbe.h"

ExternalStorage gExternalStorage;

namespace {
    constexpr uint32 SAVESTATE_VERSION = 2;
}

bool ExternalStorage::HasImage(const string& key) const { return images.find(key) != images.end(); }

CardImage* ExternalStorage::GetImage(const string& key) {
    return HasImage(key) ? images.at(key).get() : nullptr;
}

bool ExternalStorage::AddImage(const string& key, uint8* imageData, size_t size, bool readOnly) {
    if (key.empty() || key.length() > MAX_KEY_LENGTH || size == 0 || size % (CardImage::BLOCK_SIZE) != 0 || HasImage(key))
        return false;

    images.emplace(key, make_shared<CardImage>(imageData, size / CardImage::BLOCK_SIZE, readOnly));

    return true;
}

bool ExternalStorage::AddImage(const string& key, std::shared_ptr<CardImage> image) {
    if (key.empty() || key.size() > MAX_KEY_LENGTH || HasImage(key) || !image || !image->BlocksTotal()) return false;
    images.emplace(key, std::move(image));
    return true;
}

bool ExternalStorage::Mount(const string& key, EmHAL::Slot slot) {
    if (static_cast<int>(slot) < 0 || slot > EmHAL::MAX_SLOT || IsMounted(slot) || !HasImage(key) ||
        GetSlot(key) != EmHAL::Slot::none || !EmHAL::SupportsImageInSlot(slot, *images.at(key)))
        return false;

    slots[static_cast<uint8>(slot)] = make_unique<MountedImage>(key, *images.at(key));
    EmHAL::Mount(slot, *images.at(key));

    return true;
}

bool ExternalStorage::Mount(const string& key) {
    if (!HasImage(key)) return false;

    for (auto slot : {EmHAL::Slot::sdcard, EmHAL::Slot::memorystick})
        if (EmHAL::SupportsImageInSlot(slot, *images.at(key))) return Mount(key, slot);

    return false;
}

bool ExternalStorage::Unmount(EmHAL::Slot slot) {
    if (!IsMounted(slot)) return false;

    slots[static_cast<uint8>(slot)].reset();
    EmHAL::Unmount(slot);

    return true;
}

bool ExternalStorage::Unmount(const string& key) {
    if (GetSlot(key) == EmHAL::Slot::none) return false;

    return Unmount(GetSlot(key));
}

void ExternalStorage::Remount() {
    remountFailed = false;

    // Check the whole proposed set before releasing either live attachment.
    for (uint8 slot = 0; slot <= static_cast<uint8>(EmHAL::MAX_SLOT); ++slot) {
        const auto& key = mountedKeysFromSavestate[slot];
        if (key.empty()) continue;
        if (!HasImage(key) ||
            !EmHAL::SupportsImageInSlot(static_cast<EmHAL::Slot>(slot), *images.at(key))) {
            remountFailed = true;
            return;
        }
        for (uint8 previous = 0; previous < slot; ++previous) {
            if (mountedKeysFromSavestate[previous] == key) {
                remountFailed = true;
                return;
            }
        }
    }

    for (uint8 slot = 0; slot <= static_cast<uint8>(EmHAL::MAX_SLOT); slot++) {
        const string& key(mountedKeysFromSavestate[slot]);

        if (key.empty()) {
            if (slots[slot]) {
                slots[slot].reset();
                EmHAL::Unmount(static_cast<EmHAL::Slot>(slot));
            }
            continue;
        }

        slots[slot] = make_unique<MountedImage>(key, *images.at(key));
        EmHAL::Remount(static_cast<EmHAL::Slot>(slot), *images.at(key));
    }
}

bool ExternalStorage::RemountFailed() const { return remountFailed; }

bool ExternalStorage::IsMounted(EmHAL::Slot slot) const {
    return static_cast<int>(slot) >= 0 && slot <= EmHAL::MAX_SLOT &&
        slots[static_cast<uint8>(slot)].operator bool();
}

bool ExternalStorage::IsMounted(const string& key) const {
    for (auto& slot : slots)
        if (slot && slot->key == key) return true;

    return false;
}

EmHAL::Slot ExternalStorage::GetSlot(const string& key) const {
    for (uint8 slot = 0; slot <= static_cast<uint8>(EmHAL::MAX_SLOT); slot++) {
        if (slots[slot] && slots[slot]->key == key) return static_cast<EmHAL::Slot>(slot);
    }

    return EmHAL::Slot::none;
}

CardImage* ExternalStorage::GetImageInSlot(EmHAL::Slot slot) {
    return IsMounted(slot) ? &(slots[static_cast<uint8>(slot)]->image) : nullptr;
}

string ExternalStorage::GetImageKeyInSlot(EmHAL::Slot slot) {
    return IsMounted(slot) ? slots[static_cast<uint8>(slot)]->key : "";
}

void ExternalStorage::RekeyImage(string oldKey, string newKey) {
    if (newKey == oldKey || newKey.empty() || HasImage(newKey) || !HasImage(oldKey) || newKey.length() > MAX_KEY_LENGTH) return;

    images.emplace(newKey, images.at(oldKey));
    images.erase(oldKey);

    for (auto& slot : slots)
        if (slot && slot->key == oldKey) slot->key = newKey;
}

bool ExternalStorage::RemoveImage(const string& key) {
    if (!HasImage(key)) return false;

    Unmount(key);
    images.erase(key);

    return true;
}

void ExternalStorage::UnmountAll() {
    for (uint8 slot = 0; slot <= static_cast<uint8>(EmHAL::MAX_SLOT); slot++) {
        if (slots[slot]) Unmount(static_cast<EmHAL::Slot>(slot));
    }
}

void ExternalStorage::Clear() {
    UnmountAll();

    images.clear();
}

ExternalStorage::MountedImage::MountedImage(const string key, CardImage& image)
    : key(key), image(image) {}

template <typename T>
void ExternalStorage::DoSaveLoad(T& helper) {
    for (uint8 slot = 0; slot < 2; slot++)
        helper.DoString(mountedKeysFromSavestate[slot], MAX_KEY_LENGTH);
}

template <typename T>
void ExternalStorage::Save(T& savestate) {
    for (uint8 slot = 0; slot <= static_cast<uint8>(EmHAL::MAX_SLOT); slot++)
        mountedKeysFromSavestate[slot] = slots[slot] ? slots[slot]->key : "";

    typename T::chunkT* chunk = savestate.GetChunk(ChunkType::externalStorage);
    if (!chunk) return;

    const bool secondSD = !mountedKeysFromSavestate[static_cast<int>(EmHAL::Slot::sdcard2)].empty();
    chunk->Put32(secondSD ? SAVESTATE_VERSION : 1);

    SaveChunkHelper helper(*chunk);
    DoSaveLoad(helper);
    if (secondSD) helper.DoString(mountedKeysFromSavestate[2], MAX_KEY_LENGTH);
}

template void ExternalStorage::Save<Savestate<ChunkType>>(Savestate<ChunkType>&);
template void ExternalStorage::Save<SavestateProbe<ChunkType>>(SavestateProbe<ChunkType>&);

void ExternalStorage::Load(SavestateLoader<ChunkType>& loader) {
    Chunk* chunk = loader.GetChunk(ChunkType::externalStorage);
    if (!chunk) {
        for (auto& key : mountedKeysFromSavestate) key.clear();
        return;
    }

    const uint32 version = chunk->Get32();
    if (version < 1 || version > SAVESTATE_VERSION) {
        logPrintf("unable to restore ExternalStorage: unsupported savestate version\n");
        loader.NotifyError();

        return;
    }

    string keys[static_cast<int>(EmHAL::MAX_SLOT) + 1];
    bool valid = true;
    for (unsigned i = 0; i < (version == 1 ? 2u : 3u); ++i) {
        char bytes[MAX_KEY_LENGTH + 1]{};
        chunk->GetBuffer(bytes, sizeof(bytes));
        const auto end = static_cast<const char*>(std::memchr(bytes, 0, sizeof(bytes)));
        if (!end) valid = false;
        else keys[i].assign(bytes, end - bytes);
    }
    valid = valid && !chunk->HasError() && chunk->IsAtEnd();
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (!keys[i].empty() && keys[i] == keys[j]) valid = false;
    if (!valid) { loader.NotifyError(); return; }
    for (unsigned i = 0; i < 3; ++i) mountedKeysFromSavestate[i] = keys[i];
}
