// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef PE_RUNTIME_PREVIEW
#include "fake_services.hpp"
#include "pe/core/file.hpp"
#include "pe/gfx/gl_batch.hpp"
#include "pe/gfx/gl_program.hpp"
#include "pe/ui/runtime_launcher.hpp"
#include "pe/ui/runtime_models.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <map>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

class RuntimePreviewServices : public pe::host::FakeServices {
public:
    using FakeServices::FakeServices;
    nlohmann::json launches=nlohmann::json::array();
    void will_play(const std::string& file) override {
        int profile=-1;
        for(int i=0;i<static_cast<int>(people.size());++i) if(people[i].playing) profile=i;
        launches.push_back({{"file",file},{"profile",profile}});
    }
};

int main(int argc,char**argv) {
    if(argc<3) return 2;
    const std::string assets=argv[1],out=argv[2];
    std::filesystem::create_directories(out);
    const char* env=std::getenv("PE_LAUNCHER_DATA");const std::string data=env?env:out+"/data";
    auto platform=reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    const auto display=platform?platform(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,nullptr):eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major=0,minor=0;if(!eglInitialize(display,&major,&minor) || !eglBindAPI(EGL_OPENGL_API))return 3;
    EGLint count=0;EGLConfig config{};const EGLint attributes[]{EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_NONE};
    if(!eglChooseConfig(display,attributes,&config,1,&count) || count<1)return 4;
    const EGLint context_attributes[]{EGL_CONTEXT_MAJOR_VERSION,4,EGL_CONTEXT_MINOR_VERSION,5,EGL_CONTEXT_OPENGL_PROFILE_MASK,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,EGL_NONE};
    const auto context=eglCreateContext(display,config,EGL_NO_CONTEXT,context_attributes);
    if(!eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context))return 5;
    int width=1920,height=1080;
    if(argc>4 && std::string(argv[3])!="--interactive"){width=std::stoi(argv[3]);height=std::stoi(argv[4]);}
    pe::gfx::set_glsl_prefix("#version 450 core\n");
    pe::gfx::GlBatch batch;if(!batch.init())return 6;
    pe::gfx::Font font;std::string blob;if(!pe::read_file(assets+"/fonts/montserrat-medium.pefont",&blob) || !font.load(blob))return 7;
    auto texture=batch.create_font_texture(font);
    RuntimePreviewServices services(out);
    services.people={{"Eden",true},{"Marina",false}};
    pe::ui::Textures textures(batch,services);textures.load_art(assets);
    pe::audio::Mixer mixer;
    {
        pe::ui::RuntimeLauncher launcher(services,textures,{&font,texture},batch,true,data,&mixer);
        GLuint fbo=0,render=0;glGenFramebuffers(1,&fbo);glGenTextures(1,&render);glBindTexture(GL_TEXTURE_2D,render);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,render,0);
        pe::gfx::DrawList list;std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width)*height*4),flipped(pixels.size());
        const std::map<std::string,pe::ui::Key> keys{{"cross",pe::ui::Key::cross},{"circle",pe::ui::Key::circle},{"square",pe::ui::Key::square},{"triangle",pe::ui::Key::triangle},{"options",pe::ui::Key::options},{"l1",pe::ui::Key::l1},{"r1",pe::ui::Key::r1},{"up",pe::ui::Key::up},{"down",pe::ui::Key::down},{"left",pe::ui::Key::left},{"right",pe::ui::Key::right}};
        auto tick=[&](int n){for(int i=0;i<n;++i){launcher.update(1.0f/60);list.clear();launcher.draw(list);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);batch.sync_font_texture(texture,font);batch.draw(list,pe::gfx::fit_viewport(width,height),width,height);std::int16_t audio[1600]{};mixer.render(audio,800);std::this_thread::sleep_for(std::chrono::milliseconds(1));}};
        auto capture=[&](const std::string& filename){glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());const auto row=static_cast<std::size_t>(width)*4;for(int y=0;y<height;++y)std::copy_n(pixels.data()+static_cast<std::size_t>(height-1-y)*row,row,flipped.data()+static_cast<std::size_t>(y)*row);stbi_write_png(filename.c_str(),width,height,4,flipped.data(),width*4);};
        auto loading=[&](float seconds){
            const auto* scene=launcher.loading_scene();if(!scene)throw std::runtime_error("Active pack has no loading scene");
            std::string text,body,runtime;
            if(!pe::read_file(assets+"/../../loading_text.glsl",&text) || !pe::read_file(assets+"/../../loading_scene.glsl",&body) || !pe::read_file(assets+"/../../runtime_loading.glsl",&runtime))throw std::runtime_error("Loading shaders are unavailable");
            const std::string directive="#include \"runtime_loading.glsl\"";
            const auto include=body.find(directive);
            if(include!=std::string::npos)body.replace(include,directive.size(),runtime);
            const std::string fragment="uniform sampler2D loading_art;uniform float seconds;uniform vec2 surface;out vec4 color;\n"+text+body+
                "\nvoid main(){uint state[24];for(int i=0;i<24;++i)state[i]=0u;state[23]=0x50454c31u;color=vec4(loading_scene(gl_FragCoord.xy,surface,seconds,state),1.0);}";
            const char* vertex="void main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));gl_Position=vec4(p*2.0-1.0,0,1);}";
            const auto program=pe::gfx::build_program("runtime loading",vertex,fragment.c_str());if(!program)throw std::runtime_error("Loading shader compilation failed");
            const auto atlas=batch.create_texture(scene->atlas.width,scene->atlas.height,scene->atlas.rgba.data());
            GLuint vao=0;glGenVertexArrays(1,&vao);glBindVertexArray(vao);glUseProgram(program);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,atlas);
            glUniform1i(glGetUniformLocation(program,"loading_art"),0);glUniform1f(glGetUniformLocation(program,"seconds"),seconds);glUniform2f(glGetUniformLocation(program,"surface"),float(width),float(height));
            glViewport(0,0,width,height);glDisable(GL_BLEND);glDrawArrays(GL_TRIANGLES,0,3);
            glDeleteVertexArrays(1,&vao);batch.delete_texture(atlas);glDeleteProgram(program);
        };
        tick(180);std::cout<<"READY "<<launcher.active_id()<<" "<<launcher.error()<<std::endl;
        std::string input;
        while(std::getline(std::cin,input)){
            std::istringstream parser(input);std::string command,arg;int frames=2;parser>>command>>arg>>frames;
            if(command=="exit")break;
            if(command=="select"){launcher.select(arg);tick(120);}
            else if(command=="key" && keys.contains(arg)){launcher.press(keys.at(arg));tick(std::max(1,frames));}
            else if(command=="tick"){tick(arg.empty()?60:std::stoi(arg));}
            else if(command=="capture"){capture(arg);}
            else if(command=="loading"){loading(arg.empty()?0.5f:std::stof(arg));}
            else if(command=="status"){std::cout<<"STATE active="<<launcher.active_id()<<" done="<<launcher.done()<<" game="<<launcher.selected_game()<<" error="<<launcher.error()<<std::endl;}
            else if(command=="picker"){launcher.open_picker();tick(2);}
            else if(command=="prefs"){std::cout<<"PREFERENCES "<<nlohmann::json(services.preferences()).dump()<<std::endl;}
            else if(command=="profiles"){std::cout<<"PROFILES "<<nlohmann::json(services.profiles()).dump()<<std::endl;}
            else if(command=="launches"){std::cout<<"LAUNCHES "<<services.launches.dump()<<std::endl;}
            else if(command=="remove-first"){const auto games=services.games();if(!games.empty())services.removed.push_back(games.front().file);}
            else if(command=="offer"){services.update_version=arg;tick(30);}
            else if(command=="update-ready"){services.update_steps=600;tick(2);}
            else if(command=="sync"){services.save_sync_on=arg!="off";services.save_sync_conflict=arg=="conflict";services.save_sync_fails=arg=="failed";}
            else if(command=="room"){services.room.phase=arg=="connected"?Eden::Multiplayer::Phase::Connected:arg=="connecting"?Eden::Multiplayer::Phase::Connecting:Eden::Multiplayer::Phase::Idle;tick(30);}
            else if(command=="recreate"){
                launcher.release_pack_textures();textures.release();batch.delete_texture(texture);batch.release();
                if(!batch.init())return 10;texture=batch.create_font_texture(font);launcher.set_font_texture(texture);textures.load_art(assets);launcher.reload_pack_textures();tick(90);
            }
            std::cout<<"OK "<<command<<" GL="<<glGetError()<<std::endl;
        }
        glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&render);
    }
    textures.release();batch.delete_texture(texture);batch.release();eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,context);eglTerminate(display);return 0;
}
#endif
