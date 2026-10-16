// SPDX-License-Identifier: GPL-3.0-or-later
#include "updater.hpp"
#include "update_check.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;
static void write(const fs::path& path,const std::string& text) {fs::create_directories(path.parent_path());std::ofstream(path)<<text;}
int main(int argc,char** argv) {
    assert(argc==2);const fs::path root=argv[1],data=root/"data",stage=root/"staged";
    std::string reason;int checks=0;
    auto check=[&](bool expected) {assert(self_update::launcher_update_compatible(data.string(),stage.string(),reason)==expected);++checks;};
    check(true);write(data/"config/launcher.json",R"({"selected":"builtin"})");check(true);
    write(data/"config/launcher.json",R"({"selected":"sample"})");
    const auto manifest=data/"launchers/sample/manifest.json",descriptor=stage/"launcher-runtime.json";
    write(manifest,R"({"host_contract":"launcher-v1.0"})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0"})");check(true);
    write(manifest,R"({"host_contract":"launcher-v1.0","api":{"major":1,"min_minor":1},"required_capabilities":["updates.launcher-packs.v1"]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":0,"capabilities":["updates.launcher-packs.v1"]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":1,"capabilities":[]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":1,"capabilities":["updates.launcher-packs.v1"]})");check(true);
    write(descriptor,R"({"supports":"launcher-v2.0,launcher-v1.0","api_major":1,"api_minor":2,"capabilities":["extra.v1","updates.launcher-packs.v1"]})");check(true);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":2,"api_minor":1,"capabilities":["updates.launcher-packs.v1"]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":-1,"capabilities":["updates.launcher-packs.v1"]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":1,"capabilities":"updates.launcher-packs.v1"})");check(false);
    write(descriptor,R"({"supports":"launcher-v2.0","api_major":1,"api_minor":1,"capabilities":["updates.launcher-packs.v1"]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":1,"api_minor":0,"capabilities":["updates.launcher-packs.v1"]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":1,"capabilities":["updates.launcher-packs.v1",]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":1,"capabilities":["updates.launcher-packs.v1"]})");
    write(manifest,R"({"host_contract":"launcher-v1.0","api":{"major":1,"min_minor":0},"required_capabilities":[]})");check(true);
    write(manifest,R"({"host_contract":"launcher-v1.0","api":{"major":1,"min_minor":1e0}})");check(false);
    write(data/"config/launcher.json",R"({"selected":"../escape"})");check(false);
    unsigned number=0;const char* value=nullptr;size_t length=0;char text[64]{};
    auto need=[&](bool ok){assert(ok);++checks;};
    std::string json=R"({"value":4294967295,"object":{"a":["b"]}})";
    need(update_check_json_uint(json.data(),json.size(),"value",&number)==1 && number==4294967295u);
    need(update_check_json_value(json.data(),json.size(),"object",&value,&length)==1 && std::string(value,length)==R"({"a":["b"]})");
    for(const auto* malformed:{R"({"v":4294967296})",R"({"v":-1})",R"({"v":01})",R"({"v":1.0})",R"({"v":null})",R"({"v":0,"v":1})",R"({"v":1,})"}) {
        const std::string item=malformed;need(update_check_json_uint(item.data(),item.size(),"v",&number)==-1);
    }
    json=R"(["one","two\u002ev1"])";
    need(update_check_json_array_string(json.data(),json.size(),1,text,sizeof(text))==1 && std::string{text}=="two.v1");
    need(update_check_json_array_string(json.data(),json.size(),2,text,sizeof(text))==0);
    for(const auto* malformed:{R"(["one",])",R"([true])",R"(["one"] trailing)",R"(["unterminated])",R"(["one\u0000hidden"])"}) {
        const std::string item=malformed;need(update_check_json_array_string(item.data(),item.size(),0,text,sizeof(text))==-1);
    }
    std::cout<<"PASS: "<<checks<<" application-update compatibility and bounded JSON checks\n";
}
