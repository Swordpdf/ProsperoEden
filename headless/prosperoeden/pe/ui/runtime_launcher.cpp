#include "../../../hud.h"
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pe/ui/runtime_launcher.hpp"
#include "pe/ui/runtime_models.hpp"
#include "pe/ui/runtime_catalog.hpp"
#include "pe/ui/launcher_updates.hpp"
#include "pe/ui/runtime_filesystem.hpp"
#include "pe/ui/launch_commit.hpp"
#include "pe/audio/wav.hpp"
#include "pe/core/file.hpp"
#include "pe/core/log.hpp"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <sys/stat.h>

namespace pe::ui {
using Json = nlohmann::json;
namespace {
constexpr std::size_t kScriptBytes = 2 * 1024 * 1024;
constexpr std::size_t kVmBytes = 16 * 1024 * 1024;
constexpr int kInstructions = 300000;
const std::set<std::string> kCapabilities{"draw.atlas.v1", "actions.launch.v1", "audio.named-events.v1", "loading.scene.v1", "state.v1", "updates.launcher-packs.v1", "updates.launcher-packs.view.v1"};
bool identifier(std::string_view text) {
    return !text.empty() && text.size() <= 48 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}
std::string pack_path(const std::string& root, const std::string& relative) {
    return runtime_fs::resource_path(root, relative);
}
Json read_json(const std::string& path, std::size_t limit = 65536) {
    std::string text;
    if (!pe::read_file(path, &text, limit)) throw std::runtime_error("Cannot read " + path);
    return Json::parse(text);
}
bool mkdirs(const std::string& path) {
    for (std::size_t end = 1; end <= path.size(); ++end) if (end == path.size() || path[end] == '/') {
        const auto part = path.substr(0, end);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}
bool write_json(const std::string& path, const Json& value) {
    const auto text = value.dump(2);
    if (text.size() > 131072) return false;
    if (!mkdirs(path.substr(0, path.find_last_of('/')))) return false;
    const auto tmp = path + ".tmp";
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    file << text << '\n'; file.flush(); const bool ok = file.good(); file.close();
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
    return true;
}
void push_json(lua_State* lua, const Json& value, int depth = 0) {
    if (depth > 16) { lua_pushnil(lua); return; }
    if (value.is_object()) {
        lua_createtable(lua, 0, static_cast<int>(value.size()));
        for (auto it = value.begin(); it != value.end(); ++it) { push_json(lua, it.value(), depth + 1); lua_setfield(lua, -2, it.key().c_str()); }
    } else if (value.is_array()) {
        lua_createtable(lua, static_cast<int>(value.size()), 0); int index = 1;
        for (const auto& item : value) { push_json(lua, item, depth + 1); lua_rawseti(lua, -2, index++); }
    } else if (value.is_string()) { const auto& text = value.get_ref<const std::string&>(); lua_pushlstring(lua, text.data(), text.size()); }
    else if (value.is_boolean()) lua_pushboolean(lua, value.get<bool>());
    else if (value.is_number_integer() || value.is_number_unsigned()) lua_pushinteger(lua, value.get<lua_Integer>());
    else if (value.is_number()) lua_pushnumber(lua, value.get<double>());
    else lua_pushnil(lua);
}
Json table_json(lua_State* lua, int index, int depth = 0, int* count = nullptr) {
    int local = 0; if (!count) count = &local;
    if (++*count > 8192 || depth > 12) throw std::runtime_error("Argument exceeds API limits");
    index = lua_absindex(lua, index);
    switch (lua_type(lua, index)) {
    case LUA_TNIL: return nullptr;
    case LUA_TBOOLEAN: return lua_toboolean(lua, index) != 0;
    case LUA_TNUMBER:
        if (lua_isinteger(lua, index)) return lua_tointeger(lua, index);
        if (!std::isfinite(lua_tonumber(lua, index))) throw std::runtime_error("Non-finite number");
        return lua_tonumber(lua, index);
    case LUA_TSTRING: {
        std::size_t n = 0; const char* p = lua_tolstring(lua, index, &n);
        if (n > 65536) throw std::runtime_error("String exceeds API limits");
        return std::string(p, n);
    }
    case LUA_TTABLE: {
        Json result = Json::object(); bool array = true; int length = 0;
        lua_pushnil(lua);
        while (lua_next(lua, index)) {
            if (lua_type(lua, -2) != LUA_TSTRING && !lua_isinteger(lua, -2)) { lua_pop(lua, 2); throw std::runtime_error("Invalid table key"); }
            if (!lua_isinteger(lua, -2) || lua_tointeger(lua, -2) < 1 || lua_tointeger(lua, -2) > 8192) array = false;
            std::string key = lua_isinteger(lua, -2) ? std::to_string(lua_tointeger(lua, -2)) : lua_tostring(lua, -2);
            result[key] = table_json(lua, -1, depth + 1, count); ++length; lua_pop(lua, 1);
        }
        if (array && length > 0) {
            Json list = Json::array();
            for (int i = 1; i <= length; ++i) { const auto key = std::to_string(i); if (!result.contains(key)) return result; list.push_back(result[key]); }
            return list;
        }
        return result;
    }
    default: throw std::runtime_error("Only data values may cross the launcher API");
    }
}
const char* key_name(Key key) {
    constexpr const char* keys[]{"cross","circle","square","triangle","options","l1","r1","up","down","left","right"};
    return keys[static_cast<int>(key)];
}
Color color(const Json& p, const char* key = "color", std::uint32_t fallback = 0xffffff) {
    const auto word = p.value(key, fallback); const float alpha = std::clamp(p.value(std::string(key)=="color"?"alpha":(std::string(key)+"_alpha").c_str(), 1.0f), 0.0f, 1.0f);
    return {float((word >> 16) & 255) / 255, float((word >> 8) & 255) / 255, float(word & 255) / 255, alpha};
}
Rect rect(const Json& p) { return {p.value("x",0.0f),p.value("y",0.0f),p.value("w",0.0f),p.value("h",0.0f)}; }
}

struct RuntimeLauncher::Impl {
    struct Pack {
        Impl* host{}; std::string id, folder; Json manifest, state = Json::object();
        lua_State* lua{}; int module = LUA_NOREF, instructions{}; std::size_t memory{}; bool live{};
        std::map<std::string, std::uint32_t> textures;
        gfx::Font font; std::uint32_t font_texture{};
        std::map<std::string, std::unique_ptr<audio::DecodedWav>> recordings;
        std::map<std::string, audio::Clip> clips;
        std::unique_ptr<RuntimeLoadingScene> loading;
        std::uint64_t audio_fence{};
        ~Pack() { if (host) { for(const auto& [id,texture]:textures) host->batch.delete_texture(texture); host->batch.delete_texture(font_texture); } if (lua) { lua_sethook(lua, nullptr, 0, 0); lua_close(lua); } }
    };
    #include "pe/ui/runtime_update_view.inc"
    std::unique_ptr<launcher_updates::Manager> pack_updates;
    std::string updates_id, updates_name, resume_pack;
    bool updates_page{}, rollback_confirmation{}, auto_checked{}, commit_started{}, replacement_seen{}, recovery_attempted{};
    float updates_poll{};
    Json updates_status = {{"phase", "idle"}};
    Preferences cached_preferences;
    std::map<std::string, std::pair<float,Json>> query_cache;
    Json query_result;
    Services& services; Textures& covers; Fonts fonts; gfx::GlBatch& batch; audio::Mixer* mixer;
    std::function<void(gfx::Font&)> configure_font;
    std::string data_root, active = "builtin", last_error, next_id, selected, discovery_error, discovery_report;
    std::unique_ptr<Launcher> builtin; std::unique_ptr<Pack> pack;
    // Mixer clip references remain valid until the frontend has stopped its audio thread.
    std::vector<std::unique_ptr<Pack>> retired;
    std::vector<Json> installed; RuntimeCatalog catalog; std::future<std::vector<Game>> scan;
    std::future<Eden::Multiplayer::RoomDirectory> directory; Eden::Multiplayer::RoomDirectory room_directory;
    std::atomic<bool> stop{false}; std::future<std::optional<std::string>> keyboard;
    Json keyboard_status = { {"ready",false} }, launch = {{"stage","idle"}}, offer;
    std::string launch_file, launch_profile; bool sync_started{}, first_start{}, picker{}, exit_update{};
    int picker_row{}, draw_commands{}; float time{}, poll{}, catalog_poll{}; std::uint64_t source_generation{}; gfx::DrawList* drawing{};
    Impl(Services& s, Textures& t, const Fonts& f, gfx::GlBatch& b, bool first, std::string root, audio::Mixer* m, std::function<void(gfx::Font&)> configure)
        : cached_preferences(s.preferences()), services(s), covers(t), fonts(f), batch(b), mixer(m), data_root(std::move(root)), first_start(first) {
        configure_font=std::move(configure);
        auto version=s.version(); if(version.starts_with("v")) version.erase(0,1);
        launcher_updates::Host host; host.version=version; host.capabilities=kCapabilities;
        pack_updates=std::make_unique<launcher_updates::Manager>(data_root,std::move(host));
        const auto recovery=pack_updates->status("");
        if(recovery.value("phase","")=="failed") last_error="Launcher recovery: "+recovery.value("error","");
    }
    ~Impl() { stop = true; if (directory.valid()) directory.wait(); if (keyboard.valid()) keyboard.wait(); release_textures(); }
    std::string current_profile() { const auto list=services.profiles(); for (int i=0;i<static_cast<int>(list.size());++i) if (list[i].playing) return services.profile_key(i); return {}; }
    Game& game(const Json& p) {
        const std::string file = p.value("game", "");
        if (auto* found = catalog.find(file)) return *found;
        throw std::runtime_error("Game is no longer in the library");
    }
    void refresh() { if (!scan.valid()) scan = std::async(std::launch::async, [&] { return services.games(); }); }
    void discover() {
        installed.clear(); discovery_error.clear(); const auto root = data_root + "/launchers";
        std::string details;
        try {
            for (const auto& id : runtime_fs::entries(root)) {
                if (id=="builtin" || !identifier(id) || installed.size() >= 64) continue;
                try {
                    const auto folder = root + "/" + id;
                    runtime_fs::check(folder, runtime_fs::Kind::directory);
                    const auto m = read_json(pack_path(folder, "manifest.json"));
                    if (m.at("id") != id || m.at("schema") != 1) throw std::runtime_error("Manifest ID or schema does not match");
                    installed.push_back({{"id",id},{"name",m.value("name",id)}});
                } catch (const std::exception& error) {
                    if (discovery_error.empty()) discovery_error = "Skipped " + id + ": " + error.what();
                    details += "PE_LAUNCHER_SKIPPED id=" + id + " reason=" + error.what() + "\n";
                }
            }
        } catch (const std::exception& error) {
            discovery_error = error.what();
        }
        std::sort(installed.begin(), installed.end(), [](const auto& a, const auto& b) { return a.at("name").template get<std::string>() < b.at("name").template get<std::string>(); });
        const auto report = "PE_LAUNCHER_DISCOVERY root=" + root + " found=" + std::to_string(installed.size()) +
                            " error=" + discovery_error + "\n" + details + "PE_LAUNCHER_INSTALLED " + Json(installed).dump() + "\n";
        if (report != discovery_report) { std::fputs(report.c_str(), stderr); discovery_report = report; }
    }
    static void* allocate(void* ud, void* ptr, std::size_t old_size, std::size_t new_size) {
        auto& p = *static_cast<Pack*>(ud); if (!ptr) old_size = 0;
        if (!new_size) { std::free(ptr); p.memory -= old_size; return nullptr; }
        if (new_size > kVmBytes || p.memory - old_size > kVmBytes - new_size) return nullptr;
        void* result = std::realloc(ptr, new_size); if (result) p.memory = p.memory - old_size + new_size; return result;
    }
    static void hook(lua_State* lua, lua_Debug*) {
        auto* p = *static_cast<Pack**>(lua_getextraspace(lua));
        if ((p->instructions -= 1000) <= 0) luaL_error(lua, "Launcher callback exceeded its instruction budget");
    }
    struct Invocation { Pack* pack; const char* method; const Json* argument; bool required; };
    static int invoke_callback(lua_State* lua) {
        const auto* call=static_cast<Invocation*>(lua_touserdata(lua,1));
        lua_rawgeti(lua,LUA_REGISTRYINDEX,call->pack->module);
        lua_getfield(lua,-1,call->method);lua_remove(lua,-2);
        if(!lua_isfunction(lua,-1)) {
            if(call->required) return luaL_error(lua,"Missing launcher callback: %s",call->method);
            return 0;
        }
        push_json(lua,*call->argument);
        lua_call(lua,1,0);return 0;
    }
    bool call(Pack& p, const char* method, const Json& argument = Json::object(), bool required = false) {
        const int base = lua_gettop(p.lua);
        Invocation invocation{&p,method,&argument,required};
        p.instructions = kInstructions; lua_sethook(p.lua, hook, LUA_MASKCOUNT, 1000);
        // Argument/table allocation is also protected against VM memory exhaustion.
        lua_pushcfunction(p.lua,invoke_callback);lua_pushlightuserdata(p.lua,&invocation);
        const int rc = lua_pcall(p.lua, 1, 0, 0); lua_sethook(p.lua, nullptr, 0, 0);
        if (rc != LUA_OK) { const char* error = lua_tostring(p.lua, -1); last_error = error ? error : "Launcher callback failed"; lua_settop(p.lua, base); return false; }
        lua_settop(p.lua, base); return true;
    }
    static int bridge(lua_State* lua) {
        auto* p = static_cast<Pack*>(lua_touserdata(lua, lua_upvalueindex(1)));
        const int operation = static_cast<int>(lua_tointeger(lua, lua_upvalueindex(2)));
        Json result;
        try {
            const char* raw = lua_tostring(lua, 1); if (!raw || std::strlen(raw) > 128) throw std::runtime_error("Invalid API operation");
            const std::string name = raw; const Json params = lua_gettop(lua) >= 2 ? table_json(lua, 2) : Json::object();
            p->host->guard_update_operation(*p,operation,name);
            if (operation == 0) { push_json(lua, p->host->query_cached(*p, name, params)); return 1; }
            else if (operation == 1) { if (!p->live && !p->host->updater_context) throw std::runtime_error("Actions are unavailable during pack initialization"); result = p->host->action(*p, name, params); }
            else { p->host->draw_command(*p, name, params); result = true; }
        } catch (const std::exception& error) { result = {{"ok",false},{"error",error.what()}}; }
        push_json(lua, result); return 1;
    }
    void register_api(Pack& p) {
        luaL_requiref(p.lua, "_G", luaopen_base, 1); lua_pop(p.lua, 1);
        luaL_requiref(p.lua, "math", luaopen_math, 1); lua_pop(p.lua, 1);
        luaL_requiref(p.lua, "table", luaopen_table, 1); lua_pop(p.lua, 1);
        luaL_requiref(p.lua, "string", luaopen_string, 1); lua_pop(p.lua, 1);
        luaL_requiref(p.lua, "utf8", luaopen_utf8, 1); lua_pop(p.lua, 1);
        for (const char* unsafe : {"load","loadfile","dofile","pcall","xpcall","collectgarbage","print","setmetatable"}) { lua_pushnil(p.lua); lua_setglobal(p.lua, unsafe); }
        lua_newtable(p.lua);
        for (int op = 0; op < 3; ++op) { lua_pushlightuserdata(p.lua, &p); lua_pushinteger(p.lua, op); lua_pushcclosure(p.lua, bridge, 2); lua_setfield(p.lua, -2, op == 0 ? "query" : op == 1 ? "action" : "draw"); }
        lua_setglobal(p.lua, "host");
    }
    void release_textures() {
        const auto release = [&](Pack& p) { for (auto& [id, texture] : p.textures) { batch.delete_texture(texture); texture = 0; } batch.delete_texture(p.font_texture); p.font_texture = 0; };
        if (pack) release(*pack);
        if(retained_update_view) release(*retained_update_view);
        for (auto& p : retired) release(*p);
    }
    void load_textures(Pack& p) {
        std::size_t decoded = 0;
        for (auto it = p.manifest.at("assets").begin(); it != p.manifest.at("assets").end(); ++it) {
            if (p.textures.size() >= 32) throw std::runtime_error("Too many pack images");
            const auto path = pack_path(p.folder, it.value().get<std::string>());
            const auto image = load_runtime_image(path,4096,8192,96*1024*1024-decoded);
            decoded += image.rgba.size(); if (decoded > 96 * 1024 * 1024) throw std::runtime_error("Launcher images exceed resource budget");
            const auto texture = batch.create_texture(image.width, image.height, image.rgba.data());
            if (!texture) throw std::runtime_error("Launcher texture upload failed");
            p.textures[it.key()] = texture;
        }
        p.font_texture = batch.create_font_texture(p.font); if (!p.font_texture) throw std::runtime_error("Launcher font upload failed");
    }
    bool select(const std::string& id, bool save = true) {
        if(save && updates_page) { last_error="Close launcher updates before changing launchers";return false; }
        if(save && pack_updates->busy() && pack_updates->status("").value("phase","")=="checking") pack_updates->cancel();
        else if(save && pack_updates->busy()) { last_error="Finish the launcher update before changing launchers"; return false; }
        if (launch.value("stage", "idle") != "idle" || exit_update) { last_error = "Finish the pending operation before changing launchers"; return false; }
        const auto updating=services.update_status().phase;
        if(save && updating!=UpdatePhase::idle && updating!=UpdatePhase::failed && updating!=UpdatePhase::cancelled) { last_error="Finish or cancel the update before switching launchers";return false; }
        if(save && (updating==UpdatePhase::failed || updating==UpdatePhase::cancelled)) services.finish_update();
        if (id != "builtin" && !identifier(id)) return false;
        if(retired.size()>=8) { last_error="Audio output is not releasing the previous launcher"; return false; }
        try {
            query_cache.clear();
            std::unique_ptr<Pack> candidate;
            if (id != "builtin") {
                candidate = std::make_unique<Pack>(); auto& p = *candidate; p.host = this; p.id = id; p.folder = data_root + "/launchers/" + id;
                runtime_fs::check(data_root + "/launchers", runtime_fs::Kind::directory);
                runtime_fs::check(p.folder, runtime_fs::Kind::directory);
                p.manifest = read_json(pack_path(p.folder,"manifest.json"));
                if (p.manifest.at("id") != id || p.manifest.at("schema") != 1 || p.manifest.at("api").at("major") != 1 || (p.manifest.at("api").value("min_minor",0) < 0 || p.manifest.at("api").value("min_minor",0) > 1) || p.manifest.value("host_contract","")!="launcher-v1.0") throw std::runtime_error("Launcher API is incompatible");
                for (const auto& capability : p.manifest.value("required_capabilities", Json::array())) if (!kCapabilities.contains(capability.get<std::string>())) throw std::runtime_error("Required launcher capability is unavailable");
                std::string blob; if (!pe::read_file(pack_path(p.folder,p.manifest.at("font")), &blob, 16 * 1024 * 1024) || !p.font.load(blob)) throw std::runtime_error("Invalid launcher font");
                if(configure_font) configure_font(p.font);
                try { p.state = read_json(data_root + "/config/launcher-state/" + id + ".json",131072); } catch (...) {}
                if(!p.state.is_object()) p.state=Json::object();
                load_textures(p);
                if(p.manifest.contains("loading_scene")) {
                    const auto model=read_json(pack_path(p.folder,p.manifest.at("loading_scene")));
                    p.loading=std::make_unique<RuntimeLoadingScene>(prepare_loading_scene(model,[&](const std::string& name){return pack_path(p.folder,name);}));
                }
                const auto sound_name=p.manifest.value("sounds",std::string{});
                const auto sounds = sound_name.empty()?std::string{}:pack_path(p.folder,sound_name); std::size_t pcm = 0;
                if (!sounds.empty()) {
                    for (const auto& name : runtime_fs::entries(sounds)) {
                        if (name.size() < 4 || !name.ends_with(".wav") || p.recordings.size() >= 128) continue;
                        std::string bytes; if (!pe::read_file(pack_path(p.folder,p.manifest.at("sounds").get<std::string>()+"/"+name), &bytes, 4 * 1024 * 1024)) continue;
                        auto wav = std::make_unique<audio::DecodedWav>(audio::decode_wav(bytes)); if (!wav->ok()) continue;
                        pcm += wav->samples.size()*sizeof(float); if (pcm > 32 * 1024 * 1024) throw std::runtime_error("Launcher audio exceeds budget");
                        const auto cue = name.substr(0,name.size()-4); p.clips[cue] = {wav->samples.data(),wav->samples.size()/2}; p.recordings[cue] = std::move(wav);
                    }
                }
                p.lua = lua_newstate(allocate,&p); if (!p.lua) throw std::runtime_error("Cannot create launcher runtime");
                *static_cast<Pack**>(lua_getextraspace(p.lua)) = &p; register_api(p);
                std::string script; if (!pe::read_file(pack_path(p.folder,p.manifest.at("entry")), &script, kScriptBytes)) throw std::runtime_error("Cannot read launcher script");
                p.instructions = kInstructions; lua_sethook(p.lua,hook,LUA_MASKCOUNT,1000);
                const int loaded = luaL_loadbufferx(p.lua,script.data(),script.size(),"launcher","t");
                if (loaded != LUA_OK || lua_pcall(p.lua,0,1,0) != LUA_OK || !lua_istable(p.lua,-1)) throw std::runtime_error(lua_tostring(p.lua,-1) ? lua_tostring(p.lua,-1) : "Launcher must return its callback table");
                lua_sethook(p.lua,nullptr,0,0); p.module = luaL_ref(p.lua,LUA_REGISTRYINDEX);
                if (!call(p,"init",{{"state",p.state},{"first_start",first_start},{"api_major",1},{"api_minor",1}},true)) throw std::runtime_error(last_error);
            }
            if (save && !write_json(data_root + "/config/launcher.json",{{"schema",1},{"selected",id}})) throw std::runtime_error("Could not save launcher selection");
            if (pack) { call(*pack,"shutdown"); write_json(data_root+"/config/launcher-state/"+pack->id+".json",pack->state); release_textures(); pack->audio_fence=mixer?mixer->stop_all_fenced():0; retired.push_back(std::move(pack)); }
            builtin.reset(); pack = std::move(candidate); active = id; picker = false; next_id.clear();
            if (pack) { pack->live = true; if(catalog.empty()) refresh(); } else { catalog.release_cached_views(); use_theme(services.preferences().theme); builtin = std::make_unique<Launcher>(services,covers,fonts,first_start); if(offer.is_object() && offer.contains("version")) builtin->accept_update_offer(offer.get<UpdateOffer>()); }
            if(save) { last_error.clear(); auto_checked=false; }
            std::fprintf(stderr,"PE_LAUNCHER_SELECTED id=%s\n",id.c_str()); return true;
        } catch (const std::exception& error) { last_error = error.what(); std::fprintf(stderr,"PE_LAUNCHER_REJECTED id=%s reason=%s\n",id.c_str(),last_error.c_str()); return false; }
    }
    void fallback() {
        // Preserve the saved selection for later repair; do not repeatedly reload it this session.
        stop=true;
        if(launch.value("stage","idle")=="syncing") services.stop_save_sync();
        services.cancel_pairing();
        launch = {{"stage","idle"}}; next_id.clear(); select("builtin",false);
    }
    const Json& query_cached(Pack& p, const std::string& name, const Json& args) {
        if(name=="library") return catalog.snapshot(scan.valid());
        if(name=="translate" || name=="state" || name=="measure" || name=="status" || name=="launchers" || name=="launcher_updates") {
            query_result=query(p,name,args); return query_result;
        }
        const auto key=name+":"+args.dump();
        const auto it=query_cache.find(key);
        if(it!=query_cache.end() && time-it->second.first<0.25f) return it->second.second;
        Json result=query(p,name,args);
        if(query_cache.size()>=128) query_cache.clear();
        return query_cache.insert_or_assign(key, std::make_pair(time,std::move(result))).first->second.second;
    }
    Json query(Pack& p, const std::string& name, const Json& args) {
        if (name == "launcher_updates") return pack_updates->status(p.id);
        if (name == "runtime") return {{"api_major",1},{"api_minor",1},{"host_contract","launcher-v1.0"},{"capabilities",kCapabilities}};
        if (name == "state") return p.state;
        if (name == "home") return services.home();
        if (name == "profiles") { Json list=services.profiles(); for(std::size_t i=0;i<list.size();++i) list[i]["handle"]=services.profile_key(static_cast<int>(i)); return list; }
        if (name == "preferences") return cached_preferences;
        if (name == "choices") return {{"resolution",services.resolution_labels()},{"resolution_keys",services.resolution_keys()},{"filter",services.filter_labels()},{"astc",services.astc_labels()},{"post_processing",services.post_processing_labels()},{"language",services.language_labels()},{"frame_gen_state",services.frame_gen_state()}};
        if (name == "game_settings") { const auto& g=game(args); Json result=services.game_settings(g.title_id); result["docked"]=services.docked(g.title_id); return result; }
        if (name == "status") { const auto room = services.room_status(); return {{"clock",services.clock()},{"controllers",services.controllers()},{"version",services.version()},{"room",{{"phase",static_cast<int>(room.phase)},{"error",room.error},{"name",room.room},{"members",room.members}}},{"launch",launch},{"update",offer},{"update_status",services.update_status()},{"save_sync",services.save_sync()},{"keyboard",keyboard_status}}; }
        if (name == "game_files") { const std::string path=args.value("path",services.files_folder()); std::vector<std::string> folders; const bool ok=services.folders(path,&folders); return {{"path",path},{"folders",folders},{"ok",ok},{"info",services.folder_info(path)},{"current",services.files_folder()},{"saved",services.saved_files_folder()},{"default",services.default_files_folder()},{"access",services.filesystem_access()}}; }
        if (name == "mods") { const auto& g=game(args); return {{"mods",services.mods(g.title_id)},{"enabled",services.mods_enabled(g.title_id)},{"folder",services.mods_folder(g.title_id)}}; }
        if (name == "cheats") { const auto& g=game(args); return {{"cheats",services.cheats(g.title_id)},{"enabled",services.cheats_enabled(g.title_id)},{"folder",services.cheats_folder(g.title_id)}}; }
        if (name == "sources") return services.sources();
        if (name == "downloads") return services.downloads();
        if (name == "save_sync") return services.save_sync();
        if (name == "save_sync_setup") return services.save_sync_setup();
        if (name == "pair_servers") return services.pair_servers();
        if (name == "pairing") return services.pairing();
        if (name == "room_form") { const auto r=services.room_connection(); return {{"host",r.host},{"port",r.port},{"nickname",r.nickname}}; }
        if (name == "public_rooms") { Json rows=Json::array(); for (const auto& r:room_directory.rooms) rows.push_back({{"name",r.name},{"game",r.game},{"address",r.address},{"port",r.port},{"players",r.players},{"capacity",r.capacity},{"password",r.password},{"compatible",r.compatible}}); return {{"rooms",rows},{"error",room_directory.error},{"refreshing",directory.valid()}}; }
        if (name == "setup") return {{"details",services.setup_details()},{"save_transfer",services.save_transfer_available()}};
        if (name == "launchers") { discover(); Json all={{{"id","builtin"},{"name","Built-in launcher"}}}; for (const auto& row:installed) all.push_back(row); return {{"installed",all},{"active",active},{"error",last_error.empty()?discovery_error:last_error}}; }
        if (name == "translate") return std::string{tr(args.value("text","").c_str())};
        if (name == "measure") return p.font.measure(args.value("text",""),args.value("size",24.0f));
        throw std::runtime_error("Unknown launcher query");
    }
    Json action(Pack& p, const std::string& name, const Json& args) {
        bool ok=true; std::string message;
        if ((name.starts_with("update.") || name.starts_with("launch.")) && (pack_updates->busy() || commit_started)) {
            if(!commit_started && pack_updates->status("").value("phase","")=="checking") pack_updates->cancel();
            else throw std::runtime_error("Finish or cancel the launcher update first");
        }
        if(name!="sound" && name!="state.save") query_cache.clear();
        if (name == "state.save") { if (!args.is_object() || args.dump().size()>131072) throw std::runtime_error("Invalid UI state"); p.state=args; ok=write_json(data_root+"/config/launcher-state/"+p.id+".json",p.state); }
        else if (name == "launcher.select") { const auto id=args.at("id").get<std::string>(); if (id!="builtin" && !identifier(id)) throw std::runtime_error("Invalid launcher ID"); next_id=id; }
        else if (name == "launcher.picker") { discover(); picker=true; picker_row=0; }
        else if (name == "library.refresh") refresh();
        else if (name == "profile.choose") {
            if (launch.value("stage","idle")!="idle") throw std::runtime_error("A game launch is pending");
            int index=args.value("index",-1); const auto profiles=services.profiles(); if(args.contains("handle")) { index=-1; for(int i=0;i<static_cast<int>(profiles.size());++i) if(services.profile_key(i)==args.at("handle").get<std::string>()) index=i; } if (index<0 || index>=static_cast<int>(profiles.size())) throw std::runtime_error("Invalid profile");
            if (services.room_status().phase!=Eden::Multiplayer::Phase::Idle) { services.leave_room(); throw std::runtime_error("Leaving the room. Select the profile again when disconnected."); }
            ok=services.choose_profile(index); if(ok) cached_preferences=services.preferences();
        }
        else if (name == "profile.add") return {{"ok",true},{"index",services.add_profile()}};
        else if (name == "profile.rename" || name == "profile.remove") {
            const int index=args.at("index");
            if(index<0 || index>=static_cast<int>(services.profiles().size()) || launch.value("stage","idle")!="idle")
                throw std::runtime_error("Invalid or busy profile");
            ok=name=="profile.rename"?services.rename_profile(index,args.value("step",1)):services.remove_profile(index);
        }
        else if (name == "preferences.set") {
            const auto before=services.preferences(); Json values=before;
            if(args.contains("menu_bind")) {
                const auto& binding=args.at("menu_bind");
                if(!binding.is_array() || binding.size()!=kMenuActions ||
                   !std::all_of(binding.begin(),binding.end(),[](const Json& button){return button.is_number_integer();}))
                    throw std::runtime_error("Invalid menu mapping");
            }
            for (auto it=args.begin();it!=args.end();++it) { if (!values.contains(it.key()) || (values[it.key()].is_boolean()!=it.value().is_boolean()) || (values[it.key()].is_array()!=it.value().is_array())) throw std::runtime_error("Invalid preference field/type"); values[it.key()]=it.value(); }
            // Preserve native fields added after this API contract was defined.
            auto v=before;from_json(values,v);
            if (v.volume<0 || v.volume>100 || v.menu_volume<0 || v.menu_volume>100 || v.renderer<0 || v.renderer>1 || v.output<0 || v.output>2 || v.resolution<0 || v.resolution>=static_cast<int>(services.resolution_labels().size()) || v.filter<0 || v.filter>=static_cast<int>(services.filter_labels().size()) || v.astc<0 || v.astc>1 || v.language<0 || v.language>=static_cast<int>(services.language_labels().size()) || v.refresh<0 || v.refresh>1 || (v.vibration_strength!=25 && v.vibration_strength!=50 && v.vibration_strength!=75 && v.vibration_strength!=100) || v.hud_size<0 || v.hud_size>2 || v.hud_corner<0 || v.hud_corner>3 || v.frame_gen_target<0 || v.frame_gen_target>5 || v.frame_gen_multiplier<0 || v.frame_gen_multiplier>2) throw std::runtime_error("Preference is out of range");
            if(!Eden::ValidHudFields(v.hud_fields)) throw std::runtime_error("Invalid overlay fields");
            if(v.post_processing<0 || v.post_processing>=static_cast<int>(services.post_processing_labels().size())) throw std::runtime_error("Invalid post-processing");
            std::set<int> mapped; for(int button:v.mapping) if(button<0 || button>=kPadButtons || !mapped.insert(button).second) throw std::runtime_error("Invalid controller mapping");
            std::set<int> menu_buttons; for(int button:v.menu_bind) if(button<0 || button>=kMenuActions || !menu_buttons.insert(button).second) throw std::runtime_error("Invalid menu mapping");
            ok=services.set_preferences(v); if(ok) cached_preferences=v; if(ok && v.vibration_strength!=before.vibration_strength && v.vibration) services.vibration_pulse(v.vibration_strength);
        }
        else if (name == "game_settings.set") { const auto& g=game(args); auto settings=services.game_settings(g.title_id); Json values=settings; for(auto it=args.at("patch").begin();it!=args.at("patch").end();++it) { if(!values.contains(it.key())) throw std::runtime_error("Unknown game setting"); values[it.key()]=it.value(); } from_json(values,settings);ok=services.set_game_settings(g.title_id,settings); }
        else if (name == "game.docked") { const auto& g=game(args); ok=services.set_docked(g.title_id,args.at("value")); }
        else if (name == "game_files.set") ok=services.set_files_folder(args.at("path"));
        else if (name == "mods.enabled") { const auto& g=game(args); ok=services.set_mods_enabled(g.title_id,args.at("value")); }
        else if (name == "mod.enabled") { const auto& g=game(args); ok=services.set_mod_enabled(g.title_id,args.at("name"),args.at("value")); }
        else if (name == "cheat.enabled") { const auto& g=game(args); ok=services.set_cheat_enabled(g.title_id,args.at("mod"),args.at("name"),args.at("value")); }
        else if (name == "cheats.enabled") { const auto& g=game(args); ok=services.set_cheats_enabled(g.title_id,args.at("value")); }
        else if (name == "mods.folder") { const auto& g=game(args); ok=services.make_mods_folder(g.title_id); }
        else if (name == "save.import" || name == "save.export") { const auto& g=game(args); ok=name=="save.import"?services.save_import(g.title_id,&message):services.save_export(g.title_id,&message); }
        else if (name == "sources.refresh") services.refresh_sources();
        else if (name == "download.start") { const auto& g=game(args); const int source=args.value("source",0); if(source<0 || source>=static_cast<int>(g.sources.size())) throw std::runtime_error("Invalid download source"); ok=services.download(g,source,args.value("first",false)); }
        else if (name == "download.cancel") ok=services.cancel_download(args.at("key"));
        else if (name == "game.delete") { const auto& g=game(args); if(!args.value("confirmed",false)) throw std::runtime_error("Confirmation required"); ok=services.delete_game(g,&message); refresh(); }
        else if (name == "room.refresh") { if(!directory.valid()) { stop=false; directory=std::async(std::launch::async,[&]{return services.public_rooms(stop);}); } }
        else if (name == "room.leave") services.leave_room();
        else if (name == "room.join_public") {
            const int index=args.at("index");
            if(index<0 || index>=static_cast<int>(room_directory.rooms.size())) throw std::runtime_error("Public room no longer exists");
            const auto& room=room_directory.rooms[index];
            if(!room.compatible) throw std::runtime_error("The room protocol is incompatible");
            Eden::Multiplayer::Connection r; r.host=room.address; r.port=room.port; r.nickname=args.value("nickname","Player"); r.password=args.value("password","");
            services.remember_room(r,false); ok=services.connect_public_room(r);
        }
        else if (name == "keyboard.cancel") stop=true;
        else if (name == "keyboard.seed") {
            if(keyboard.valid()) throw std::runtime_error("Keyboard is already open");
            const auto& g=game(args); const auto initial=services.game_settings(g.title_id).rng_seed;
            keyboard_status={{"ready",false}}; stop=false;
            keyboard=std::async(std::launch::async,[&,initial]()->std::optional<std::string>{
                const auto answer=services.ask_seed(initial,stop); return answer?std::optional<std::string>{std::to_string(*answer)}:std::nullopt;
            });
        }
        else if (name == "sync.finish") services.end_save_sync();
        else if (name == "sync.cancel") services.stop_save_sync();
        else if (name == "room.join") { Eden::Multiplayer::Connection r; r.host=args.at("host"); const int port=args.value("port",24872); if(port<1 || port>65535 || r.host.size()>240) throw std::runtime_error("Invalid room address"); r.port=static_cast<std::uint16_t>(port); r.nickname=args.value("nickname","Player"); r.password=args.value("password",""); services.remember_room(r,true); ok=services.connect_room(r); }
        else if (name == "keyboard.open") { if(keyboard.valid()) throw std::runtime_error("Keyboard is already open"); const int field=args.value("field",0); if(field<0 || field>3) throw std::runtime_error("Invalid keyboard field"); const auto initial=args.value("text",""); keyboard_status={{"ready",false}}; stop=false; keyboard=std::async(std::launch::async,[&,field,initial]{return services.room_text(field,initial,stop);}); }
        else if (name == "pair.start") ok=services.start_pairing(args.at("profile"),args.at("server"));
        else if (name == "pair.cancel") services.cancel_pairing();
        else if (name == "pair.unlink") ok=services.unlink_profile(args.at("index"));
        else if (name == "sync.answer") {
            const auto choice=args.at("choice").get<std::string>();const auto status=services.save_sync();
            if((choice=="console" || choice=="server" || choice=="neither") && status.stage==SaveSyncStage::conflict)
                services.choose_save_data(choice=="console"?SaveChoice::console:choice=="server"?SaveChoice::server:SaveChoice::neither);
            else if(choice=="retry" && !launch_file.empty() && status.stage==SaveSyncStage::failed) { services.end_save_sync(); services.start_save_sync(launch_file,true); sync_started=true; }
            else if(choice=="play_anyway" && !launch_file.empty() && status.stage==SaveSyncStage::failed) { services.end_save_sync(); launch["stage"]="ready"; }
            else throw std::runtime_error("Unavailable save-sync answer");
        }
        else if (name == "launcher_updates.open" || name == "launcher_updates.install" || name == "launcher_updates.rollback") {
            if(args.value("view",std::string{})!="" && args.value("view",std::string{})!="launcher") throw std::runtime_error("Unknown updater view");
            if(!update_available()) throw std::runtime_error("Finish the pending operation first");
            open_updates(p.id); rollback_confirmation=name=="launcher_updates.rollback";
            custom_update_view=name=="launcher_updates.open" && args.value("view",std::string{})=="launcher" && has_update_view(p);
            updater_first=true;updater_dirty=true;
            if(custom_update_view) std::fprintf(stderr,"PE_LAUNCHER_UPDATE_VIEW id=%s\n",p.id.c_str());
        }
        else if(name=="launcher_updates.download") {
            if(!updater_context || !updater_input_context || p.id!=updates_id || !updates_page || commit_started || pack_updates->busy() || !update_available()) throw std::runtime_error("Launcher update is not ready");
            const auto status=pack_updates->status(p.id);
            if(!args.value("confirmed",false) || status.value("phase",std::string{})!="available" || args.value("version",std::string{})!=status.at("release").at("version").get<std::string>()) throw std::runtime_error("Confirm the offered launcher version");
            ok=pack_updates->download();
        }
        else if(name=="launcher_updates.close") {
            if(!updater_context || !updater_input_context || commit_started || pack_updates->replacing()) throw std::runtime_error("Launcher update is installing");
            updater_close=true;
            if(pack_updates->busy() || pack_updates->status(p.id).value("phase",std::string{})=="ready") pack_updates->cancel();
        }
        else if (name == "launcher_updates.check") {
            if(!update_available()) throw std::runtime_error("Finish the pending operation first");
            ok=pack_updates->check(p.id);
        }
        else if (name == "launcher_updates.cancel") pack_updates->cancel();
        else if (name == "launch.request") {
            const auto& g=game(args); if(g.remote || !launch_available(services,g.file) || launch.value("stage","idle")!="idle") throw std::runtime_error("Game cannot start now");
            launch_file=g.file; launch_profile=current_profile(); sync_started=false; launch={{"stage",services.save_sync_wanted(g.file)?"syncing":"ready"},{"game",g.file},{"title",g.name}};
        }
        else if (name == "launch.cancel") { services.stop_save_sync(); launch={{"stage","idle"}}; launch_file.clear(); }
        else if (name == "launch.commit") { if(launch.value("stage","idle")!="ready" || launch_profile!=current_profile()) throw std::runtime_error("Launch is not ready"); selected=commit_launch(services,launch_file); if(selected.empty()) throw std::runtime_error("Game or room state changed before launch"); launch["stage"]="committed"; }
        else if (name == "update.start") { if(!offer.value("installable",false)) throw std::runtime_error("Update is not installable"); ok=services.start_update(); }
        else if (name == "update.cancel") { services.cancel_update(); services.finish_update(); }
        else if (name == "update.apply") { if(services.update_status().phase!=UpdatePhase::ready) throw std::runtime_error("Update is not ready"); ok=services.apply_update(); if(ok) exit_update=true; }
        else if (name == "sound") { char suffix[8]{}; std::snprintf(suffix,sizeof(suffix),"_%02d",std::clamp(args.value("variation",1),1,8)); const auto cue=args.at("event").get<std::string>()+suffix; const auto it=p.clips.find(cue); if(mixer && it!=p.clips.end()) { audio::PlayParams params; params.bus=audio::Bus::ui; params.pitch=std::clamp(args.value("pitch",1.0f),0.5f,2.0f); params.gain=std::clamp(args.value("gain",1.0f),0.0f,1.0f); mixer->play_clip(&it->second,params); } }
        else throw std::runtime_error("Unknown launcher action");
        return {{"ok",ok},{"message",message}};
    }
    void draw_command(Pack& p, const std::string& kind, const Json& args) {
        if(!drawing || ++draw_commands>8192) throw std::runtime_error("Drawing is unavailable or exceeded its budget");
        for(auto it=args.begin();it!=args.end();++it) if(it.value().is_number()) {
            const double value=it.value().get<double>();
            if(!std::isfinite(value) || std::abs(value)>4294967295.0) throw std::runtime_error("Invalid drawing number");
        }
        auto& list=*drawing; const auto r=rect(args); const auto c=color(args); const float radius=args.value("radius",0.0f);
        if(kind=="rect") list.rounded_rect(r,radius,c);
        else if(kind=="gradient") { if(args.value("axis","vertical")=="horizontal") list.hgradient_rect(r,radius,c,color(args,"other")); else list.gradient_rect(r,radius,c,color(args,"other")); }
        else if(kind=="border") list.bordered_rect(r,radius,c,args.value("border",1.0f),color(args,"edge"));
        else if(kind=="shadow") list.shadow(r,radius,args.value("softness",8.0f),c);
        else if(kind=="circle") list.circle(r.x,r.y,args.value("radius",1.0f),c);
        else if(kind=="ring") list.ring(r.x,r.y,args.value("radius",1.0f),args.value("thickness",2.0f),c);
        else if(kind=="line") list.line(r.x,r.y,args.value("x2",r.x),args.value("y2",r.y),args.value("thickness",1.0f),c);
        else if(kind=="triangle") list.triangle(r,c,args.value("outline",0.0f));
        else if(kind=="text") { auto text=args.value("text",""); float size=std::clamp(args.value("size",24.0f),8.0f,160.0f); if(cached_preferences.large_text && size<30) size=std::min(30.0f,size*1.3f); if(args.contains("width")) text=p.font.fit(text,size,args.at("width")); const auto align=args.value("align","left"); gfx::TextStyle style; style.size=size; style.color=c; style.align=align=="center"?gfx::Align::center:align=="right"?gfx::Align::right:gfx::Align::left; style.tracking=args.value("tracking",0.0f); list.text(p.font,p.font_texture,text,r.x,r.y,style); }
        else if(kind=="image") { const auto name=args.at("asset").get<std::string>(); const auto it=p.textures.find(name); if(it==p.textures.end()) throw std::runtime_error("Unknown image resource"); const auto uv=args.value("uv",std::vector<float>{0,0,1,1}); if(uv.size()!=4) throw std::runtime_error("Invalid atlas region"); if(args.contains("radius")) list.rounded_image(it->second,r,{uv[0],uv[1],uv[2],uv[3]},radius,c); else list.image(it->second,r,{uv[0],uv[1],uv[2],uv[3]},c); }
        else if(kind=="cover") { const auto& g=game(args); const auto cover=covers.cover(g.cover,r.w); if(cover.texture) list.image(cover.texture,r,{0,0,1,1},c); else list.rounded_rect(r,0,args.contains("fallback")?color(args,"fallback"):th().row); }
        else if(kind=="qr") {
            const auto text=args.at("text").get<std::string>(); if(text.size()>2048) throw std::runtime_error("QR text is too long");
            const auto qr=make_qr_code(text); if(qr.size<=0) throw std::runtime_error("QR code cannot be generated");
            const float cell=std::min(r.w,r.h)/float(qr.size+8); list.rounded_rect(r,0,color(args,"background",0xffffff));
            for(int y=0;y<qr.size;++y) for(int x=0;x<qr.size;++x) if(qr.at(x,y)) list.rounded_rect({r.x+(x+4)*cell,r.y+(y+4)*cell,cell,cell},0,c);
        }
        else if(kind=="clip") list.push_clip(r);
        else if(kind=="unclip") list.pop_clip();
        else if(kind=="transform") list.push_transform(args.value("scale",1.0f),args.value("ox",0.0f),args.value("oy",0.0f),args.value("dx",0.0f),args.value("dy",0.0f));
        else if(kind=="untransform") list.pop_transform();
        else if(kind=="opacity") list.push_opacity(args.value("value",1.0f));
        else if(kind=="unopacity") list.pop_opacity();
        else throw std::runtime_error("Unknown draw command");
    }
    bool update_available() {
        const auto phase=services.update_status().phase;
        return launch.value("stage","idle")=="idle" && !exit_update &&
            (phase==UpdatePhase::idle || phase==UpdatePhase::failed || phase==UpdatePhase::cancelled) &&
            !(builtin && builtin->done());
    }
    void open_updates(const std::string& id) {
        if(!launcher_updates::valid_id(id) || !update_available()) return;
        abandon_update_view("Opened native updater");updater_close=false;
        updates_id=id; updates_page=true; rollback_confirmation=false;
        updates_name=id;
        for(const auto& row:installed) if(row.value("id","")==id) updates_name=row.value("name",id);
        updates_status=pack_updates->status(id);
        if(updates_status.value("phase","")=="idle" && !pack_updates->busy()) pack_updates->check(id);
    }
    void update_packs(float dt) {
        updates_poll+=dt;
        if(updates_poll<0.1f) return;
        updates_poll=0; pack_updates->poll();updater_dirty=true;
        if(!auto_checked && time>=5 && !updates_page && update_available() && !pack_updates->busy()) {
            auto_checked=true; if(active!="builtin") pack_updates->check(active,true);
        }
        if(!updates_page && !commit_started) return;
        const auto id=updates_id.empty()?active:updates_id;
        updates_status=pack_updates->status(id);
        const auto phase=updates_status.value("phase","idle");
        if(phase=="ready" && !pack_updates->busy() && !commit_started) {
            if(!update_available()) { pack_updates->cancel(); return; }
            resume_pack=active==id?id:"";
            // Textures are deleted on the rendering thread; audio ownership is fenced by select().
            if(!resume_pack.empty()) retain_update_view();
            if(!resume_pack.empty() && !select("builtin",false)) { pack_updates->cancel(); return; }
            commit_started=true; replacement_seen=false;
            if(!pack_updates->commit()) { commit_started=false; last_error="Could not start launcher replacement"; }
        }
        if(commit_started && !pack_updates->busy() && (phase=="installed" || phase=="failed" || phase=="cancelled")) {
            if(replacement_seen) return;
            replacement_seen=true; commit_started=false; discover(); query_cache.clear();
            const auto resume=resume_pack; resume_pack.clear();
            if(!resume.empty() && !select(resume,false)) {
                if(phase=="installed" && !recovery_attempted) {
                    recovery_attempted=true; last_error="Updated launcher failed to load; restoring previous version";
                    resume_pack=resume; commit_started=true; replacement_seen=false;
                    if(!pack_updates->rollback(resume)) commit_started=false;
                }
            } else if(phase=="installed") recovery_attempted=false;
            updates_page=true;
        }
        // A recovery rollback also stages before changing live files.
        if(commit_started && !pack_updates->busy() && phase=="ready") pack_updates->commit();
    }
    void press_updates(Key key) {
        if(commit_started || pack_updates->replacing()) return;
        if(key==Key::circle) {
            if(pack_updates->busy() || updates_status.value("phase","")=="ready") pack_updates->cancel();
            else if(rollback_confirmation) rollback_confirmation=false;
            else updates_page=false;
        } else if(key==Key::options) {
            if(!pack_updates->set_automatic(!pack_updates->automatic())) last_error="Could not save update preferences";
        } else if(!pack_updates->busy() && update_available()) {
            if(key==Key::triangle) { rollback_confirmation=false; pack_updates->check(updates_id); }
            else if(key==Key::square) rollback_confirmation=!rollback_confirmation;
            else if(key==Key::cross) {
                if(rollback_confirmation) { rollback_confirmation=false; pack_updates->rollback(updates_id); }
                else if(updates_status.value("phase","")=="available") pack_updates->download();
            }
        }
    }
    void draw_updates(gfx::DrawList& list) {
        list.reset_state(); const auto& t=th(); list.rounded_rect({0,0,1920,1080},0,t.base);
        list.text(*fonts.font,fonts.texture,"Launcher updates",184,144,{48,t.title});
        auto text=[&](const std::string& value,float y,float size=28) {
            list.text(*fonts.font,fonts.texture,fonts.font->fit(value,size,1460),220,y,{size,t.text});
        };
        text(updates_name,225,32);
        const auto phase=updates_status.value("phase","idle");
        static const std::map<std::string,std::string> labels{{"idle","Ready"},{"unconfigured","No update feed"},{"checking","Checking for updates"},
            {"current","Up to date"},{"available","Update available"},{"downloading","Downloading"},{"validating","Checking downloaded files"},
            {"ready","Ready to install"},{"installing","Installing"},{"installed","Update installed"},{"failed","Update failed"},{"cancelled","Cancelled"}};
        const auto label=labels.find(phase);
        text(phase=="installed" && updates_status.value("rollback",false)?"Previous version restored":(label==labels.end()?"Ready":label->second),290);
        if(updates_status.contains("installed_version")) text("Installed: "+updates_status.at("installed_version").get<std::string>(),345);
        if(updates_status.contains("release") && updates_status["release"].is_object()) {
            const auto& release=updates_status["release"];
            text("Available: "+release.value("version", ""),400);
            // Fit a short preview of the publisher's release notes.
            std::string notes=release.value("notes",""); float y=460;
            while(!notes.empty() && y<710) {
                auto end=notes.find('\n'); std::string line=notes.substr(0,end);
                if(fonts.font->measure(line,24)>1460) {
                    line=fonts.font->fit(line,24,1400); const auto cut=line.find_last_of(' ');
                    if(cut!=line.npos && cut>0) line.resize(cut);
                    end=line.size();
                }
                text(line,y,24); y+=34;
                if(end==notes.npos) break;
                notes.erase(0,end+(notes[end]=='\n'?1:0));
            }
        }
        if(updates_status.contains("total")) text("Downloaded "+std::to_string(updates_status.value("done",0ULL)/1024)+" / "+std::to_string(updates_status.value("total",0ULL)/1024)+" KiB",760);
        if(rollback_confirmation) text("Restore the previous pack? Press confirm to continue.",815);
        else if(phase=="available") text("Install this compatible update? Your launcher state is retained.",815);
        else if(phase=="unconfigured") text("This launcher does not provide an update feed.",815);
        else if(phase=="current") text("No newer compatible release is available.",815);
        text(updates_status.value("error",last_error),870,24);
        text(std::string("Automatic checks for the selected launcher: ")+(pack_updates->automatic()?"On":"Off"),920,24);
        constexpr const char* faces[]{"Cross","Circle","Square","Triangle"}; const auto binding=cached_preferences.menu_bind;
        text(std::string(faces[binding[0]])+": confirm    "+faces[binding[1]]+": back / cancel    "+faces[binding[2]]+": rollback    "+faces[binding[3]]+": check    Options: automatic checks",1000,22);
    }
    void update(float dt) {
        time+=dt; poll+=dt; catalog_poll+=dt; update_packs(dt);
        if(pack && catalog_poll>=2.0f) { catalog_poll=0; const auto sources=services.sources(); if(sources.generation!=source_generation) { source_generation=sources.generation; refresh(); } }
        if(scan.valid() && scan.wait_for(std::chrono::seconds(0))==std::future_status::ready) { try { catalog.replace(scan.get()); } catch(const std::exception& e) {last_error=e.what();} }
        if(directory.valid() && directory.wait_for(std::chrono::seconds(0))==std::future_status::ready) { try {room_directory=directory.get();}catch(const std::exception& e){room_directory.error=e.what();} }
        if(keyboard.valid() && keyboard.wait_for(std::chrono::seconds(0))==std::future_status::ready) { try {const auto text=keyboard.get(); keyboard_status={{"ready",true},{"cancelled",!text},{"text",text.value_or("")}};}catch(const std::exception& e){keyboard_status={{"ready",true},{"error",e.what()}};} }
        const bool poll_services=poll>=0.25f;
        if(poll_services) { poll=0; cached_preferences=services.preferences(); }
        const auto& prefs=cached_preferences; look()={prefs.large_text,prefs.high_contrast,prefs.reduce_motion}; covers.pump(dt);
        for(auto it=retired.begin();it!=retired.end();) {
            if(mixer && !(*it)->audio_fence) (*it)->audio_fence=mixer->stop_all_fenced();
            if(!mixer || ((*it)->audio_fence && mixer->completed_fence()>=(*it)->audio_fence)) it=retired.erase(it); else ++it;
        }
        if(launch.value("stage","idle")=="syncing") {
            auto status=services.save_sync();
            if(!sync_started && status.stage!=SaveSyncStage::working && status.stage!=SaveSyncStage::conflict) { services.start_save_sync(launch_file,true); sync_started=true; status=services.save_sync(); }
            if(sync_started && status.before && status.stage==SaveSyncStage::done) { services.end_save_sync(); launch["stage"]="ready"; }
            launch["sync"]=status;
        }
        finish_update_close();sync_update_view();
        if(updates_page || commit_started) return;
        if(pack) {
            if(poll_services) { UpdateOffer found; if(services.take_update(&found)) offer=found; }
            if(!call(*pack,"update",{{"dt",std::min(dt,0.05f)},{"time",time}})) fallback();
            if(!next_id.empty()) { const auto next=next_id; next_id.clear(); select(next); }
        } else if(builtin) builtin->update(dt);
    }
    void draw(gfx::DrawList& list) {
        if(updates_page || commit_started) { if(!draw_update_view(list)) draw_updates(list); return; }
        if(pack) { drawing=&list; draw_commands=0; const bool ok=call(*pack,"draw",{{"time",time}},true); drawing=nullptr; if(!ok) {list.clear(); fallback(); if(builtin) builtin->draw(list);} else batch.sync_font_texture(pack->font_texture,pack->font); }
        else if(builtin) builtin->draw(list);
        if(picker) {
            list.reset_state();
            const auto& t=th(); list.rounded_rect({0,0,1920,1080},0,t.base.with_alpha(0.96f));
            list.text(*fonts.font,fonts.texture,"Choose launcher",184,144,{48,t.title});
            const int total=static_cast<int>(installed.size())+1;
            for(int i=0;i<total;++i) { const Rect r{260,220+float(i-picker_row/7*7)*90,1400,80}; if(r.y<220 || r.y>850) continue;
                list.bordered_rect(r,12,i==picker_row?t.row_focus:t.row,i==picker_row?2:0,t.accent);
                list.text(*fonts.font,fonts.texture,i==0?"Built-in launcher":installed[i-1].value("name",""),300,r.y+52,{28,t.text}); }
            constexpr const char* faces[]{"Cross", "Circle", "Square", "Triangle"};
            const auto binding=services.preferences().menu_bind;
            const auto hints=std::string(faces[binding[0]])+": select     "+faces[binding[1]]+": back     "+faces[binding[3]]+": updates";
            list.text(*fonts.font,fonts.texture,hints,260,1000,{24,t.meta});
            const auto& error = last_error.empty() ? discovery_error : last_error;
            if(!error.empty()) list.text(*fonts.font,fonts.texture,fonts.font->fit(error,24,1400),260,935,{24,t.warning});
        }
    }
};

RuntimeLauncher::RuntimeLauncher(Services& s, Textures& t, const Fonts& f, gfx::GlBatch& b, bool first, std::string root, audio::Mixer* m, std::function<void(gfx::Font&)> configure)
    : impl_(std::make_unique<Impl>(s,t,f,b,first,std::move(root),m,std::move(configure))) {
    auto& h=*impl_; h.discover(); h.refresh(); std::string selected="builtin";
    try { selected=read_json(h.data_root+"/config/launcher.json").value("selected","builtin"); }catch(...){}
    if(!h.last_error.empty()) selected="builtin";
    struct stat safe{}; if(stat((h.data_root+"/config/force-builtin-launcher").c_str(),&safe)==0) selected="builtin";
    if(!h.select(selected,false)) h.fallback();
}
RuntimeLauncher::~RuntimeLauncher()=default;
void RuntimeLauncher::press(Key key) {
    auto& h=*impl_;
    if(h.updates_page || h.commit_started) {
        if(h.custom_update_view) {
            if(!h.commit_started && !h.pack_updates->replacing()) h.call_update_view("updater_input",{{"key",key_name(key)}},true);
        } else h.press_updates(menu_key_for(key,h.cached_preferences.menu_bind));
        return;
    }
    if(h.picker) { key=menu_key_for(key,h.services.preferences().menu_bind); if(key==Key::circle) h.picker=false; else if(key==Key::up || key==Key::down) h.picker_row=std::clamp(h.picker_row+(key==Key::down?1:-1),0,static_cast<int>(h.installed.size())); else if(key==Key::triangle && h.picker_row>0) h.open_updates(h.installed[h.picker_row-1].at("id").get<std::string>()); else if(key==Key::cross) h.select(h.picker_row==0?"builtin":h.installed[h.picker_row-1].at("id").get<std::string>()); return; }
    if(h.pack) { if(!h.call(*h.pack,"input",{{"key",key_name(key)}})) h.fallback(); }
    else if(h.builtin) { h.builtin->press(key); if(h.builtin->take_launcher_picker()) open_picker(); }
}
void RuntimeLauncher::update(float dt){impl_->update(dt);}
void RuntimeLauncher::draw(gfx::DrawList& list){impl_->draw(list);}
std::vector<audio::Cue> RuntimeLauncher::take_cues(){return impl_->builtin?impl_->builtin->take_cues():std::vector<audio::Cue>{};}
int RuntimeLauncher::menu_volume()const{return impl_->cached_preferences.menu_volume;}
int RuntimeLauncher::output()const{return impl_->cached_preferences.output;}
bool RuntimeLauncher::done()const{
    const auto& h=*impl_;
    const bool finished=h.exit_update || !h.selected.empty() || (h.builtin && h.builtin->done());
    if(!finished || h.commit_started) return false;
    if(h.pack_updates->busy()) {
        if(h.pack_updates->status("").value("phase","")!="checking") return false;
        h.pack_updates->cancel();
    }
    return true;
}
const std::string& RuntimeLauncher::selected_game()const{return impl_->builtin?impl_->builtin->selected_game():impl_->selected;}
void RuntimeLauncher::set_font_texture(std::uint32_t t){impl_->fonts.texture=t;if(impl_->builtin)impl_->builtin->set_font_texture(t);}
void RuntimeLauncher::release_pack_textures(){if(impl_->retained_update_view)impl_->abandon_update_view("Display resources changed during update");impl_->release_textures();}
bool RuntimeLauncher::reload_pack_textures(){try{if(impl_->pack)impl_->load_textures(*impl_->pack);return true;}catch(const std::exception& e){impl_->last_error=e.what();impl_->fallback();return false;}}
bool RuntimeLauncher::select(std::string_view id){return impl_->select(std::string(id));}
void RuntimeLauncher::open_picker(){impl_->discover();impl_->picker=true;impl_->picker_row=0;}
const std::string& RuntimeLauncher::active_id()const{return impl_->active;}
const std::string& RuntimeLauncher::error()const{return impl_->last_error;}
bool RuntimeLauncher::script_active()const{return impl_->pack!=nullptr;}
const RuntimeLoadingScene* RuntimeLauncher::loading_scene()const{return impl_->pack?impl_->pack->loading.get():nullptr;}
}
