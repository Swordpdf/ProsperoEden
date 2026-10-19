// SPDX-License-Identifier: GPL-3.0-or-later
// Freestanding guest checks against Eden's public SVC and CMIF service ABI.
typedef unsigned int u32;
typedef unsigned long long u64;
typedef struct { u64 rc, first, second; } Registers;
#define SVC(name, number) \
static inline Registers name(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) { \
    register u64 x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c; \
    register u64 x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f; \
    __asm__ volatile("svc " #number : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5) \
                     : : "x6", "x7", "memory", "cc"); \
    return (Registers){x0, x1, x2}; \
}
SVC(exit_process, 0x7) SVC(create_thread, 0x8) SVC(start_thread, 0x9)
SVC(exit_thread, 0xa) SVC(sleep_thread, 0xb) SVC(signal_event, 0x11)
SVC(close_handle, 0x16) SVC(reset_signal, 0x17) SVC(wait_sync, 0x18)
SVC(get_tick, 0x1e) SVC(connect_port, 0x1f) SVC(send_sync, 0x21)
SVC(debug_string, 0x27) SVC(create_event, 0x45)
SVC(query_memory, 0x6) SVC(map_shared, 0x13) SVC(unmap_shared, 0x14) SVC(get_info, 0x29)
SVC(set_heap_size, 0x1) SVC(create_transfer, 0x15)
#define ONE(name, x) name((u64)(x), 0, 0, 0, 0, 0)
#define PRINT(s) debug_string((u64)(s), sizeof(s) - 1, 0, 0, 0, 0)
static volatile u64 shared_value;
static unsigned char thread_stack[4096] __attribute__((aligned(16)));
static char file_path[0x301];
static unsigned char written[512], read_back[512];
static short tone[12000 * 2];
static char audio_name[0x100], audio_name_out[0x100];
static u32 stage;
static void print_value(const char* label, u64 value);

__attribute__((noreturn)) static void fail(u64 result) {
    PRINT("EDEN_CORE_FIXTURE_FAIL");
    print_value("EDEN_GUEST_FAIL_STAGE=", stage);
    print_value("EDEN_GUEST_FAIL_RESULT=", result);
    ONE(exit_process, 0);
    for (;;) {}
}
static void check(u64 result) { if (result) fail(result); }
static void require(int ok) { if (!ok) fail(0xffffffff); }
static void close(u32 handle) { check(ONE(close_handle, handle).rc); }
static Registers wait(u32 handle, u64 timeout) {
    return wait_sync(0, (u64)&handle, 1, timeout, 0, 0);
}
static void thread_entry(u64 event) {
    shared_value = 0x12345678;
    check(ONE(signal_event, event).rc);
    ONE(exit_thread, 0);
    for (;;) {}
}

// Bounded input/output descriptors and one returned handle cover these services.
// kind: 0 = none, 1 = input pointer, 2 = input map alias, 3 = output map alias.
static u64* ipc_full(u32 handle, u32 command, int pid, int kind, void* buffer, u32 size,
                   const u64* args, u32 words, u32* out_handle,
                   u32 copy_handle, void* receive, u32 receive_size,
                   u32 second_handle, void* performance, u32 performance_size) {
    require(!second_handle || copy_handle);
    require(!performance || (receive && kind == 2));
    u32* tls;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tls));
    for (u32 i = 0; i < 64; ++i) tls[i] = 0;
    tls[0] = 4 | (kind == 1 ? 1u << 16 : kind == 2 ? 1u << 20 : kind == 3 ? 1u << 24 : 0);
    if (receive) tls[0] |= 1u << 24;
    if (performance) tls[0] += 1u << 24;
    tls[1] = (8 + words * 2) | (pid || copy_handle ? 1u << 31 : 0);
    u32 i = 2;
    if (pid || copy_handle) {
        tls[i++] = (pid ? 1 : 0) | (copy_handle ? (second_handle ? 4 : 2) : 0);
        if (pid) i += 2;
        if (copy_handle) tls[i++] = copy_handle;
        if (second_handle) tls[i++] = second_handle;
    }
    const u64 address = (u64)buffer;
    if (kind == 1) {
        tls[i++] = size << 16 | ((address >> 32) & 15) << 12 | ((address >> 36) & 7) << 6;
        tls[i++] = (u32)address;
    } else if (kind) {
        tls[i++] = size; tls[i++] = (u32)address;
        tls[i++] = 1 | ((address >> 32) & 15) << 28 | ((address >> 36) & 7) << 2;
    }
    if (receive) {
        const u64 output_address = (u64)receive;
        tls[i++] = receive_size; tls[i++] = (u32)output_address;
        tls[i++] = 1 | ((output_address >> 32) & 15) << 28 | ((output_address >> 36) & 7) << 2;
    }
    if (performance) {
        const u64 output_address = (u64)performance;
        tls[i++] = performance_size; tls[i++] = (u32)output_address;
        tls[i++] = 1 | ((output_address >> 32) & 15) << 28 | ((output_address >> 36) & 7) << 2;
    }
    i = (i + 3) & ~3u;
    require(i + 4 + words * 2 <= 64);
    tls[i] = 0x49434653; tls[i + 2] = command;
    for (u32 j = 0; j < words; ++j) ((u64*)&tls[i + 4])[j] = args[j];
    check(ONE(send_sync, handle).rc);
    i = 2;
    if (tls[1] >> 31) {
        const u32 header = tls[i++];
        require((header == 0x20 || header == 2) && out_handle); // one moved session or copied handle
        *out_handle = tls[i++];
    } else require(!out_handle);
    i = (i + 3) & ~3u;
    require(tls[i] == 0x4f434653);
    check(tls[i + 2]);
    return (u64*)&tls[i + 4];
}
static u64* ipc_ex(u32 handle, u32 command, int pid, int kind, void* buffer, u32 size,
                   const u64* args, u32 words, u32* out_handle,
                   u32 copy_handle, void* receive, u32 receive_size) {
    return ipc_full(handle, command, pid, kind, buffer, size, args, words, out_handle,
                    copy_handle, receive, receive_size, 0, 0, 0);
}
static u64* ipc(u32 handle, u32 command, int pid, int kind, void* buffer, u32 size,
                const u64* args, u32 words, u32* out_handle) {
    return ipc_ex(handle, command, pid, kind, buffer, size, args, words, out_handle, 0, 0, 0);
}
static u32 service(u32 sm, const char* name) {
    u64 name_word = 0;
    for (u32 i = 0; i < 8 && name[i]; ++i) name_word |= (u64)(unsigned char)name[i] << (i * 8);
    u32 handle;
    ipc(sm, 1, 0, 0, 0, 0, &name_word, 1, &handle);
    return handle;
}
static u64 info(u32 type) {
    Registers r = get_info(0, type, 0xffff8001, 0, 0, 0); check(r.rc); return r.first;
}
static u64 shared_address(void) {
    const u64 start = info(12), end = start + info(13);
    const u64 alias = info(2), alias_end = alias + info(3), heap = info(4), heap_end = heap + info(5);
    u64 address = start;
    for (u32 region = 0; region < 64 && address < end; ++region) {
        struct { u64 address, size; u32 state, attribute, permission, ipc_refs, device_refs, pad; } memory;
        check(query_memory((u64)&memory, 0, address, 0, 0, 0).rc);
        require(memory.size && memory.address + memory.size > address);
        if (address >= alias && address < alias_end) { address = alias_end; continue; }
        if (address >= heap && address < heap_end) { address = heap_end; continue; }
        u64 limit = memory.address + memory.size;
        if (address < alias && alias < limit) limit = alias;
        if (address < heap && heap < limit) limit = heap;
        if (memory.state == 0 && limit - address >= 0x40000 && end - address >= 0x40000) return address;
        address = memory.address + memory.size;
    }
    fail(0xfffffffe);
}
static void print_value(const char* label, u64 value) {
    char text[80]; u32 length = 0;
    while (label[length] && length < 63) { text[length] = label[length]; ++length; }
    const char hex[] = "0123456789abcdef";
    for (u32 i = 0; i < 16; ++i) text[length + i] = hex[(value >> (60 - 4 * i)) & 15];
    debug_string((u64)text, length + 16, 0, 0, 0, 0);
}
typedef struct { u64 sample, buttons; int lx, ly, rx, ry; u32 attributes, pad; } PadSample;
static int read_pad(u64 mapping, PadSample* sample) {
    volatile u64* ring = (volatile u64*)(mapping + 0x9a00 + 0x28);
    const u64 tail = ring[2];
    if (!ring[3] || tail >= 17) return 0;
    volatile u64* entry = ring + 4 + tail * 6;
    const u64 before = entry[0];
    *sample = *(volatile PadSample*)(entry + 1);
    return entry[0] == before && ring[2] == tail && before == sample->sample * 2 &&
           sample->sample && (sample->attributes & 1);
}
static void check_hid(u32 sm) {
    stage = 5;
    u32 hid = service(sm, "hid"), applet, memory;
    const u64 zero[] = {0}, style[] = {1, 0};
    ipc(hid, 0, 1, 0, 0, 0, zero, 1, &applet);
    ipc(applet, 0, 0, 0, 0, 0, 0, 0, &memory);
    const u64 address = shared_address();
    check(map_shared(memory, address, 0x40000, 1, 0, 0).rc);
    const u32 player = 0;
    ipc(hid, 100, 1, 0, 0, 0, style, 2, 0);
    ipc(hid, 102, 1, 1, (void*)&player, sizeof(player), zero, 1, 0);
    ipc(hid, 103, 1, 0, 0, 0, zero, 1, 0);
    PRINT("EDEN_GUEST_HID_READY");
    const u64 deadline = ONE(get_tick, 0).rc + 19200000ULL * 15;
    u32 phase = 0; PadSample pressed = {0};
    while (ONE(get_tick, 0).rc < deadline && phase < 3) {
        PadSample sample;
        if (read_pad(address, &sample)) {
            if (phase == 0 && !(sample.buttons & 1)) phase = 1;
            else if (phase == 1 && (sample.buttons & 1)) { pressed = sample; phase = 2; }
            else if (phase == 2 && (sample.buttons & 1)) pressed = sample;
            else if (phase == 2 && !(sample.buttons & 1)) phase = 3;
        }
        ONE(sleep_thread, 1000000);
    }
    if (phase != 3) {
        print_value("EDEN_HID_PHASE=", phase);
        print_value("EDEN_HID_STYLE=", *(volatile u32*)(address + 0x9a00));
        volatile u64* ring = (volatile u64*)(address + 0x9a00 + 0x28);
        print_value("EDEN_HID_TAIL=", ring[2]); print_value("EDEN_HID_COUNT=", ring[3]);
        PadSample last = {0};
        print_value("EDEN_HID_VALID=", read_pad(address, &last));
        print_value("EDEN_HID_ATTRIBUTES=", last.attributes);
        print_value("EDEN_HID_SAMPLE=", last.sample);
        print_value("EDEN_HID_BUTTONS=", last.buttons);
        fail(0xffffffff);
    }
    print_value("EDEN_GUEST_HID_BUTTONS=", pressed.buttons);
    print_value("EDEN_GUEST_HID_LX=", (u32)pressed.lx);
    print_value("EDEN_GUEST_HID_RY=", (u32)pressed.ry);
    check(unmap_shared(memory, address, 0x40000, 0, 0, 0).rc);
    close(memory); close(applet); close(hid);
    PRINT("EDEN_GUEST_HID_PASS");
}
static void check_audio(u32 sm) {
    stage = 6;
    const u32 manager = service(sm, "audout:u");
    u32 audio, event;
    const u64 config[] = {48000ULL | (2ULL << 32), 0};
    const u32* format = (u32*)ipc_ex(manager, 1, 1, 2, audio_name, sizeof(audio_name), config, 2,
                                    &audio, 0xffff8001, audio_name_out, sizeof(audio_name_out));
    require(format[0] == 48000 && format[1] == 2 && format[2] == 2 && format[3] == 1);
    ipc(audio, 4, 0, 0, 0, 0, 0, 0, &event);
    for (u32 frame = 0; frame < 12000; ++frame) {
        const short value = frame == 11999 ? 0 : frame % 96 < 48 ? 1024 : -1024;
        tone[frame * 2] = value; tone[frame * 2 + 1] = value;
    }
    const u64 buffer[] = {0, (u64)tone, sizeof(tone), sizeof(tone), 0};
    const u64 tag[] = {0xede10001};
    ipc(audio, 3, 0, 2, (void*)buffer, sizeof(buffer), tag, 1, 0);
    ipc(audio, 1, 0, 0, 0, 0, 0, 0, 0);
    require(*(u32*)ipc(audio, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    const u64 deadline = ONE(get_tick, 0).rc + 19200000ULL * 2;
    u64 released = 0;
    while (ONE(get_tick, 0).rc < deadline && !released) {
        Registers ready = wait(event, 100000000);
        require(ready.rc == 0 || ready.rc == 0xea01);
        if (!ready.rc) check(ONE(reset_signal, event).rc);
        const u32 count = *(u32*)ipc(audio, 5, 0, 3, &released, sizeof(released), 0, 0, 0);
        require(count <= 1 && (count == 0 || released == tag[0]));
    }
    require(released == tag[0]);
    // Eden's sample counter includes 25 ms reporting leeway. Allow the tail to drain.
    ONE(sleep_thread, 50000000);
    require(*ipc(audio, 10, 0, 0, 0, 0, 0, 0, 0) >= 12000);
    ipc(audio, 2, 0, 0, 0, 0, 0, 0, 0);
    require(*(u32*)ipc(audio, 0, 0, 0, 0, 0, 0, 0, 0) == 1);
    close(event); close(audio); close(manager);
    PRINT("EDEN_GUEST_AUDIO_PASS");
}

#include "core-renderer.inc"
#include "core-save.inc"
#include "core-threads.inc"
#include "core-fp.inc"
#include "core-lifecycle.inc"
#include "core-churn.inc"

#include "core-hooks.inc"
#include "core-code-memory.inc"

void guest_main(void) {
#ifdef EDEN_GUEST_CODE_MEMORY
    check_code_memory();
    PRINT("EDEN_CORE_FIXTURE_PASS");
    ONE(exit_process, 0);
    return;
#endif
#ifdef EDEN_GUEST_HOOKS
    check_hooks();
    PRINT("EDEN_CORE_FIXTURE_PASS");
    ONE(exit_process, 0);
    return;
#endif
#ifdef EDEN_GUEST_CHURN
    check_churn();
#endif
#ifdef EDEN_GUEST_LIFECYCLE
    check_state();
    check_memory();
#endif
#ifdef EDEN_GUEST_FP
    check_fp();
#endif
#ifdef EDEN_GUEST_THREADS_EXPECTED
    check_threads();
#endif
#ifdef EDEN_GUEST_CPU_EXPECTED
    extern u64 cpu_pressure(u64 seed);
    stage = 60;
    u64 result = 1;
    for (u32 pass = 0; pass < 2; ++pass) {
        result = cpu_pressure(result);
        print_value("EDEN_GUEST_CPU_RESULT=", result);
    }
    require(result == EDEN_GUEST_CPU_EXPECTED);
    PRINT("EDEN_GUEST_CPU_PASS");
#endif
    stage = 1;
    shared_value = 40; shared_value += 2; require(shared_value == 42);
    const u64 before = ONE(get_tick, 0).rc;
    ONE(sleep_thread, 2000000);
    require(ONE(get_tick, 0).rc > before);
    PRINT("EDEN_GUEST_TIMER_PASS");
    stage = 2;
    Registers event = ONE(create_event, 0); check(event.rc);
    const u32 write_event = event.first, read_event = event.second;
    require(wait(read_event, 0).rc == 0xea01);
    Registers thread = create_thread(0, (u64)thread_entry, write_event,
                                     (u64)(thread_stack + sizeof(thread_stack)), 44, -2);
    check(thread.rc); check(ONE(start_thread, thread.first).rc);
    Registers signaled = wait(read_event, 1000000000); check(signaled.rc); require(signaled.first == 0);
    require(shared_value == 0x12345678);
    check(ONE(reset_signal, read_event).rc);
    require(wait(read_event, 1000000).rc == 0xea01);
    check(wait(thread.first, 1000000000).rc);
    close(thread.first); close(read_event); close(write_event);
    PRINT("EDEN_GUEST_SYNC_PASS");
    stage = 3;
    Registers connection = connect_port(0, (u64)"sm:", 0, 0, 0, 0); check(connection.rc);
    u32 sm = connection.first, fs, sd, file;
    const u64 zero[] = {0};
    ipc(sm, 0, 1, 0, 0, 0, zero, 1, 0);
    const char name[8] __attribute__((aligned(8))) = "fsp-srv";
    ipc(sm, 1, 0, 0, 0, 0, (const u64*)name, 1, &fs);
    ipc(fs, 1, 1, 0, 0, 0, zero, 1, 0);
    ipc(fs, 18, 0, 0, 0, 0, 0, 0, &sd);
    PRINT("EDEN_GUEST_SERVICE_PASS");
    stage = 4;
    const char path[] = "/eden-offline-fixture.bin";
    for (u32 i = 0; i < sizeof(path); ++i) file_path[i] = path[i];
    const u64 create_args[] = {0, sizeof(written)};
    ipc(sd, 0, 0, 1, file_path, sizeof(file_path), create_args, 2, 0);
    const u64 mode[] = {3}; // read and write
    ipc(sd, 8, 0, 1, file_path, sizeof(file_path), mode, 1, &file);
    for (u32 i = 0; i < sizeof(written); ++i) written[i] = (unsigned char)(i * 37 + 11);
    const u64 write_args[] = {1, 0, sizeof(written)}; // flush, offset, length
    ipc(file, 1, 0, 2, written, sizeof(written), write_args, 3, 0);
    ipc(file, 2, 0, 0, 0, 0, 0, 0, 0);
    require(*ipc(file, 4, 0, 0, 0, 0, 0, 0, 0) == sizeof(written));
    const u64 read_args[] = {0, 0, sizeof(read_back)};
    require(*ipc(file, 0, 0, 3, read_back, sizeof(read_back), read_args, 3, 0) == sizeof(read_back));
    for (u32 i = 0; i < sizeof(written); ++i) require(read_back[i] == written[i]);
    close(file);
    ipc(sd, 1, 0, 1, file_path, sizeof(file_path), 0, 0, 0);
    close(sd);
#ifdef EDEN_GUEST_SAVE_PHASE
    check_save(fs);
#endif
    close(fs);
    PRINT("EDEN_GUEST_STORAGE_PASS");
#ifdef EDEN_GUEST_DEVICES
    check_audio(sm);
    check_renderer(sm);
    check_hid(sm);
#endif
    close(sm);
    PRINT("EDEN_CORE_FIXTURE_PASS");
}
