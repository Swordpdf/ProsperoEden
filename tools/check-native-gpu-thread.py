#!/usr/bin/env python3
"""Exercise the actual native lifecycle and verify the generated worker body is preserved."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
upstream = cache / 'source/src'
generated = cache / 'native-local/headless/gpu_thread.cpp'
source = (upstream / 'video_core/gpu_thread.cpp').read_text()
worker = generated.read_text()
body = source.split('thread = std::jthread([&](std::stop_token stop_token) {', 1)[1].split('\n    });', 1)[0]
dispatch_call = '                scheduler.Push(system.GPU(), submit_list->channel, std::move(submit_list->entries));'
development = 'EDEN_DEV_PROFILE:BOOL=ON' in (cache / 'native-local/CMakeCache.txt').read_text()
# The producer-side waits are timed in every build (Timer's `always`), the per-dispatch scope only
# in a development one, so each wrap is asserted at the count its build must produce, then undone.
for call, timer, indent, always in (
    (dispatch_call, 'dispatch_timer(::Eden::Performance::gpu_dispatch)', '                ', False),
    ('            state.queue.PopWait(next, stop_token);',
     'idle_timer(::Eden::Performance::gpu_queue_wait, 0, true)', '            ', True),
):
    wrapped = indent + '{ ::Eden::Performance::Timer ' + timer + ';\n' + call + '\n' + indent + '}'
    assert worker.count(wrapped) == int(development or always)
    worker = worker.replace(wrapped, call)

# Likewise the wrap around the push that found the queue full; its body is the plain spin the
# upstream rewrite left behind.
queue_push = ('    while (!stop_source.stop_requested() &&\n'
              '           !state.queue.TryEmplace(std::move(command_data), fence, block))\n'
              '        std::this_thread::sleep_for(std::chrono::microseconds(100));')
full_wrap = ('    if (stop_source.stop_requested() ||\n'
             '        !state.queue.TryEmplace(std::move(command_data), fence, block)) {\n'
             '        ::Eden::Performance::Timer full_timer(::Eden::Performance::gpu_queue_full, 0, true);\n'
             + queue_push + '\n    }')
assert worker.count(full_wrap) == 1
worker = worker.replace(full_wrap, queue_push)

trace_when = 'count <= 3 || ::Eden::Performance::BootTrace()' if development else 'count <= 3'
dispatch_trace = ('                const auto count = ++dispatches;\n'
                  f'                if ({trace_when}) LOG_INFO(Render_OpenGL, "EDEN_GPU_DISPATCH_BEGIN count={{}}", count);\n'
                  + dispatch_call + '\n'
                  f'                if ({trace_when}) LOG_INFO(Render_OpenGL, "EDEN_GPU_DISPATCH_END count={{}}", count);')
assert worker.count(dispatch_trace) == 1
untraced = worker.replace(dispatch_trace, dispatch_call).replace('        unsigned dispatches = 0;\n', '')
loading_wait = """            if (Eden::LoadingTick(renderer, false)) {
                if (!state.queue.TryPop(next)) {
                    Eden::LoadingTick(renderer, true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            } else {
            state.queue.PopWait(next, stop_token);
            }"""
assert untraced.count(loading_wait) == 1
untraced = untraced.replace(loading_wait, '            state.queue.PopWait(next, stop_token);')
assert body in untraced, 'GPU worker semantics changed beyond dispatch markers and startup animation wait'

assert 'stop_source.get_token()' in worker and 'thread.get_stop_token()' not in worker
assert 'ThreadManager::~ThreadManager() { Stop(); }' in worker
gpu = (cache / 'native-local/headless/gpu.cpp').read_text()
assert 'GPU::~GPU() { impl->NotifyShutdown(); }' in gpu
shutdown = gpu.split('    void NotifyShutdown() {', 1)[1].split('    /// Obtain', 1)[0]
assert 'sync_cv.notify_all();\n        }\n        gpu_thread.Stop();' in shutdown
assert shutdown.index('gpu_thread.Stop();') < shutdown.index('sync_requests.clear();') < shutdown.index('sync_requests_stopped = true;')
assert '~Impl() { gpu_thread.Stop(); if (renderer && Settings::IsOpenGL()) { renderer->Context().MakeCurrent(); glFinish(); } }' in gpu
core = (cache / 'native-local/headless/core.cpp').read_text()
shutdown_core = core.split('    void ShutdownMainProcess() {', 1)[1].split('    bool IsShuttingDown()', 1)[0]
assert shutdown_core.index('gpu_core->NotifyShutdown()') < shutdown_core.index('services.reset()') < shutdown_core.index('gpu_core.reset()')
code = r'''
#include <cassert>
#include <cerrno>
#include <memory>
#include <pthread.h>
#include <semaphore>
#include <stop_token>
#include <system_error>
#include <functional>
#include <thread>
#include "common/bounded_threadsafe_queue.h"
static int fault, attributes_live, joins;
static std::binary_semaphore* joining;
static int init(pthread_attr_t* a) {
    if (fault == 1) return ENOMEM;
    int result = pthread_attr_init(a); if (!result) ++attributes_live; return result;
}
static int stack(pthread_attr_t* a, size_t size) {
    assert(size == 1024 * 1024);
    return fault == 2 ? EINVAL : pthread_attr_setstacksize(a, size);
}
static int destroy(pthread_attr_t* a) { --attributes_live; return pthread_attr_destroy(a); }
static int create(pthread_t* t, const pthread_attr_t* a, void* (*f)(void*), void* p) {
    return fault == 3 ? EAGAIN : pthread_create(t, a, f, p);
}
static int join(pthread_t t, void** out) {
    ++joins;
    if (joining) joining->release();
    return pthread_join(t, out);
}
namespace VideoCore { struct RendererBase { void* ReadRasterizer() { return this; } }; }
namespace Core::Frontend { struct GraphicsContext {}; }
namespace Tegra::Control { struct Scheduler {}; }
namespace VideoCommon::GPUThread {
class ThreadManager {
public:
    ~ThreadManager() { Stop(); }
    void StartThread(VideoCore::RendererBase&, Core::Frontend::GraphicsContext&, Tegra::Control::Scheduler&);
    void Stop();
    Common::SPSCQueue<int, 4> queue;
    std::binary_semaphore ready{0};
    bool finished{};
    std::function<void()> command;
private:
    void RunWorker(VideoCore::RendererBase&, Core::Frontend::GraphicsContext&, Tegra::Control::Scheduler&) {
        pthread_attr_t a;
        assert(pthread_getattr_np(pthread_self(), &a) == 0);
        size_t size;
        assert(pthread_attr_getstacksize(&a, &size) == 0 && size == 1024 * 1024);
        assert(pthread_attr_destroy(&a) == 0);
        int value = 0;
        queue.PopWait(value, stop_source.get_token());
        assert(value == 42 && !stop_source.stop_requested());
        if (command) { command(); finished = true; return; }
        ready.release();
        queue.PopWait(value, stop_source.get_token());
        assert(stop_source.stop_requested());
        finished = true;
    }
    void* rasterizer{};
    pthread_t thread{};
    std::stop_source stop_source;
    bool thread_joinable{};
};
}
#define pthread_attr_init init
#define pthread_attr_setstacksize stack
#define pthread_attr_destroy destroy
#define pthread_create create
#define pthread_join join
#include "native_gpu_thread.inc"
int main(int argc, char**) {
    VideoCore::RendererBase renderer;
    Core::Frontend::GraphicsContext context;
    Tegra::Control::Scheduler scheduler;
    using VideoCommon::GPUThread::ThreadManager;
    if (argc > 1) {
        ThreadManager manager;
        manager.command = [&] { manager.ready.release(); throw std::runtime_error("injected worker failure"); };
        manager.queue.EmplaceWait(42);
        manager.StartThread(renderer, context, scheduler);
        manager.ready.acquire();
        manager.Stop();
        return 1;
    }
    for (fault = 1; fault <= 3; ++fault) {
        ThreadManager manager;
        try { manager.StartThread(renderer, context, scheduler); assert(false); }
        catch (const std::system_error&) {}
        assert(attributes_live == 0 && joins == 0);
    }
    fault = 0;
    ThreadManager manager;
    for (int i = 1; i <= 3; ++i) {
        manager.finished = false;
        manager.queue.EmplaceWait(42);
        manager.StartThread(renderer, context, scheduler);
        manager.ready.acquire();
        manager.Stop();
        manager.Stop();
        assert(manager.finished && joins == i && attributes_live == 0);
    }
    { ThreadManager implicit;
      implicit.queue.EmplaceWait(42);
      implicit.StartThread(renderer, context, scheduler);
      implicit.ready.acquire(); }
    assert(joins == 4 && attributes_live == 0);
    // A command already executing must exit before the protected owner goes.
    // Old ordering deterministically exposes the cleared unique_ptr, without
    // dereferencing it or relying on a scheduling delay.
    for (bool early_join : {false, true}) {
        std::binary_semaphore entered{0}, release{0}, join_started{0};
        bool pointer_alive = false, owner_destroyed = false;
        struct Protected {
            ThreadManager worker;
            bool& destroyed;
            explicit Protected(bool& flag) : destroyed(flag) {}
            ~Protected() { worker.Stop(); destroyed = true; }
        };
        auto owner = std::make_unique<Protected>(owner_destroyed);
        owner->worker.command = [&] {
            entered.release();
            release.acquire();
            pointer_alive = owner != nullptr && !owner_destroyed;
        };
        owner->worker.queue.EmplaceWait(42);
        owner->worker.StartThread(renderer, context, scheduler);
        entered.acquire();
        joining = &join_started;
        std::thread teardown([&] {
            if (early_join) owner->worker.Stop();
            owner.reset();
        });
        join_started.acquire();
        assert(!owner_destroyed);
        release.release();
        teardown.join();
        joining = nullptr;
        assert(owner_destroyed && pointer_alive == early_join);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='eden-gpu-thread-') as folder:
    temp = Path(folder)
    (temp / 'check.cpp').write_text(code)
    subprocess.run(['c++', '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror',
                    '-I', str(upstream), '-I', str(root / 'headless'), str(temp / 'check.cpp'),
                    '-o', str(temp / 'check')], check=True)
    subprocess.run([str(temp / 'check')], check=True, timeout=15)
    failed = subprocess.run([str(temp / 'check'), 'throw'], capture_output=True, text=True, timeout=15)
    assert failed.returncode != 0 and '[ProsperoEden] GPU worker: injected worker failure' in failed.stderr
print('Native GPU stack, stop/wake/join, creation failures and blocked-command teardown ordering/rejection PASS')
