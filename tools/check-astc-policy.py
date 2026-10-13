#!/usr/bin/env python3
"""Host check of where an ASTC texture kept as BC3 is converted (headless/astc_policy.h)."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
code = r"""
#include "astc_policy.h"
#include <cassert>
using namespace Eden::AstcPolicy;
static void Play(unsigned draws) { for (unsigned i = 0; i < draws; ++i) Draw(); }
int main() {
    // Nothing drawn yet, and a loading screen: in place.
    assert(!InBackground());
    Play(400); assert(!InBackground());
    Frame(); assert(!InBackground());
    Play(700); Frame(); assert(!InBackground());
    // The first frame of play counts as busy once it has its draws, and the frames after it.
    Play(kBusyFrame - 1); assert(!InBackground());
    Draw(); assert(InBackground());
    Play(2500); Frame(); assert(InBackground());
    Play(3000); Frame(); assert(InBackground());
    // A menu opens: its first frame still follows a busy one, the next does not.
    Play(300); assert(InBackground());
    Frame(); Play(300); assert(!InBackground());
    Frame(); assert(!InBackground());
    return 0;
}
"""
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'policy'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(root / 'headless'), '-x', 'c++', '-', '-o', str(exe)], input=code, text=True, check=True)
    subprocess.run([str(exe)], check=True)
print('ASTC conversion: PASS in place on light screens, in the background from the first busy frame, and back')
