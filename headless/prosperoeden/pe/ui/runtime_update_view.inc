// SPDX-License-Identifier: GPL-3.0-or-later
// Included inside RuntimeLauncher::Impl. The detached view owns its original VM and resources.
std::unique_ptr<Pack> retained_update_view;
bool custom_update_view{}, updater_context{}, updater_input_context{}, updater_first{}, updater_dirty{}, updater_close{};
Json updater_sent;

Pack* update_view() {
    if(retained_update_view) return retained_update_view.get();
    return pack && pack->id==updates_id?pack.get():nullptr;
}
bool has_update_view(Pack& p) {
    const int base=lua_gettop(p.lua);
    lua_rawgeti(p.lua,LUA_REGISTRYINDEX,p.module);
    bool complete=true;
    for(const char* name:{"updater_status","updater_input","updater_draw"}) {
        lua_getfield(p.lua,-1,name); complete=complete && lua_isfunction(p.lua,-1); lua_pop(p.lua,1);
    }
    lua_settop(p.lua,base); return complete;
}
void retire_update_view() {
    if(!retained_update_view) return;
    retained_update_view->audio_fence=0;
    retired.push_back(std::move(retained_update_view));
}
void abandon_update_view(const char* reason) {
    if(custom_update_view) std::fprintf(stderr,"PE_LAUNCHER_UPDATE_VIEW_NATIVE reason=%s\n",reason);
    custom_update_view=false; updater_dirty=false; updater_sent=Json();
    retire_update_view();
}
bool call_update_view(const char* method,const Json& event,bool input=false) {
    auto* p=update_view();
    if(!custom_update_view || !p) return false;
    updater_context=true; updater_input_context=input;
    const bool ok=call(*p,method,event,true);
    updater_input_context=false; updater_context=false;
    if(!ok) abandon_update_view(last_error.c_str());
    return ok;
}
void sync_update_view() {
    if(!custom_update_view || !updater_dirty) return;
    updater_dirty=false;
    Json event={{"status",updates_status},{"preferences",cached_preferences},{"controllers",services.controllers()},
                {"busy",pack_updates->busy() || commit_started},{"can_update",update_available()}};
    if(!updater_first && event==updater_sent) return;
    updater_sent=event;event["first"]=updater_first;updater_first=false;
    call_update_view("updater_status",event);
}
void retain_update_view() {
    if(!custom_update_view || !pack || pack->id!=updates_id) return;
    call(*pack,"shutdown");
    write_json(data_root+"/config/launcher-state/"+pack->id+".json",pack->state);
    pack->live=false;
    pack->audio_fence=mixer?mixer->stop_all_fenced():0;
    retained_update_view=std::move(pack);
}
void finish_update_close() {
    if(!updater_close || commit_started || pack_updates->busy() || pack_updates->replacing()) return;
    updater_close=false;updates_page=false;
    custom_update_view=false;updater_sent=Json();retire_update_view();
}
void guard_update_operation(Pack& p,int operation,const std::string& name) {
    if(!updater_context) return;
    if(update_view()!=&p) throw std::runtime_error("Invalid updater view owner");
    if(operation==0) {
        if(name!="launcher_updates" && name!="measure" && name!="runtime") throw std::runtime_error("Query is unavailable in an updater view");
    } else if(operation==1) {
        if(name=="sound") return;
        if(!updater_input_context || (name!="launcher_updates.check" && name!="launcher_updates.download" && name!="launcher_updates.cancel" && name!="launcher_updates.close"))
            throw std::runtime_error("Action is unavailable in an updater view");
    } else if(name=="cover") throw std::runtime_error("Updater views cannot load game resources");
}
bool draw_update_view(gfx::DrawList& list) {
    if(!custom_update_view || !update_view()) return false;
    drawing=&list;draw_commands=0;
    const bool ok=call_update_view("updater_draw",{{"time",time}});
    drawing=nullptr;
    if(ok) { auto* p=update_view();batch.sync_font_texture(p->font_texture,p->font); }
    else list.clear();
    return ok;
}
