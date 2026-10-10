// SPDX-License-Identifier: GPL-3.0-or-later
#include "pe/ui/launcher.hpp"
#include <algorithm>
#include <charconv>

namespace pe::ui {
using Eden::Multiplayer::Phase;
using audio::Cue;
static constexpr int kVisibleRoomMembers = 8;

void Launcher::open_multiplayer() {
    room_connection_ = services_.room_connection();
    room_status_ = services_.room_status();
    room_row_ = 0;
    open_modal(Modal::multiplayer);
}

void Launcher::press_multiplayer(Key key) {
    if (key == Key::circle) {
        room_edit_stop_ = true;
        room_connection_.password.clear();
        close_modal();
        return;
    }
    if (room_edit_.valid()) return;
    if (key == Key::square) {
        services_.leave_room();
        room_connection_.password.clear();
        return;
    }
    if (room_status_.phase == Phase::Connected) {
        if (key == Key::up || key == Key::down)
            room_row_ = std::clamp(room_row_ + (key == Key::down ? 1 : -1), 0,
                std::max(0, static_cast<int>(room_status_.members.size()) - kVisibleRoomMembers));
        if (key == Key::cross) services_.leave_room();
        return;
    }
    if (key == Key::up || key == Key::down) {
        room_row_ = (room_row_ + (key == Key::down ? 1 : 4)) % 5;
        cue(Cue::focus);
        return;
    }
    if (key != Key::cross) return;
    const bool idle = room_status_.phase == Phase::Idle || room_status_.phase == Phase::Failed;
    if (room_row_ == 4) {
        if (idle) {
            if (!services_.connect_room(room_connection_))
                say(tr("Could not start the connection or save its settings."), true);
            room_connection_.password.clear();
        } else services_.leave_room();
        return;
    }
    if (!idle) {
        say(tr("Leave the room before changing connection details."), true);
        return;
    }
    const std::string fields[]{room_connection_.host, std::to_string(room_connection_.port),
                               room_connection_.nickname, room_connection_.password};
    room_edit_field_ = room_row_;
    room_edit_stop_ = false;
    try {
        room_edit_ = std::async(std::launch::async, [this, value = fields[room_row_], field = room_row_] {
            return services_.room_text(field, value, room_edit_stop_);
        });
    } catch (const std::exception&) {
        say(tr("The keyboard could not be opened."), true);
    }
}

void Launcher::update_multiplayer() {
    if (modal_ == Modal::multiplayer) {
        auto next = services_.room_status();
        if (next.phase != room_status_.phase) room_row_ = 0;
        room_status_ = std::move(next);
        if (room_status_.phase == Phase::Connected)
            room_row_ = std::min(room_row_, std::max(0,
                static_cast<int>(room_status_.members.size()) - kVisibleRoomMembers));
    }
    if (!room_edit_.valid() || room_edit_.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
        return;
    std::optional<std::string> answer;
    try { answer = room_edit_.get(); }
    catch (const std::exception&) {
        say(tr("The keyboard could not be opened."), true);
        return;
    }
    if (!answer || room_edit_stop_ || modal_ != Modal::multiplayer) return;
    switch (room_edit_field_) {
    case 0: room_connection_.host = *answer; break;
    case 1: {
        unsigned port = 0;
        const auto result = std::from_chars(answer->data(), answer->data() + answer->size(), port);
        if (result.ec != std::errc{} || result.ptr != answer->data() + answer->size() || port == 0 || port > 65535)
            say(tr("Enter a UDP port between 1 and 65535."), true);
        else room_connection_.port = static_cast<std::uint16_t>(port);
        break;
    }
    case 2: room_connection_.nickname = *answer; break;
    case 3: room_connection_.password = *answer; break;
    }
}

void Launcher::draw_multiplayer(Canvas& c, float open) {
    c.list.push_opacity(open);
    glass(c, {500, 150, 920, 780}, 26, theme::kPanel.with_alpha(0.97f), theme::kPanelEdge, 1.6f);
    text(c, tr("Multiplayer"), 542, 216, theme::kDisplay, theme::kTitle);
    const char* states[]{TR("Offline"), TR("Connecting..."), TR("Connected"), TR("Leaving..."), TR("Connection failed")};
    const auto state = states[static_cast<int>(room_status_.phase)];
    text_fit(c, tr(state), 542, 264, theme::kSmall, theme::kCopy, 836);
    const std::string notice = !message_.empty() && message_age_ < 6 ? message_ :
        !room_status_.error.empty() ? room_status_.error :
        tr("Join a room before launching a game, then choose Local Wireless in the game.");
    text_block(c, notice, 542, 303, theme::kSmall, 28, theme::kCopy, 836, 2);
    if (room_status_.phase == Phase::Connected) {
        text_fit(c, room_status_.room, 542, 380, theme::kSmall, theme::kTitle, 836);
        for (int row = 0; row < kVisibleRoomMembers &&
             room_row_ + row < static_cast<int>(room_status_.members.size()); ++row)
            text_fit(c, room_status_.members[room_row_ + row], 542, 430.0f + 44.0f * row,
                     theme::kSmall, theme::kCopy, 836);
        if (room_status_.members.size() > kVisibleRoomMembers)
            text_fit(c, std::to_string(room_row_ + 1) + " - " +
                std::to_string(std::min(room_row_ + kVisibleRoomMembers,
                                       static_cast<int>(room_status_.members.size()))) +
                " / " + std::to_string(room_status_.members.size()),
                542, 808, theme::kSmall, theme::kCopy, 836);
        const Hint hints[]{{Pad::updown, TR("Browse")}, {Pad::cross, TR("Leave room")},
                           {Pad::circle, TR("Back")}};
        draw_hints(c, hints, 3, 542, 878, theme::kCopy, 836);
        c.list.pop_opacity();
        return;
    }
    const bool idle = room_status_.phase == Phase::Idle || room_status_.phase == Phase::Failed;
    const char* labels[]{TR("Room address"), TR("UDP port"), TR("Nickname"), TR("Password"),
                         idle ? TR("Join room") : TR("Leave / cancel")};
    const std::string values[]{room_connection_.host, std::to_string(room_connection_.port),
        room_connection_.nickname, room_connection_.password.empty() ? "" : "********", ""};
    for (int row = 0; row < 5; ++row) {
        const float top = 350.0f + 76.0f * row;
        const Rect rect{542, top, 836, 70};
        if (row == room_row_) plate_focus(c, kRowPlate, rect, 1);
        else plate_rest(c, kRowPlate, rect);
        text_fit(c, tr(labels[row]), 562, top + 28, theme::kSmall, theme::kTitle, 796);
        text_fit(c, values[row], 562, top + 55, theme::kSmall, theme::kCopy, 796);
    }
    const Hint hints[]{{Pad::cross, TR("Select")}, {Pad::square, TR("Go offline")}, {Pad::circle, TR("Back")}};
    draw_hints(c, hints, 3, 542, 878, theme::kCopy, 836);
    c.list.pop_opacity();
}
} // namespace pe::ui
