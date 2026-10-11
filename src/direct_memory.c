// SPDX-License-Identifier: GPL-3.0-or-later
// Where the app's long-lived memory goes in the pool the CPU shares with the GPU.
//
// The system gives out direct memory from the bottom of the pool: the lowest room that fits.
// A game's own memory (4 GiB) and its graphics come and go with each session, while two kinds
// of blocks stay for good: the pieces the heap grows by (headless/heap_arenas.inc) and the
// stacks of threads (the Payload SDK takes 2 MiB of direct memory for each, and keeps freed
// ones for the next threads). Taken from the bottom like the rest, those landed between a
// running game's graphics memory, and when the game ended they were left spread over the
// middle of the pool. The next game's graphics get the largest free stretch, which was then
// much smaller than what was free in total, and smaller again after every game.
//
// So blocks that stay are taken from the top of the pool downwards, and everything a session
// takes and gives back is left to the system as before. The link wraps the calls
// (tools/link-headless-native.sh): the heap asks for the top itself (eden_direct_high), and
// the other blocks that stay are known by their size: a thread's stack is 2 MiB, and so is the
// one block the graphics driver keeps for good (its zero block, taken in the middle of a game
// and then left where it was). The app's own 2 MiB blocks, the slots of the guest page tables,
// go back when a game ends: they ask below the wrap (eden_direct_low) and stay where the
// system puts them.
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int __real_sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
int __real_sceKernelReleaseDirectMemory(int64_t, size_t);
int64_t sceKernelGetDirectMemorySize(void);

#define EDEN_DIRECT_STEP ((int64_t)2 << 20)       /* blocks at the top are placed in 2 MiB steps */
#define EDEN_DIRECT_SMALL ((size_t)2 << 20)       /* a thread's stack; the driver's zero block */

static atomic_flag eden_direct_lock = ATOMIC_FLAG_INIT;
static int64_t eden_direct_floor;                 /* the lowest block taken at the top; 0: none yet */
static int64_t eden_direct_holes[1024];           /* 2 MiB blocks given back above the floor */
static unsigned eden_direct_hole_count;

static void eden_direct_acquire(void) {
  while (atomic_flag_test_and_set_explicit(&eden_direct_lock, memory_order_acquire)) {}
}
static void eden_direct_release(void) { atomic_flag_clear_explicit(&eden_direct_lock, memory_order_release); }

/* Test only: every block alive, with the address it was asked from. */
#ifdef EDEN_DIRECT_TRACE
struct eden_direct_block { int64_t start; size_t size; void *caller; };
static struct eden_direct_block eden_direct_blocks[4096];
static void eden_direct_note(int64_t start, size_t size, void *caller) {
  for (unsigned i = 0; i < 4096; ++i) {
    if (caller ? eden_direct_blocks[i].size == 0 : eden_direct_blocks[i].size != 0 && eden_direct_blocks[i].start == start) {
      eden_direct_blocks[i] = (struct eden_direct_block){start, caller ? size : 0, caller};
      return;
    }
  }
}
void eden_direct_trace_dump(const char *phase) {
  for (unsigned i = 0; i < 4096; ++i)
    if (eden_direct_blocks[i].size != 0)
      printf("EDEN_DIRECT_LIVE phase=%s start=%llx kib=%zu caller=%p\n", phase,
             (unsigned long long)eden_direct_blocks[i].start, eden_direct_blocks[i].size >> 10,
             eden_direct_blocks[i].caller);
  printf("EDEN_DIRECT_TRACE phase=%s anchor=%p\n", phase, (void *)&eden_direct_trace_dump);
}
#else
#define eden_direct_note(start, size, caller) ((void)0)
#endif

/* Memory from as high in the pool as there is room; from anywhere when the top has none. */
int eden_direct_high(size_t size, size_t alignment, int type, int64_t *start) {
  const int64_t total = sceKernelGetDirectMemorySize();
  const int64_t bytes = (int64_t)size;
  if (bytes <= 0 || total <= 0) return __real_sceKernelAllocateDirectMemory(0, total, size, alignment, type, start);
  eden_direct_acquire();
  /* A block given back earlier left a hole of its size. */
  while (size == EDEN_DIRECT_SMALL && eden_direct_hole_count != 0) {
    const int64_t hole = eden_direct_holes[--eden_direct_hole_count];
    if (__real_sceKernelAllocateDirectMemory(hole, hole + bytes, size, alignment, type, start) == 0) {
      eden_direct_note(*start, size, __builtin_return_address(0));
      eden_direct_release();
      return 0;
    }
  }
  /* Below what is at the top already; past what something else has put there, for a while. */
  int64_t next = eden_direct_floor != 0 ? eden_direct_floor : total;
  for (unsigned tries = 0; tries < 512 && next >= bytes; ++tries) {
    const int64_t at = (next - bytes) / EDEN_DIRECT_STEP * EDEN_DIRECT_STEP;
    next = at;
    if (__real_sceKernelAllocateDirectMemory(at, at + bytes, size, alignment, type, start) == 0) {
      eden_direct_floor = at;
      eden_direct_note(*start, size, __builtin_return_address(0));
      eden_direct_release();
      return 0;
    }
  }
  eden_direct_release();
  const int result = __real_sceKernelAllocateDirectMemory(0, total, size, alignment, type, start);
  if (result == 0) eden_direct_note(*start, size, __builtin_return_address(0));
  return result;
}

/* As the system places it: for what a session takes and gives back. */
int eden_direct_low(size_t size, size_t alignment, int type, int64_t *start) {
  const int result =
      __real_sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, alignment, type, start);
  if (result == 0 && start) eden_direct_note(*start, size, __builtin_return_address(0));
  return result;
}

int __wrap_sceKernelAllocateDirectMemory(int64_t from, int64_t to, size_t size, size_t alignment, int type,
                                         int64_t *start) {
  /* A thread's stack or the driver's zero block, asked for anywhere in the pool. */
  if (from == 0 && size == EDEN_DIRECT_SMALL && start && to >= sceKernelGetDirectMemorySize())
    return eden_direct_high(size, alignment, type, start);
  const int result = __real_sceKernelAllocateDirectMemory(from, to, size, alignment, type, start);
  if (result == 0 && start) eden_direct_note(*start, size, __builtin_return_address(0));
  return result;
}

int __wrap_sceKernelReleaseDirectMemory(int64_t start, size_t size) {
  const int result = __real_sceKernelReleaseDirectMemory(start, size);
  if (result != 0) return result;
  eden_direct_acquire();
  eden_direct_note(start, size, NULL);
  if (size == EDEN_DIRECT_SMALL && eden_direct_floor != 0 && start >= eden_direct_floor &&
      eden_direct_hole_count < sizeof(eden_direct_holes) / sizeof(eden_direct_holes[0]))
    eden_direct_holes[eden_direct_hole_count++] = start;
  eden_direct_release();
  return result;
}
