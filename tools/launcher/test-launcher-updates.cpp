// SPDX-License-Identifier: GPL-3.0-or-later
// Test driver uses fixture transport; production requests always use HTTPS.
#include "pe/ui/launcher_updates.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
using namespace pe::ui::launcher_updates;
int main(int argc, char** argv) {
    if (argc < 4) return 2;
    const std::string root=argv[1], fixtures=argv[2], mode=argv[3], id=argc>4?argv[4]:"test-pack";
    try {
        Host host; host.version="1.003.000";
        host.capabilities={"draw.atlas.v1","actions.launch.v1","audio.named-events.v1","loading.scene.v1","state.v1","updates.launcher-packs.v1"};
        Fetch fetch=[&](const std::string& url, std::size_t limit, const Sink& sink, const std::atomic<bool>& cancel) {
            if (!https_url(url)) throw std::runtime_error("HTTPS required");
            std::ifstream file(fixtures+"/"+url.substr(url.find_last_of('/')+1),std::ios::binary);
            if(!file) throw std::runtime_error("Fixture not found");
            char bytes[4096]; std::size_t total=0;
            while(file) {
                file.read(bytes,sizeof(bytes)); const auto size=static_cast<std::size_t>(file.gcount());
                total+=size;
                if(cancel || total>limit || (size && !sink(bytes,size))) throw std::runtime_error("Transport stopped");
            }
        };
        if(mode=="recover") { Manager::recover(root); std::cout<<"{}\n"; return 0; }
        if(mode=="choose") {
            Json feed,m; std::ifstream(fixtures+"/launcher-updates.json")>>feed;
            std::ifstream(root+"/launchers/"+id+"/manifest.json")>>m;
            std::cout<<choose_release(feed,m,host).dump()<<'\n'; return 0;
        }
        if(mode=="destroy-check") {
            std::atomic<bool> entered{false},finished{false};
            Fetch delayed=[&](const std::string&,std::size_t,const Sink&,const std::atomic<bool>& cancel) {
                entered=true;
                while(!cancel) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                finished=true;
                throw std::runtime_error("Cancelled");
            };
            auto manager=std::make_unique<Manager>(root,host,delayed);
            manager->check(id);
            for(int i=0;!entered && i<1000;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if(!entered)throw std::runtime_error("Worker did not start");
            const auto before=std::chrono::steady_clock::now();manager.reset();
            const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-before).count();
            for(int i=0;!finished && i<1000;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if(!finished)throw std::runtime_error("Worker did not cancel");
            // Let the detached worker finish reporting before its test captures leave scope.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::cout<<Json({{"cancelled",true},{"destructor_ms",ms}}).dump()<<'\n';return 0;
        }
        Manager manager(root,host,fetch);
        auto wait=[&] {
            for(int i=0;manager.busy() && i<3000;++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if(manager.busy()) throw std::runtime_error("Test timed out");
            manager.poll();
        };
        if(mode=="automatic-off") {
            if(!manager.set_automatic(false)) return 3;
            std::cout<<Json({{"started",manager.check(id,true)}}).dump()<<'\n';return 0;
        }
        if(mode=="rollback") { if(!manager.rollback(id))return 3; wait(); }
        else {
            if(!manager.check(id,mode=="automatic")) {std::cout<<"{\"started\":false}\n";return 0;}
            wait();
            if(mode=="install" || mode=="stage" || mode=="cancel-ready") { if(manager.download())wait(); }
        }
        if(mode=="cancel-ready") manager.cancel();
        if((mode=="install" || mode=="rollback") && manager.commit()) wait();
        std::cout<<manager.status(id).dump()<<'\n';return 0;
    } catch(const std::exception& e) {std::cout<<Json({{"error",e.what()}}).dump()<<'\n';return 1;}
}
