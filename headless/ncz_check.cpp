// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of headless/ncz.h (tools/check-ncz.py): NCAs made up here, packed as nsz packs them
// (block and solid), must read back byte for byte, wherever and however much is read.
#include "ncz.h"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <zstd.h>

using namespace Eden::Ncz;

namespace {
int failures = 0;
void Expect(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

std::mt19937_64 rng(20261020);

std::vector<u8> Random(std::size_t size) {
    std::vector<u8> out(size);
    for (auto& b : out) b = static_cast<u8>(rng());
    return out;
}

// AES-128 of one block (ECB): the reference keystream is built from it by hand, not by CTR mode.
std::array<u8, 16> AesBlock(const std::array<u8, 16>& key, const std::array<u8, 16>& in) {
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    std::array<u8, 16> out{};
    int length = 0;
    assert(EVP_EncryptInit_ex(context, EVP_aes_128_ecb(), nullptr, key.data(), nullptr) == 1);
    EVP_CIPHER_CTX_set_padding(context, 0);
    assert(EVP_EncryptUpdate(context, out.data(), &length, in.data(), 16) == 1 && length == 16);
    EVP_CIPHER_CTX_free(context);
    return out;
}

struct Spec {
    u64 offset, size, type;
};

struct Made {
    std::vector<u8> nca;        // what must be read back
    std::vector<u8> plaintext;  // the NCA from 0x4000, decrypted
    std::vector<Section> sections;
};

// An NCA of these sections: a random header, data partly random and partly runs of one byte (so
// some blocks compress and some do not), sections of type 3 and 4 encrypted as an NCA's are.
Made MakeNca(const std::vector<Spec>& specs) {
    Made made;
    const u64 end = specs.back().offset + specs.back().size;
    made.nca = Random(static_cast<std::size_t>(kHeaderSize));
    made.plaintext.resize(static_cast<std::size_t>(end - kHeaderSize));
    for (std::size_t at = 0; at < made.plaintext.size();) {
        const std::size_t run = std::min<std::size_t>(made.plaintext.size() - at, 1 + rng() % 60000);
        const bool random = rng() % 2;
        const u8 fill = static_cast<u8>(rng());
        for (std::size_t i = 0; i < run; ++i) made.plaintext[at + i] = random ? static_cast<u8>(rng()) : fill;
        at += run;
    }
    for (const Spec& spec : specs) {
        Section section;
        section.offset = spec.offset;
        section.size = spec.size;
        section.crypto_type = spec.type;
        const auto key = Random(16), counter = Random(16);
        std::copy(key.begin(), key.end(), section.key.begin());
        std::copy(counter.begin(), counter.end(), section.counter.begin());
        made.sections.push_back(section);
    }
    made.nca.resize(static_cast<std::size_t>(end));
    for (u64 x = kHeaderSize; x < end; ++x) {
        u8 byte = made.plaintext[x - kHeaderSize];
        for (const Section& s : made.sections) {
            if (x < s.offset || x >= s.End() || !s.Encrypted()) continue;
            std::array<u8, 16> block{};
            std::copy(s.counter.begin(), s.counter.begin() + 8, block.begin());
            const u64 index = x / 16;
            for (int i = 0; i < 8; ++i) block[15 - i] = static_cast<u8>(index >> (8 * i));
            static std::array<u8, 16> last_in{}, last_out{};
            static const Section* last_section = nullptr;
            if (last_section != &s || last_in != block) {
                last_out = AesBlock(s.key, block);
                last_in = block;
                last_section = &s;
            }
            byte ^= last_out[x % 16];
        }
        made.nca[x] = byte;
    }
    return made;
}

void Put64(std::vector<u8>& out, u64 value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(value >> (8 * i)));
}
void Put32(std::vector<u8>& out, u32 value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(value >> (8 * i)));
}

std::vector<u8> SectionTable(const Made& made) {
    std::vector<u8> out(made.nca.begin(), made.nca.begin() + kHeaderSize);
    out.insert(out.end(), {'N', 'C', 'Z', 'S', 'E', 'C', 'T', 'N'});
    Put64(out, made.sections.size());
    for (const Section& s : made.sections) {
        Put64(out, s.offset);
        Put64(out, s.size);
        Put64(out, s.crypto_type);
        Put64(out, 0);
        out.insert(out.end(), s.key.begin(), s.key.end());
        out.insert(out.end(), s.counter.begin(), s.counter.end());
    }
    return out;
}

// As nsz -B: a block that does not compress smaller is stored as it is.
std::vector<u8> BlockNcz(const Made& made, unsigned exponent, unsigned* raw_blocks = nullptr) {
    std::vector<u8> out = SectionTable(made);
    const u64 block = u64{1} << exponent, total = made.plaintext.size();
    const u32 blocks = static_cast<u32>((total + block - 1) / block);
    out.insert(out.end(), {'N', 'C', 'Z', 'B', 'L', 'O', 'C', 'K', 2, 1, 0, static_cast<u8>(exponent)});
    Put32(out, blocks);
    Put64(out, total);
    std::vector<std::vector<u8>> data;
    for (u32 i = 0; i < blocks; ++i) {
        const u8* from = made.plaintext.data() + i * block;
        const std::size_t size = static_cast<std::size_t>(std::min(block, total - i * block));
        std::vector<u8> packed(ZSTD_compressBound(size));
        const std::size_t made_size = ZSTD_compress(packed.data(), packed.size(), from, size, 3);
        assert(!ZSTD_isError(made_size));
        if (made_size < size) {
            packed.resize(made_size);
        } else {
            packed.assign(from, from + size);
            if (raw_blocks) ++*raw_blocks;
        }
        Put32(out, static_cast<u32>(packed.size()));
        data.push_back(std::move(packed));
    }
    for (const auto& d : data) out.insert(out.end(), d.begin(), d.end());
    return out;
}

// As nsz -S: a zstd stream (written in pieces, as a streaming compressor does). `frames` > 1 cuts
// it into that many frames one after the other, and `trailing` bytes follow it.
std::vector<u8> SolidNcz(const Made& made, int frames = 1, std::size_t trailing = 0) {
    std::vector<u8> out = SectionTable(made);
    ZSTD_CCtx* context = ZSTD_createCCtx();
    std::vector<u8> buffer(ZSTD_CStreamOutSize());
    const std::size_t total = made.plaintext.size();
    for (int frame = 0; frame < frames; ++frame) {
        const std::size_t from = total * frame / frames, to = total * (frame + 1) / frames;
        ZSTD_inBuffer input{made.plaintext.data() + from, to - from, 0};
        for (;;) {
            ZSTD_outBuffer output{buffer.data(), buffer.size(), 0};
            const std::size_t left = ZSTD_compressStream2(context, &output, &input, ZSTD_e_end);
            assert(!ZSTD_isError(left));
            out.insert(out.end(), buffer.data(), buffer.data() + output.pos);
            if (left == 0) break;
        }
    }
    ZSTD_freeCCtx(context);
    const auto extra = Random(trailing);
    out.insert(out.end(), extra.begin(), extra.end());
    return out;
}

// NCZ_CHECK_EXPORT=<folder>: each case's NCZ and the NCA it must read as, for checking them
// against nsz's own decompressor.
void Export(const std::string& name, const std::vector<u8>& ncz, const std::vector<u8>& nca) {
    const char* folder = std::getenv("NCZ_CHECK_EXPORT");
    if (!folder) return;
    std::string file;
    for (const char c : name) file += std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
    for (const auto& [suffix, data] : {std::pair{".ncz", &ncz}, std::pair{".nca", &nca}}) {
        std::FILE* out = std::fopen((std::string(folder) + "/" + file + suffix).c_str(), "wb");
        assert(out && std::fwrite(data->data(), 1, data->size(), out) == data->size());
        std::fclose(out);
    }
}

ReadFn Over(const std::vector<u8>& file) {
    return [&file](u8* data, std::size_t size, u64 offset) -> std::size_t {
        if (offset >= file.size()) return 0;
        const std::size_t part = static_cast<std::size_t>(std::min<u64>(size, file.size() - offset));
        // Never more than 100 KiB at once: callers must not assume one read is enough.
        const std::size_t given = std::min<std::size_t>(part, 100000);
        std::memcpy(data, file.data() + offset, given);
        return given;
    };
}

// What Eden's AESCipher does in ncz.cpp: AES-128-CTR from CounterFor.
EncryptFn Ctr(const Layout& layout) {
    return [&layout](std::size_t index, u64 offset, u8* data, std::size_t size) {
        assert(offset % 16 == 0);
        const Section& section = layout.sections[index];
        const auto iv = CounterFor(section, offset);
        EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
        int length = 0;
        assert(EVP_EncryptInit_ex(context, EVP_aes_128_ctr(), nullptr, section.key.data(), iv.data()) == 1);
        assert(EVP_EncryptUpdate(context, data, &length, data, static_cast<int>(size)) == 1);
        assert(static_cast<std::size_t>(length) == size);
        EVP_CIPHER_CTX_free(context);
    };
}

// Reads `reader` whole in pieces, then at random places and lengths, against `expected`.
void ReadsBack(Reader& reader, const std::vector<u8>& expected, const std::string& name, int random_reads = 1500) {
    Expect(reader.Size() == expected.size(), name + ": size");
    std::vector<u8> all(expected.size());
    for (std::size_t at = 0; at < all.size();) {
        const std::size_t want = std::min<std::size_t>(all.size() - at, 1 + rng() % 200000);
        const std::size_t got = reader.Read(all.data() + at, want, at);
        Expect(got == want, name + ": sequential read short at " + std::to_string(at));
        if (got != want) return;
        at += got;
    }
    Expect(all == expected, name + ": sequential bytes");
    for (int i = 0; i < random_reads; ++i) {
        u64 offset = rng() % expected.size();
        if (i % 7 == 0) offset = kHeaderSize - 1 - rng() % 40;  // across the header's end
        const std::size_t want = 1 + static_cast<std::size_t>(rng() % 70000);
        std::vector<u8> piece(want);
        const std::size_t got = reader.Read(piece.data(), want, offset);
        const std::size_t should = static_cast<std::size_t>(std::min<u64>(want, expected.size() - offset));
        Expect(got == should, name + ": random read size at " + std::to_string(offset));
        Expect(std::equal(piece.begin(), piece.begin() + should, expected.begin() + offset),
               name + ": random read bytes at " + std::to_string(offset));
        if (failures > 20) return;
    }
    u8 byte = 0;
    Expect(reader.Read(&byte, 1, expected.size()) == 0, name + ": nothing past the end");
}

void Block(const std::vector<Spec>& specs, unsigned exponent, const std::string& name) {
    const Made made = MakeNca(specs);
    unsigned raw = 0;
    const std::vector<u8> file = BlockNcz(made, exponent, &raw);
    Export(name, file, made.nca);
    std::string error;
    auto layout = ParseLayout(Over(file), file.size(), &error);
    Expect(layout.has_value(), name + ": parses (" + error + ")");
    if (!layout) return;
    Expect(layout->block && layout->block_exponent == exponent, name + ": is a block NCZ");
    Reader reader(*layout, Over(file), {});
    Reader encrypted(*layout, Over(file), Ctr(reader.GetLayout()));
    ReadsBack(encrypted, made.nca, name);
    std::printf("  %s: %zu bytes, %zu blocks (%u stored as they are), %zu sections\n", name.c_str(),
                made.nca.size(), layout->block_sizes.size(), raw, layout->sections.size());
}

std::string CheckPath(const std::string& name) { return std::string(std::getenv("NCZ_CHECK_DIR")) + "/" + name; }

bool Exists(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file) std::fclose(file);
    return file != nullptr;
}

// A solid NCZ in a game: read whole and at random places; one larger than kSmallSolid is unpacked
// once, and the unpacked copy is found again by the next reader.
void Solid(const std::vector<Spec>& specs, const std::string& name, bool expect_unpack, int frames = 1,
           std::size_t trailing = 0) {
    const Made made = MakeNca(specs);
    const std::vector<u8> file = SolidNcz(made, frames, trailing);
    Export(name, file, made.nca);
    std::string error;
    auto layout = ParseLayout(Over(file), file.size(), &error);
    Expect(layout.has_value() && !layout->block, name + ": parses as solid (" + error + ")");
    if (!layout) return;
    const std::string path = CheckPath(name + ".unpacked");
    int unpacks = 0;
    std::vector<std::pair<u64, u64>> progress;
    const auto options = [&] {
        return SolidOptions{[&] { return OpenUnpacked(path, layout->StreamSize()); },
                            [&](const Layout& l, const ReadFn& read) {
                                ++unpacks;
                                return UnpackToFile(l, read, path, &error, [&](u64 done, u64 total) {
                                    progress.emplace_back(done, total);
                                });
                            },
                            {}, {}};
    };
    {
        Reader encrypted(*layout, Over(file), Ctr(*layout), options());
        ReadsBack(encrypted, made.nca, name, 400);
        Expect(unpacks == (expect_unpack ? 1 : 0), name + ": unpacked " + std::to_string(unpacks) + " times");
    }
    if (expect_unpack) {
        Expect(!progress.empty() && progress.back().first == progress.back().second, name + ": progress reaches the end");
        Expect(!Exists(path + ".part"), name + ": no .part is left");
        Reader again(*layout, Over(file), Ctr(*layout), options());
        std::vector<u8> piece(4096);
        const u64 deep = made.nca.size() - 5000;
        Expect(again.Read(piece.data(), piece.size(), deep) == 4096 &&
                   std::equal(piece.begin(), piece.end(), made.nca.begin() + deep),
               name + ": the next reader reads the unpacked copy");
        Expect(unpacks == 1, name + ": the unpacked copy is not made again");
        Expect(OpenUnpacked(path, layout->StreamSize() + 1) == nullptr, name + ": a copy of another size is not used");
        std::remove(path.c_str());
    }
    std::printf("  %s: %zu bytes solid (%d frame(s), %zu trailing bytes), unpacked %d time(s)\n", name.c_str(),
                made.nca.size(), frames, trailing, unpacks);
}

// A solid NCZ outside a game (a library scan opening an update, or a game at its start): reads are
// found by decompressing up to them, the window around them serves the next ones, for every reader
// of the NCZ and, through the pieces file, for the next start of the app; nothing is unpacked. Once
// a game runs, a read unpacks it, and outside a game again the unpacked copy is read.
void OutsideAGame() {
    const Made made = MakeNca({{kHeaderSize, 30u << 20, 3}, {kHeaderSize + (30u << 20), 12u << 20, 4}});
    const std::vector<u8> file = SolidNcz(made);
    std::string error;
    auto layout = ParseLayout(Over(file), file.size(), &error);
    assert(layout && !layout->block);
    u64 read_bytes = 0;
    const ReadFn counted = [&](u8* data, std::size_t size, u64 offset) {
        const std::size_t got = Over(file)(data, size, offset);
        read_bytes += got;
        return got;
    };
    bool in_game = false;
    int unpacks = 0;
    const std::string unpacked = CheckPath("outside.unpacked"), pieces_file = CheckPath("outside.pieces");
    auto pieces = std::make_shared<SolidPieces>(pieces_file);
    const auto options = [&](std::shared_ptr<SolidPieces> shared) {
        return SolidOptions{[&] { return OpenUnpacked(unpacked, layout->StreamSize()); },
                            [&](const Layout& l, const ReadFn& read) {
                                ++unpacks;
                                return UnpackToFile(l, read, unpacked, &error);
                            },
                            [&] { return in_game; }, std::move(shared)};
    };
    const auto check = [&](Reader& reader, u64 offset, std::size_t size, const std::string& what) {
        std::vector<u8> piece(size);
        const bool ok = reader.Read(piece.data(), size, offset) == size &&
                        std::equal(piece.begin(), piece.end(), made.nca.begin() + offset);
        Expect(ok, what);
    };
    // The "patch tables": 64 KiB near the end of the second section, then 16 KiB after them.
    const u64 tables = made.nca.size() - (3u << 20);
    {
        Reader scan(*layout, counted, Ctr(*layout), options(pieces));
        check(scan, 0, static_cast<std::size_t>(kHeaderSize + 4096), "outside a game: the header and the start");
        check(scan, tables, 65536, "outside a game: a read near the end");
        const u64 after_first = read_bytes;
        Expect(after_first >= file.size() * 9 / 10, "outside a game: the read near the end decompressed up to it");
        check(scan, tables + 70000, 16384, "outside a game: the next read in the window");
        Expect(read_bytes == after_first, "outside a game: the window serves the next read");
    }
    {
        const u64 before = read_bytes;
        Reader again(*layout, counted, Ctr(*layout), options(pieces));
        check(again, tables + 1000, 4096, "outside a game: another reader of the NCZ");
        Expect(read_bytes == before, "outside a game: another reader uses the window too");
    }
    pieces.reset();
    {
        // The next start of the app: the pieces file holds what was found.
        const u64 before = read_bytes;
        Reader next_start(*layout, counted, Ctr(*layout), options(std::make_shared<SolidPieces>(pieces_file)));
        check(next_start, tables + 2000, 4096, "next start: a read near the end");
        Expect(read_bytes - before < 0x4000 + 8192, "next start: found in the pieces file, not decompressed again");
        // The start is not kept in the file: it decompresses again, and no further than 16 MiB.
        const u64 before_start = read_bytes;
        check(next_start, kHeaderSize + 100, 4096, "next start: a read at the start");
        Expect(read_bytes - before_start < file.size() / 2, "next start: the start decompresses no further than it needs");
        check(next_start, kHeaderSize + (20u << 20), 4096, "next start: a read outside the windows");
        Expect(unpacks == 0, "outside a game: nothing is unpacked");
    }
    {
        // A pieces file damaged on the storage: what it holds is found again by decompressing.
        std::FILE* damaged = std::fopen(pieces_file.c_str(), "r+b");
        assert(damaged);
        fseeko(damaged, 100, SEEK_SET);
        const int byte = std::fgetc(damaged);
        fseeko(damaged, 100, SEEK_SET);
        std::fputc(byte ^ 0x40, damaged);
        std::fclose(damaged);
        Reader after_damage(*layout, counted, Ctr(*layout), options(std::make_shared<SolidPieces>(pieces_file)));
        check(after_damage, kHeaderSize + 50, 4096, "a damaged pieces file: still read right");
        check(after_damage, tables + 3000, 4096, "a damaged pieces file: other pieces still read right");
    }
    {
        // A piece whose place in the NCZ is damaged (its record's offset): not read at the wrong place.
        std::remove(pieces_file.c_str());
        {
            Reader fresh(*layout, counted, Ctr(*layout), options(std::make_shared<SolidPieces>(pieces_file)));
            check(fresh, tables, 4096, "a fresh pieces file");
        }
        std::FILE* damaged = std::fopen(pieces_file.c_str(), "r+b");
        assert(damaged);
        u8 head[16];
        assert(std::fread(head, 1, 16, damaged) == 16 && std::memcmp(head, "NCZPIECE", 8) == 0);
        u64 offset = 0;
        for (int i = 7; i >= 0; --i) offset = (offset << 8) | head[8 + i];
        const u64 moved = offset - (256u << 10);
        for (int i = 0; i < 8; ++i) head[8 + i] = static_cast<u8>(moved >> (8 * i));
        fseeko(damaged, 0, SEEK_SET);
        std::fwrite(head, 1, 16, damaged);
        std::fclose(damaged);
        Reader moved_piece(*layout, counted, Ctr(*layout), options(std::make_shared<SolidPieces>(pieces_file)));
        check(moved_piece, kHeaderSize + moved + 1000, 4096, "a piece with a damaged offset: read right");
    }
    {
        Reader game(*layout, counted, Ctr(*layout), options(std::make_shared<SolidPieces>(pieces_file)));
        in_game = true;
        check(game, kHeaderSize + (25u << 20), 4096, "in a game: a read");
        Expect(unpacks == 1, "in a game: it is unpacked once");
        check(game, kHeaderSize + (2u << 20), 100000, "in a game: then read from the unpacked copy");
    }
    {
        in_game = false;
        const u64 before = read_bytes;
        Reader after(*layout, counted, Ctr(*layout), options(std::make_shared<SolidPieces>(pieces_file)));
        check(after, kHeaderSize + (15u << 20), 4096, "outside a game again: a read anywhere");
        Expect(read_bytes - before < 0x4000 + 8192, "outside a game again: from the unpacked copy");
        Expect(unpacks == 1, "outside a game again: not unpacked again");
    }
    {
        // An unpack stopped (the game was left) leaves nothing, and says why.
        std::remove(unpacked.c_str());
        const std::string stopped = CheckPath("stopped.unpacked");
        int pieces_seen = 0;
        auto none = UnpackToFile(*layout, Over(file), stopped, &error, {}, [&] { return ++pieces_seen > 20; });
        Expect(!none && !Exists(stopped) && !Exists(stopped + ".part"), "a stopped unpack leaves no file");
        Expect(error.find("stopped") != std::string::npos, "a stopped unpack says so: " + error);
    }
    {
        // A copy without its mark (cut short by a power cut) is not used.
        std::FILE* out = std::fopen(unpacked.c_str(), "wb");
        std::vector<u8> zeros(static_cast<std::size_t>(layout->StreamSize()) + 16);
        std::fwrite(zeros.data(), 1, zeros.size(), out);
        std::fclose(out);
        Expect(OpenUnpacked(unpacked, layout->StreamSize()) == nullptr, "a copy without its mark is not used");
        std::remove(unpacked.c_str());
    }
    std::remove(pieces_file.c_str());
    std::puts("  outside a game: found without unpacking, shared and kept for the next start; unpacked once in a "
              "game and read from then on");
}

void Damaged() {
    const Made made = MakeNca({{kHeaderSize, 300000, 3}, {kHeaderSize + 300000, 200000, 1}});
    const std::vector<u8> good = BlockNcz(made, 14);
    std::string error;
    const auto rejects = [&](std::vector<u8> file, const std::string& what) {
        Expect(!ParseLayout(Over(file), file.size(), &error).has_value(), "rejects " + what);
        Expect(!error.empty(), what + " says why");
    };
    {
        auto file = good;
        file[kHeaderSize] = 'X';
        rejects(file, "a file without NCZSECTN");
    }
    {
        auto file = good;
        for (int i = 0; i < 8; ++i) file[kHeaderSize + 8 + i] = 0;
        rejects(file, "no sections");
    }
    {
        auto file = good;
        file[kHeaderSize + 16 + kSectionEntrySize] ^= 0x10;  // the second section's offset
        rejects(file, "sections with a gap");
    }
    {
        auto file = good;
        const std::size_t block_head = kHeaderSize + 16 + 2 * kSectionEntrySize;
        file[block_head + 11] = 13;
        rejects(file, "a block size under 2^14");
        file[block_head + 11] = 27;
        rejects(file, "a block size over 64 MiB");
        file = good;
        file[block_head + 12] ^= 1;
        rejects(file, "a wrong block count");
        file = good;
        file[block_head + 16] ^= 1;
        rejects(file, "a wrong decompressed size");
    }
    rejects(std::vector<u8>(good.begin(), good.begin() + kHeaderSize + 40), "a cut-short section table");
    rejects(std::vector<u8>(good.begin(), good.end() - 1), "blocks cut short");
    rejects(std::vector<u8>(100, 0), "a tiny file");
    {
        // A block whose zstd data is damaged: the header still reads, the data does not.
        auto file = good;
        auto layout = ParseLayout(Over(file), file.size(), &error);
        assert(layout);
        std::size_t compressed = 0;
        while (layout->block_sizes[compressed] >= layout->BlockSize()) ++compressed;
        for (u32 i = 0; i < layout->block_sizes[compressed]; ++i) file[layout->block_offsets[compressed] + i] ^= 0x5a;
        Reader reader(*layout, Over(file), Ctr(*layout));
        std::vector<u8> piece(100);
        Expect(reader.Read(piece.data(), 100, 0) == 100, "a damaged block leaves the header readable");
        const u64 inside = kHeaderSize + compressed * layout->BlockSize();
        Expect(reader.Read(piece.data(), 100, inside) == 0, "a damaged block reads as nothing");
    }
    {
        // A solid stream cut short: the start reads, past the cut nothing does (neither by unpacking
        // nor by decompressing), and no file stays.
        const Made big = MakeNca({{kHeaderSize, 40u << 20, 3}});
        auto file = SolidNcz(big);
        file.resize(file.size() / 2);
        auto layout = ParseLayout(Over(file), file.size(), &error);
        assert(layout && !layout->block);
        const std::string path = CheckPath("cut.unpacked");
        Reader reader(*layout, Over(file), Ctr(*layout),
                      SolidOptions{{}, [&](const Layout& l, const ReadFn& read) { return UnpackToFile(l, read, path, &error); },
                                   [] { return false; }, {}});
        std::vector<u8> piece(100);
        Expect(reader.Read(piece.data(), 100, kHeaderSize + 1000) == 100, "a cut-short stream reads at its start");
        Reader in_game(*layout, Over(file), Ctr(*layout),
                       SolidOptions{{}, [&](const Layout& l, const ReadFn& read) { return UnpackToFile(l, read, path, &error); },
                                    {}, {}});
        Expect(in_game.Read(piece.data(), 100, big.nca.size() - 200) == 0, "a cut-short stream reads nothing past the cut");
        Expect(error.find("damaged") != std::string::npos, "a cut-short stream says so: " + error);
        Expect(!Exists(path) && !Exists(path + ".part"), "a cut-short stream leaves no file");
    }
    {
        // An unpack that cannot write (the storage is full): it is tried once and says why, and the
        // NCZ is then read without unpacking, right.
        const Made big = MakeNca({{kHeaderSize, 40u << 20, 3}});
        const auto file = SolidNcz(big);
        auto layout = ParseLayout(Over(file), file.size(), &error);
        int tries = 0;
        Reader reader(*layout, Over(file), Ctr(*layout), SolidOptions{{}, [&](const Layout& l, const ReadFn& read) {
                                                               ++tries;
                                                               return UnpackToFile(l, read, "/nonexistent-ncz-folder/x", &error);
                                                           }, {}, {}});
        std::vector<u8> piece(100);
        for (const u64 at : {u64{big.nca.size() - 200}, kHeaderSize + 10, u64{big.nca.size() / 2}})
            Expect(reader.Read(piece.data(), 100, at) == 100 && std::equal(piece.begin(), piece.end(), big.nca.begin() + at),
                   "an unwritable unpack: read without it at " + std::to_string(at));
        Expect(tries == 1, "an unwritable unpack is tried once");
        Expect(error.find("cannot create") != std::string::npos, "an unwritable unpack says so: " + error);
    }
}
} // namespace

int main() {
    // The counter nsz's AESCTR uses: the section counter's first 8 bytes, then offset / 16.
    Section section;
    for (int i = 0; i < 16; ++i) section.counter[i] = static_cast<u8>(0xa0 + i);
    const auto iv = CounterFor(section, 0x123456789abcdef0ull);
    const std::array<u8, 16> expected_iv{0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
                                         0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    Expect(iv == expected_iv, "counter block");

    std::puts("Block NCZs:");
    Block({{kHeaderSize, 1u << 20, 3}}, 14, "one CTR section, whole blocks");
    Block({{kHeaderSize, 300001, 3}, {kHeaderSize + 300001, 123457, 1}, {kHeaderSize + 423458, 777777, 4}}, 14,
          "CTR, plain and BKTR sections, a short last block");
    Block({{0xC00, 0x4000 - 0xC00 + 500000, 3}, {0x4000 + 500000, 65536, 3}}, 16,
          "a first section that starts inside the header");
    Block({{0x4200, 400000, 3}}, 15, "data before the first section");
    Block({{kHeaderSize, 5000, 3}}, 20, "one block smaller than its block size");
    std::vector<Spec> many;
    for (u64 at = kHeaderSize, i = 0; i < 300; ++i) {
        const u64 size = 1 + rng() % 9000;
        many.push_back({at, size, i % 3 == 1 ? 1u : 3u});
        at += size;
    }
    Block(many, 14, "300 sections (a split BKTR)");

    std::puts("Solid NCZs:");
    Solid({{kHeaderSize, 3u << 20, 3}}, "solid-small", false);
    Solid({{kHeaderSize, 6u << 20, 3}, {kHeaderSize + (6u << 20), 6u << 20, 1}}, "solid-in-one-window", false, 3, 1000);
    Solid({{kHeaderSize, 20u << 20, 3}, {kHeaderSize + (20u << 20), 20u << 20, 4}}, "solid-unpacked", true);
    Solid({{kHeaderSize, 12u << 20, 3}, {kHeaderSize + (12u << 20), 12u << 20, 4}}, "solid-frames-unpacked", true, 4, 777);

    OutsideAGame();

    std::puts("Damaged files:");
    Damaged();

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("NCZ: PASS");
    return 0;
}
