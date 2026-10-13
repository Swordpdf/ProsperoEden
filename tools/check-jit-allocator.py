#!/usr/bin/env python3
"""Exercise the production Xbyak allocator with real RW/RX mappings."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
scratch = Path((root / '.local/headless-cache').read_text().strip())
code = (scratch / 'native-local/headless/block_of_code.cpp').read_text()
assert ', EdenJitAllocator())' in code
source = r'''
#include "jit-allocator.h"
#include <cassert>
#include <cerrno>
#include <limits>
#include <memory>
#include <vector>
#include <csignal>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef EDEN_JIT_ALIAS_NATIVE
static unsigned fail_stage, stage;
static std::vector<void*> views;
static std::vector<int> handles;
static bool enabled;
extern "C" std::int64_t sceKernelGetDirectMemorySize() { return std::int64_t{1}<<40; }
extern "C" int sceKernelEnableDmemAliasing() { enabled=true; return 0; }
// The same source file also holds the heap's growing range and the sparse tables; this check
// does not use them (tools/check-heap-growth.py and eden-memory-check do).
extern "C" int sceKernelDebugOutText(int, const char*) { return 0; }
extern "C" int sceKernelReserveVirtualRange(void**, size_t, int, size_t) { return std::int32_t(0x80020001u); }
extern "C" int sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, size_t size,
                                               size_t, int type, std::int64_t* out) {
    if (++stage==fail_stage) return std::int32_t(0x80020001u);
    assert(type==12);
    int fd=memfd_create("native-direct-mock",MFD_CLOEXEC); assert(fd>=0);
    handles.push_back(fd); assert(ftruncate(fd,size)==0);
    *out=std::int64_t(fd)<<32; return 0;
}
// The pool's two ends (src/direct_memory.c), which the heap and the sparse tables ask: not
// used by this check either, and here the same mocked allocation.
extern "C" int eden_direct_high(size_t size, size_t alignment, int type, std::int64_t* out) {
    return sceKernelAllocateDirectMemory(0, std::int64_t{1}<<40, size, alignment, type, out);
}
extern "C" int eden_direct_low(size_t size, size_t alignment, int type, std::int64_t* out) {
    return sceKernelAllocateDirectMemory(0, std::int64_t{1}<<40, size, alignment, type, out);
}
extern "C" int sceKernelReleaseDirectMemory(std::int64_t physical, size_t) {
    return close(physical>>32);
}
extern "C" int sceKernelMapDirectMemory(void** out, size_t size, int prot, int,
                                           std::int64_t physical, size_t) {
    if (++stage==fail_stage) return std::int32_t(0x80020001u);
    const auto offset=physical & 0xffffffff;
    assert(!(prot&PROT_EXEC) && (!offset || enabled));
    assert(reinterpret_cast<uintptr_t>(*out)==0x1000000000ull);
    const bool low = fail_stage==5 && stage==2;
    *out=mmap(low ? reinterpret_cast<void*>(0x280000000ull) : nullptr,size,prot,
              MAP_SHARED | (low ? MAP_FIXED_NOREPLACE : 0),physical>>32,offset);
    assert(*out!=MAP_FAILED); views.push_back(*out); return 0;
}
extern "C" int __real_mprotect(void*,size_t,int);
extern "C" int __wrap_mprotect(void* p,size_t size,int prot) {
    assert(!((prot&PROT_WRITE) && (prot&PROT_EXEC)));
    if (++stage==fail_stage) { errno=EPERM; return -1; }
    return __real_mprotect(p,size,prot);
}
#endif
void fault(const unsigned char* p, bool write) {
    auto child=fork(); assert(child>=0);
    if (!child) {
        if (write) *const_cast<volatile unsigned char*>(p)=0;
        else reinterpret_cast<void(*)()>(const_cast<unsigned char*>(p))();
        _exit(0);
    }
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
}
int main() {
    auto* allocator = EdenJitAllocator();
    assert(!allocator->alloc(std::numeric_limits<std::size_t>::max()));
    allocator->free(nullptr);
    std::vector<std::unique_ptr<Xbyak::CodeGenerator>> caches;
    std::vector<const unsigned char*> pointers;
    const auto page = sysconf(_SC_PAGESIZE);
    for (auto mib : {512, 64, 64, 16}) {
        auto code = std::make_unique<Xbyak::CodeGenerator>(std::size_t(mib)*1024*1024,
            Xbyak::DontSetProtectRWE, allocator);
        code->mov(code->eax, 42); code->ret();
        auto* p = code->getCode(); pointers.push_back(p);
        auto* writable=code->writableAddress(p); pointers.push_back(writable);
        assert(code->hasWritableAlias() && p!=writable);
        assert(code->getCode<int(*)()>()()==42);
        fault(p,true); fault(writable,false);
        code->rewrite(1,84,4);
        assert(code->getCode<int(*)()>()()==84);
        caches.push_back(std::move(code));
    }
    caches.clear();
    for (auto* p : pointers) {
        unsigned char state;
        errno=0;
        assert(mincore(const_cast<unsigned char*>(p), page, &state)==-1 && errno==ENOMEM);
    }
#ifdef EDEN_JIT_ALIAS_NATIVE
    for (fail_stage=1; fail_stage<=5; ++fail_stage) {
        stage=0; views.clear(); handles.clear();
        if (fail_stage<=2 || fail_stage==5) assert(!allocator->alloc(page));
        else {
            Xbyak::CodeGenerator code(page,Xbyak::DontSetProtectRWE,allocator);
            assert(!code.hasWritableAlias());
            code.mov(code.eax,42); code.ret();
            assert(mprotect(const_cast<unsigned char*>(code.getCode()),page,PROT_READ|PROT_EXEC)==0);
            assert(code.getCode<int(*)()>()()==42);
        }
        for (int fd : handles) { errno=0; assert(fcntl(fd,F_GETFD)==-1 && errno==EBADF); }
        for (void* p : views) { unsigned char state; errno=0; assert(mincore(p,page,&state)==-1 && errno==ENOMEM); }
    }
#endif
}
'''
with tempfile.TemporaryDirectory() as temp:
    cpp=Path(temp)/'check.cpp'; exe=Path(temp)/'check'
    cpp.write_text(source)
    for native_mock in (False, True):
        subprocess.run(['clang++-18','-std=c++20','-O2','-I'+str(root/'headless'),
            '-I'+str(scratch/'native-local/headless/include'),
            '-I'+str(scratch/'source/.cache/cpm/xbyak/v7.40.1'),str(cpp),
            *(['-DEDEN_JIT_ALIAS_NATIVE=1','-DPS5_NATIVE=1','-Wl,--wrap=mprotect'] if native_mock else []),
            str(root/'src/memory_pages.cpp'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True, cwd=temp)
print('Production JIT aliases: 512/64/64/16 MiB, overflow, RX write/RW execute faults, live rewrite, both-view release and native API partial-failure cleanup PASS (host backend/mocked native APIs)')
