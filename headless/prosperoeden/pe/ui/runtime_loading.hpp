// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pe/ui/runtime_assets.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>

namespace pe::ui {
// Validated, owned CPU data. Render backends upload independent copies after launcher teardown.
struct RuntimeLoadingScene { gfx::Image atlas; };
inline RuntimeLoadingScene prepare_loading_scene(const nlohmann::json& scene,
                                                 const std::function<std::string(const std::string&)>& resolve) {
    if(scene.at("schema")!=1 || !scene.at("layers").is_array() || scene.at("layers").size()>16) throw std::runtime_error("Unsupported loading scene");
    RuntimeLoadingScene result;
    result.atlas=load_runtime_image(resolve(scene.at("atlas")),4096,4096,32*1024*1024);
    if(result.atlas.width<512 || result.atlas.height<2) throw std::runtime_error("Invalid loading atlas");
    auto& bytes=result.atlas.rgba;
    const auto word=[&](std::size_t index,std::uint32_t value) {
        for(int i=0;i<4;++i) bytes[index*4+i]=static_cast<std::uint8_t>(value>>(i*8));
    };
    const auto number=[&](std::size_t index,float value) {
        if(!std::isfinite(value) || std::abs(value)>100000) throw std::runtime_error("Invalid loading scene number");
        std::uint32_t bits{}; std::memcpy(&bits,&value,4);word(index,bits);
    };
    word(0,0x50454c31);word(1,scene.at("layers").size());word(2,scene.value("background",0u));
    std::size_t offset=8;
    for(const auto& layer:scene.at("layers")) {
        const auto rect=layer.at("rect").get<std::vector<float>>();
        const auto uv=layer.value("uv",std::vector<float>{0,0,1,1});
        if(rect.size()!=4 || uv.size()!=4 || rect[2]<=0 || rect[3]<=0) throw std::runtime_error("Invalid loading layer rectangle");
        if(uv[0]<0 || uv[1]<0 || uv[2]<=0 || uv[3]<=0 || uv[0]+uv[2]>1 || uv[1]+uv[3]>1)
            throw std::runtime_error("Invalid loading atlas region");
        const float alpha=layer.value("alpha",1.0f),radius=layer.value("radius",0.0f),width=layer.value("width",1.0f);
        const float period=layer.value("period",0.0f),start=layer.value("move_start",0.0f),end=layer.value("move_end",0.0f);
        const float fade_in=layer.value("fade_in",0.0f),fade_start=layer.value("fade_out_start",0.0f),fade_end=layer.value("fade_out_end",0.0f);
        if(alpha<0 || alpha>1 || radius<0 || width<0 || period<0 || period>60 || start<0 || end<start ||
           fade_in<0 || fade_start<0 || fade_end<fade_start || (period>0 && (end>period || fade_in>period || fade_end>period)))
            throw std::runtime_error("Invalid loading animation track");
        const std::string kind=layer.value("kind","image");
        if(kind!="image" && kind!="border") throw std::runtime_error("Unknown loading layer kind");
        word(offset,kind=="image"?1:2);
        for(int i=0;i<4;++i) number(offset+1+i,rect[i]);
        for(int i=0;i<4;++i) number(offset+5+i,uv[i]);
        word(offset+9,layer.value("color",0xffffffu));
        number(offset+10,layer.value("alpha",1.0f));number(offset+11,layer.value("radius",0.0f));number(offset+12,layer.value("width",1.0f));
        number(offset+13,layer.value("period",0.0f));number(offset+14,layer.value("y_from",0.0f));number(offset+15,layer.value("y_to",0.0f));
        number(offset+16,layer.value("move_start",0.0f));number(offset+17,layer.value("move_end",0.0f));
        number(offset+18,layer.value("fade_in",0.0f));number(offset+19,layer.value("fade_out_start",0.0f));number(offset+20,layer.value("fade_out_end",0.0f));
        for(int i=21;i<24;++i) number(offset+i,0);
        offset+=24;
    }
    return result;
}
}
