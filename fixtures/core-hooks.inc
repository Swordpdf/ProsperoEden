// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the writable-code alias and instruction-cache sequence used by Skyline hooks.
#ifdef EDEN_GUEST_HOOKS
SVC(map_process_memory, 0x74) SVC(unmap_process_memory, 0x75)
static unsigned char hook_stacks[4][4096] __attribute__((aligned(16)));
static u64 hook_ready, hook_go, hook_values[4];
__asm__(".pushsection .text\n.balign 64\n.global hook_target\nhook_target:\n"
        "mov x0, #1\nret\n"
        ".balign 64\n.global hook_prime0\nhook_prime0:\nmov x0, #1\nret\n"
        ".balign 64\n.global hook_prime1\nhook_prime1:\nmov x0, #1\nret\n"
        ".balign 64\n.global hook_prime2\nhook_prime2:\nmov x0, #1\nret\n"
        ".balign 64\n.global hook_prime3\nhook_prime3:\nmov x0, #1\nret\n.popsection\n");
extern u64 hook_target(void);
extern u64 hook_prime0(void), hook_prime1(void), hook_prime2(void), hook_prime3(void);
static void hook_worker(u64 index) {
    require(hook_target() == 1); // Compile the original block on every guest core.
    // Each core translates a distinct block in this same 4 KiB executable page, even when
    // compiled blocks are shared. Its instruction-byte page therefore predates the patch.
    u64 prime = index == 0 ? hook_prime0() : index == 1 ? hook_prime1() :
                index == 2 ? hook_prime2() : hook_prime3();
    require(prime == 1);
    __atomic_fetch_add(&hook_ready, 1, __ATOMIC_ACQ_REL);
    while (!(__atomic_load_n(&hook_go, __ATOMIC_ACQUIRE) & (1ULL << index))) ONE(sleep_thread, 100000);
    hook_values[index] = hook_target();
    ONE(exit_thread, 0);
    for (;;) {}
}
static void check_hooks(void) {
    stage = 90;
    Registers own = get_info(0, 65001, 0, 0, 0, 0); // MesosphereCurrentProcess, Skyline's first path.
    check(own.rc);
    const u64 source = (u64)hook_target & ~4095ULL;
    const u64 alias = shared_address();
    check(map_process_memory(alias, own.first, source, 4096, 0, 0).rc);
    volatile u32* patch = (volatile u32*)(alias + ((u64)hook_target - source));
    require(*patch == 0xd2800020); // mov x0, #1
    // Prime after mapping, so any mapping-side cache flush cannot hide a stale byte cache.
    u32 handles[4];
    for (u32 i = 0; i < 4; ++i) {
        Registers thread = create_thread(0, (u64)hook_worker, i,
            (u64)(hook_stacks[i] + sizeof(hook_stacks[i])), 44, i);
        check(thread.rc); handles[i] = thread.first;
        check(ONE(start_thread, handles[i]).rc);
    }
    const u64 deadline = ONE(get_tick, 0).rc + 19200000ULL * 3;
    while (__atomic_load_n(&hook_ready, __ATOMIC_ACQUIRE) != 4) {
        require(ONE(get_tick, 0).rc < deadline);
        ONE(sleep_thread, 100000);
    }
    stage = 91;
    *patch = 0xd2800040; // mov x0, #2, written through Skyline's RW alias of RX text.
    __asm__ volatile("dc cvau, %0\ndsb ish\nic ivau, %1\ndsb ish\nisb"
                     : : "r"(patch), "r"(hook_target) : "memory");
    // Core 1, not the patching core, must compile the replacement first. Releasing all
    // cores together could let the patching core publish fresh code and hide a stale cache.
    __atomic_store_n(&hook_go, 2, __ATOMIC_RELEASE);
    check(wait(handles[1], 1000000000).rc);
    __atomic_store_n(&hook_go, 15, __ATOMIC_RELEASE);
    stage = 92;
    u32 correct = 1;
    for (u32 i = 0; i < 4; ++i) {
        check(wait(handles[i], 1000000000).rc);
        print_value("EDEN_GUEST_HOOK_VALUE=", hook_values[i]);
        correct &= hook_values[i] == 2;
        close(handles[i]);
    }
    check(unmap_process_memory(alias, own.first, source, 4096, 0, 0).rc);
    close(own.first);
    stage = 93;
    require(correct);
    PRINT("EDEN_GUEST_HOOKS_PASS");
}
#endif
