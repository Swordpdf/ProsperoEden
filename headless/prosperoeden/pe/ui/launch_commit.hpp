// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pe/ui/services.hpp"

namespace pe::ui {
inline bool launch_available(Services& services, const std::string& file) {
    return services.game_exists(file) && services.can_launch_game();
}
// Both launchers resolve paths through the same backend and register play exactly once.
inline std::string commit_launch(Services& services, const std::string& file) {
    if (!launch_available(services, file)) return {};
    const auto path = services.game_path(file);
    if (path.empty()) return {};
    services.will_play(file);
    return path;
}
}
