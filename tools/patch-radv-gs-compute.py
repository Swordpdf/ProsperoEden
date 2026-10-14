#!/usr/bin/env python3
"""RADV's geometry-shader-as-compute passes: no null pointer for a dropped draw.

A draw the GPU-side setup drops (its counts do not fit) left the vertex output, index, count and
output index buffers at 0, and the passes that still ran read and wrote through them: a GPU page
fault (seen in a game's first battle). They now start at a 256-byte sink, and the setup
points them at its read-only sink when it drops the draw. Applied to the driver checkout after
patch-radv-wsi.py; running it again changes nothing.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
src = Path(sys.argv[1]) if len(sys.argv) > 1 else root.parent / 'mihawk-vulkan-review/.deps/work/radv-src'
vulkan = src / 'src/amd/vulkan'

cmd = vulkan / 'radv_cmd_buffer.c'
text = cmd.read_text()
anchor = '''   poly_vertex_params_init(vp, gsc->vs_outputs, wg_size);
   poly_geometry_params_init(gp, mode, wg_size);
'''
sink = '''
   /* ProsperoEden: pointers the GPU fills in only for a draw that fits start at the sink, not 0
    * (a dropped draw read them as 0 and faulted the GPU in a game's battles). */
   const uint64_t null_sink = radv_gs_compute_upload(cmd_buffer, 256, NULL, NULL);
   vp->output_buffer = null_sink;
   vp->index_buffer = null_sink;
   gp->count_buffer = (uint64_t)null_sink;
   gp->output_index_buffer = (uint64_t)null_sink;
'''
# Checked by its code, not its comment: a tree patched by an earlier wording counts as patched.
if 'const uint64_t null_sink = radv_gs_compute_upload' not in text:
    assert text.count(anchor) == 1, 'radv_gs_compute_prepare changed'
    cmd.write_text(text.replace(anchor, anchor + sink))

cl = vulkan / 'cl/radv_gs_compute.cl'
text = cl.read_text()
anchor = '''   if (!ok) {
      vertex_count = 0;
      instance_count = 0;
'''
sink = '''      /* ProsperoEden: nothing must read through a null pointer when the draw is dropped. */
      vp->output_buffer = (uintptr_t)block->ro_sink;
      p->count_buffer = (global uint *)block->ro_sink;
      p->output_index_buffer = (global uint *)block->ro_sink;
'''
if sink not in text:
    assert text.count(anchor) == 1, 'radv_gs_compute_setup changed'
    cl.write_text(text.replace(anchor, anchor + sink))
print('RADV geometry-shader compute: dropped draws use the sinks')
