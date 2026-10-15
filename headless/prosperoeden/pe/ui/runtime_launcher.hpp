// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pe/ui/launcher.hpp"
#include "pe/ui/runtime_loading.hpp"
#include <memory>

namespace pe::ui {
// Native resource and workflow owner for built-in or separately installed launchers.
class RuntimeLauncher {
public:
    RuntimeLauncher(Services&, Textures&, const Fonts&, gfx::GlBatch&, bool first_start,
                    std::string data_root, audio::Mixer* mixer = nullptr,
                    std::function<void(gfx::Font&)> configure_font = {});
    ~RuntimeLauncher();
    RuntimeLauncher(const RuntimeLauncher&) = delete;
    RuntimeLauncher& operator=(const RuntimeLauncher&) = delete;
    void press(Key);
    void update(float);
    void draw(gfx::DrawList&);
    std::vector<audio::Cue> take_cues();
    int menu_volume() const;
    int output() const;
    bool done() const;
    const std::string& selected_game() const;
    void set_font_texture(std::uint32_t);
    void release_pack_textures();
    bool reload_pack_textures();
    bool select(std::string_view id);
    void open_picker();
    const std::string& active_id() const;
    const std::string& error() const;
    bool script_active() const;
    const RuntimeLoadingScene* loading_scene() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
