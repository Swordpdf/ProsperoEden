#!/usr/bin/env python3
"""Exercise actual native topology discovery/pinning with mocked kernel APIs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'headless/performance.cpp').read_text();a=s.index('std::array<unsigned, 5> worker_cpus{};');body=s[a:s.index('\n#endif',a)]
code=r'''
#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <stdexcept>
#include <cassert>
#include <cstdint>
using cpuset_t=uint64_t;
#undef CPU_ISSET
#undef CPU_SET
#define CPU_ISSET(c,p) ((*(p)>>(c))&1)
#define CPU_SET(c,p) (*(p)|=uint64_t(1)<<(c))
#define CPU_LEVEL_WHICH 1
#define CPU_WHICH_TID 1
static uint64_t current=0x1fff, fail_mask=0;
static unsigned max_leaf=0xb, level_type=1, apics[64];
static bool fail_restore=false, timing_share=false;
static bool AdjacentCpusShareCore(){return timing_share;}
static constexpr std::array names{"CPUCore_0","CPUCore_1","CPUCore_2","CPUCore_3","GPU"};
static unsigned __get_cpuid_max(unsigned,void*) {return max_leaf;}
#define __cpuid_count(l,s,a,b,c,d) do {a=1;b=2;c=level_type<<8;d=apics[__builtin_ctzll(current)];} while(0)
static int cpuset_getaffinity(int,int,int,size_t n,cpuset_t* out){assert(n==8);*out=current;return 0;}
static int cpuset_setaffinity(int,int,int,size_t n,const cpuset_t* in){
 assert(n==8 && *in && !(*in&~0x1fffULL));
 if(*in==fail_mask || (fail_restore && *in==0x1fff)) {errno=22;return -1;}
 current=*in;return 0;
}
'''+body+r'''
int main(){
 for(unsigned i=0;i<64;++i) apics[i]=i;
 CheckWorkerTopology();assert(worker_topology_ready && current==0x1fff);
 assert((worker_cpus==std::array<unsigned,5>{0,2,4,6,8}));
 cpuset_t allowed=current;PinWorker(4,allowed);assert(current==256 && allowed==256);
 // The probe's process-wide CPU set decides, not a narrower mask the thread inherited.
 current=0x1fff;allowed=3;PinWorker(4,allowed);assert(current==256 && allowed==256);
 current=0x1fff;
 PinWorker(5,allowed);assert(current==0x1fff);
 for(unsigned i=0;i<7;++i) apics[i]=i*2;
 for(unsigned i=7;i<13;++i) apics[i]=(i-7)*2+1;
 CheckWorkerTopology();assert((worker_cpus==std::array<unsigned,5>{0,1,2,3,4}) && current==0x1fff);
 current=3;CheckWorkerTopology();assert(!worker_topology_ready && current==3);
 current=0x1fff;fail_mask=4;CheckWorkerTopology();assert(!worker_topology_ready && current==0x1fff);
 fail_mask=0;level_type=0;CheckWorkerTopology();assert(!worker_topology_ready && current==0x1fff);
 // CPUID that tells nothing: the timing decides, and gives the adjacent layout or none.
 timing_share=true;CheckWorkerTopology();assert(worker_topology_ready && current==0x1fff);
 assert((worker_cpus==std::array<unsigned,5>{0,2,4,6,8}));
 level_type=1;for(unsigned i=0;i<64;++i) apics[i]=5;
 CheckWorkerTopology();assert(worker_topology_ready && (worker_cpus==std::array<unsigned,5>{0,2,4,6,8}));
 timing_share=false;CheckWorkerTopology();assert(!worker_topology_ready && current==0x1fff);
 // Too few CPUs for it, or a CPU the thread may not move to: no layout is assumed.
 timing_share=true;current=0xff;CheckWorkerTopology();assert(!worker_topology_ready && current==0xff);
 current=0x1fff;fail_mask=4;CheckWorkerTopology();assert(!worker_topology_ready);
 fail_mask=0;timing_share=false;
 for(unsigned i=0;i<7;++i) apics[i]=i*2;
 for(unsigned i=7;i<13;++i) apics[i]=(i-7)*2+1;
 level_type=1;CheckWorkerTopology();assert(worker_topology_ready);
 fail_mask=16;allowed=current;PinWorker(4,allowed);assert(allowed==0x1fff && current==0x1fff);
 fail_mask=0;fail_restore=true;bool threw=false;
 try{CheckWorkerTopology();}catch(const std::runtime_error&){threw=true;}
 assert(threw);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    exe=Path(tmp)/'affinity'
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-x','c++','-','-o',str(exe)],input=code,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('Worker topology: PASS physical sibling exclusion, OS/APIC permutations, allowed masks, the layout by timing, fallback and restoration failures')
