// SPDX-License-Identifier: GPL-3.0-or-later
// The lock Eden's kernel used for a guest thread's context and for its slab heaps until it made
// them std::mutex. A thread's context guard is taken on the host thread that switches to the
// guest thread and released on the host thread that switches away from it, and it is still held
// when the thread's object goes back to its slab. A std::mutex allows neither: on the PS5 (as on
// FreeBSD) it is an object of the thread library that knows its owner, so the release from
// another host thread fails and the lock stays taken, and destroying the KThread destroys a lock
// that is in use. This one is a flag, as before. It has the size and alignment of std::mutex, so
// that a source file that sees Eden's own header lays the objects out the same way.
#pragma once
#include <atomic>
#include <mutex>
#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace Eden {
// One wait of a spin loop. PAUSE tells the core it is spinning: the other hardware thread of the
// same core gets its issue slots, and the loop does not flood the memory system with requests.
inline void CpuRelax() noexcept {
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

class KernelSpinLock {
public:
    KernelSpinLock() noexcept = default;
    KernelSpinLock(const KernelSpinLock&) = delete;
    KernelSpinLock& operator=(const KernelSpinLock&) = delete;

    // A waiter only reads the flag until it is clear. Every test_and_set is a write, which takes
    // the line from every other core, including the one that holds the lock and has to clear it.
    void lock() noexcept {
        while (flag.test_and_set(std::memory_order_acquire))
            while (flag.test(std::memory_order_relaxed)) CpuRelax();
    }
    void unlock() noexcept { flag.clear(std::memory_order_release); }
    // The scheduler retries this in a loop while another core holds a thread's context: a taken
    // lock is refused with a read, without a write.
    [[nodiscard]] bool try_lock() noexcept {
        return !flag.test(std::memory_order_relaxed) && !flag.test_and_set(std::memory_order_acquire);
    }

private:
    union {
        std::atomic_flag flag{};
        alignas(std::mutex) unsigned char same_size_as_a_mutex[sizeof(std::mutex)];
    };
};
static_assert(sizeof(KernelSpinLock) == sizeof(std::mutex) && alignof(KernelSpinLock) == alignof(std::mutex));
} // namespace Eden
