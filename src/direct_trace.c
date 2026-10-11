// SPDX-License-Identifier: GPL-3.0-or-later
// Test only: who holds direct memory. Every allocation and release of the app, its libraries and
// the graphics driver passes here (the link wraps the four calls); the blocks alive now can be
// listed with the address they were taken from.
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int __real_sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
int __real_sceKernelAllocateMainDirectMemory(size_t, size_t, int, int64_t *);
int __real_sceKernelReleaseDirectMemory(int64_t, size_t);
int __real_sceKernelCheckedReleaseDirectMemory(int64_t, size_t);

struct eden_direct_block { int64_t start; size_t size; void *caller; int type; };
static struct eden_direct_block eden_direct_blocks[4096];
static atomic_flag eden_direct_lock = ATOMIC_FLAG_INIT;
static unsigned eden_direct_lost, eden_direct_unknown;

static void eden_direct_take(int64_t start, size_t size, int type, void *caller) {
  while (atomic_flag_test_and_set_explicit(&eden_direct_lock, memory_order_acquire)) {}
  unsigned i = 0;
  while (i < 4096 && eden_direct_blocks[i].size != 0) ++i;
  if (i < 4096) eden_direct_blocks[i] = (struct eden_direct_block){start, size, caller, type};
  else ++eden_direct_lost;
  atomic_flag_clear_explicit(&eden_direct_lock, memory_order_release);
}

static void eden_direct_give(int64_t start, size_t size) {
  while (atomic_flag_test_and_set_explicit(&eden_direct_lock, memory_order_acquire)) {}
  unsigned i = 0;
  while (i < 4096 && !(eden_direct_blocks[i].size != 0 && eden_direct_blocks[i].start == start)) ++i;
  if (i < 4096 && eden_direct_blocks[i].size == size) {
    eden_direct_blocks[i].size = 0;
  } else if (i < 4096 && size < eden_direct_blocks[i].size) {  // the front of a block
    eden_direct_blocks[i].start += (int64_t)size;
    eden_direct_blocks[i].size -= size;
  } else {
    ++eden_direct_unknown;
  }
  atomic_flag_clear_explicit(&eden_direct_lock, memory_order_release);
}

int __wrap_sceKernelAllocateDirectMemory(int64_t from, int64_t to, size_t size, size_t alignment, int type,
                                         int64_t *start) {
  const int result = __real_sceKernelAllocateDirectMemory(from, to, size, alignment, type, start);
  if (result == 0 && start) eden_direct_take(*start, size, type, __builtin_return_address(0));
  return result;
}

int __wrap_sceKernelAllocateMainDirectMemory(size_t size, size_t alignment, int type, int64_t *start) {
  const int result = __real_sceKernelAllocateMainDirectMemory(size, alignment, type, start);
  if (result == 0 && start) eden_direct_take(*start, size, type, __builtin_return_address(0));
  return result;
}

int __wrap_sceKernelReleaseDirectMemory(int64_t start, size_t size) {
  const int result = __real_sceKernelReleaseDirectMemory(start, size);
  if (result == 0) eden_direct_give(start, size);
  return result;
}

int __wrap_sceKernelCheckedReleaseDirectMemory(int64_t start, size_t size) {
  const int result = __real_sceKernelCheckedReleaseDirectMemory(start, size);
  if (result == 0) eden_direct_give(start, size);
  return result;
}

// Not from inside an allocator: it prints.
void eden_direct_trace_dump(const char *phase) {
  static struct eden_direct_block copy[4096];
  while (atomic_flag_test_and_set_explicit(&eden_direct_lock, memory_order_acquire)) {}
  for (unsigned i = 0; i < 4096; ++i) copy[i] = eden_direct_blocks[i];
  const unsigned lost = eden_direct_lost, unknown = eden_direct_unknown;
  atomic_flag_clear_explicit(&eden_direct_lock, memory_order_release);
  unsigned count = 0;
  for (unsigned i = 0; i < 4096; ++i) {
    if (copy[i].size == 0) continue;
    ++count;
    printf("EDEN_DIRECT_LIVE phase=%s start=%llx kib=%zu type=%d caller=%p\n", phase,
           (unsigned long long)copy[i].start, copy[i].size >> 10, copy[i].type, copy[i].caller);
  }
  printf("EDEN_DIRECT_TRACE phase=%s live=%u lost=%u unknown_releases=%u anchor=%p\n", phase, count, lost, unknown,
         (void *)&eden_direct_trace_dump);
}
