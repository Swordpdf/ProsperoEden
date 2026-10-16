// SPDX-License-Identifier: GPL-3.0-or-later
// Link instead of remote/http.c in the updater integration test binary only.
#include "../../headless/remote/http.h"
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
extern "C" int remote_http_get(const remote_http_request* request, remote_http_result* result) {
    *result={};
    if(!request->https_only || request->authorization || !request->url || std::string(request->url).find("https://")!=0) return -1;
    const char* root=std::getenv("PE_UPDATE_FIXTURES"); if(!root)return -1;
    const std::string url=request->url;
    std::ifstream file(std::string(root)+"/"+url.substr(url.find_last_of('/')+1),std::ios::binary|std::ios::ate);
    if(!file) {result->status=404;return 0;}
    result->status=200;result->length=static_cast<std::uint64_t>(file.tellg());file.seekg(0);
    if(request->begin && !request->begin(request->user,200,result->length)) return -1;
    char bytes[4096];
    while(file) {
        if(request->stop && request->stop(request->user)) {result->stopped=1;return -1;}
        file.read(bytes,sizeof(bytes));const auto size=static_cast<std::size_t>(file.gcount());
        if(size && !request->sink(request->user,bytes,size)) {result->stopped=1;return -1;}
    }
    return 0;
}
