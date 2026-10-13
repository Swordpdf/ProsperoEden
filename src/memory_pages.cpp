// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

#ifdef PS5_NATIVE
// Keep CPU heaps/tables and JIT views out of RADV's high-word-2 GPU window.
// This is a non-fixed hint: the kernel retains ownership of collision handling.
constexpr std::uintptr_t cpu_mapping_hint = 0x1000000000ull;
// Where the guest's own memory is mapped: above every address a guest can have (2^39, the end of
// the 39-bit address space of a 64-bit title). A guest page table entry is the page's host
// address less its guest address, and zero means "no pointer" (common/page_table.h). At the
// general hint the guest's memory sat at 67 to 71 GiB, inside the guest's address space: a game
// that mapped 2 MiB there at the matching offset got 512 entries of zero ("memory mapping base
// yield a nullptr within the table"), read zeros from that memory, jumped to address 0 and was
// restarted as a guest fault. A hint, as the other is: where the system places the block instead
// is reported (EDEN_GUEST_MEMORY).
constexpr std::uintptr_t guest_memory_hint = 0x8000000000ull;
static bool cpu_mapping_address(void* base) {
    return reinterpret_cast<std::uintptr_t>(base) >= 0x300000000ull && base != MAP_FAILED;
}
// A reserved range must also end below the driver's device memory at 0x40_0000_0000.
static bool cpu_mapping_range(void* base, std::size_t size) {
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    return cpu_mapping_address(base) && size <= 0x4000000000ull && start <= 0x4000000000ull - size;
}
extern "C" {
std::int64_t sceKernelGetDirectMemorySize();
std::int32_t sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t,
                                         std::size_t, int, std::int64_t*);
std::int32_t sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
std::int32_t sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
std::int32_t sceKernelReserveVirtualRange(void**, std::size_t, int, std::size_t);
std::int32_t sceKernelEnableDmemAliasing();
int sceKernelDebugOutText(int, const char*);
}
#endif

namespace Common {
namespace {
// The header sits in the page before the data. Blocks of at least LargePage start their data
// one LargePage in, with the address and direct memory LargePage-aligned, so the kernel can map
// them with 2 MiB pages: the guest backing, JIT caches and page tables are walked on every
// guest memory access, and 16 KiB pages cover only a few MiB of TLB reach.
struct Header { std::int64_t physical; std::size_t total; std::size_t lead; };
constexpr std::size_t LargePage = 0x200000;
// Development A/B: dev-settings large_pages=off keeps every block 16 KiB-aligned. The heap takes
// its blocks before the frontend parses the file, so read it here with plain system calls.
bool DevSetting(const char* entry) {
    char text[4096];
    const int fd = open("/app0/dev-settings.txt", O_RDONLY);
    if (fd < 0) return false;
    const auto count = read(fd, text, sizeof(text) - 1);
    close(fd);
    text[count > 0 ? count : 0] = '\0';
    return std::strstr(text, entry) != nullptr;
}
// A line for the log from inside the allocator: no stdio (it may allocate). On the console the
// app's stderr is a stream of the C library and not descriptor 2, so the line goes to the kernel
// log.
void Note(const char* line) noexcept {
#ifdef PS5_NATIVE
    sceKernelDebugOutText(0, line);
#else
    (void)!write(2, line, std::strlen(line));
#endif
}
// The same for a line written outside the allocator: on the console it is in the app's log too.
void Report(const char* line) noexcept {
    Note(line);
#ifdef PS5_NATIVE
    std::fputs(line, stderr);
#endif
}
bool LargePagesEnabled() {
    static std::atomic<int> state{0}; // 0 unknown, 1 on, 2 off
    int value = state.load(std::memory_order_acquire);
    if (value == 0) {
        value = DevSetting("large_pages=off") ? 2 : 1;
        state.store(value, std::memory_order_release);
    }
    return value == 1;
}
std::size_t lead_size(std::size_t size, std::size_t page) {
    return size >= LargePage && LargePagesEnabled() ? LargePage : page;
}
std::size_t allocation_size(std::size_t size, std::size_t page, std::size_t lead) {
    if (!size || page < sizeof(Header) || size > std::numeric_limits<std::size_t>::max() - 2 * lead)
        return 0;
    return (size + lead - 1) / lead * lead + lead;
}
const Header& header_of(const void* pointer, std::size_t page) {
    return *reinterpret_cast<const Header*>(static_cast<const std::uint8_t*>(pointer) - page);
}
}

// PS5 direct memory is real memory from the first byte: there is no page the system fills in
// when it is first touched. A block from here is dense and zeroed. What must not be dense (the
// guest page tables, the heap) uses the two families below.
namespace {
void* AllocatePagesAt(std::size_t size, [[maybe_unused]] std::uintptr_t hint) noexcept {
    const long page = sysconf(_SC_PAGESIZE);
    const std::size_t lead = page > 0 ? lead_size(size, static_cast<std::size_t>(page)) : 0;
    const auto total = page > 0 ? allocation_size(size, page, lead) : 0;
    if (!total) { errno = EINVAL; return nullptr; }
    void* base = nullptr;
    std::int64_t physical = -1;
#ifdef PS5_NATIVE
    base = reinterpret_cast<void*>(hint);
    const auto limit = sceKernelGetDirectMemorySize();
    auto rc = sceKernelAllocateDirectMemory(0, limit, total, lead, 12, &physical);
    if (rc != 0) {
        std::fprintf(stderr, "Direct allocation failed: rc=%08x bytes=%zu limit=%lld\n",
                     unsigned(rc), total, static_cast<long long>(limit));
        errno = ENOMEM;
        return nullptr;
    }
    rc = sceKernelMapDirectMemory(&base, total, PROT_READ | PROT_WRITE, 0, physical, lead);
    if (rc != 0 || !cpu_mapping_address(base)) {
        std::fprintf(stderr, "Direct mapping failed: rc=%08x bytes=%zu\n", unsigned(rc), total);
        if (rc == 0 && base && base != MAP_FAILED && munmap(base, total) != 0) std::abort();
        if (sceKernelReleaseDirectMemory(physical, total) != 0) std::abort();
        errno = ENOMEM;
        return nullptr;
    }
    if (lead == LargePage) {
        // The heap allocates through here: no stdio (it may allocate).
        char line[96];
        const int length = std::snprintf(line, sizeof(line), "EDEN_LARGE_ALLOC bytes=%zu va=%p pa=%llx\n",
                                         total, base, static_cast<unsigned long long>(physical));
        if (length > 0) Note(line);
    }
    std::memset(base, 0, total);
#else
    base = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return nullptr;
#endif
    auto* data = static_cast<std::uint8_t*>(base) + lead;
    new (data - page) Header{physical, total, lead};
    return data;
}
} // namespace

void* AllocateMemoryPages(std::size_t size) noexcept {
#ifdef PS5_NATIVE
    return AllocatePagesAt(size, cpu_mapping_hint);
#else
    return AllocatePagesAt(size, 0);
#endif
}

// The guest's memory (host_memory.cpp): as AllocateMemoryPages, above the guest's address space.
void* AllocateGuestMemory(std::size_t size) noexcept {
#ifdef PS5_NATIVE
    void* base = AllocatePagesAt(size, guest_memory_hint);
    if (!base) base = AllocatePagesAt(size, cpu_mapping_hint);
    char line[112];
    const int length = std::snprintf(line, sizeof(line), "EDEN_GUEST_MEMORY va=%p bytes=%zu above_guest=%d\n", base, size,
                                     int(reinterpret_cast<std::uintptr_t>(base) >= guest_memory_hint));
    if (length > 0) Report(line);
    return base;
#else
    return AllocatePagesAt(size, 0);
#endif
}

#ifdef PS5_NATIVE
// Direct-memory start of an AllocateMemoryPages block's first data byte.
std::int64_t DirectMemoryStart(const void* pointer) noexcept {
    const long page = sysconf(_SC_PAGESIZE);
    if (!pointer || page <= 0) std::abort();
    const auto& header = header_of(pointer, page);
    return header.physical + static_cast<std::int64_t>(header.lead);
}

// A second view of our own direct allocation; ownership stays with pointer.
void* MapExecutableAlias(void* pointer, std::size_t size) noexcept {
    const long page = sysconf(_SC_PAGESIZE);
    if (!pointer || page <= 0) return nullptr;
    const auto header = header_of(pointer, page);
    if (header.total != allocation_size(size, page, header.lead)) std::abort();
    // This wrapper reports zero unconditionally; the mapping is the actual check.
    static const auto enabled = sceKernelEnableDmemAliasing();
    (void)enabled;
    const auto span = header.total - header.lead;
    void* alias = reinterpret_cast<void*>(cpu_mapping_hint);
    const auto rc = sceKernelMapDirectMemory(&alias, span, PROT_READ, 0,
                                              header.physical + static_cast<std::int64_t>(header.lead),
                                              header.lead);
    if (rc != 0 || !cpu_mapping_address(alias)) {
        std::printf("EDEN_JIT_ALIAS_MAP rc=%08x bytes=%zu\n", unsigned(rc), span);
        if (rc == 0 && alias && alias != MAP_FAILED && munmap(alias, span) != 0) std::abort();
        errno = rc ? unsigned(rc) & 0xffff : ENOMEM;
        return nullptr;
    }
    // Direct mapping wrappers reject EXEC on some versions. Use the same
    // checked mprotect contract already qualified for this owned memory type.
    if (mprotect(alias, span, PROT_READ | PROT_EXEC) != 0) {
        const auto error = errno;
        if (munmap(alias, span) != 0) std::abort();
        errno = error;
        return nullptr;
    }
    return alias;
}
#endif

void FreeMemoryPages(void* pointer, std::size_t size) noexcept {
    if (!pointer) return;
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) std::abort();
    const auto header = header_of(pointer, page);
    auto* base = static_cast<std::uint8_t*>(pointer) - header.lead;
    if (header.total != allocation_size(size, page, header.lead) || munmap(base, header.total) != 0) std::abort();
#ifdef PS5_NATIVE
    if (sceKernelReleaseDirectMemory(header.physical, header.total) != 0) std::abort();
#endif
}
// Xbyak's allocator interface supplies only the pointer at release. The owned
// header already records the exact mapped size; retain the same checked free.
void FreeMemoryPages(void* pointer) noexcept {
    if (!pointer) return;
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) std::abort();
    const auto header = header_of(pointer, page);
    if (header.total <= header.lead) std::abort();
    FreeMemoryPages(pointer, header.total - header.lead);
}

// ---- Sparse pages: Eden's large tables ----
//
// Eden indexes its guest page table (and the GPU's address tables) directly: 8 bytes for every
// 4 KiB of a 512 GiB address space is a 1 GiB table, of which a game writes a few MiB. On a PC
// that costs nothing: the table is mapped read-only, the system shows one shared page of zeroes
// wherever nothing was written, and SparseLargeVector makes a page writable before its first
// write. The PS5 has no such page, so the same thing is built from mappings:
//
//   - the table's address range is reserved;
//   - every 2 MiB slot of it shows one shared block of zeroes, read-only (the JIT reads the
//     table for any address a game touches, mapped or not);
//   - CommitSparsePage, called before a first write, gives the slot a zeroed block of its own.
//
// A session's page table then takes 2 MiB per GiB of address space the game maps, not 1 GiB.
// The Linux build mirrors it (read-only until committed), so the host checks fail on a write
// that was not announced, as the console would.
namespace {
constexpr std::size_t SparseSlot = LargePage;
struct SparseRange {
    std::atomic<std::uintptr_t> begin{0}; // 0: free
    std::uintptr_t end = 0;
    std::atomic<bool>* owned = nullptr; // per slot: it has a block of its own
    bool guest = false;                 // the guest's memory, not a table (counted apart)
    std::int64_t* physical = nullptr;   // that block (PS5)
};
std::array<SparseRange, 64> sparse_ranges;
std::mutex sparse_mutex;
std::atomic<std::size_t> sparse_reserved{0}, sparse_committed{0};
// The guest's memory when it is such a range (below: "The guest's memory, backed where it is used").
std::atomic<std::size_t> guest_reserved{0}, guest_committed{0};
std::atomic<SparseRange*> guest_range{nullptr};
std::atomic<int> sparse_state{0}; // 0 unknown, 1 in use, 2 not available (tables are dense)

SparseRange* SparseRangeOf(std::uintptr_t address) {
    for (auto& range : sparse_ranges) {
        const auto begin = range.begin.load(std::memory_order_acquire);
        if (begin != 0 && address >= begin && address < range.end) return &range;
    }
    return nullptr;
}

#ifdef PS5_NATIVE
// From as high in the pool as there is room (src/direct_memory.c): what is committed this way,
// the heap's pieces, stays for good, while a game's memory and its graphics come and go below.
extern "C" int eden_direct_high(std::size_t size, std::size_t alignment, int type, std::int64_t* start);
// Where the system puts it: a 2 MiB block of this file's, which a session gives back.
extern "C" int eden_direct_low(std::size_t size, std::size_t alignment, int type, std::int64_t* start);
#endif

#ifdef PS5_NATIVE
std::int64_t zero_block = -1;

// A 2 MiB block of direct memory, zeroed through a mapping of its own that is gone again.
bool ZeroedBlock(std::int64_t* physical) {
    if (eden_direct_low(SparseSlot, SparseSlot, 12, physical) != 0)
        return false;
    void* view = reinterpret_cast<void*>(cpu_mapping_hint);
    if (sceKernelMapDirectMemory(&view, SparseSlot, PROT_READ | PROT_WRITE, 0, *physical, SparseSlot) != 0) {
        (void)sceKernelReleaseDirectMemory(*physical, SparseSlot);
        return false;
    }
    std::memset(view, 0, SparseSlot);
    if (munmap(view, SparseSlot) != 0) std::abort();
    return true;
}
bool MapSlot(std::uintptr_t at, int protection, std::int64_t physical) {
    void* address = reinterpret_cast<void*>(at);
    return sceKernelMapDirectMemory(&address, SparseSlot, protection, MAP_FIXED, physical, SparseSlot) == 0 &&
           address == reinterpret_cast<void*>(at);
}
#endif

// Reserves `span` bytes whose every slot reads as zeroes. Null when the platform refuses. The
// guest's memory goes above the guest's address space, as it does when it is taken whole.
void* ReserveSparse(std::size_t span, [[maybe_unused]] bool guest = false) {
#ifdef PS5_NATIVE
    void* address = reinterpret_cast<void*>(guest ? guest_memory_hint : cpu_mapping_hint);
    if (sceKernelReserveVirtualRange(&address, span, 0, SparseSlot) != 0) return nullptr;
    if (guest ? reinterpret_cast<std::uintptr_t>(address) < guest_memory_hint : !cpu_mapping_range(address, span)) {
        (void)munmap(address, span);
        return nullptr;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    for (std::size_t offset = 0; offset < span; offset += SparseSlot) {
        if (!MapSlot(start + offset, PROT_READ, zero_block)) {
            (void)munmap(address, span);
            return nullptr;
        }
    }
    return address;
#else
    void* address = mmap(nullptr, span, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return address == MAP_FAILED ? nullptr : address;
#endif
}

// Gives the slot at `at` memory of its own, zeroed and writable. sparse_mutex is held.
bool OwnSlot(std::uintptr_t at, std::int64_t* physical) {
#ifdef PS5_NATIVE
    // The block is zeroed before it appears in the table: a JIT thread may read the slot at any
    // moment, and the mapping changes in one step.
    if (!ZeroedBlock(physical)) return false;
    if (MapSlot(at, PROT_READ | PROT_WRITE, *physical)) return true;
    (void)sceKernelReleaseDirectMemory(*physical, SparseSlot);
    return false;
#else
    *physical = 0;
    return mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ | PROT_WRITE) == 0;
#endif
}

void ReleaseSparse(SparseRange& range) {
    const auto begin = range.begin.load(std::memory_order_relaxed);
    const std::size_t span = range.end - begin;
    if (munmap(reinterpret_cast<void*>(begin), span) != 0) std::abort();
    std::size_t owned = 0;
    for (std::size_t slot = 0; slot < span / SparseSlot; ++slot) {
        if (!range.owned[slot].load(std::memory_order_relaxed)) continue;
        ++owned;
#ifdef PS5_NATIVE
        if (sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
#endif
    }
    (range.guest ? guest_reserved : sparse_reserved).fetch_sub(span, std::memory_order_relaxed);
    (range.guest ? guest_committed : sparse_committed).fetch_sub(owned * SparseSlot, std::memory_order_relaxed);
    range.guest = false;
    range.begin.store(0, std::memory_order_release);
    delete[] range.owned;
    delete[] range.physical;
    range.owned = nullptr;
    range.physical = nullptr;
}

// Once: can tables be sparse here? On the console this is a check of the three mapping steps
// above with real memory; a refusal leaves the tables dense, as they were before.
bool SparseAvailable() {
    int state = sparse_state.load(std::memory_order_acquire);
    if (state != 0) return state == 1;
    const std::lock_guard lock{sparse_mutex};
    state = sparse_state.load(std::memory_order_relaxed);
    if (state != 0) return state == 1;
    bool ok = true;
    const char* step = "ok";
#ifdef PS5_NATIVE
    ok = !DevSetting("sparse_tables=off");
    if (!ok) step = "switched-off";
    if (ok) {
        (void)sceKernelEnableDmemAliasing(); // reports zero regardless; the mappings below are the check
        ok = ZeroedBlock(&zero_block);
        if (!ok) step = "zero-block";
    }
#endif
    if (ok) {
        auto* probe = static_cast<volatile std::uint64_t*>(ReserveSparse(2 * SparseSlot));
        const std::size_t words = SparseSlot / sizeof(std::uint64_t);
        std::int64_t physical = -1;
        if (probe == nullptr) {
            ok = false;
            step = "reserve";
        } else {
            if (probe[0] != 0 || probe[words - 1] != 0 || probe[words] != 0 || probe[2 * words - 1] != 0) {
                ok = false;
                step = "zero-read";
            } else if (!OwnSlot(reinterpret_cast<std::uintptr_t>(probe) + SparseSlot, &physical)) {
                ok = false;
                step = "own";
            } else {
                probe[words] = 0x5a17ed5a17ed5a17ull;
                probe[2 * words - 1] = 0x1234567890abcdefull;
                if (probe[words] != 0x5a17ed5a17ed5a17ull || probe[2 * words - 1] != 0x1234567890abcdefull ||
                    probe[0] != 0 || probe[words - 1] != 0 || probe[words + 1] != 0) {
                    ok = false;
                    step = "own-read";
                }
#ifdef PS5_NATIVE
                // The written slot goes back; the shared block must still be all zeroes.
                if (munmap(const_cast<std::uint64_t*>(probe) + words, SparseSlot) != 0 ||
                    sceKernelReleaseDirectMemory(physical, SparseSlot) != 0)
                    std::abort();
                if (!MapSlot(reinterpret_cast<std::uintptr_t>(probe) + SparseSlot, PROT_READ, zero_block) ||
                    probe[words] != 0 || probe[2 * words - 1] != 0) {
                    ok = false;
                    step = "shared-zero";
                }
#endif
            }
            if (munmap(const_cast<std::uint64_t*>(probe), 2 * SparseSlot) != 0) std::abort();
        }
    }
    char line[96];
    const int length = std::snprintf(line, sizeof(line), "EDEN_SPARSE_TABLES available=%d step=%s\n", ok ? 1 : 0, step);
    if (length > 0) Report(line);
    sparse_state.store(ok ? 1 : 2, std::memory_order_release);
    return ok;
}
} // namespace

// The first table asks this; the app asks at start, so the log says it in every session.
bool SparseTablesAvailable() noexcept {
    return SparseAvailable();
}

namespace {
// A sparse range of `size` bytes: every slot reads as zeroes until CommitSparsePage gives it a
// block of its own. Null when the size is too small for one or the platform refuses.
void* NewSparseRange(std::size_t size, bool guest) {
    const std::size_t span = (size + SparseSlot - 1) / SparseSlot * SparseSlot;
    if (size < 2 * SparseSlot || span < size || !SparseAvailable()) return nullptr;
    const std::lock_guard lock{sparse_mutex};
    SparseRange* free_range = nullptr;
    for (auto& range : sparse_ranges)
        if (range.begin.load(std::memory_order_relaxed) == 0) {
            free_range = &range;
            break;
        }
    const std::size_t slots = span / SparseSlot;
    auto* owned = free_range ? new (std::nothrow) std::atomic<bool>[slots] : nullptr;
    auto* physical = owned ? new (std::nothrow) std::int64_t[slots] : nullptr;
    void* address = physical ? ReserveSparse(span, guest) : nullptr;
    if (address == nullptr) {
        delete[] owned;
        delete[] physical;
        return nullptr;
    }
    for (std::size_t slot = 0; slot < slots; ++slot) {
        owned[slot].store(false, std::memory_order_relaxed);
        physical[slot] = -1;
    }
    free_range->owned = owned;
    free_range->physical = physical;
    free_range->guest = guest;
    free_range->end = reinterpret_cast<std::uintptr_t>(address) + span;
    free_range->begin.store(reinterpret_cast<std::uintptr_t>(address), std::memory_order_release);
    (guest ? guest_reserved : sparse_reserved).fetch_add(span, std::memory_order_relaxed);
    if (guest) guest_range.store(free_range, std::memory_order_release);
    return address;
}
} // namespace

void* AllocateSparsePages(std::size_t size) noexcept {
    if (void* const address = NewSparseRange(size, false)) return address;
    if (size >= 2 * SparseSlot && SparseAvailable()) {
        // The platform took the check's two slots but not this table: it is dense, as before.
        char line[96];
        const int length = std::snprintf(line, sizeof(line), "EDEN_SPARSE_TABLES dense bytes=%zu\n", size);
        if (length > 0) Report(line);
    }
    return AllocateMemoryPages(size);
}

void FreeSparsePages(void* base, std::size_t size) noexcept {
    if (!base) return;
    {
        const std::lock_guard lock{sparse_mutex};
        SparseRange* range = SparseRangeOf(reinterpret_cast<std::uintptr_t>(base));
        if (range != nullptr) {
            if (range->begin.load(std::memory_order_relaxed) != reinterpret_cast<std::uintptr_t>(base)) std::abort();
            ReleaseSparse(*range);
            return;
        }
    }
    FreeMemoryPages(base, size); // a table that was allocated dense
}

// ---- The guest's memory, backed where it is used ----
//
// A game's console has 4 GiB, and the guest's kernel divides them into pools: the game's, the
// applets', the system's. Taken whole, the block is 4 GiB of the pool the CPU shares with the
// GPU from the moment a game starts. But the kernel hands a pool's pages out as they are asked
// for, and a page it has not handed out was never written: the applets' pool is not used by a
// game at all, most of the system's is not, and a small game leaves most of its own. So the
// block is a sparse range like the tables above, each 2 MiB slot one shared block of zeroes
// until it is committed, and it is committed where the guest's kernel uses it: everything
// outside the pools when the kernel starts, and a pool's pages when they are handed out (the
// two places are in headless/CMakeLists.txt, "The guest's memory is backed where it is used").
// Reading a slot that was never committed gives zeroes, as the block taken whole would;
// writing one that was not announced stops the app, on the console as in the host checks.
void CommitSparsePage(std::uintptr_t page) noexcept;

void* AllocateGuestMemoryWhereUsed(std::size_t size) noexcept {
    // Development A/B: dev-settings guest_memory=whole takes the block whole, as before.
    void* base = DevSetting("guest_memory=whole") ? nullptr : NewSparseRange(size, true);
    if (base == nullptr) return AllocateGuestMemory(size);
    char line[112];
    const int length = std::snprintf(line, sizeof(line), "EDEN_GUEST_MEMORY va=%p bytes=%zu above_guest=1 backed=where-used\n",
                                     base, size);
    if (length > 0) Report(line);
    return base;
}

// Memory behind `bytes` of the guest's memory from `offset` on (whole slots). Nothing to do for
// a block that was taken whole.
void CommitGuestMemory(std::uint64_t offset, std::size_t bytes) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range == nullptr || bytes == 0) return;
    const auto begin = range->begin.load(std::memory_order_relaxed);
    const std::size_t span = range->end - begin;
    if (offset >= span) return;
    if (bytes > span - offset) bytes = span - static_cast<std::size_t>(offset);
    const std::size_t last = (static_cast<std::size_t>(offset) + bytes - 1) / SparseSlot;
    for (std::size_t slot = static_cast<std::size_t>(offset) / SparseSlot; slot <= last; ++slot)
        if (!range->owned[slot].load(std::memory_order_acquire)) CommitSparsePage(begin + slot * SparseSlot);
}

// The guest's memory, however it was taken.
void FreeGuestMemory(void* base, std::size_t size) noexcept {
    if (SparseRange* const range = guest_range.load(std::memory_order_acquire);
        range != nullptr && range->begin.load(std::memory_order_relaxed) == reinterpret_cast<std::uintptr_t>(base)) {
        char line[112];
        const int length = std::snprintf(line, sizeof(line), "EDEN_GUEST_MEMORY given back: held=%zu of=%zu\n",
                                         guest_committed.load(std::memory_order_relaxed), size);
        if (length > 0) Report(line);
        guest_range.store(nullptr, std::memory_order_release);
    }
    FreeSparsePages(base, size);
}

// The span of the guest's memory and the memory behind it; both 0 for a block taken whole.
void GuestMemoryUsage(std::size_t* span, std::size_t* held) noexcept {
    *span = guest_reserved.load(std::memory_order_relaxed);
    *held = guest_committed.load(std::memory_order_relaxed);
}

void CommitSparsePage(std::uintptr_t page) noexcept {
    SparseRange* range = SparseRangeOf(page);
    if (range == nullptr) return; // dense: already writable
    const auto begin = range->begin.load(std::memory_order_relaxed);
    const std::size_t slot = (page - begin) / SparseSlot;
    if (range->owned[slot].load(std::memory_order_acquire)) return;
    const std::lock_guard lock{sparse_mutex};
    if (range->owned[slot].load(std::memory_order_relaxed)) return;
    if (!OwnSlot(begin + slot * SparseSlot, &range->physical[slot])) {
        // Out of memory in the middle of a guest mapping: there is no table to continue with.
        Report("EDEN_SPARSE_TABLES commit failed: out of memory\n");
        std::abort();
    }
    (range->guest ? guest_committed : sparse_committed).fetch_add(SparseSlot, std::memory_order_relaxed);
    range->owned[slot].store(true, std::memory_order_release);
}

// Eden gives back a table page once a guest unmapped all of it, and expects zeroes there
// afterwards. A slot's block is shared by every page of the slot, so the page is zeroed and the
// block stays: a later CommitSparsePage for it finds the slot owned. A page of a slot that was
// never written is the shared block of zeroes, read-only, and is left alone.
void DecommitVectorPage(std::uintptr_t page) noexcept {
    const long size = sysconf(_SC_PAGESIZE);
    if (size <= 0) std::abort();
    if (SparseRange* range = SparseRangeOf(page)) {
        const std::size_t slot = (page - range->begin.load(std::memory_order_relaxed)) / SparseSlot;
        if (!range->owned[slot].load(std::memory_order_acquire)) return;
    }
    std::memset(reinterpret_cast<void*>(page), 0, static_cast<std::size_t>(size));
}

// Address space the sparse tables span, and the memory they hold (for the log).
void SparseUsage(std::size_t* reserved, std::size_t* committed) noexcept {
    *reserved = sparse_reserved.load(std::memory_order_relaxed);
    *committed = sparse_committed.load(std::memory_order_relaxed);
}

// ---- A range that grows: the heap ----
//
// ReserveMemoryRange takes address space only. CommitMemoryRange backs a part of it with zeroed
// memory, for good. Both work in whole large pages; the heap commits its range from the start,
// one piece after another, as allocations need it (headless/heap_arenas.inc).
void* ReserveMemoryRange(std::size_t size) noexcept {
    if (size == 0 || size % LargePage != 0) return nullptr;
#ifdef PS5_NATIVE
    // Development A/B: dev-settings heap=whole takes the heap's memory at start, as before.
    if (DevSetting("heap=whole")) return nullptr;
    void* address = reinterpret_cast<void*>(cpu_mapping_hint);
    if (sceKernelReserveVirtualRange(&address, size, 0, LargePage) != 0) return nullptr;
    if (!cpu_mapping_range(address, size)) {
        (void)munmap(address, size);
        return nullptr;
    }
    return address;
#else
    void* address = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return address == MAP_FAILED ? nullptr : address;
#endif
}

bool CommitMemoryRange(void* address, std::size_t size) noexcept {
    if (address == nullptr || size == 0 || size % LargePage != 0 ||
        reinterpret_cast<std::uintptr_t>(address) % LargePage != 0)
        return false;
#ifdef PS5_NATIVE
    std::int64_t physical = -1;
    if (eden_direct_high(size, LargePage, 12, &physical) != 0) return false;
    void* at = address;
    if (sceKernelMapDirectMemory(&at, size, PROT_READ | PROT_WRITE, MAP_FIXED, physical, LargePage) != 0 ||
        at != address) {
        (void)sceKernelReleaseDirectMemory(physical, size);
        return false;
    }
    // The heap grows through here: no stdio (it may allocate).
    char line[96];
    const int length = std::snprintf(line, sizeof(line), "EDEN_HEAP_PIECE bytes=%zu va=%p pa=%llx\n", size, address,
                                     static_cast<unsigned long long>(physical));
    if (length > 0) Note(line);
    std::memset(address, 0, size);
    return true;
#else
    return mprotect(address, size, PROT_READ | PROT_WRITE) == 0;
#endif
}

// ---- A range that fills: a JIT code cache ----
//
// The JIT writes a code cache from its start, one block after another, and starts again at the
// beginning when the cache is full. Taken whole, the four caches of a 64-bit game are 656 MiB of
// the pool the CPU shares with the GPU, and a large game fills little more than half of that.
// So a cache is address space only, in two ranges (the one the JIT writes through and the one
// the code runs from), and memory is put behind both a step at a time as the JIT reaches it:
// CommitCodeRange is asked before every block. Memory is not taken back while the cache lives.
namespace {
constexpr std::size_t CodeStep = std::size_t{16} << 20;
constexpr std::size_t CodeSteps = 64; // the largest cache: 1 GiB
struct CodeRange {
    std::atomic<std::uintptr_t> executable{0}; // 0: free
    std::uintptr_t writable = 0;
    std::size_t span = 0;
    std::atomic<std::size_t> committed{0};
    std::int64_t physical[CodeSteps] = {};
};
std::array<CodeRange, 16> code_ranges;
std::mutex code_mutex;
std::atomic<std::size_t> code_reserved{0}, code_committed{0};

// Memory behind the next step of both ranges. code_mutex is held.
bool CommitCodeStep(CodeRange& range, std::size_t offset) {
    void* const writable = reinterpret_cast<void*>(range.writable + offset);
    void* const executable = reinterpret_cast<void*>(range.executable.load(std::memory_order_relaxed) + offset);
#ifdef PS5_NATIVE
    std::int64_t physical = -1;
    if (eden_direct_low(CodeStep, LargePage, 12, &physical) != 0) return false;
    // A step that fails half way stays as it is: the caller has no cache to go on with.
    void* at = writable;
    if (sceKernelMapDirectMemory(&at, CodeStep, PROT_READ | PROT_WRITE, MAP_FIXED, physical, LargePage) != 0 ||
        at != writable)
        return false;
    std::memset(writable, 0, CodeStep);
    at = executable;
    if (sceKernelMapDirectMemory(&at, CodeStep, PROT_READ, MAP_FIXED, physical, LargePage) != 0 || at != executable)
        return false;
    // As MapExecutableAlias: the mapping call refuses EXEC on some versions.
    if (mprotect(executable, CodeStep, PROT_READ | PROT_EXEC) != 0) return false;
    range.physical[offset / CodeStep] = physical;
#else
    if (mprotect(writable, CodeStep, PROT_READ | PROT_WRITE) != 0 ||
        mprotect(executable, CodeStep, PROT_READ | PROT_EXEC) != 0)
        return false;
#endif
    code_committed.fetch_add(CodeStep, std::memory_order_relaxed);
    return true;
}
} // namespace

// Address space for a code cache of `size` bytes: returns the range to write through and sets
// the one to run from. Null when a cache this small is better taken whole, or when the
// platform refuses: the caller then takes the cache whole, as before.
void* ReserveCodeRange(std::size_t size, void** executable) noexcept {
    if (size < 2 * CodeStep || size % CodeStep != 0 || size / CodeStep > CodeSteps) return nullptr;
    const std::lock_guard lock{code_mutex};
    CodeRange* free_range = nullptr;
    for (auto& range : code_ranges)
        if (range.executable.load(std::memory_order_relaxed) == 0) {
            free_range = &range;
            break;
        }
    if (free_range == nullptr) return nullptr;
    void* views[2] = {nullptr, nullptr};
#ifdef PS5_NATIVE
    // Development A/B: dev-settings jit_memory=whole takes every cache whole, as before.
    if (DevSetting("jit_memory=whole")) return nullptr;
    (void)sceKernelEnableDmemAliasing(); // reports zero regardless; the first step is the check
    for (void*& view : views) {
        view = reinterpret_cast<void*>(cpu_mapping_hint);
        if (sceKernelReserveVirtualRange(&view, size, 0, LargePage) != 0) {
            view = nullptr;
        } else if (!cpu_mapping_range(view, size)) {
            (void)munmap(view, size);
            view = nullptr;
        }
    }
#else
    const int fd = memfd_create("eden-jit", MFD_CLOEXEC);
    if (fd >= 0 && ftruncate(fd, static_cast<off_t>(size)) == 0) {
        for (void*& view : views) {
            view = mmap(nullptr, size, PROT_NONE, MAP_SHARED, fd, 0);
            if (view == MAP_FAILED) view = nullptr;
        }
    }
    if (fd >= 0 && close(fd) != 0) std::abort();
#endif
    if (views[0] == nullptr || views[1] == nullptr) {
        for (void* view : views)
            if (view != nullptr) (void)munmap(view, size);
        return nullptr;
    }
    free_range->writable = reinterpret_cast<std::uintptr_t>(views[0]);
    free_range->span = size;
    free_range->committed.store(0, std::memory_order_relaxed);
    free_range->executable.store(reinterpret_cast<std::uintptr_t>(views[1]), std::memory_order_release);
    code_reserved.fetch_add(size, std::memory_order_relaxed);
    *executable = views[1];
    return views[0];
}

// Memory behind the first `bytes` of the cache that runs from `executable` (more than the
// cache: all of it). True when it is there, and for a cache that was taken whole. False when
// the console has none left.
bool CommitCodeRange(const void* executable, std::size_t bytes) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(executable);
    for (auto& range : code_ranges) {
        if (address == 0 || range.executable.load(std::memory_order_acquire) != address) continue;
        if (bytes > range.span) bytes = range.span;
        if (bytes <= range.committed.load(std::memory_order_acquire)) return true;
        const std::lock_guard lock{code_mutex};
        std::size_t committed = range.committed.load(std::memory_order_relaxed);
        while (committed < bytes) {
            if (!CommitCodeStep(range, committed)) return false;
            committed += CodeStep;
            range.committed.store(committed, std::memory_order_release);
        }
        char line[112];
        const int length = std::snprintf(line, sizeof(line), "EDEN_JIT_MEMORY code=%p held=%zu of=%zu\n", executable,
                                         committed, range.span);
        if (length > 0) Report(line);
        return true;
    }
    return true;
}

// Gives back a cache ReserveCodeRange made: its memory and both ranges. False for any other.
bool FreeCodeRange(void* executable) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(executable);
    if (address == 0) return false;
    const std::lock_guard lock{code_mutex};
    for (auto& range : code_ranges) {
        if (range.executable.load(std::memory_order_relaxed) != address) continue;
        const std::size_t committed = range.committed.load(std::memory_order_relaxed);
        if (munmap(reinterpret_cast<void*>(range.writable), range.span) != 0 || munmap(executable, range.span) != 0)
            std::abort();
#ifdef PS5_NATIVE
        for (std::size_t step = 0; step < committed / CodeStep; ++step)
            if (sceKernelReleaseDirectMemory(range.physical[step], CodeStep) != 0) std::abort();
#endif
        code_reserved.fetch_sub(range.span, std::memory_order_relaxed);
        code_committed.fetch_sub(committed, std::memory_order_relaxed);
        range.committed.store(0, std::memory_order_relaxed);
        range.executable.store(0, std::memory_order_release);
        return true;
    }
    return false;
}

// Address space the growing code caches span, and the memory behind them (for the log).
void CodeRangeUsage(std::size_t* reserved, std::size_t* committed) noexcept {
    *reserved = code_reserved.load(std::memory_order_relaxed);
    *committed = code_committed.load(std::memory_order_relaxed);
}
} // namespace Common
