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

s = (root/'tools/prepare-vulkan-port.py').read_text()
helper = s[s.index('std::mutex eden_attributeless_mutex;'):s.index('// A static key without attributes')]
check("""
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <set>
using u64=uint64_t; using u32=uint32_t;
namespace Shader { namespace IR { constexpr size_t NUM_GENERICS=32; }
struct Info { struct Loads { bool uses; bool Generic(size_t i) const { return uses && i==0; } } loads; }; }
""" + helper + """
int main() {
    EdenRecordAttributeless({1,123}, Shader::Info{{false}});
    assert(EdenAttributeless({1,123}));
    EdenRecordAttributeless({2,123}, Shader::Info{{true}});
    assert(!EdenAttributeless({2,123}));
    assert(!EdenAttributeless({0,123}));
    EdenRecordAttributeless({0,123}, Shader::Info{{false}});
    assert(EdenAttributeless({0,123}));
    assert(!EdenAttributeless({2,123}));
    assert(EdenAttributeless({1,123}));
}
""")
assert 'EdenAttributeless({graphics_key.unique_hashes[0], graphics_key.unique_hashes[1]})' in s
assert 'EdenRecordAttributeless({key.unique_hashes[0], key.unique_hashes[1]}, programs[1].info)' in s
print('PASS: dual and single vertex identity, repeated lookups and generation invalidation')
