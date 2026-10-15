#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the actual generated helper with sanitizers; run with clang++ installed."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
def check(code):
    with tempfile.TemporaryDirectory() as folder:
        source = Path(folder)/'check.cpp'; source.write_text(code)
        binary = Path(folder)/'check'
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++20', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread',
                        '-I'+str(root/'headless'), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

import sys
# Use the pinned Eden source from make prepare (or pass its directory).
source = Path(sys.argv[1]) if len(sys.argv)>1 else Path((root/'.local/headless-cache').read_text().strip())/'source'
g = (source/'src/core/gpu_dirty_memory_manager.h').read_text()
g = g.replace('#include "core/device_memory_manager.h"', 'using PAddr=uint64_t; namespace Core { constexpr size_t DEVICE_PAGEBITS=12; }')
p = (root/'headless/performance.h').read_text()
queue = p[p.index('struct DeferredWrite {'):p.index('// A guest read of a GPU-tracked page')]
s = (root/'headless/CMakeLists.txt').read_text()
loop = s[s.index('                    // Collect represents'):s.index('\n                }', s.index('                    // Collect represents'))]
loop = loop.replace('system.GetGPUDirtyMemoryManager()[core]', 'manager')
check("""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <array>
using u64=uint64_t; using u32=uint32_t;
"""+g+queue+"""
void verify(uint64_t start, uint64_t end) {
    Core::GPUDirtyMemoryManager manager;
    DeferredWrite write{start,end};
"""+loop+"""
    uint64_t next=start & ~63ULL;
    std::function<void(PAddr,size_t)> callback=[&](PAddr a,size_t n) {
        assert(a==next); next+=n;
    };
    manager.Gather(callback);
    assert(next==((end+63)&~63ULL));
}
int main() {
    DeferWrite(0x10000,8,0);
    assert(ExtendDeferredWrite(0x10ff8,8,0));
    auto writes=TakeDeferredWrites(0);
    assert(writes.size()==1);
    verify(writes[0].address,writes[0].end);
    for (uint64_t offset=0;offset<4096;++offset)
        for (uint64_t size : {1,8,64,2048,4096,8193})
            verify(0x10000+offset,0x10000+offset+size);
}
""")
print('PASS: merged writes and 24576 unaligned/cross-region ranges preserve dirty coverage')
