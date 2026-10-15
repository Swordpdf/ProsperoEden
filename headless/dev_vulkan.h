// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Development A/B switches for Vulkan device features, set from /app0/dev-settings.txt
// before the renderer creates its device (tools/prepare-vulkan-port.py applies them).
namespace Eden::DevVulkan {
inline bool disable_null_descriptor = false;
// Pipelines whose vertex shader reads no vertex attribute get a static vertex input (no attribute)
// although VK_EXT_vertex_input_dynamic_state is on: with the dynamic one the console's driver drew
// them wrong (a full-screen triangle made from the vertex index covering a game's cutscenes with
// a flat colour). dev-settings static_vi=off: as upstream.
inline bool static_vertex_input = true;
inline bool disable_descriptor_buffer = false;
inline bool robustness2 = false; // robustBufferAccess2 + robustImageAccess2
inline bool trace_pipelines = false; // report first pipeline uses to klog
inline bool disable_sparse = false; // no sparse binding: multi-range SSBOs are gathered copies
inline bool disable_multi_range = false; // bind multi-range SSBOs the pre-2026 way
// Standard border colours only (no RADV border table). On by default: with custom colours
// some games fault the GPU at page 0. Not the driver: RADV's custom border colours read back
// exactly on the console; the cause is open. The three
// standard colours stay exact, others go to the nearest one. dev-settings custom_border=on
// restores them.
inline bool disable_custom_border = true;
inline bool sync_submissions = false; // wait for the GPU after every scheduler flush
inline bool gpu_time = false; // EDEN_GPU_TIME: GPU execution time of scheduler submissions
// No VK_EXT_conditional_rendering (dev-settings conditional_rendering=off). The extension is on:
// RADV's predication is correct on the console (2.7M predicated conditions in one game, 45k
// skipped, correct output). LOD swaps and wrong draws blamed on it came from compute
// synchronization and Eden's host path (hcr_mode).
inline bool disable_conditional_rendering = false;
// Barriers around compute dispatches: indirect dispatches had none, and nothing made a
// dispatch's writes visible to later indirect arguments, vertex fetch, shaders or copies, which
// AMD runs concurrently. Games then swapped whole LOD sets for tens of seconds (racing
// culling/streaming feedback; none with RADV_DEBUG=syncshaders). dev-settings compute_barriers=off restores Eden's original synchronization.
inline bool compute_barriers = true;
// Host conditional rendering. 1 (default): Eden's evaluation, except that the
// cases it approximates take the exact CPU evaluation. Upstream (0, dev-settings hcr=eden) draws
// unconditionally when both compared values are pending host queries or accuracy is low, and
// predicates from buffers a pending query has not reached yet: in one measured game exactly the
// 1/60 of conditions that should skip. 2 (hcr=cpu): every condition evaluated on the CPU and the
// driver predicates the draws from constant 0/1 buffers (the driver check with exact inputs).
inline int hcr_mode = 1;
} // namespace Eden::DevVulkan
