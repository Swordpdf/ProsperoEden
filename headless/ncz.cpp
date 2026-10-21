// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Eden's side of NCZ files (ncz_vfs.h): headless/ncz.h's reader as a VfsFile, its sections
// encrypted again with Eden's AES, and the folder solid NCZs are unpacked to.
#include "ncz_vfs.h"

#include <atomic>
#include <cstdio>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/vfs/vfs.h"
#include "ncz.h"

namespace Eden::Ncz {
namespace {
std::atomic<bool> unpack_allowed{false};

// The unpack folder and what is in use there. Only short work is done under `lock`; `unpacking`
// is held while one file is unpacked, since two at once would only compete for the storage.
struct UnpackFolder {
    std::mutex lock, unpacking;
    std::string folder;
    std::function<void(const std::function<bool(const std::string&)>&)> clear;
    std::multiset<std::string> in_use;  // unpacked files being read
    std::set<std::string> this_game;    // unpacked files read since the game started
};
UnpackFolder& Folder() {
    static UnpackFolder folder;
    return folder;
}

bool EndsWith(const std::string& name, const char* suffix) {
    const std::size_t length = std::char_traits<char>::length(suffix);
    return name.size() >= length && name.compare(name.size() - length, length, suffix) == 0;
}

std::string SizeLabel(u64 bytes) {
    return bytes >= (1ull << 30) ? fmt::format("{:.1f} GiB", static_cast<double>(bytes) / (1ull << 30))
                                 : fmt::format("{} MiB", (bytes + (1ull << 20) - 1) >> 20);
}

// A solid NCZ's files in the folder: <its name>-<its NCA size>.unpacked and .pieces. Its name is
// the NCA's ID, so the same content has the same files wherever it comes from.
std::string FileOf(const std::string& ncz_name, u64 nca_size, const char* kind) {
    return fmt::format("{}-{:x}.{}", ncz_name, nca_size, kind);
}

// The pieces found in each solid NCZ outside a game, for every reader of it: one object per NCZ
// for as long as any reader holds it (two on one pieces file would write over each other), and
// the most recently used 16 kept when none does. Those in its file stay there for later starts.
std::shared_ptr<SolidPieces> PiecesOf(const std::string& name) {
    static std::mutex lock;
    static std::map<std::string, std::weak_ptr<SolidPieces>> live;
    static std::list<std::shared_ptr<SolidPieces>> recent;
    std::string folder;
    {
        auto& unpack = Folder();
        std::scoped_lock guard{unpack.lock};
        folder = unpack.folder;
    }
    std::scoped_lock guard{lock};
    std::shared_ptr<SolidPieces> pieces = live[name].lock();
    if (pieces) {
        recent.remove(pieces);
    } else {
        pieces = std::make_shared<SolidPieces>(folder.empty() ? std::string{} : folder + "/" + name);
        live[name] = pieces;
    }
    recent.push_back(pieces);
    if (recent.size() > 16) recent.pop_front();
    for (auto it = live.begin(); it != live.end();) it = it->second.expired() ? live.erase(it) : std::next(it);
    return pieces;
}

// An unpacked file in use for as long as its reader lives.
class HeldPlaintext final : public Plaintext {
public:
    HeldPlaintext(std::unique_ptr<Plaintext> inner_, std::string name_)
        : inner(std::move(inner_)), name(std::move(name_)) {
        auto& folder = Folder();
        std::scoped_lock lock{folder.lock};
        folder.in_use.insert(name);
        if (unpack_allowed.load(std::memory_order_relaxed)) folder.this_game.insert(name);
    }
    ~HeldPlaintext() override {
        auto& folder = Folder();
        std::scoped_lock lock{folder.lock};
        if (const auto it = folder.in_use.find(name); it != folder.in_use.end()) folder.in_use.erase(it);
    }
    bool Read(u8* data, std::size_t size, u64 offset) override { return inner->Read(data, size, offset); }

private:
    std::unique_ptr<Plaintext> inner;
    std::string name;
};

// The copy of a solid NCZ unpacked before, at any time (also outside a game).
std::unique_ptr<Plaintext> Unpacked(const std::string& ncz_name, u64 nca_size) {
    std::string folder;
    {
        auto& unpack = Folder();
        std::scoped_lock lock{unpack.lock};
        folder = unpack.folder;
    }
    if (folder.empty()) return nullptr;
    const std::string name = FileOf(ncz_name, nca_size, "unpacked");
    auto plaintext = OpenUnpacked(folder + "/" + name, nca_size - kHeaderSize);
    if (!plaintext) return nullptr;
    LOG_INFO(Loader, "{}: reading the copy unpacked before ({})", ncz_name, name);
    return std::make_unique<HeldPlaintext>(std::move(plaintext), name);
}

// Unpacks a solid NCZ, while its game runs: what was unpacked for another game is removed first.
std::unique_ptr<Plaintext> Unpack(const std::string& ncz_name, const Layout& layout, const ReadFn& read) {
    auto& unpack = Folder();
    std::scoped_lock unpacking{unpack.unpacking};
    std::string folder;
    {
        std::scoped_lock lock{unpack.lock};
        folder = unpack.folder;
    }
    const std::string size = SizeLabel(layout.StreamSize());
    if (folder.empty()) {
        LOG_ERROR(Loader, "{} is a solid NCZ of {} and there is no folder to unpack it to", ncz_name, size);
        return nullptr;
    }
    const std::string name = FileOf(ncz_name, layout.nca_size, "unpacked");
    const std::string path = folder + "/" + name;
    if (auto plaintext = OpenUnpacked(path, layout.StreamSize()))  // another reader made it meanwhile
        return std::make_unique<HeldPlaintext>(std::move(plaintext), name);
    // Only the game being played keeps its unpacked copies: the console's storage is not for all.
    // The pieces files are small and are kept. Under the lock: a copy opened meanwhile is in use.
    {
        std::scoped_lock lock{unpack.lock};
        if (unpack.clear)
            unpack.clear([&](const std::string& file) {
                return EndsWith(file, ".pieces") || file == name || unpack.in_use.contains(file) ||
                       unpack.this_game.contains(file);
            });
    }
    LOG_INFO(Loader, "{} is solid-compressed: unpacking its {} to {} once, for this and later starts", ncz_name,
             size, path);
    std::fprintf(stderr, "[ProsperoEden] NSZ: unpacking %s (%s, solid) to %s\n", ncz_name.c_str(), size.c_str(),
                 path.c_str());
    std::string error;
    auto plaintext = UnpackToFile(
        layout, read, path, &error,
        [&ncz_name](u64 done, u64 total) {
            std::fprintf(stderr, "[ProsperoEden] NSZ: %s %.0f%%\n", ncz_name.c_str(),
                         100.0 * static_cast<double>(done) / static_cast<double>(total));
        },
        [] { return !unpack_allowed.load(std::memory_order_relaxed); });
    if (!plaintext) {
        LOG_ERROR(Loader, "Cannot unpack {}: {}. It needs {} free. It is read without unpacking instead, which is "
                  "slow; nsz -D, then nsz -C -B (block compression) on a computer, lets it be played without "
                  "unpacking", ncz_name, error, size);
        std::fprintf(stderr, "[ProsperoEden] NSZ: cannot unpack %s: %s\n", ncz_name.c_str(), error.c_str());
        return nullptr;
    }
    return std::make_unique<HeldPlaintext>(std::move(plaintext), name);
}

class NczFile final : public FileSys::VfsFile {
public:
    NczFile(FileSys::VirtualFile base_, Layout layout, std::string name_)
        : base(std::move(base_)), name(std::move(name_)) {
        const std::string ncz_name = base->GetName();
        const FileSys::VirtualFile source = base;
        ReadFn read = [source](u8* data, std::size_t size, u64 offset) { return source->Read(data, size, offset); };
        SolidOptions solid;
        if (!layout.block) {
            solid.unpacked = [ncz_name, nca_size = layout.nca_size] { return Unpacked(ncz_name, nca_size); };
            solid.unpack = [ncz_name](const Layout& l, const ReadFn& r) { return Unpack(ncz_name, l, r); };
            solid.may_unpack = [] { return unpack_allowed.load(std::memory_order_relaxed); };
            solid.pieces = PiecesOf(FileOf(ncz_name, layout.nca_size, "pieces"));
        }
        reader = std::make_unique<Reader>(
            std::move(layout), read,
            [this](std::size_t index, u64 offset, u8* data, std::size_t size) { Encrypt(index, offset, data, size); },
            std::move(solid));
    }

    std::string GetName() const override { return name; }
    std::size_t GetSize() const override { return static_cast<std::size_t>(reader->Size()); }
    bool Resize(std::size_t) override { return false; }
    FileSys::VirtualDir GetContainingDirectory() const override { return base->GetContainingDirectory(); }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        std::scoped_lock guard{lock};
        return reader->Read(data, length, offset);
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override { return 0; }
    bool Rename(std::string_view) override { return false; }

private:
    // Called from Read, with the lock held. A BKTR's sections (thousands, split by their counters)
    // share one key: one cipher per key.
    void Encrypt(std::size_t index, u64 offset, u8* data, std::size_t size) const {
        const Section& section = reader->GetLayout().sections[index];
        auto& cipher = ciphers[section.key];
        if (!cipher) {
            Core::Crypto::Key128 key;
            std::copy(section.key.begin(), section.key.end(), key.begin());
            cipher = std::make_unique<Core::Crypto::AESCipher<Core::Crypto::Key128>>(key, Core::Crypto::Mode::CTR);
        }
        cipher->SetIV(CounterFor(section, offset));
        cipher->Transcode(data, size, data, Core::Crypto::Op::Encrypt);
    }

    FileSys::VirtualFile base;
    std::string name;
    mutable std::mutex lock;
    mutable std::unique_ptr<Reader> reader;
    mutable std::map<std::array<u8, 16>, std::unique_ptr<Core::Crypto::AESCipher<Core::Crypto::Key128>>> ciphers;
};
} // namespace

FileSys::VirtualFile Open(FileSys::VirtualFile file) {
    if (!file) return nullptr;
    const ReadFn read = [&file](u8* data, std::size_t size, u64 offset) { return file->Read(data, size, offset); };
    std::string error;
    auto layout = ParseLayout(read, file->GetSize(), &error);
    if (!layout) {
        LOG_ERROR(Loader, "Cannot read {} as an NCZ: {}", file->GetName(), error);
        return nullptr;
    }
    std::string name = file->GetName();
    if (name.size() > 4 && name.ends_with(".ncz")) name.replace(name.size() - 4, 4, ".nca");
    LOG_DEBUG(Loader, "{}: {} NCZ of {} bytes ({} sections)", file->GetName(), layout->block ? "block" : "solid",
              layout->nca_size, layout->sections.size());
    return std::make_shared<NczFile>(std::move(file), std::move(*layout), std::move(name));
}

void SetUnpackAllowed(bool allowed) {
    if (allowed && !unpack_allowed.load(std::memory_order_relaxed)) {
        auto& folder = Folder();
        std::scoped_lock lock{folder.lock};
        folder.this_game.clear();  // a new game starts
    }
    unpack_allowed.store(allowed, std::memory_order_relaxed);
}

void SetUnpackFolder(std::string folder,
                     std::function<void(const std::function<bool(const std::string&)>& keep)> clear) {
    // What an unpack cut short (the app closed, the power went) left behind goes now.
    if (clear) clear([](const std::string& file) { return !EndsWith(file, ".part"); });
    auto& unpack = Folder();
    std::scoped_lock lock{unpack.lock};
    unpack.folder = std::move(folder);
    unpack.clear = std::move(clear);
}
} // namespace Eden::Ncz
