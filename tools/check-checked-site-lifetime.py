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

s = (root/'headless/checked-fastmem.cmake').read_text()
a=s.index('using Eden::CheckedSite;'); b=s.index('// In a site',a)
check("""
#include "dynarmic/checked_site.h"
#include <cassert>
#include <tuple>
#include <vector>
#include <cstdint>
using u64=uint64_t;
namespace IR { struct LocationDescriptor { u64 value; u64 Value() const { return value; } }; }
"""+s[a:b]+"""
struct Emitter { std::deque<Eden::CheckedSite> checked_sites; };
int main() {
    for (unsigned session=0; session<20; ++session) {
        Emitter emitter;
        for (unsigned cache=0; cache<10; ++cache) {
            std::vector<CheckedSite*> pointers;
            for (unsigned i=0; i<10000; ++i)
                pointers.push_back(NewCheckedSite(&emitter, std::make_tuple(IR::LocationDescriptor{i},i)));
            for (unsigned i=0; i<pointers.size(); ++i) {
                assert(pointers[i]->location==i && pointers[i]->name==i);
                assert(pointers[i]->budget==64 && !pointers[i]->demoted);
                --pointers[i]->budget;
            }
            // Only after generated code has retired may the emitter clear its records.
            if(cache!=9) emitter.checked_sites.clear();
        }
    }
}
""")
c = (root/'headless/CMakeLists.txt').read_text()
assert 'std::deque<Eden::CheckedSite> checked_sites;' in c
assert 'checked_sites.clear();' in c
assert 'NewCheckedSite(this, *fastmem_marker)' in s
print('PASS: stable site addresses, cache retirement, and repeated emitter destruction (ASan/LSan)')
