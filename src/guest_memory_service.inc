// SPDX-License-Identifier: GPL-3.0-or-later
// ---- The guest's memory as a service: shared until written (included by memory_pages.cpp) ----
//
// After Gregory Price's "A Compressed RAM Service" (Linux Plumbers 2026, mm/cram.c): memory that
// is read where it lies and promoted when it is written. There the device compresses; here the
// emulator is the device, and the two forms a processor can read without a fault are a slot that
// shares the block of zeroes and a slot that shares a block with another slot of the same bytes.
//
//   Zero    the slot shows the shared block of zeroes, read-only
//   Own     the slot has a block of its own, read-write
//   Shared  the slot shows a block it shares with other slots of the same bytes, read-only
//   Fine    the slot is 128 pages of 16 KiB, read-only: its pages of zeroes show the zero block, the
//           others a compact block that holds only them (a written slot with many pages of zeroes);
//           a write makes it whole again, and it waits longer each time before it may be Fine again
//
// A write to a Zero or Shared slot faults; HandleGuestWriteFault gives the slot a block of its
// own (zeroed, or a copy) and the write repeats. So memory is no longer taken when the guest's
// kernel hands pages out, only when they are written; a slot the guest's kernel clears or frees
// whole goes back to the zero block; and a scanner gives back slots that hold only zeroes and
// lets slots of the same bytes share one block. A slot is locked (bit 7 of its state) while it
// changes; a writer that faults on a locked slot waits for it.
//
// 64-bit games only: a 32-bit game's fastmem window aliases the backing, which takes it whole.

namespace {
enum : std::uint8_t { SlotZero = 0, SlotOwn = 1, SlotShared = 2, SlotFine = 3, SlotLock = 0x80, SlotMask = 0x7f };
constexpr std::size_t FinePage = 16384, FinePages = SparseSlot / FinePage;
// A written slot becomes Fine with at least this many pages of zeroes (512 KiB given back).
constexpr std::size_t FineMinZeroPages = 32;
// A slot made whole again by a write waits 1, 2, 4 ... 1024 scan passes before it may be Fine again.
constexpr unsigned FineBackoffMax = 10;

// Duplicate sharing is off unless asked for (dev-settings cram_dedup=on): on the console it found a
// dozen 2 MiB pairs that were written apart within a minute, and with it on a large game held 20 FPS
// instead of 30 (the GPU worker waited for every fence, once a frame) where it held 30 without.
std::atomic<bool> service_requested{true}, scan_requested{true}, dedup_requested{false}, fine_requested{false};
std::atomic<SparseRange*> service_range{nullptr}; // the guest range the service runs for
std::atomic<SparseRange*> probe_range{nullptr};   // the self-test's range
std::atomic<int> service_state{0};                // 0 not tested, 1 works here, 2 does not
const char* service_refused = "";

struct Counters {
    std::atomic<std::uint64_t> faults{0}, promotions{0}, copies{0}, waits{0};
    std::atomic<std::uint64_t> returns_clear{0}, returns_free{0}, returns_scan{0}, dedup{0};
    std::atomic<std::uint64_t> reserve_used{0}, failures{0}, passes{0}, scan_ns{0};
    std::atomic<std::uint64_t> fined{0}, unfined{0}, fine_failures{0};
};
Counters counters;
void ResetCounters() noexcept {
    for (auto* counter : {&counters.faults, &counters.promotions, &counters.copies, &counters.waits,
                          &counters.returns_clear, &counters.returns_free, &counters.returns_scan, &counters.dedup,
                          &counters.reserve_used, &counters.failures, &counters.passes, &counters.scan_ns,
                          &counters.fined, &counters.unfined, &counters.fine_failures})
        counter->store(0, std::memory_order_relaxed);
}

bool LockSlot(SparseRange& range, std::size_t slot, bool count_wait = false) noexcept {
    auto& state = range.state[slot];
    bool waited = false;
    for (;;) {
        std::uint8_t value = state.load(std::memory_order_relaxed);
        if (!(value & SlotLock) &&
            state.compare_exchange_weak(value, value | SlotLock, std::memory_order_acquire, std::memory_order_relaxed))
            break;
        waited = true;
        sched_yield();
    }
    if (waited && count_wait) counters.waits.fetch_add(1, std::memory_order_relaxed);
    return waited;
}
std::size_t FineKept(SparseRange& range, std::size_t slot) noexcept {
    return range.fine[slot] & 0xff;
}
std::uint8_t SlotState(SparseRange& range, std::size_t slot) noexcept {
    return range.state[slot].load(std::memory_order_acquire) & SlotMask;
}
// Sets the state and lets the slot go.
void UnlockSlot(SparseRange& range, std::size_t slot, std::uint8_t to) noexcept {
    range.owned[slot].store(to != SlotZero, std::memory_order_release);
    range.state[slot].store(to, std::memory_order_release);
}
std::uintptr_t SlotAddress(SparseRange& range, std::size_t slot) noexcept {
    return range.begin.load(std::memory_order_relaxed) + slot * SparseSlot;
}

// ---- Blocks: the pool, a reserve for when the pool says no, and shared blocks' references ----
constexpr std::size_t ReserveBlocks = 16; // 32 MiB
std::int64_t reserve[ReserveBlocks];
std::atomic<std::size_t> reserve_count{0};
std::atomic_flag reserve_lock = ATOMIC_FLAG_INIT;
constexpr std::size_t SharedBlocks = 4096;
std::int64_t shared_block[SharedBlocks];
int shared_refs[SharedBlocks];
std::atomic_flag shared_lock = ATOMIC_FLAG_INIT;

void Spin(std::atomic_flag& flag) noexcept {
    while (flag.test_and_set(std::memory_order_acquire)) sched_yield();
}

#ifdef PS5_NATIVE
// A zeroed block from the reserve (the pool refused one).
bool ReserveBlock(std::int64_t* physical) noexcept {
    Spin(reserve_lock);
    const std::size_t count = reserve_count.load(std::memory_order_relaxed);
    const bool ok = count > 0;
    if (ok) {
        *physical = reserve[count - 1];
        reserve_count.store(count - 1, std::memory_order_relaxed);
    }
    reserve_lock.clear(std::memory_order_release);
    if (ok) counters.reserve_used.fetch_add(1, std::memory_order_relaxed);
    return ok;
}
void FillReserve() noexcept {
    for (;;) {
        Spin(reserve_lock);
        const bool full = reserve_count.load(std::memory_order_relaxed) >= ReserveBlocks;
        reserve_lock.clear(std::memory_order_release);
        if (full) return;
        std::int64_t physical = -1;
        if (!ZeroedBlock(&physical)) return;
        Spin(reserve_lock);
        const std::size_t count = reserve_count.load(std::memory_order_relaxed);
        if (count < ReserveBlocks) {
            reserve[count] = physical;
            reserve_count.store(count + 1, std::memory_order_relaxed);
            physical = -1;
        }
        reserve_lock.clear(std::memory_order_release);
        if (physical >= 0 && sceKernelReleaseDirectMemory(physical, SparseSlot) != 0) std::abort();
    }
}
void EmptyReserve() noexcept {
    Spin(reserve_lock);
    for (std::size_t i = 0; i < reserve_count.load(std::memory_order_relaxed); ++i)
        if (sceKernelReleaseDirectMemory(reserve[i], SparseSlot) != 0) std::abort();
    reserve_count.store(0, std::memory_order_relaxed);
    reserve_lock.clear(std::memory_order_release);
}
#endif

// One more slot shows `physical`; the first one makes it a shared block. False when the table is full.
bool ShareBlock(std::int64_t physical) noexcept {
    Spin(shared_lock);
    std::size_t free_entry = SharedBlocks;
    bool done = false;
    for (std::size_t i = 0; i < SharedBlocks && !done; ++i) {
        if (shared_refs[i] > 0 && shared_block[i] == physical) {
            ++shared_refs[i];
            done = true;
        } else if (shared_refs[i] == 0 && free_entry == SharedBlocks) {
            free_entry = i;
        }
    }
    if (!done && free_entry < SharedBlocks) {
        shared_block[free_entry] = physical;
        shared_refs[free_entry] = 1;
        done = true;
    }
    shared_lock.clear(std::memory_order_release);
    return done;
}
// One slot less shows `physical`: the references left (0: the block is no longer shown).
int UnshareBlock(std::int64_t physical) noexcept {
    Spin(shared_lock);
    int left = 0;
    for (std::size_t i = 0; i < SharedBlocks; ++i)
        if (shared_refs[i] > 0 && shared_block[i] == physical) {
            left = --shared_refs[i];
            break;
        }
    shared_lock.clear(std::memory_order_release);
    return left;
}

// ---- The changes of a slot (the slot is locked) ----

// Zero -> Own: a zeroed block of its own, read-write.
bool PromoteZero(SparseRange& range, std::size_t slot) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
#ifdef PS5_NATIVE
    std::int64_t physical = -1;
    if (!ZeroedBlock(&physical) && !ReserveBlock(&physical)) return false;
    if (!MapSlot(at, PROT_READ | PROT_WRITE, physical)) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    range.physical[slot] = physical;
#else
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ | PROT_WRITE) != 0) return false;
    range.physical[slot] = 0;
#endif
    guest_committed.fetch_add(SparseSlot, std::memory_order_relaxed);
    return true;
}

// Shared -> Own: the last slot to show a block takes it back read-write; the others copy it.
bool PromoteShared(SparseRange& range, std::size_t slot) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
#ifdef PS5_NATIVE
    const std::int64_t shared = range.physical[slot];
    Spin(shared_lock);
    bool last = false;
    for (std::size_t i = 0; i < SharedBlocks; ++i)
        if (shared_refs[i] > 0 && shared_block[i] == shared) {
            last = shared_refs[i] == 1;
            if (last) shared_refs[i] = 0;
            break;
        }
    shared_lock.clear(std::memory_order_release);
    if (last) return mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ | PROT_WRITE) == 0;
    std::int64_t physical = -1;
    if (eden_direct_low(SparseSlot, SparseSlot, 12, &physical) != 0 && !ReserveBlock(&physical)) return false;
    void* view = reinterpret_cast<void*>(cpu_mapping_hint);
    if (sceKernelMapDirectMemory(&view, SparseSlot, PROT_READ | PROT_WRITE, 0, physical, SparseSlot) != 0) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    std::memcpy(view, reinterpret_cast<const void*>(at), SparseSlot);
    if (munmap(view, SparseSlot) != 0) std::abort();
    if (!MapSlot(at, PROT_READ | PROT_WRITE, physical)) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    range.physical[slot] = physical;
    if (UnshareBlock(shared) == 0 && sceKernelReleaseDirectMemory(shared, SparseSlot) != 0) std::abort();
    guest_committed.fetch_add(SparseSlot, std::memory_order_relaxed);
    counters.copies.fetch_add(1, std::memory_order_relaxed);
    return true;
#else
    (void)range;
    (void)slot;
    (void)at;
    return false; // the host never shares
#endif
}

// Own, Shared or Fine -> Zero: the slot shows the zero block again and its memory goes back.
void ReturnSlot(SparseRange& range, std::size_t slot) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
    const std::uint8_t state = SlotState(range, slot);
    if (state == SlotFine) {
        const std::size_t kept = FineKept(range, slot);
#ifdef PS5_NATIVE
        if (!MapSlot(at, PROT_READ, zero_block)) std::abort();
        if (kept != 0 && sceKernelReleaseDirectMemory(range.physical[slot], kept * FinePage) != 0) std::abort();
#else
        if (madvise(reinterpret_cast<void*>(at), SparseSlot, MADV_DONTNEED) != 0) std::abort();
#endif
        guest_committed.fetch_sub(kept * FinePage, std::memory_order_relaxed);
        range.fine[slot] &= 0xf00;
        range.physical[slot] = -1;
        return;
    }
#ifdef PS5_NATIVE
    if (!MapSlot(at, PROT_READ, zero_block)) std::abort();
    if (state == SlotShared) {
        if (UnshareBlock(range.physical[slot]) == 0) {
            if (sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
            guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
        }
    } else {
        if (sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
        guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
    }
#else
    (void)state;
    if (madvise(reinterpret_cast<void*>(at), SparseSlot, MADV_DONTNEED) != 0 ||
        mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ) != 0)
        std::abort();
    guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
#endif
    range.physical[slot] = -1;
}

bool AllZero(std::uintptr_t at) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    for (std::size_t i = 0; i < SparseSlot / 8; i += 8) {
        if (word[i] | word[i + 1] | word[i + 2] | word[i + 3] | word[i + 4] | word[i + 5] | word[i + 6] | word[i + 7])
            return false;
    }
    return true;
}

bool PageZero(std::uintptr_t at) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    for (std::size_t i = 0; i < FinePage / 8; i += 8)
        if (word[i] | word[i + 1] | word[i + 2] | word[i + 3] | word[i + 4] | word[i + 5] | word[i + 6] | word[i + 7])
            return false;
    return true;
}

// Fine -> Own: a written Fine slot becomes one block of 2 MiB again (a copy of what it shows), and
// stays whole for the rest of the session.
bool PromoteFine(SparseRange& range, std::size_t slot) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
    const std::size_t kept = FineKept(range, slot);
#ifdef PS5_NATIVE
    std::int64_t physical = -1;
    if (eden_direct_low(SparseSlot, SparseSlot, 12, &physical) != 0 && !ReserveBlock(&physical)) return false;
    void* view = reinterpret_cast<void*>(cpu_mapping_hint);
    if (sceKernelMapDirectMemory(&view, SparseSlot, PROT_READ | PROT_WRITE, 0, physical, SparseSlot) != 0) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    std::memcpy(view, reinterpret_cast<const void*>(at), SparseSlot);
    if (munmap(view, SparseSlot) != 0) std::abort();
    if (!MapSlot(at, PROT_READ | PROT_WRITE, physical)) {
        (void)sceKernelReleaseDirectMemory(physical, SparseSlot);
        return false;
    }
    if (kept != 0 && sceKernelReleaseDirectMemory(range.physical[slot], kept * FinePage) != 0) std::abort();
    range.physical[slot] = physical;
#else
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ | PROT_WRITE) != 0) return false;
#endif
    guest_committed.fetch_add(SparseSlot - kept * FinePage, std::memory_order_relaxed);
    const unsigned times = std::min<unsigned>(((range.fine[slot] >> 8) & 0xf) + 1, FineBackoffMax);
    range.fine[slot] = static_cast<std::uint16_t>(times << 8);
    range.fine_after[slot] = static_cast<std::uint32_t>(counters.passes.load(std::memory_order_relaxed)) + (1u << times);
    counters.unfined.fetch_add(1, std::memory_order_relaxed);
    return true;
}

#ifdef PS5_NATIVE
bool MapRun(std::uintptr_t at, std::size_t bytes, std::int64_t physical) noexcept {
    void* address = reinterpret_cast<void*>(at);
    return sceKernelMapDirectMemory(&address, bytes, PROT_READ, MAP_FIXED, physical, FinePage) == 0 &&
           address == reinterpret_cast<void*>(at);
}
#endif

// Own -> Fine (the slot is locked and fenced, and has `zero` marked in a mask of its pages): its other
// pages go into a compact block, and the slot is mapped again page run by page run, read-only.
// The 2 MiB mapping is taken away whole first (a large mapping is never split); a thread that
// touches the slot meanwhile faults, waits for the lock and repeats its access.
bool MakeFine(SparseRange& range, std::size_t slot, const bool (&zero)[FinePages], std::size_t kept) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
#ifdef PS5_NATIVE
    std::int64_t compact = -1;
    if (eden_direct_low(kept * FinePage, FinePage, 12, &compact) != 0) return false;
    void* view = reinterpret_cast<void*>(cpu_mapping_hint);
    if (sceKernelMapDirectMemory(&view, kept * FinePage, PROT_READ | PROT_WRITE, 0, compact, FinePage) != 0) {
        (void)sceKernelReleaseDirectMemory(compact, kept * FinePage);
        return false;
    }
    std::size_t next = 0;
    for (std::size_t page = 0; page < FinePages; ++page)
        if (!zero[page]) {
            std::memcpy(static_cast<std::uint8_t*>(view) + next * FinePage,
                        reinterpret_cast<const void*>(at + page * FinePage), FinePage);
            ++next;
        }
    if (munmap(view, kept * FinePage) != 0) std::abort();
    const std::int64_t whole = range.physical[slot];
    if (munmap(reinterpret_cast<void*>(at), SparseSlot) != 0) std::abort();
    bool ok = true;
    next = 0;
    for (std::size_t page = 0; page < FinePages && ok;) {
        std::size_t end = page + 1;
        while (end < FinePages && zero[end] == zero[page]) ++end;
        const std::size_t bytes = (end - page) * FinePage;
        ok = MapRun(at + page * FinePage, bytes, zero[page] ? zero_block : compact + std::int64_t(next * FinePage));
        if (!zero[page]) next += end - page;
        page = end;
    }
    if (!ok) {
        // The slot shows its block again, as it was (fenced: read-only until the caller unfences).
        if (!MapSlot(at, PROT_READ, whole)) std::abort();
        (void)sceKernelReleaseDirectMemory(compact, kept * FinePage);
        return false;
    }
    if (sceKernelReleaseDirectMemory(whole, SparseSlot) != 0) std::abort();
    range.physical[slot] = compact;
#else
    for (std::size_t page = 0; page < FinePages; ++page)
        if (zero[page] && madvise(reinterpret_cast<void*>(at + page * FinePage), FinePage, MADV_DONTNEED) != 0)
            std::abort();
#endif
    guest_committed.fetch_sub(SparseSlot - kept * FinePage, std::memory_order_relaxed);
    range.fine[slot] = static_cast<std::uint16_t>((range.fine[slot] & 0xf00) | kept);
    counters.fined.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// CommitSparsePage for a range with slot states.
bool ServiceCommit(SparseRange& range, std::size_t slot) noexcept {
    LockSlot(range, slot);
    const std::uint8_t state = SlotState(range, slot);
    const bool ok = state == SlotZero ? PromoteZero(range, slot) : state == SlotFine ? PromoteFine(range, slot) : true;
    UnlockSlot(range, slot, ok && (state == SlotZero || state == SlotFine) ? SlotOwn : state);
    return ok;
}

// ---- The fault ----
SparseRange* WriteRangeOf(std::uintptr_t address) noexcept {
    for (SparseRange* range : {service_range.load(std::memory_order_acquire), probe_range.load(std::memory_order_acquire)}) {
        if (range == nullptr || range->state == nullptr) continue;
        const auto begin = range->begin.load(std::memory_order_acquire);
        if (begin != 0 && address >= begin && address < range->end) return range;
    }
    return nullptr;
}
std::atomic<std::uintptr_t> probe_expected{0}; // the self-test's write, whatever si_addr says
std::atomic<bool> probe_mismatch{false};
} // namespace

// A write fault at `address`: true when it was a write to a slot of the guest's memory that is
// now writable (the faulting instruction repeats), false for anything else.
bool HandleGuestWriteFault(void* fault) noexcept {
    auto address = reinterpret_cast<std::uintptr_t>(fault);
    if (const auto expected = probe_expected.load(std::memory_order_acquire); expected != 0 && address != expected) {
        probe_mismatch.store(true, std::memory_order_relaxed);
        address = expected;
    }
    SparseRange* const range = WriteRangeOf(address);
    if (range == nullptr) return false;
    // The same address faulting again and again is not a slot to promote but a real fault.
    static thread_local std::uintptr_t last = 0;
    static thread_local unsigned repeats = 0;
    repeats = address == last ? repeats + 1 : 1;
    last = address;
    if (repeats > 16) {
        repeats = 0;
        return false;
    }
    counters.faults.fetch_add(1, std::memory_order_relaxed);
    const std::size_t slot = (address - range->begin.load(std::memory_order_relaxed)) / SparseSlot;
    const bool waited = LockSlot(*range, slot, true);
    const std::uint8_t state = SlotState(*range, slot);
    if (waited) {
        // Someone changed the slot meanwhile (or is mapping it again): the access may now succeed,
        // and a read must never make a Fine slot whole. It faults again if it was a write.
        UnlockSlot(*range, slot, state);
        return true;
    }
    bool ok = true;
    if (state == SlotFine) {
        ok = PromoteFine(*range, slot);
    } else if (state == SlotZero) {
        ok = PromoteZero(*range, slot);
        if (ok) counters.promotions.fetch_add(1, std::memory_order_relaxed);
    } else if (state == SlotShared) {
        ok = PromoteShared(*range, slot);
    }
    UnlockSlot(*range, slot, ok ? SlotOwn : state);
    if (!ok) {
        counters.failures.fetch_add(1, std::memory_order_relaxed);
        Note("EDEN_CRAM out of memory: a written slot of the guest's memory found no block\n");
    }
    return ok;
}

namespace {
struct sigaction service_previous_segv{}, service_previous_bus{};

void ServiceSignal(int signal, siginfo_t* info, void* context) {
    if (HandleGuestWriteFault(info ? info->si_addr : nullptr)) return;
    const struct sigaction& previous = signal == SIGSEGV ? service_previous_segv : service_previous_bus;
    if (previous.sa_flags & SA_SIGINFO) {
        previous.sa_sigaction(signal, info, context);
    } else if (previous.sa_handler == SIG_DFL) {
        std::signal(signal, SIG_DFL); // the faulting instruction repeats and ends the process
    } else if (previous.sa_handler != SIG_IGN) {
        previous.sa_handler(signal);
    }
}

bool InstallServiceSignal() noexcept {
    static std::once_flag once;
    static bool installed = false;
    std::call_once(once, [] {
        struct sigaction action{};
        action.sa_sigaction = ServiceSignal;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        installed = sigaction(SIGSEGV, &action, &service_previous_segv) == 0 &&
                    sigaction(SIGBUS, &action, &service_previous_bus) == 0;
    });
    return installed;
}

// Gives a range slot states (a guest range, or the self-test's).
bool AddStates(SparseRange& range) noexcept {
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    auto* state = new (std::nothrow) std::atomic<std::uint8_t>[slots];
    auto* fine = state ? new (std::nothrow) std::uint16_t[slots]() : nullptr;
    auto* fine_after = fine ? new (std::nothrow) std::uint32_t[slots]() : nullptr;
    if (fine_after == nullptr) {
        delete[] state;
        delete[] fine;
        return false;
    }
    range.fine = fine;
    range.fine_after = fine_after;
    for (std::size_t slot = 0; slot < slots; ++slot)
        state[slot].store(range.owned[slot].load(std::memory_order_relaxed) ? SlotOwn : SlotZero, std::memory_order_relaxed);
    range.state = state;
    return true;
}

// ---- The scanner ----
std::thread scanner;
std::mutex scanner_mutex;
std::condition_variable scanner_wake;
bool scanner_stop = false;

std::uint64_t Mix(std::uint64_t hash, std::uint64_t word) noexcept {
    hash ^= word + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash * 0xff51afd7ed558ccdull;
}
std::uint64_t SampleHash(std::uintptr_t at) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    std::uint64_t hash = 0;
    for (std::size_t i = 0; i < SparseSlot / 8; i += SparseSlot / 8 / 64) hash = Mix(hash, word[i] ^ word[i + 1]);
    return hash;
}
std::uint64_t FullHash(std::uintptr_t at, std::size_t bytes) noexcept {
    const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
    std::uint64_t hash = 0;
    for (std::size_t i = 0; i < bytes / 8; ++i) hash = Mix(hash, word[i]);
    return hash;
}

void Fence(std::uintptr_t at) noexcept {
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ) != 0) std::abort();
}
void Unfence(std::uintptr_t at) noexcept {
    if (mprotect(reinterpret_cast<void*>(at), SparseSlot, PROT_READ | PROT_WRITE) != 0) std::abort();
}

// An Own slot that holds only zeroes goes back. Fenced while it is looked at again.
bool ScanZero(SparseRange& range, std::size_t slot) noexcept {
    LockSlot(range, slot);
    if (SlotState(range, slot) != SlotOwn) {
        UnlockSlot(range, slot, SlotState(range, slot));
        return false;
    }
    const std::uintptr_t at = SlotAddress(range, slot);
    Fence(at);
    const bool zero = AllZero(at);
    if (zero) ReturnSlot(range, slot);
    else Unfence(at);
    UnlockSlot(range, slot, zero ? SlotZero : SlotOwn);
    return zero;
}

// An Own slot with many pages of zeroes becomes Fine. A racy count first, then the count again fenced.
bool ScanFine(SparseRange& range, std::size_t slot) noexcept {
    const std::uintptr_t at = SlotAddress(range, slot);
    if (counters.passes.load(std::memory_order_relaxed) < range.fine_after[slot]) return false;
    std::size_t zeros = 0;
    for (std::size_t page = 0; page < FinePages; ++page) zeros += PageZero(at + page * FinePage);
    if (zeros < FineMinZeroPages) return false;
    LockSlot(range, slot);
    if (SlotState(range, slot) != SlotOwn) {
        UnlockSlot(range, slot, SlotState(range, slot));
        return false;
    }
    Fence(at);
    bool zero[FinePages];
    zeros = 0;
    for (std::size_t page = 0; page < FinePages; ++page) zeros += zero[page] = PageZero(at + page * FinePage);
    bool fined = false;
    if (zeros == FinePages) {
        ReturnSlot(range, slot);
        UnlockSlot(range, slot, SlotZero);
        counters.returns_scan.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (zeros >= FineMinZeroPages) {
        fined = MakeFine(range, slot, zero, FinePages - zeros);
        if (!fined) counters.fine_failures.fetch_add(1, std::memory_order_relaxed);
    }
    if (!fined) Unfence(at);
    UnlockSlot(range, slot, fined ? SlotFine : SlotOwn);
    return fined;
}

// Two slots of the same bytes show one block (the console; the host only counts them).
bool ScanShare(SparseRange& range, std::size_t keep, std::size_t other) noexcept {
    const std::size_t first = std::min(keep, other), second = std::max(keep, other);
    LockSlot(range, first);
    LockSlot(range, second);
    const std::uint8_t keep_state = SlotState(range, keep), other_state = SlotState(range, other);
    bool shared = false;
    if ((keep_state == SlotOwn || keep_state == SlotShared) && other_state == SlotOwn) {
        const std::uintptr_t keep_at = SlotAddress(range, keep), other_at = SlotAddress(range, other);
        if (keep_state == SlotOwn) Fence(keep_at);
        Fence(other_at);
        const bool same = std::memcmp(reinterpret_cast<const void*>(keep_at), reinterpret_cast<const void*>(other_at),
                                      SparseSlot) == 0;
#ifdef PS5_NATIVE
        // A block becoming shared counts both slots (the first ShareBlock makes it 1, the second 2); one
        // already shared counts one more. A table that is full shares nothing.
        bool counted = false;
        if (same && ShareBlock(range.physical[keep])) {
            counted = keep_state == SlotShared || ShareBlock(range.physical[keep]);
            if (!counted) (void)UnshareBlock(range.physical[keep]);
        }
        if (counted) {
            if (!MapSlot(other_at, PROT_READ, range.physical[keep])) std::abort();
            if (sceKernelReleaseDirectMemory(range.physical[other], SparseSlot) != 0) std::abort();
            range.physical[other] = range.physical[keep];
            guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
            shared = true;
        }
#else
        (void)same;
#endif
        if (!shared) {
            if (keep_state == SlotOwn) Unfence(keep_at);
            Unfence(other_at);
        }
        if (same) counters.dedup.fetch_add(1, std::memory_order_relaxed);
    }
    UnlockSlot(range, second, second == keep ? (shared ? SlotShared : keep_state) : (shared ? SlotShared : other_state));
    UnlockSlot(range, first, first == keep ? (shared ? SlotShared : keep_state) : (shared ? SlotShared : other_state));
    return shared;
}

void ScanPass(SparseRange& range) noexcept {
    const auto start = std::chrono::steady_clock::now();
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    std::vector<std::pair<std::uint64_t, std::size_t>> samples;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (SlotState(range, slot) != SlotOwn) continue;
        const std::uintptr_t at = SlotAddress(range, slot);
        if (AllZero(at)) { // a read without the lock: only a hint, checked again fenced
            if (ScanZero(range, slot)) counters.returns_scan.fetch_add(1, std::memory_order_relaxed);
        } else if (fine_requested.load(std::memory_order_relaxed) && ScanFine(range, slot)) {
            // now Fine
        } else if (dedup_requested.load(std::memory_order_relaxed)) {
            samples.emplace_back(SampleHash(at), slot);
        }
    }
    std::sort(samples.begin(), samples.end());
    for (std::size_t i = 0; i < samples.size();) {
        std::size_t j = i + 1;
        while (j < samples.size() && samples[j].first == samples[i].first) ++j;
        if (j - i > 1) {
            std::vector<std::pair<std::uint64_t, std::size_t>> full;
            for (std::size_t k = i; k < j; ++k)
                full.emplace_back(FullHash(SlotAddress(range, samples[k].second), SparseSlot), samples[k].second);
            std::sort(full.begin(), full.end());
            for (std::size_t a = 0; a < full.size();) {
                std::size_t b = a + 1;
                while (b < full.size() && full[b].first == full[a].first) {
                    (void)ScanShare(range, full[a].second, full[b].second);
                    ++b;
                }
                a = b;
            }
        }
        i = j;
    }
    counters.passes.fetch_add(1, std::memory_order_relaxed);
    counters.scan_ns.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
}

void ServiceReport(SparseRange& range, const char* when) noexcept {
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    std::size_t own = 0, shared = 0, fine = 0, fine_kept = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        const auto state = SlotState(range, slot);
        own += state == SlotOwn;
        shared += state == SlotShared;
        if (state == SlotFine) {
            ++fine;
            fine_kept += FineKept(range, slot);
        }
    }
    char line[640];
    const int length = std::snprintf(line, sizeof(line),
        "EDEN_CRAM %s held=%zuMiB own=%zu shared=%zu zero=%zu faults=%llu promotions=%llu copies=%llu waits=%llu "
        "returns_clear=%llu returns_free=%llu returns_scan=%llu dedup=%llu reserve_used=%llu failures=%llu "
        "passes=%llu scan_ms=%llu fine=%zu fine_held=%zuMiB fined=%llu unfined=%llu fine_failures=%llu\n",
        when, guest_committed.load(std::memory_order_relaxed) >> 20, own, shared, slots - own - shared - fine,
        (unsigned long long)counters.faults.load(), (unsigned long long)counters.promotions.load(),
        (unsigned long long)counters.copies.load(), (unsigned long long)counters.waits.load(),
        (unsigned long long)counters.returns_clear.load(), (unsigned long long)counters.returns_free.load(),
        (unsigned long long)counters.returns_scan.load(), (unsigned long long)counters.dedup.load(),
        (unsigned long long)counters.reserve_used.load(), (unsigned long long)counters.failures.load(),
        (unsigned long long)counters.passes.load(), (unsigned long long)(counters.scan_ns.load() / 1000000),
        fine, fine_kept * FinePage >> 20, (unsigned long long)counters.fined.load(),
        (unsigned long long)counters.unfined.load(), (unsigned long long)counters.fine_failures.load());
    if (length > 0) Report(line);
}

// What finer slots would give: 16 KiB pages of the Own slots that hold only zeroes, and those that
// repeat another page (by a full hash). Read once, when the session ends.
void MeasureFinePages(SparseRange& range) noexcept {
    constexpr std::size_t Fine = 16384;
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    std::vector<std::uint64_t> hashes;
    std::size_t pages = 0, zero = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (SlotState(range, slot) != SlotOwn) continue;
        for (std::size_t offset = 0; offset < SparseSlot; offset += Fine) {
            const std::uintptr_t at = SlotAddress(range, slot) + offset;
            ++pages;
            const auto* word = reinterpret_cast<const volatile std::uint64_t*>(at);
            bool is_zero = true;
            for (std::size_t i = 0; i < Fine / 8 && is_zero; ++i) is_zero = word[i] == 0;
            if (is_zero) ++zero;
            else hashes.push_back(FullHash(at, Fine));
        }
    }
    std::sort(hashes.begin(), hashes.end());
    std::size_t repeats = 0;
    for (std::size_t i = 1; i < hashes.size(); ++i) repeats += hashes[i] == hashes[i - 1];
    char line[192];
    const int length = std::snprintf(line, sizeof(line),
        "EDEN_CRAM_FINE pages16k=%zu zero=%zu (%zuMiB) repeats=%zu (%zuMiB)\n", pages, zero, zero * Fine >> 20, repeats,
        repeats * Fine >> 20);
    if (length > 0) Report(line);
}

void ScannerLoop(SparseRange* range) {
    auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::unique_lock lock{scanner_mutex};
    while (!scanner_stop) {
        scanner_wake.wait_for(lock, std::chrono::seconds(20), [] { return scanner_stop; });
        if (scanner_stop) break;
        lock.unlock();
        if (scan_requested.load(std::memory_order_relaxed)) ScanPass(*range);
#ifdef PS5_NATIVE
        FillReserve();
#endif
        if (std::chrono::steady_clock::now() >= next_report) {
            ServiceReport(*range, "now");
            next_report = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        }
        lock.lock();
    }
}

// Once: does a write to a slot that shows the zero block come back here, with its address, and
// resume? A refusal keeps the guest's memory committed when it is handed out, as before.
bool ServiceAvailable() noexcept {
    int state = service_state.load(std::memory_order_acquire);
    if (state != 0) return state == 1;
    const char* step = "ok";
    long long fault_ns = -1;
    void* base = nullptr;
    if (!SparseAvailable()) step = "sparse";
    else if (!InstallServiceSignal()) step = "install";
    else if ((base = NewSparseRange(2 * SparseSlot, false)) == nullptr) step = "reserve";
    if (base != nullptr) {
        SparseRange* range = nullptr;
        {
            const std::lock_guard lock{sparse_mutex};
            range = SparseRangeOf(reinterpret_cast<std::uintptr_t>(base));
        }
        if (range == nullptr || !AddStates(*range)) {
            step = "states";
        } else {
            probe_range.store(range, std::memory_order_release);
            auto* word = reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uint8_t*>(base) + SparseSlot + 64);
            const auto before = counters.faults.load(std::memory_order_relaxed);
            probe_expected.store(reinterpret_cast<std::uintptr_t>(word), std::memory_order_release);
            const auto start = std::chrono::steady_clock::now();
            *word = 0x5a17ed5a17ed5a17ull; // faults once: the slot shows the zero block
            fault_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            probe_expected.store(0, std::memory_order_release);
            if (*word != 0x5a17ed5a17ed5a17ull) step = "value";
            else if (counters.faults.load(std::memory_order_relaxed) != before + 1) step = "fault-count";
            else if (probe_mismatch.load(std::memory_order_relaxed)) step = "fault-address";
            else if (SlotState(*range, 1) != SlotOwn || SlotState(*range, 0) != SlotZero) step = "state";
            else {
                // Back to the zero block, then written again: a slot comes and goes.
                LockSlot(*range, 1);
                ReturnSlot(*range, 1);
                UnlockSlot(*range, 1, SlotZero);
                if (*word != 0) step = "return";
                else {
                    *word = 7;
                    if (*word != 7 || SlotState(*range, 1) != SlotOwn) step = "again";
                }
            }
            if (std::strcmp(step, "ok") == 0) {
                // Fenced and unfenced: read while fenced, written after.
                const std::uintptr_t at = SlotAddress(*range, 1);
                Fence(at);
                if (*word != 7) step = "fence-read";
                Unfence(at);
                *word = 9;
                if (std::strcmp(step, "ok") == 0 && *word != 9) step = "unfence";
            }
            if (std::strcmp(step, "ok") == 0) {
                // Two slots of the same bytes: one block, then copied apart by a write, then the last
                // one takes the block back.
                auto* other = reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uint8_t*>(base) + 64);
                *other = 9;
                const bool shared = ScanShare(*range, 1, 0);
#ifdef PS5_NATIVE
                if (!shared || SlotState(*range, 0) != SlotShared || SlotState(*range, 1) != SlotShared) step = "share";
                else if (*other != 9 || *word != 9) step = "share-read";
                else {
                    *other = 10; // a copy of its own
                    if (*other != 10 || *word != 9 || SlotState(*range, 0) != SlotOwn) step = "share-copy";
                    else {
                        *word = 11; // the last one: the block is its own again
                        if (*word != 11 || *other != 10 || SlotState(*range, 1) != SlotOwn) step = "share-last";
                    }
                }
#else
                if (shared) step = "share-host";
#endif
            }
            if (std::strcmp(step, "ok") == 0) {
                // Slot 1 with one page of data at page 3: Fine (one 16 KiB page kept), read in place,
                // then a write makes it whole again with every byte where it was.
                LockSlot(*range, 1);
                if (SlotState(*range, 1) != SlotZero) ReturnSlot(*range, 1);
                UnlockSlot(*range, 1, SlotZero);
                auto* page3 = reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uint8_t*>(base) + SparseSlot + 3 * FinePage + 8);
                *page3 = 0x0f1e2d3c4b5a6978ull;
                const auto held = guest_committed.load(std::memory_order_relaxed);
                if (!ScanFine(*range, 1) || SlotState(*range, 1) != SlotFine) step = "fine";
                else if (*page3 != 0x0f1e2d3c4b5a6978ull || *word != 0) step = "fine-read";
                else if (held - guest_committed.load(std::memory_order_relaxed) != SparseSlot - FinePage) step = "fine-held";
                else {
                    *word = 13; // page 0: the slot is whole again
                    if (*word != 13 || *page3 != 0x0f1e2d3c4b5a6978ull || SlotState(*range, 1) != SlotOwn ||
                        (range->fine[1] & 0xf00) == 0)
                        step = "fine-write";
                }
            }
            for (std::size_t slot = 0; slot < 2; ++slot)
                if (SlotState(*range, slot) != SlotZero) {
                    LockSlot(*range, slot);
                    ReturnSlot(*range, slot);
                    UnlockSlot(*range, slot, SlotZero);
                }
            probe_range.store(nullptr, std::memory_order_release);
        }
        FreeSparsePages(base, 2 * SparseSlot);
    }
    ResetCounters();
    const bool ok = std::strcmp(step, "ok") == 0;
    service_refused = step;
    char line[128];
    const int length = std::snprintf(line, sizeof(line), "EDEN_CRAM_CHECK ok=%d step=%s fault_ns=%lld\n", ok ? 1 : 0,
                                     step, fault_ns);
    if (length > 0) Report(line);
    service_state.store(ok ? 1 : 2, std::memory_order_release);
    return ok;
}

// The guest range starts being a service (when the platform passed the check).
void ServiceStart(SparseRange& range) noexcept {
    if (!service_requested.load(std::memory_order_relaxed) || !ServiceAvailable() || !AddStates(range)) {
        char line[96];
        const int length = std::snprintf(line, sizeof(line), "EDEN_CRAM off: %s\n",
                                         !service_requested.load() ? "switched-off" : service_refused);
        if (length > 0) Report(line);
        return;
    }
    ResetCounters();
    range.promote = true;
#ifdef PS5_NATIVE
    FillReserve();
#endif
    service_range.store(&range, std::memory_order_release);
    Eden::Fastmem::guest_write_fault.store(&HandleGuestWriteFault, std::memory_order_release);
    {
        const std::lock_guard lock{scanner_mutex};
        scanner_stop = false;
    }
    scanner = std::thread(ScannerLoop, &range);
    char line[128];
    const int length = std::snprintf(line, sizeof(line), "EDEN_CRAM on: scan=%d dedup=%d fine=%d reserve=%zuMiB\n",
                                     scan_requested.load() ? 1 : 0, dedup_requested.load() ? 1 : 0,
                                     fine_requested.load() ? 1 : 0,
                                     reserve_count.load() * (SparseSlot >> 20));
    if (length > 0) Report(line);
}

void ServiceStop(SparseRange& range) noexcept {
    if (service_range.load(std::memory_order_acquire) != &range) return;
    {
        const std::lock_guard lock{scanner_mutex};
        scanner_stop = true;
    }
    scanner_wake.notify_all();
    if (scanner.joinable()) scanner.join();
    ServiceReport(range, "end");
    MeasureFinePages(range);
    service_range.store(nullptr, std::memory_order_release);
#ifdef PS5_NATIVE
    EmptyReserve();
#endif
}

// What a guest range keeps when it is given back: each block once, shared ones too.
void ReleaseServiceBlocks(SparseRange& range) noexcept {
#ifdef PS5_NATIVE
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        const auto state = SlotState(range, slot);
        if (state == SlotFine) {
            const std::size_t kept = FineKept(range, slot);
            if (kept != 0 && sceKernelReleaseDirectMemory(range.physical[slot], kept * FinePage) != 0) std::abort();
            guest_committed.fetch_sub(kept * FinePage, std::memory_order_relaxed);
        } else if (state == SlotOwn) {
            if (sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
        } else if (state == SlotShared && UnshareBlock(range.physical[slot]) == 0) {
            if (sceKernelReleaseDirectMemory(range.physical[slot], SparseSlot) != 0) std::abort();
            guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
        }
        if (state == SlotOwn) guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
        range.owned[slot].store(false, std::memory_order_relaxed);
    }
#else
    const std::size_t slots = (range.end - range.begin.load(std::memory_order_relaxed)) / SparseSlot;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (SlotState(range, slot) == SlotOwn) guest_committed.fetch_sub(SparseSlot, std::memory_order_relaxed);
        if (SlotState(range, slot) == SlotFine) guest_committed.fetch_sub(FineKept(range, slot) * FinePage, std::memory_order_relaxed);
        range.owned[slot].store(false, std::memory_order_relaxed);
    }
#endif
}
} // namespace

// ---- What the guest's kernel tells the service ----

// Development switches (dev-settings cram=off, cram_scan=off, cram_dedup=off), before the guest's
// memory is taken.
void SetGuestMemoryService(bool on, bool scan, bool dedup) noexcept {
    service_requested.store(on, std::memory_order_relaxed);
    scan_requested.store(scan, std::memory_order_relaxed);
    dedup_requested.store(dedup, std::memory_order_relaxed);
}

// Settings > Performance > Compact memory (off unless chosen): written slots with many pages of
// zeroes keep only the others. It gives back more memory, and a game that writes such a slot again
// waits for it to be made whole (a fault and a 2 MiB copy, about 0.3 ms).
void SetGuestMemoryFine(bool on) noexcept {
    fine_requested.store(on, std::memory_order_relaxed);
}

// The guest's kernel hands pages out: they are committed now only without the service.
void CommitGuestMemoryOnHandOut(std::uint64_t offset, std::size_t bytes) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range != nullptr && range->promote) return;
    CommitGuestMemory(offset, bytes);
}

// The guest's kernel fills pages (`at` is the backing at `offset`). With the service, a fill with
// zeroes leaves Zero slots alone and gives whole Own or Shared slots back; anything else is written.
void ClearGuestMemory(std::uint8_t* at, std::uint64_t offset, std::size_t bytes, int fill) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range == nullptr || !range->promote || fill != 0) {
        if (range != nullptr && !range->promote) CommitGuestMemory(offset, bytes);
        std::memset(at, fill, bytes); // (with the service, a write to a Zero slot promotes it)
        return;
    }
    const std::uint64_t end = offset + bytes;
    for (std::uint64_t cursor = offset; cursor < end;) {
        const std::size_t slot = static_cast<std::size_t>(cursor / SparseSlot);
        const std::uint64_t slot_end = (cursor / SparseSlot + 1) * SparseSlot;
        const std::uint64_t piece_end = std::min(end, slot_end);
        const bool whole = cursor % SparseSlot == 0 && piece_end == slot_end;
        if (whole) {
            LockSlot(*range, slot);
            const auto state = SlotState(*range, slot);
            if (state != SlotZero) {
                ReturnSlot(*range, slot);
                counters.returns_clear.fetch_add(1, std::memory_order_relaxed);
            }
            UnlockSlot(*range, slot, SlotZero);
        } else if (SlotState(*range, slot) != SlotZero) {
            std::memset(at + (cursor - offset), 0, static_cast<std::size_t>(piece_end - cursor));
        }
        cursor = piece_end;
    }
}

// A page the guest's kernel fills one at a time (optimized allocation): the same rule.
void FillGuestMemory(std::uint8_t* at, int fill, std::size_t bytes) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range != nullptr && range->promote && fill == 0) {
        const auto begin = range->begin.load(std::memory_order_relaxed);
        const auto address = reinterpret_cast<std::uintptr_t>(at);
        if (address >= begin && address < range->end && SlotState(*range, (address - begin) / SparseSlot) == SlotZero)
            return;
    }
    std::memset(at, fill, bytes);
}

// The guest's kernel freed pages: every slot they cover whole goes back.
void GuestMemoryFreed(std::uint64_t offset, std::size_t bytes) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range == nullptr || !range->promote || bytes < SparseSlot) return;
    const std::uint64_t first = (offset + SparseSlot - 1) / SparseSlot;
    const std::uint64_t last = (offset + bytes) / SparseSlot; // exclusive
    for (std::uint64_t slot = first; slot < last; ++slot) {
        LockSlot(*range, static_cast<std::size_t>(slot));
        if (SlotState(*range, static_cast<std::size_t>(slot)) != SlotZero) {
            ReturnSlot(*range, static_cast<std::size_t>(slot));
            counters.returns_free.fetch_add(1, std::memory_order_relaxed);
        }
        UnlockSlot(*range, static_cast<std::size_t>(slot), SlotZero);
    }
}

// For the host checks: one scan pass now, and a slot's state (0 Zero, 1 Own, 2 Shared, -1 none).
void GuestMemoryScanNow() noexcept {
    if (SparseRange* const range = service_range.load(std::memory_order_acquire)) ScanPass(*range);
}
int GuestMemorySlotState(std::uint64_t offset) noexcept {
    SparseRange* const range = guest_range.load(std::memory_order_acquire);
    if (range == nullptr || range->state == nullptr) return -1;
    return SlotState(*range, static_cast<std::size_t>(offset / SparseSlot));
}
