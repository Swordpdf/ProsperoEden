// SPDX-License-Identifier: GPL-3.0-or-later
// The lock and condition each of Eden's Vulkan pipelines keeps for the one moment its build ends.
// On the PS5 (as on FreeBSD) a std::mutex or std::condition_variable is an object of the thread
// library, made the first time it is used and kept until it is destroyed, and a process can hold
// only so many. Every pipeline locks and signals its pair once, when its build ends, and keeps it
// for as long as it is cached: a game with thousands of pipelines (13,000 in a large open-world
// game) holds thousands of them, and the next lock anywhere fails with "mutex lock failed: Cannot
// allocate memory" (#204). These take one of a fixed set of shared locks and conditions, chosen
// by the object's address, so the count stays the same however many pipelines a game builds.
// Pipelines that share one only wait a little for each other: the lock is held while a flag is
// set. A shared condition can have waiters of several pipelines, so a signal wakes all of them;
// each checks its own pipeline's flag and waits again if it is not yet built.
#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace Eden {
namespace StripedWait {
inline constexpr std::size_t kSlots = 64;

inline std::size_t SlotOf(const void* object) noexcept {
    // Objects are at least 8-byte aligned and pipelines are allocated far apart: mix the bits so
    // that neighbours do not land on the same slot.
    const auto bits = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(object));
    return static_cast<std::size_t>((bits * 0x9E3779B97F4A7C15ULL) >> 58) % kSlots;
}

inline std::mutex& MutexFor(const void* object) noexcept {
    static std::mutex mutexes[kSlots];
    return mutexes[SlotOf(object)];
}

inline std::condition_variable_any& ConditionFor(const void* object) noexcept {
    static std::condition_variable_any conditions[kSlots];
    return conditions[SlotOf(object)];
}
} // namespace StripedWait

// In place of a std::mutex member: lock() and unlock() go to the shared lock for this address.
class StripedMutex {
public:
    StripedMutex() noexcept = default;
    StripedMutex(const StripedMutex&) = delete;
    StripedMutex& operator=(const StripedMutex&) = delete;

    void lock() { StripedWait::MutexFor(this).lock(); }
    void unlock() noexcept { StripedWait::MutexFor(this).unlock(); }
    [[nodiscard]] bool try_lock() { return StripedWait::MutexFor(this).try_lock(); }

private:
    // The size and alignment of the std::mutex it replaces, as KernelSpinLock keeps them, so that a
    // file that sees Eden's own header lays a pipeline out the same way.
    [[maybe_unused]] alignas(std::mutex) unsigned char same_size_as_a_mutex[sizeof(std::mutex)]{};
};
static_assert(sizeof(StripedMutex) == sizeof(std::mutex) && alignof(StripedMutex) == alignof(std::mutex));

// In place of a std::condition_variable member waited on with a StripedMutex.
class StripedCondition {
public:
    StripedCondition() noexcept = default;
    StripedCondition(const StripedCondition&) = delete;
    StripedCondition& operator=(const StripedCondition&) = delete;

    template <typename Lock, typename Predicate>
    void wait(Lock& lock, Predicate predicate) {
        StripedWait::ConditionFor(this).wait(lock, std::move(predicate));
    }
    // Other pipelines' waiters can share this condition: waking only one could wake the wrong one.
    void notify_one() noexcept { StripedWait::ConditionFor(this).notify_all(); }
    void notify_all() noexcept { StripedWait::ConditionFor(this).notify_all(); }

private:
    [[maybe_unused]] alignas(std::condition_variable) unsigned char same_size_as_a_condition[sizeof(std::condition_variable)]{};
};
static_assert(sizeof(StripedCondition) == sizeof(std::condition_variable) &&
              alignof(StripedCondition) == alignof(std::condition_variable));
} // namespace Eden
