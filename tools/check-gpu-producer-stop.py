#!/usr/bin/env python3
"""Run the generated GPU producer against a full queue with no consumer."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
cache=Path((root/'.local/headless-cache').read_text().strip())
worker=(cache/'native-local/headless/gpu_thread.cpp').read_text()
start=worker.index('u64 ThreadManager::PushCommand(')
body=worker[start:worker.index('\n} // namespace',start)]
assert 'state.queue.EmplaceWait(' not in body
main=(root/'headless/main.cpp').read_text()
assert 'system.Pause();' not in main
code=r'''
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stop_token>
#include <thread>
#include "common/bounded_threadsafe_queue.h"
using u64=unsigned long long;
using CommandData=int;
struct Item { int command{}; u64 fence{}; bool block{}; };
struct ThreadManager {
 struct { std::mutex write_lock; u64 last_fence{}; std::atomic<u64> signaled_fence{};
          Common::SPSCQueue<Item,2> queue; std::condition_variable_any cv; } state;
 std::stop_source stop_source;
 u64 PushCommand(CommandData&&,bool,bool);
 // The worker file defines these after PushCommand; here there is no worker to end.
 void NotifyShutdown();
 void Stop() { stop_source.request_stop(); }
};
'''+body+r'''
int main() {
 ThreadManager manager;
 assert(manager.PushCommand(1,false,true)==1);
 assert(manager.PushCommand(2,false,true)==2);
 auto blocked=std::async(std::launch::async,[&]{return manager.PushCommand(3,false,true);});
 assert(blocked.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
 manager.stop_source.request_stop();
 assert(blocked.wait_for(std::chrono::seconds(1))==std::future_status::ready);
 assert(blocked.get()==0);
 Item item; assert(manager.state.queue.TryPop(item)&&item.command==1);
 assert(manager.state.queue.TryPop(item)&&item.command==2);
 assert(!manager.state.queue.TryPop(item));
 assert(manager.PushCommand(4,false,true)==0);
 assert(!manager.state.queue.TryPop(item));
 ThreadManager synchronous;
 auto fence_wait=std::async(std::launch::async,[&]{return synchronous.PushCommand(5,true,false);});
 assert(fence_wait.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
 synchronous.stop_source.request_stop();
 assert(fence_wait.wait_for(std::chrono::seconds(1))==std::future_status::ready);
 fence_wait.get();
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'check.cpp';p.write_text(code);exe=Path(tmp)/'check'
 subprocess.run(['c++','-std=c++20','-pthread','-fsanitize=address,undefined','-I'+str(cache/'source/src'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=10)
print('GPU producer cancellation: PASS full queue, ordered contents, stopped enqueue and fence wait')