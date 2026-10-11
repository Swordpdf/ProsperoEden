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
class KernelSpinLock {
public:
    KernelSpinLock() noexcept = default;
    KernelSpinLock(const KernelSpinLock&) = delete;
    KernelSpinLock& operator=(const KernelSpinLock&) = delete;

    void lock() noexcept {
        while (flag.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__)
            _mm_pause();
#endif
        }
    }
    void unlock() noexcept { flag.clear(std::memory_order_release); }
    [[nodiscard]] bool try_lock() noexcept { return !flag.test_and_set(std::memory_order_acquire); }

private:
    union {
        std::atomic_flag flag{};
        alignas(std::mutex) unsigned char same_size_as_a_mutex[sizeof(std::mutex)];
    };
};
static_assert(sizeof(KernelSpinLock) == sizeof(std::mutex) && alignof(KernelSpinLock) == alignof(std::mutex));
} // namespace Eden
