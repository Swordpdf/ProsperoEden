#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""RADV on the PS5 may also split a long command stream before a transfer or a barrier.

One AGC submission carries at most about a million words, and each starts from reset GPU state,
so a stream is split only where its state can be re-emitted: radv_split_cmd_stream_if_due, which
RADV called before draws and dispatches only. A game that loads a new area records long runs of
buffer uploads, copies and barriers with hardly a draw between them; such a run grew past one
submission with nowhere to split it, and the winsys gave up ("a submission holds more words
between two splits than one AGC submission can", then VK_ERROR_DEVICE_LOST). The same check now
runs where a transfer command or a barrier starts, under the same conditions as before a draw.
Applied to the driver checkout after patch-radv-disk-cache.py; running it again changes nothing.
"""
from pathlib import Path
import sys
import hashlib

fingerprint = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
if sys.argv[1:2] == ['--digest']:
    print(fingerprint)
    raise SystemExit(0)
if sys.argv[1:2] == ['--check']:
    stamp = Path(sys.argv[2])
    raise SystemExit(0 if stamp.is_file() and stamp.read_text().strip() == fingerprint else 1)

root = Path(__file__).resolve().parents[1]
src = Path(sys.argv[1]) if len(sys.argv) > 1 else root.parent / 'mihawk-vulkan-review/.deps/work/radv-src'
vulkan = src / 'src/amd/vulkan'
prototype = 'void radv_split_cmd_stream_if_due(struct radv_cmd_buffer *cmd_buffer);\n'
call = '   radv_split_cmd_stream_if_due(cmd_buffer); /* ProsperoEden: tools/patch-radv-transfer-split.py */\n'
changed = False


def patch(path, edits):
    global changed
    text = path.read_text()
    new = text
    for old, replacement, marker in edits:
        if marker in new:
            continue  # already patched
        assert new.count(old) == 1, f'{path.name}: pinned code changed: {old[:60]!r}'
        new = new.replace(old, replacement)
    if new != text:
        path.write_text(new)
        changed = True


# The check, callable from the transfer commands (it was static to radv_cmd_buffer.c).
patch(vulkan / 'radv_cmd_buffer.c', [
    ('static void\nradv_split_cmd_stream_if_due(struct radv_cmd_buffer *cmd_buffer)\n',
     prototype + '\nvoid\nradv_split_cmd_stream_if_due(struct radv_cmd_buffer *cmd_buffer)\n',
     '\nvoid\nradv_split_cmd_stream_if_due('),
    ('radv_CmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo *pDependencyInfo)\n'
     '{\n   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);\n',
     'radv_CmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo *pDependencyInfo)\n'
     '{\n   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);\n' + call,
     'radv_CmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo *pDependencyInfo)\n'
     '{\n   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);\n' + call),
])


def entry(signature):
    head = signature + '{\n   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);\n'
    return (head, head + call, head + call)


includes = '#include "radv_meta.h"\n'
patch(vulkan / 'meta/radv_meta_buffer.c', [
    (includes, includes + prototype, includes + prototype),
    entry('radv_CmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize fillSize,\n'
          '                   uint32_t data)\n'),
    entry('radv_CmdCopyBuffer2(VkCommandBuffer commandBuffer, const VkCopyBufferInfo2 *pCopyBufferInfo)\n'),
    entry('radv_CmdUpdateBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize dataSize,\n'
          '                     const void *pData)\n'),
])
patch(vulkan / 'meta/radv_meta_copy.c', [
    (includes, includes + prototype, includes + prototype),
    entry('radv_CmdCopyBufferToImage2(VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2 *pCopyBufferToImageInfo)\n'),
    entry('radv_CmdCopyImageToBuffer2(VkCommandBuffer commandBuffer, const VkCopyImageToBufferInfo2 *pCopyImageToBufferInfo)\n'),
    entry('radv_CmdCopyImage2(VkCommandBuffer commandBuffer, const VkCopyImageInfo2 *pCopyImageInfo)\n'),
])

if changed:
    # Upstream skips the whole build while its receipt names the pinned revision: set the receipt
    # aside so the next build compiles the changed files (Ninja keeps every other object).
    provenance = root.parent / 'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if provenance.exists():
        provenance.replace(provenance.with_suffix('.base.txt'))
print(f'RADV transfer split: changed={changed}')
