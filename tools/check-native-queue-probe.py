#!/usr/bin/env python3
"""Compile the actual queue probe with native API mocks; uncertain work cannot free memory."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'headless/gpu_native_probe.inc').read_text()
code=r"""
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "common/scope_exit.h"
static struct Counts { int submit,unmap,release,suspend,mode; } *seen;
static int failure; static uint32_t* storage;
static int mock_unmap(void*,size_t) { ++seen->unmap; return 0; }
static int mock_clock(clockid_t,timespec* t) {
    static long long ns; ns+=100000000; t->tv_sec=ns/1000000000; t->tv_nsec=ns%1000000000; return 0;
}
namespace Eden { void Check(bool ok,const char* msg) { if (!ok) throw std::runtime_error(msg); } }
#define munmap mock_unmap
#define clock_gettime mock_clock
"""+source+r"""
#undef munmap
#undef clock_gettime
extern "C" {
int sceAgcSetSubmitMode(int mode) { assert(mode==0); ++seen->mode; return 0; }
int sceAgcSuspendPoint() { ++seen->suspend; return 0; }
int64_t sceKernelGetDirectMemorySize() { return 1000000; }
int sceKernelAllocateDirectMemory(int64_t,int64_t,size_t n,size_t a,int type,int64_t* p) {
    assert(n==0x4000 && a==n && (type==12 || type==0)); *p=128; return 0;
}
int sceKernelMapDirectMemory(void** p,size_t n,int prot,int,int64_t physical,size_t) {
    assert(prot==0x33 && physical==128); *p=aligned_alloc(0x4000,n); storage=(uint32_t*)*p; return 0;
}
int sceKernelReleaseDirectMemory(int64_t,size_t) { assert(seen->unmap); ++seen->release; return 0; }
int sceKernelUsleep(uint32_t us) { assert(us==1000); return 0; }
uint32_t* sceAgcCbReleaseMem(void* cb,uint8_t event,int16_t gcr,uint64_t,int8_t,void*,uint32_t data,
                           uint64_t,uint16_t,uint16_t,int8_t,int32_t) {
    assert((event==40 && gcr==0x30c && (data==1 || data==3)) ||
           (event==45 && gcr==12 && data==0));
    auto** pointers=(uint32_t**)cb; pointers[2]+=8; return pointers[2];
}
int sceAgcDriverSubmitDcb(void* s) {
    ++seen->submit; auto* words=*(uint32_t**)s; assert(words==storage);
    if (failure==1) return -1;
    if (!failure) { *(uint64_t*)(words+1024)=200; *(uint64_t*)(words+1040)=250; words[1056]=101; }
    return 0;
}
}
int main() {
    seen=(Counts*)mmap(nullptr,sizeof(*seen),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(seen!=MAP_FAILED);
    for (failure=0;failure<3;++failure) {
        *seen={}; pid_t child=fork(); assert(child>=0);
        if (!child) { for (unsigned type : {12u,0u}) Eden::ProbeNativeQueue(type); std::_Exit(0); }
        int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status));
        assert(WEXITSTATUS(status)==(failure?1:0));
        assert(seen->submit==(failure?1:64));
        assert(seen->unmap==2*!failure && seen->release==2*!failure);
        assert(seen->suspend==(failure?1:66));
    }
}
"""
with tempfile.TemporaryDirectory() as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'.deps/mirror-67bada77f8a43a90da2e94e89b8e7da73c256989/src'),
                    '-x','c++','-o',str(exe),'-'],input=code,text=True,check=True)
    subprocess.run([str(exe)],check=True,stdout=subprocess.DEVNULL)
print('Native queue probe PASS: 64 cases, packet ABI, submit failure/timeout preserve GPU allocations')
