#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
mkdir -p build/fixture
clang-18 --target=aarch64-none-elf -c fixtures/core-homebrew.S -o build/fixture/core-homebrew.o
for name in core-homebrew core-devices core-save-write core-save-read core-threads core-threads-wrong core-fp core-fp-wrong core-lifecycle core-lifecycle-wrong core-churn core-integration core-hooks core-code-memory; do
    flags=()
    if [[ "$name" == core-code-memory ]]; then flags+=(-DEDEN_GUEST_CODE_MEMORY=1); fi
    if [[ "$name" == core-hooks ]]; then flags+=(-DEDEN_GUEST_HOOKS=1); fi
    if [[ "$name" == core-devices ]]; then flags+=(-DEDEN_GUEST_DEVICES=1); fi
    if [[ "$name" == core-save-write ]]; then flags+=(-DEDEN_GUEST_SAVE_PHASE=1); fi
    if [[ "$name" == core-save-read ]]; then flags+=(-DEDEN_GUEST_SAVE_PHASE=2); fi
    if [[ "$name" == core-threads ]]; then flags+=(-DEDEN_GUEST_THREADS_EXPECTED=40000); fi
    if [[ "$name" == core-threads-wrong ]]; then flags+=(-DEDEN_GUEST_THREADS_EXPECTED=39999); fi
    if [[ "$name" == core-fp ]]; then flags+=(-DEDEN_GUEST_FP=7); fi
    if [[ "$name" == core-fp-wrong ]]; then flags+=(-DEDEN_GUEST_FP=6); fi
    if [[ "$name" == core-lifecycle ]]; then flags+=(-DEDEN_GUEST_LIFECYCLE=1); fi
    if [[ "$name" == core-lifecycle-wrong ]]; then flags+=(-DEDEN_GUEST_LIFECYCLE=0); fi
    if [[ "$name" == core-churn ]]; then flags+=(-DEDEN_GUEST_CHURN=1); fi
    if [[ "$name" == core-integration ]]; then flags+=(-DEDEN_GUEST_LIFECYCLE=1 -DEDEN_GUEST_THREADS_EXPECTED=40000 -DEDEN_GUEST_FP=7); fi
    clang-18 --target=aarch64-none-elf -Os -ffreestanding -fno-builtin -fno-stack-protector \
        -mno-outline-atomics "${flags[@]}" -c fixtures/core-services.c -o "build/fixture/$name-c.o"
    ld.lld-18 -T fixtures/core-homebrew.ld build/fixture/core-homebrew.o "build/fixture/$name-c.o" \
        -o "build/fixture/$name.elf"
    llvm-objcopy-18 -O binary "build/fixture/$name.elf" "build/fixture/$name.nro"
    python3 -B headless/check.py --fixture "build/fixture/$name.nro"
done
