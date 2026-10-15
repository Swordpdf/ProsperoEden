#!/usr/bin/env python3
"""Exercise generated page-local JIT permissions with real mappings and faults."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
source = (cache / 'native-local/headless/block_of_code.cpp').read_text()
header = (cache / 'native-local/headless/include/dynarmic/backend/x64/block_of_code.h').read_text()
assert 'size_t protection_end = 0;' in header and 'size_t executable_end = 0;' in header
assert 'align(sysconf(_SC_PAGESIZE));' in source
assert 'align(sysconf(_SC_PAGESIZE));\n    prelude_complete = true;' in source
assert 'ASSERT(size <= 23);' in source
# The maximum patch span is an upstream invariant, not an estimated code size.
backend = cache / 'source/src/dynarmic/src/dynarmic/backend/x64'
for bits in (32, 64):
    emitter = (backend / f'a{bits}_emit_x64.cpp').read_text()
    sizes = re.findall(r'EnsurePatchLocationSize\(patch_location, (\d+)\)', emitter)
    assert len(sizes) == 4 and max(map(int, sizes)) <= 23
    generated = (cache / f'native-local/headless/a{bits}_emit_x64.cpp').read_text()
    unpatch = generated[generated.index(f'void A{bits}EmitX64::Unpatch('):]
    assert 'code.DisableWriting()' not in unpatch and 'code.EnableWriting()' not in unpatch
    assert '(*fast_dispatch_table_lookup)(location.Value()) = {};' in unpatch
names = ('EnableWriting', 'DisableWriting', 'ClearCache', 'SetCodePtr')
methods = [re.search(r'void BlockOfCode::' + name + r'\([^\n]*\) \{.*?^}', source, re.M | re.S).group() for name in names]
protect = re.search(r'void ProtectMemory\([^\n]*\) \{.*?^}', source, re.M | re.S).group()
assert 'maxSize_' not in ''.join(methods)
assert 'std::abort()' in protect
program = r'''
#include <algorithm>
#include <cassert>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#define ASSERT assert
#define DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
using CodePtr = const void*;
using u8 = unsigned char;
static size_t last_span, transitions, transitioned_bytes;
static size_t protection_page;
static bool fail_protect;
long test_sysconf(int) { return protection_page; }
#define sysconf test_sysconf
int checked_mprotect(void* p, size_t size, int mode) {
    assert(!((mode & PROT_WRITE) && (mode & PROT_EXEC)));
    assert(size && size % sysconf(_SC_PAGESIZE) == 0);
    ++transitions;
    transitioned_bytes += size;
    last_span = size;
    return fail_protect ? -1 : mprotect(p, size, mode);
}
#define mprotect checked_mprotect
PROTECT
#undef mprotect
struct BlockOfCode {
    unsigned char* top;
    size_t size = 0, protection_end = 0, executable_end = 0, write_begin = 0;
    bool writing = true;
    std::vector<size_t> patch_pages;
    CodePtr code_begin;
    bool prelude_complete = true;
    bool hasWritableAlias() const { return false; }
    size_t getSize() const { return size; }
    void setSize(size_t value) { size = value; }
    const unsigned char* getCode() const { return top; }
    void EnableWriting(); void DisableWriting(); void ClearCache();
    void SetCodePtr(CodePtr);
};
METHODS
void put_code(unsigned char* p) {
    const unsigned char code[] = {0xb8, 42, 0, 0, 0, 0xc3};
    std::memcpy(p, code, sizeof(code));
}
void fault(unsigned char* p, bool write) {
    auto child = fork(); assert(child >= 0);
    if (!child) {
        if (write) *static_cast<volatile unsigned char*>(p) = 1;
        else reinterpret_cast<int(*)()>(p)();
        _exit(0);
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}
int main(int argc, char** argv) {
    prctl(PR_SET_DUMPABLE, 0); // expected faults: no host crash reports (apport takes seconds each)
    assert(argc == 2);
    protection_page = std::strtoul(argv[1], nullptr, 10);
    const size_t page = protection_page, capacity = 32*page;
    auto* raw = static_cast<unsigned char*>(mmap(nullptr, capacity+page, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0));
    assert(raw != MAP_FAILED);
    auto* p = reinterpret_cast<unsigned char*>((reinterpret_cast<uintptr_t>(raw)+page-1)&~(page-1));
    BlockOfCode code{p}; code.code_begin = p+4*page;
    code.write_begin = code.executable_end = 2*page; // Separate constant pool.
    put_code(p+2*page); code.size = 4*page;
    code.EnableWriting(); assert(transitions == 0); // Fresh mapping is RW/NX.
    code.DisableWriting(); assert(last_span == 2*page);
    assert(reinterpret_cast<int(*)()>(p+2*page)() == 42);
    fault(p+2*page, true); put_code(p); fault(p, false); // Pool stays writable/NX.
    put_code(p+25*page); fault(p+25*page, false);
    const size_t prelude_transitions = transitions;
    code.EnableWriting(); assert(transitions == prelude_transitions);
    assert(reinterpret_cast<int(*)()>(p+2*page)() == 42);
    put_code(p+20*page); code.size = 20*page+6;
    code.DisableWriting();
    assert(reinterpret_cast<int(*)()>(p+20*page)() == 42);
    fault(p+20*page, true);
    transitioned_bytes = transitions = 0;
    code.EnableWriting();
    put_code(p+20*page+6); code.size += 6;
    auto* end = p+code.size;
    code.SetCodePtr(p+6*page-3); // Patch straddles two old pages.
    put_code(p+6*page-3);
    const size_t count = transitions;
    code.SetCodePtr(p+6*page-3); assert(transitions == count); // Deduplicate.
    code.SetCodePtr(end);
    code.DisableWriting();
    assert(transitioned_bytes == 6*page && transitions == 4);
    assert(reinterpret_cast<int(*)()>(p+6*page-3)() == 42);
    fault(p+5*page, true); fault(p+6*page, true);
    // Out-of-order adjacent patches merge with each other and the append tail.
    transitioned_bytes = transitions = 0;
    code.EnableWriting();
    code.SetCodePtr(p+19*page); put_code(p+19*page);
    code.SetCodePtr(p+18*page); put_code(p+18*page);
    code.SetCodePtr(p+19*page); // Already writable: no extra transition.
    code.SetCodePtr(end); code.DisableWriting();
    assert(transitions == 4 && transitioned_bytes == 6*page);
    for (size_t offset : {18*page, 19*page, 20*page}) {
        assert(reinterpret_cast<int(*)()>(p+offset)() == 42);
        fault(p+offset, true);
    }
    // Generated helper stays executable while several guest sites are writable.
    code.EnableWriting();
    code.SetCodePtr(p+6*page); put_code(p+6*page);
    fault(p+6*page, false);
    assert(reinterpret_cast<int(*)()>(p+2*page)() == 42);
    fault(p+2*page, true);
    code.SetCodePtr(end); code.DisableWriting();
    code.ClearCache(); // Preserve executable prelude; retired tail becomes RW/NX.
    assert(code.executable_end == 4*page && !code.writing);
    assert(reinterpret_cast<int(*)()>(p+2*page)() == 42);
    put_code(p+20*page); fault(p+20*page, false);
    code.EnableWriting(); code.ClearCache(); assert(code.writing);
    code.size = 4*page+6; put_code(p+4*page); code.DisableWriting();
    assert(reinterpret_cast<int(*)()>(p+4*page)() == 42);
    auto child = fork(); assert(child >= 0);
    if (!child) { fail_protect = true; code.EnableWriting(); _exit(0); }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(munmap(raw, capacity+page) == 0);
}
'''.replace('PROTECT\n', protect + '\n').replace('METHODS', '\n'.join(methods))
with tempfile.TemporaryDirectory(prefix='eden-protection-') as directory:
    directory = Path(directory)
    cpp = directory / 'check.cpp'
    cpp.write_text(program)
    binary = directory / 'check'
    subprocess.run(['g++', '-std=c++20', '-O2', str(cpp), '-o', str(binary)], check=True)
    for page in (4096, 16384):
        subprocess.run([str(binary), str(page)], check=True, timeout=10, cwd=directory)
print('Generated JIT page permissions: 4K/16K pages, bounded transitions, constants, cross-page patches, resets, W^X and failure handling PASS')
