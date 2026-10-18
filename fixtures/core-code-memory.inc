// SPDX-License-Identifier: GPL-3.0-or-later
// Skyline's two executable trampoline allocation paths, without game code or a console SDK.
#ifdef EDEN_GUEST_CODE_MEMORY
SVC(create_code_memory, 0x4b) SVC(control_code_memory, 0x4c)
SVC(set_process_permission, 0x73) SVC(map_process_code, 0x77) SVC(unmap_process_code, 0x78)
static void flush_hook_code(volatile u32* rw, u64 rx) {
    __asm__ volatile("dc cvau, %0\ndsb ish\nic ivau, %1\ndsb ish\nisb"
                     : : "r"(rw), "r"(rx) : "memory");
}
static void check_code_memory(void) {
    stage = 94;
    Registers heap = set_heap_size(0, 0x200000, 0, 0, 0, 0);
    check(heap.rc);
    const u64 address = shared_address(), source = heap.first;
    Registers code = create_code_memory(0, source, 4096, 0, 0, 0);
    check(code.rc);
    // libnx/Skyline JitType_JitMemory: one RW view and one RX view of a code-memory object.
    check(control_code_memory(code.first, 0, address, 4096, 3, 0).rc);
    check(control_code_memory(code.first, 1, address + 0x10000, 4096, 5, 0).rc);
    volatile u32* rw = (volatile u32*)address;
    rw[0] = 0xd28000e0; rw[1] = 0xd65f03c0; // mov x0, #7; ret
    flush_hook_code(rw, address + 0x10000);
    u64 (*call)(void) = (u64 (*)(void))(address + 0x10000);
    require(call() == 7);
    rw[0] = 0xd2800120; // mov x0, #9
    flush_hook_code(rw, address + 0x10000);
    require(call() == 9);
    check(control_code_memory(code.first, 3, address + 0x10000, 4096, 0, 0).rc);
    check(control_code_memory(code.first, 2, address, 4096, 0, 0).rc);
    close(code.first);
    PRINT("EDEN_GUEST_CODE_MEMORY_PASS");

    // libnx/Skyline JitType_CodeMemory fallback: map heap bytes as executable process code.
    stage = 95;
    Registers own = get_info(0, 65001, 0, 0, 0, 0);
    check(own.rc);
    volatile u32* bytes = (volatile u32*)(source + 4096);
    bytes[0] = 0xd2800160; bytes[1] = 0xd65f03c0; // mov x0, #11; ret
    check(map_process_code(own.first, address + 0x20000, (u64)bytes, 4096, 0, 0).rc);
    check(set_process_permission(own.first, address + 0x20000, 4096, 5, 0, 0).rc);
    call = (u64 (*)(void))(address + 0x20000);
    require(call() == 11);
    check(unmap_process_code(own.first, address + 0x20000, (u64)bytes, 4096, 0, 0).rc);
    close(own.first);
    PRINT("EDEN_GUEST_PROCESS_CODE_PASS");
}
#endif
