#!/usr/bin/env python3
"""Host check of the map of pages a texture cache image was ever in (headless/image_pages.h)."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r"""
#include "image_pages.h"
#include <cassert>
using namespace Eden::ImagePages;
int main() {
    const std::uint64_t page = 4096;
    // Nothing registered: every read inside the space is free of images; outside it, never.
    assert(NeverHeld(0, 1) && NeverHeld(kSpace - 1, 1) && NeverHeld(123456789, 64 * page));
    assert(!NeverHeld(kSpace, 1) && !NeverHeld(kSpace - 1, 2) && !NeverHeld(~std::uint64_t{0}, 8));
    // One byte of an image marks its page and no other.
    Mark(page, 1);
    assert(NeverHeld(page - 1, 1) && !NeverHeld(page, 1) && !NeverHeld(2 * page - 1, 1) && NeverHeld(2 * page, 1));
    assert(!NeverHeld(page - 1, 2) && !NeverHeld(0, 3 * page) && NeverHeld(page - 8, 8));
    assert(!NeverHeld(page, 0));
    // A range over a word of the map: pages 63 to 65.
    Mark(63 * page + 5, 2 * page);
    assert(NeverHeld(62 * page, page) && !NeverHeld(63 * page, 1) && !NeverHeld(64 * page, 1) &&
           !NeverHeld(65 * page, 1) && NeverHeld(66 * page, page));
    // A large image: pages 1000 to 9999, whole words in the middle.
    Mark(1000 * page, 9000 * page);
    assert(NeverHeld(999 * page, page) && !NeverHeld(1000 * page, 1) && !NeverHeld(5000 * page, 1) &&
           !NeverHeld(9999 * page + page - 1, 1) && NeverHeld(10000 * page, page));
    assert(!NeverHeld(900 * page, 200 * page) && NeverHeld(10000 * page, 5000 * page));
    // The end of the space: clipped; beyond it: nothing.
    Mark(kSpace - 1, 1 << 20);
    assert(!NeverHeld(kSpace - page, 1) && NeverHeld(kSpace - 2 * page, page));
    Mark(kSpace, page);
    Mark(5 * page, 0);
    assert(NeverHeld(5 * page, page));
    return 0;
}
"""
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'pages'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(root / 'headless'), '-x', 'c++', '-', '-o', str(exe)], input=code, text=True, check=True)
    subprocess.run([str(exe)], check=True)
print('Image pages: PASS one page, word boundaries, large ranges, zero sizes, the end of the space and beyond')
