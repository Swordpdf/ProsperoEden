// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// NCZ: an NCA as nsz (https://github.com/nicoboss/nsz) compresses it, the form NSZ and XCZ files
// hold their programs and data in. This reads one back as the NCA it was made from, byte for byte,
// without writing the NCA anywhere: Eden then reads it as it reads any NCA (ncz.cpp).
//
// The layout, as nsz writes it and its own decompressor reads it:
//   0x0000  the NCA's first 0x4000 bytes (its header), as they are
//   0x4000  "NCZSECTN", a u64 count, then that many sections of 0x40 bytes: u64 offset, u64 size,
//           u64 crypto type, u64 padding, 16-byte key, 16-byte counter. They cover the NCA from
//           the first one's offset to its end, one after the other.
//   then    either "NCZBLOCK" (u8 version, u8 type, u8 unused, u8 block size exponent, u32 block
//           count, u64 decompressed size, a u32 compressed size per block) and the blocks, or
//           (solid) one zstd stream.
// What is compressed is the NCA from 0x4000 to its end, decrypted. A section of crypto type 3
// (CTR) or 4 (BKTR) is encrypted again with AES-128-CTR, its key, and a counter of the section
// counter's first 8 bytes then the NCA offset / 16 (big endian). A block is stored as it is when
// compressing it made it no smaller; the last one holds what remains.
//
// Block NCZs (nsz -B, the default for XCZ) are read where they are asked. A solid one (the default
// for NSZ) can only be decompressed from its start. While its game runs, it is unpacked once to a
// file (SolidOptions::unpack), which later starts use as well. Outside a game, a read decompresses
// it up to the read and keeps a few MiB around it, for every reader of that NCZ and (in a file
// beside the unpacked ones) for later starts: opening an update reads its patch tables at the end
// of its data, opening a game its logo after its data, and a library scan opens them every time.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <zstd.h>

namespace Eden::Ncz {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline constexpr u64 kHeaderSize = 0x4000;
inline constexpr std::size_t kSectionEntrySize = 0x40;
inline constexpr std::size_t kMaxSections = 1u << 16;  // BKTR sections are split by their counters
inline constexpr unsigned kLowestBlockExponent = 14;    // nsz's own limits
inline constexpr unsigned kHighestBlockExponent = 32;
inline constexpr unsigned kLargestBlockExponent = 26;   // what this reads: 64 MiB blocks at most
// A solid NCZ no larger than this is never unpacked: one window holds it whole.
inline constexpr u64 kSmallSolid = 16ull << 20;

struct Section {
    u64 offset = 0, size = 0, crypto_type = 0;
    std::array<u8, 16> key{}, counter{};
    bool Encrypted() const { return crypto_type == 3 || crypto_type == 4; }
    u64 End() const { return offset + size; }
};

struct Layout {
    std::vector<Section> sections;
    u64 nca_size = 0;     // the NCA it reads as
    u64 data_offset = 0;  // where the blocks or the stream begin in the NCZ
    bool block = false;
    unsigned block_exponent = 0;
    std::vector<u64> block_offsets;  // in the NCZ
    std::vector<u32> block_sizes;    // compressed
    u64 BlockSize() const { return u64{1} << block_exponent; }
    u64 StreamSize() const { return nca_size - kHeaderSize; }
};

// Reads `size` bytes at `offset` of the NCZ; returns how many it read.
using ReadFn = std::function<std::size_t(u8* data, std::size_t size, u64 offset)>;

namespace Detail {
inline u64 Le64(const u8* p) {
    u64 value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
    return value;
}
inline u32 Le32(const u8* p) {
    return u32{p[0]} | u32{p[1]} << 8 | u32{p[2]} << 16 | u32{p[3]} << 24;
}
inline bool ReadExact(const ReadFn& read, u8* data, std::size_t size, u64 offset) {
    std::size_t done = 0;
    while (done < size) {
        const std::size_t got = read(data + done, size - done, offset + done);
        if (got == 0) return false;
        done += got;
    }
    return true;
}
} // namespace Detail

// Whether the file starts like an NCZ: an NCA header, then "NCZSECTN".
inline bool LooksLikeNcz(const ReadFn& read, u64 file_size) {
    u8 magic[8];
    return file_size >= kHeaderSize + 16 && Detail::ReadExact(read, magic, 8, kHeaderSize) &&
           std::memcmp(magic, "NCZSECTN", 8) == 0;
}

// The NCZ's layout, or nothing (and why) when it is not one this can read.
inline std::optional<Layout> ParseLayout(const ReadFn& read, u64 file_size, std::string* error) {
    const auto fail = [error](const char* why) -> std::optional<Layout> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (file_size < kHeaderSize + 16) return fail("too small for an NCZ");
    u8 head[16];
    if (!Detail::ReadExact(read, head, sizeof(head), kHeaderSize)) return fail("cannot read its section header");
    if (std::memcmp(head, "NCZSECTN", 8) != 0) return fail("no NCZSECTN after the NCA header");
    const u64 count = Detail::Le64(head + 8);
    if (count == 0 || count > kMaxSections) return fail("section count out of range");
    u64 at = kHeaderSize + 16;
    if (count * kSectionEntrySize > file_size - at) return fail("section table runs past the file");
    std::vector<u8> table(count * kSectionEntrySize);
    if (!Detail::ReadExact(read, table.data(), table.size(), at)) return fail("cannot read its section table");
    at += table.size();

    Layout layout;
    layout.sections.resize(count);
    for (u64 i = 0; i < count; ++i) {
        const u8* entry = table.data() + i * kSectionEntrySize;
        Section& section = layout.sections[i];
        section.offset = Detail::Le64(entry);
        section.size = Detail::Le64(entry + 8);
        section.crypto_type = Detail::Le64(entry + 16);
        std::memcpy(section.key.data(), entry + 32, 16);
        std::memcpy(section.counter.data(), entry + 48, 16);
        if (section.size > ~u64{0} - section.offset) return fail("a section ends past 2^64");
        // nsz writes them one after the other, and its decompressor relies on it.
        if (i > 0 && section.offset != layout.sections[i - 1].End()) return fail("sections are not contiguous");
    }
    layout.nca_size = std::max(kHeaderSize, layout.sections.back().End());
    if (layout.nca_size == kHeaderSize) return fail("no data after the NCA header");
    layout.data_offset = at;

    u8 block_head[24];
    layout.block = file_size - at >= sizeof(block_head) &&
                   Detail::ReadExact(read, block_head, sizeof(block_head), at) &&
                   std::memcmp(block_head, "NCZBLOCK", 8) == 0;
    if (!layout.block) return layout;

    layout.block_exponent = block_head[11];
    if (layout.block_exponent < kLowestBlockExponent || layout.block_exponent > kHighestBlockExponent)
        return fail("block size exponent out of range (14 to 32)");
    if (layout.block_exponent > kLargestBlockExponent)
        return fail("blocks larger than 64 MiB are not supported; nsz -D, then nsz -C -B (1 MiB blocks), makes "
                    "one that is");
    const u64 blocks = Detail::Le32(block_head + 12);
    const u64 decompressed = Detail::Le64(block_head + 16);
    if (decompressed != layout.StreamSize()) return fail("block data size does not match its sections");
    const u64 block_size = layout.BlockSize();
    if (blocks != (decompressed + block_size - 1) / block_size) return fail("block count does not match its size");
    at += sizeof(block_head);
    if (blocks * 4 > file_size - at) return fail("block size list runs past the file");
    std::vector<u8> sizes(blocks * 4);
    if (!Detail::ReadExact(read, sizes.data(), sizes.size(), at)) return fail("cannot read its block sizes");
    at += sizes.size();
    layout.data_offset = at;
    layout.block_offsets.resize(blocks);
    layout.block_sizes.resize(blocks);
    for (u64 i = 0; i < blocks; ++i) {
        const u32 size = Detail::Le32(sizes.data() + i * 4);
        if (size == 0) return fail("a block has no data");
        if (size > file_size - at) return fail("blocks run past the file");
        layout.block_offsets[i] = at;
        layout.block_sizes[i] = size;
        at += size;
    }
    return layout;
}

// The decompressed (and decrypted) NCA from 0x4000, read by offset in it.
class Plaintext {
public:
    virtual ~Plaintext() = default;
    // Fills [offset, offset + size); false when it cannot.
    virtual bool Read(u8* data, std::size_t size, u64 offset) = 0;
};

struct DCtxFree {
    void operator()(ZSTD_DCtx* context) const { ZSTD_freeDCtx(context); }
};
using DCtxPtr = std::unique_ptr<ZSTD_DCtx, DCtxFree>;

// Block NCZ: each block on its own, the last few kept.
class BlockPlaintext final : public Plaintext {
public:
    BlockPlaintext(const Layout& layout_, ReadFn read_)
        : layout(layout_), read(std::move(read_)), context(ZSTD_createDCtx()),
          kept(std::clamp<std::size_t>(static_cast<std::size_t>((8ull << 20) / layout.BlockSize()), 1, 4)) {}

    bool Read(u8* data, std::size_t size, u64 offset) override {
        const u64 block_size = layout.BlockSize();
        while (size > 0) {
            const u64 index = offset / block_size;
            const std::vector<u8>* block = Block(index);
            if (!block) return false;
            const u64 within = offset - index * block_size;
            if (within >= block->size()) return false;
            const std::size_t part = static_cast<std::size_t>(std::min<u64>(size, block->size() - within));
            std::memcpy(data, block->data() + within, part);
            data += part;
            offset += part;
            size -= part;
        }
        return true;
    }

private:
    struct Cached {
        u64 index;
        std::vector<u8> data;
    };

    const std::vector<u8>* Block(u64 index) {
        if (index >= layout.block_sizes.size() || !context) return nullptr;
        for (std::size_t i = 0; i < cache.size(); ++i)
            if (cache[i].index == index) {
                std::rotate(cache.begin(), cache.begin() + i, cache.begin() + i + 1);
                return &cache.front().data;
            }
        const u64 block_size = layout.BlockSize();
        const u64 expected = std::min(block_size, layout.StreamSize() - index * block_size);
        const u32 stored = layout.block_sizes[index];
        std::vector<u8> out(static_cast<std::size_t>(expected));
        if (stored < expected) {
            compressed.resize(stored);
            if (!Detail::ReadExact(read, compressed.data(), stored, layout.block_offsets[index])) return nullptr;
            const std::size_t made = ZSTD_decompressDCtx(context.get(), out.data(), out.size(),
                                                         compressed.data(), compressed.size());
            if (ZSTD_isError(made) || made != out.size()) return nullptr;
        } else if (!Detail::ReadExact(read, out.data(), out.size(), layout.block_offsets[index])) {
            return nullptr;
        }
        if (cache.size() == kept) cache.pop_back();
        cache.insert(cache.begin(), Cached{index, std::move(out)});
        return &cache.front().data;
    }

    const Layout& layout;
    ReadFn read;
    DCtxPtr context;
    std::size_t kept;
    std::vector<Cached> cache;
    std::vector<u8> compressed;
};

// A solid NCZ's zstd stream, decompressed from its start onwards, a piece at a time.
class SolidStream {
public:
    SolidStream(const Layout& layout_, const ReadFn& read_)
        : layout(layout_), read(read_), context(ZSTD_createDCtx()), in(ZSTD_DStreamInSize()),
          out(ZSTD_DStreamOutSize()), source(layout.data_offset) {}

    u64 Produced() const { return produced; }

    // Calls `sink` with what follows, in order, until `until` bytes have been produced in all.
    bool Next(u64 until, const std::function<bool(const u8* data, std::size_t size)>& sink) {
        if (!context) return false;
        until = std::min(until, layout.StreamSize());
        while (produced < until) {
            if (input.pos == input.size) {
                const std::size_t got = read(in.data(), in.size(), source);
                if (got == 0) return false;  // the stream ends before the NCA does
                source += got;
                input = {in.data(), got, 0};
            }
            ZSTD_outBuffer output{out.data(), out.size(), 0};
            if (ZSTD_isError(ZSTD_decompressStream(context.get(), &output, &input))) return false;
            if (output.pos == 0) continue;
            // A stream longer than the NCA: what is past its end is not the NCA's.
            const std::size_t part = static_cast<std::size_t>(std::min<u64>(output.pos, layout.StreamSize() - produced));
            if (part == 0) return false;
            // The piece may run past `until`: the caller keeps what it is given.
            if (!sink(out.data(), part)) return false;
            produced += part;
        }
        return true;
    }

private:
    const Layout& layout;
    const ReadFn& read;
    DCtxPtr context;
    std::vector<u8> in, out;
    u64 source, produced = 0;
    ZSTD_inBuffer input{nullptr, 0, 0};
};

// A solid NCZ from its start to its end, a piece at a time.
inline bool DecompressSolid(const Layout& layout, const ReadFn& read,
                            const std::function<bool(const u8* data, std::size_t size)>& sink) {
    SolidStream stream(layout, read);
    return stream.Next(layout.StreamSize(), sink) && stream.Produced() == layout.StreamSize();
}

// The whole plaintext of a solid NCZ, unpacked (UnpackFn: UnpackToFile, through the frontend).
using UnpackFn = std::function<std::unique_ptr<Plaintext>(const Layout&, const ReadFn&)>;

namespace Detail {
inline void PutLe64(u8* p, u64 value) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<u8>(value >> (8 * i));
}
inline u64 Fnv64(const u8* data, std::size_t size, u64 hash = 1469598103934665603ull) {
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}
// Writes what is buffered and makes it durable.
inline bool Sync(std::FILE* file) {
    return std::fflush(file) == 0 && fsync(fileno(file)) == 0;
}
inline void SyncFolder(const std::string& path) {
    const auto slash = path.find_last_of('/');
    const int folder = open(slash == std::string::npos ? "." : path.substr(0, slash).c_str(), O_RDONLY);
    if (folder >= 0) {
        (void)fsync(folder);
        close(folder);
    }
}
} // namespace Detail

// An unpacked file ends with this and the plaintext's size: what was cut short (or lost in a power
// cut before it was written out) does not have it.
inline constexpr char kUnpackedMark[8] = {'E', 'D', 'E', 'N', 'N', 'C', 'Z', '1'};

// The plaintext of a solid NCZ unpacked to a file (UnpackToFile).
class FilePlaintext final : public Plaintext {
public:
    FilePlaintext(std::FILE* file_, u64 size_) : file(file_), size(size_) {}
    ~FilePlaintext() override { std::fclose(file); }
    bool Read(u8* data, std::size_t length, u64 offset) override {
        if (offset > size || length > size - offset) return false;
        if (fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
        return std::fread(data, 1, length, file) == length;
    }

private:
    std::FILE* file;
    u64 size;
};

// An unpacked file made before, when it is there whole: of the plaintext's size and marked so.
inline std::unique_ptr<Plaintext> OpenUnpacked(const std::string& path, u64 size) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return nullptr;
    u8 mark[16];
    const bool whole = fseeko(file, 0, SEEK_END) == 0 && static_cast<u64>(ftello(file)) == size + sizeof(mark) &&
                       fseeko(file, static_cast<off_t>(size), SEEK_SET) == 0 &&
                       std::fread(mark, 1, sizeof(mark), file) == sizeof(mark) &&
                       std::memcmp(mark, kUnpackedMark, 8) == 0 && Detail::Le64(mark + 8) == size;
    if (!whole) {
        std::fclose(file);
        return nullptr;
    }
    return std::make_unique<FilePlaintext>(file, size);
}

// Unpacks a solid NCZ's plaintext to `path`: written as `path`.part, made durable and marked, and
// given its name only once it is whole. `progress` hears how much is done; `stop` ends it early.
inline std::unique_ptr<Plaintext> UnpackToFile(const Layout& layout, const ReadFn& read,
                                               const std::string& path, std::string* error,
                                               const std::function<void(u64 done, u64 total)>& progress = {},
                                               const std::function<bool()>& stop = {}) {
    const std::string part = path + ".part";
    std::FILE* out = std::fopen(part.c_str(), "wb");
    if (!out) {
        if (error) *error = "cannot create " + part;
        return nullptr;
    }
    u64 written = 0, reported = 0;
    bool write_failed = false, stopped = false;
    bool done = DecompressSolid(layout, read, [&](const u8* piece, std::size_t size) {
        if (stop && stop()) {
            stopped = true;
            return false;
        }
        if (std::fwrite(piece, 1, size, out) != size) {
            write_failed = true;
            return false;
        }
        written += size;
        if (progress && written - reported >= (256ull << 20)) {
            reported = written;
            progress(written, layout.StreamSize());
        }
        return true;
    });
    if (done) {
        u8 mark[16];
        std::memcpy(mark, kUnpackedMark, 8);
        Detail::PutLe64(mark + 8, layout.StreamSize());
        write_failed = std::fwrite(mark, 1, sizeof(mark), out) != sizeof(mark) || !Detail::Sync(out);
        done = !write_failed;
    }
    const bool closed = std::fclose(out) == 0;
    if (!done || !closed || written != layout.StreamSize() || std::rename(part.c_str(), path.c_str()) != 0) {
        std::remove(part.c_str());
        if (error)
            *error = stopped               ? "stopped: the game was left"
                     : write_failed || done ? "cannot write " + part + " (is the console's storage full?)"
                                            : "its zstd stream is damaged or cut short";
        return nullptr;
    }
    Detail::SyncFolder(path);
    if (progress) progress(written, layout.StreamSize());
    return OpenUnpacked(path, layout.StreamSize());
}

// Pieces of a solid NCZ's plaintext a read found by decompressing up to it (SolidPlaintext), for
// every reader of that NCZ: those far into it in a file of their own when one is given, which
// later starts read again; the rest in memory (the most recently used, up to kMemoryLimit). One
// object per file: two writing one file would write over each other's pieces (ncz.cpp).
class SolidPieces {
public:
    static constexpr u64 kMemoryLimit = 32ull << 20;  // in memory
    static constexpr u64 kFileLimit = 16ull << 20;    // in its file: past it, new ones stay in memory
    static constexpr std::size_t kRecordHead = 32;    // "NCZPIECE", offset, size, FNV-1a of the data

    explicit SolidPieces(std::string path_ = {}) : path(std::move(path_)) { Load(); }
    ~SolidPieces() {
        if (file) std::fclose(file);
    }
    SolidPieces(const SolidPieces&) = delete;
    SolidPieces& operator=(const SolidPieces&) = delete;

    bool Find(u8* data, std::size_t size, u64 offset) {
        std::scoped_lock lock{guard};
        for (auto it = pieces.begin(); it != pieces.end(); ++it) {
            if (offset < it->offset || offset + size > it->offset + it->size) continue;
            if (!it->in_file) {
                std::memcpy(data, it->data.data() + (offset - it->offset), size);
                pieces.splice(pieces.end(), pieces, it);  // the most recently used last
                return true;
            }
            if (!it->checked && !Check(*it)) {
                pieces.erase(it);  // damaged on the storage: found again by decompressing
                return false;
            }
            return fseeko(file, static_cast<off_t>(it->at + (offset - it->offset)), SEEK_SET) == 0 &&
                   std::fread(data, 1, size, file) == size;
        }
        return false;
    }

    // `keep`: also in the file (a piece that is slow to find again), else in memory only.
    void Add(u64 offset, std::vector<u8> piece, bool keep = true) {
        std::scoped_lock lock{guard};
        if (keep && file && writable && end + kRecordHead + piece.size() <= kFileLimit) {
            u8 head[kRecordHead];
            std::memcpy(head, "NCZPIECE", 8);
            Detail::PutLe64(head + 8, offset);
            Detail::PutLe64(head + 16, piece.size());
            Detail::PutLe64(head + 24, Hash(offset, piece));
            if (fseeko(file, static_cast<off_t>(end), SEEK_SET) == 0 &&
                std::fwrite(head, 1, sizeof(head), file) == sizeof(head) &&
                std::fwrite(piece.data(), 1, piece.size(), file) == piece.size() && Detail::Sync(file)) {
                pieces.push_back({offset, piece.size(), end + kRecordHead, true, true, 0, {}});
                end += kRecordHead + piece.size();
                return;
            }
            // A file that cannot take more: what it holds stays, the rest is kept in memory.
            writable = false;
        }
        for (auto it = pieces.begin(); it != pieces.end();)
            it = !it->in_file && it->offset == offset ? (in_memory -= it->size, pieces.erase(it)) : std::next(it);
        in_memory += piece.size();
        pieces.push_back({offset, piece.size(), 0, false, true, 0, std::move(piece)});
        while (in_memory > kMemoryLimit) {
            auto oldest = std::find_if(pieces.begin(), pieces.end(), [](const Piece& p) { return !p.in_file; });
            if (oldest == std::prev(pieces.end())) break;  // never the one just added
            in_memory -= oldest->size;
            pieces.erase(oldest);
        }
    }

    // Held while one reader decompresses: another that wants the same then finds it here.
    std::mutex& Pass() { return pass; }

private:
    struct Piece {
        u64 offset, size, at;
        bool in_file, checked;
        u64 hash;
        std::vector<u8> data;
    };

    // A piece's checksum: FNV-1a of where it is and its data.
    static u64 Hash(u64 offset, const std::vector<u8>& data) {
        u8 where[16];
        Detail::PutLe64(where, offset);
        Detail::PutLe64(where + 8, data.size());
        return Detail::Fnv64(data.data(), data.size(), Detail::Fnv64(where, sizeof(where)));
    }

    // Whether a piece in the file still has the data it was written with (read once, on first use).
    bool Check(Piece& piece) {
        std::vector<u8> data(static_cast<std::size_t>(piece.size));
        piece.checked = fseeko(file, static_cast<off_t>(piece.at), SEEK_SET) == 0 &&
                        std::fread(data.data(), 1, data.size(), file) == data.size() &&
                        Hash(piece.offset, data) == piece.hash;
        return piece.checked;
    }

    // Reads where the file's pieces are (their data is checked when first used); one cut short
    // ends it there, and the next piece is written over it.
    void Load() {
        if (path.empty()) return;
        file = std::fopen(path.c_str(), "r+b");
        if (!file) {
            file = std::fopen(path.c_str(), "w+b");
            return;
        }
        if (fseeko(file, 0, SEEK_END) != 0) return;
        const u64 length = static_cast<u64>(ftello(file));
        u8 head[kRecordHead];
        while (end + kRecordHead <= length && fseeko(file, static_cast<off_t>(end), SEEK_SET) == 0 &&
               std::fread(head, 1, sizeof(head), file) == sizeof(head) && std::memcmp(head, "NCZPIECE", 8) == 0) {
            const u64 size = Detail::Le64(head + 16);
            if (size == 0 || size > kFileLimit || size > length - end - kRecordHead) break;
            pieces.push_back({Detail::Le64(head + 8), size, end + kRecordHead, true, false, Detail::Le64(head + 24), {}});
            end += kRecordHead + size;
        }
    }

    std::string path;
    std::FILE* file = nullptr;
    u64 end = 0, in_memory = 0;
    bool writable = true;
    std::list<Piece> pieces;
    std::mutex guard, pass;
};

struct SolidOptions {
    std::function<std::unique_ptr<Plaintext>()> unpacked;  // the copy unpacked before, if there is one
    UnpackFn unpack;                                        // unpacks it now
    std::function<bool()> may_unpack;                       // whether it may be unpacked now; empty: yes
    std::shared_ptr<SolidPieces> pieces;                    // empty: its own, in memory
};

class SolidPlaintext final : public Plaintext {
public:
    // What one decompression keeps: from a MiB before the read to 4 MiB after it (an update's two
    // patch tables lie together at the end of its data).
    static constexpr u64 kWindowBefore = 1ull << 20, kWindowAfter = 4ull << 20;
    static constexpr u64 kLargestPiece = kSmallSolid;

    SolidPlaintext(const Layout& layout_, ReadFn read_, SolidOptions options_)
        : layout(layout_), read(std::move(read_)), options(std::move(options_)) {
        if (!options.pieces) options.pieces = std::make_shared<SolidPieces>();
    }

    bool Read(u8* data, std::size_t size, u64 offset) override {
        if (whole) return whole->Read(data, size, offset);
        if (!looked) {
            looked = true;
            if (options.unpacked) whole = options.unpacked();
            if (whole) return whole->Read(data, size, offset);
        }
        if (options.pieces->Find(data, size, offset)) return true;
        if (!unpack_failed && options.unpack && layout.StreamSize() > kSmallSolid &&
            (!options.may_unpack || options.may_unpack())) {
            whole = options.unpack(layout, read);
            if (whole) return whole->Read(data, size, offset);
            unpack_failed = true;  // read as outside a game from now on
        }
        return ReadThroughStream(data, size, offset);
    }

private:
    // Decompresses from the start to past the read, keeping the window around it.
    bool ReadThroughStream(u8* data, std::size_t size, u64 offset) {
        std::scoped_lock pass{options.pieces->Pass()};
        if (options.pieces->Find(data, size, offset)) return true;
        const u64 from = offset > kWindowBefore ? offset - kWindowBefore : 0;
        const u64 to = std::min(layout.StreamSize(),
                                std::max({offset + size, offset + kWindowAfter, std::min(kSmallSolid, layout.StreamSize())}));
        if (offset + size > to) return false;
        std::vector<u8> window;
        window.reserve(static_cast<std::size_t>(to - from));
        u64 at = 0;
        SolidStream stream(layout, read);
        const bool done = stream.Next(to, [&](const u8* piece, std::size_t part) {
            const u64 piece_end = at + part;
            if (piece_end > from && at < to) {
                const u64 keep_from = std::max(at, from), keep_to = std::min(piece_end, to);
                window.insert(window.end(), piece + (keep_from - at), piece + (keep_to - at));
            }
            at = piece_end;
            return true;
        });
        if (!done || window.size() != to - from) return false;
        std::memcpy(data, window.data() + (offset - from), size);
        // A larger read (of much of the NCZ at once) is not kept: the pieces are for small reads.
        // Only those far into it go to the file: the start decompresses again in moments.
        if (window.size() <= kLargestPiece) options.pieces->Add(from, std::move(window), from >= kSmallSolid);
        return true;
    }

    const Layout& layout;
    ReadFn read;
    SolidOptions options;
    std::unique_ptr<Plaintext> whole;
    bool looked = false, unpack_failed = false;
};

// Encrypts `size` bytes in place for `section`, as AES-128-CTR; `offset` is the NCA offset of
// data[0] and a multiple of 16.
using EncryptFn = std::function<void(std::size_t section, u64 offset, u8* data, std::size_t size)>;

// The counter block for an NCA offset in a section (nsz's AESCTR): its counter's first 8 bytes,
// then offset / 16, big endian.
inline std::array<u8, 16> CounterFor(const Section& section, u64 offset) {
    std::array<u8, 16> iv{};
    std::memcpy(iv.data(), section.counter.data(), 8);
    u64 block = offset >> 4;
    for (int i = 15; i >= 8; --i) {
        iv[i] = static_cast<u8>(block);
        block >>= 8;
    }
    return iv;
}

// The NCA an NCZ was made from. Not thread safe: its owner serialises reads.
class Reader {
public:
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Layout layout_, ReadFn read_, EncryptFn encrypt_, SolidOptions solid = {})
        : layout(std::move(layout_)), read(std::move(read_)), encrypt(std::move(encrypt_)) {
        if (layout.block)
            plaintext = std::make_unique<BlockPlaintext>(layout, read);
        else
            plaintext = std::make_unique<SolidPlaintext>(layout, read, std::move(solid));
    }

    const Layout& GetLayout() const { return layout; }
    u64 Size() const { return layout.nca_size; }

    std::size_t Read(u8* data, std::size_t size, u64 offset) {
        if (offset >= layout.nca_size) return 0;
        size = static_cast<std::size_t>(std::min<u64>(size, layout.nca_size - offset));
        std::size_t done = 0;
        if (offset < kHeaderSize) {
            const std::size_t part = static_cast<std::size_t>(std::min<u64>(size, kHeaderSize - offset));
            if (!Detail::ReadExact(read, data, part, offset)) return 0;
            done = part;
        }
        if (done == size) return done;
        const u64 from = offset + done;
        if (!plaintext->Read(data + done, size - done, from - kHeaderSize)) return done;
        Encrypt(data + done, size - done, from);
        return size;
    }

private:
    // Encrypts the plaintext of [offset, offset + size) where its sections are encrypted.
    void Encrypt(u8* data, std::size_t size, u64 offset) {
        const u64 end = offset + size;
        auto section = std::upper_bound(layout.sections.begin(), layout.sections.end(), offset,
                                        [](u64 at, const Section& s) { return at < s.End(); });
        for (; section != layout.sections.end() && section->offset < end; ++section) {
            if (!section->Encrypted()) continue;
            u64 from = std::max(offset, section->offset);
            const u64 to = std::min(end, section->End());
            const std::size_t index = static_cast<std::size_t>(section - layout.sections.begin());
            if (const u64 within = from & 15; within != 0 && from < to) {
                std::array<u8, 16> block{};
                const std::size_t part = static_cast<std::size_t>(std::min<u64>(16 - within, to - from));
                std::memcpy(block.data() + within, data + (from - offset), part);
                encrypt(index, from - within, block.data(), block.size());
                std::memcpy(data + (from - offset), block.data() + within, part);
                from += part;
            }
            if (from < to) encrypt(index, from, data + (from - offset), static_cast<std::size_t>(to - from));
        }
    }

    Layout layout;
    ReadFn read;
    EncryptFn encrypt;
    std::unique_ptr<Plaintext> plaintext;
};
} // namespace Eden::Ncz
