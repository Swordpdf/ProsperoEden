// SPDX-License-Identifier: GPL-3.0-or-later
#include "pe/ui/launcher.hpp"
#include <algorithm>
#include <charconv>

namespace pe::ui {
using Eden::Multiplayer::Phase;
using audio::Cue;
static constexpr int kVisibleRoomMembers = 8;

static void draw_room_error(Canvas& c, const std::string& error) {
    glass(c, {542, 704, 836, 152}, 14, theme::kRow, theme::kWarning, 1.6f);
    text(c, tr("Connection problem"), 562, 736, theme::kSmall, theme::kWarning);
    text_block(c, error, 562, 768, theme::kSmall, 27, theme::kTitle, 796, 3);
}

void Launcher::open_multiplayer() {
    room_connection_ = services_.room_connection();
    room_status_ = services_.room_status();
    room_row_ = 0;
    room_page_ = RoomPage::choice;
    open_modal(Modal::multiplayer);
}

void Launcher::press_multiplayer(Key key) {
    if (key == Key::circle) {
        room_edit_stop_ = true;
        room_directory_stop_ = true;
        room_directory_refresh_ = false;
        room_password_join_ = false;
        room_connection_.password.clear();
        if (room_page_ == RoomPage::choice) close_modal();
        else { room_page_ = RoomPage::choice; room_row_ = 0; message_.clear(); }
        return;
    }
    if (room_edit_.valid()) return;
    if (key == Key::square) {
        services_.leave_room();
        room_connection_.password.clear();
        return;
    }
    if (room_page_ == RoomPage::choice) {
        if (key == Key::up || key == Key::down) { room_row_ = 1 - room_row_; cue(Cue::focus); }
        if (key == Key::cross) {
            room_page_ = room_row_ == 0 ? RoomPage::public_rooms : RoomPage::private_room;
            room_row_ = 0;
            room_connection_ = services_.room_connection();
            if (room_page_ == RoomPage::public_rooms) {
                room_connection_.password.clear();
                refresh_public_rooms();
            }
        }
        return;
    }
    if (room_status_.phase == Phase::Connected) {
        if (key == Key::up || key == Key::down)
            room_row_ = std::clamp(room_row_ + (key == Key::down ? 1 : -1), 0,
                std::max(0, static_cast<int>(room_status_.members.size()) - kVisibleRoomMembers));
        if (key == Key::cross) services_.leave_room();
        return;
    }
    if (room_page_ == RoomPage::public_rooms && room_status_.phase != Phase::Connecting &&
        room_status_.phase != Phase::Disconnecting) {
        if (key == Key::triangle) { refresh_public_rooms(); return; }
        if (key == Key::up || key == Key::down) {
            room_row_ = std::clamp(room_row_ + (key == Key::down ? 1 : -1), 0,
                                   static_cast<int>(room_directory_.rooms.size()));
            cue(Cue::focus);
        }
        if (key == Key::left || key == Key::right) { // a page at a time: public lists are long
            room_row_ = std::clamp(room_row_ + (key == Key::right ? 6 : -6), 0,
                                   static_cast<int>(room_directory_.rooms.size()));
            cue(Cue::focus);
        }
        if (key != Key::cross) return;
        if (room_row_ > 0) {
            const auto& room = room_directory_.rooms[room_row_ - 1];
            if (!room.compatible) { say(tr("This room uses a different protocol version."), true); return; }
            room_connection_.host = room.address;
            room_connection_.port = room.port;
            room_connection_.password.clear();
            if (!room.password) {
                if (!services_.connect_public_room(room_connection_))
                    say(tr("Could not start the connection or save its settings."), true);
                return;
            }
        }
        room_edit_field_ = room_row_ == 0 ? 2 : 3;
        room_password_join_ = room_edit_field_ == 3;
        room_edit_stop_ = false;
        try {
            room_edit_ = std::async(std::launch::async, [this, field = room_edit_field_,
                initial = room_edit_field_ == 2 ? room_connection_.nickname : std::string{}] {
                return services_.room_text(field, initial, room_edit_stop_);
            });
        } catch (const std::exception&) {
            room_password_join_ = false;
            say(tr("The keyboard could not be opened."), true);
        }
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
    if (room_directory_request_.valid() &&
        room_directory_request_.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        Eden::Multiplayer::RoomDirectory result;
        try { result = room_directory_request_.get(); }
        catch (const std::exception&) { result.error = "Could not download public rooms."; }
        if (!room_directory_stop_.load()) {
            // While the list is on screen the cursor stays on the room it was on: a refresh
            // reorders rooms as players come and go. Elsewhere room_row_ belongs to another view.
            const bool browsing = modal_ == Modal::multiplayer && room_page_ == RoomPage::public_rooms &&
                room_status_.phase != Phase::Connected && room_status_.phase != Phase::Connecting &&
                room_status_.phase != Phase::Disconnecting;
            const auto key_of = [](const Eden::Multiplayer::PublicRoom& room) {
                return room.address + ":" + std::to_string(room.port);
            };
            std::string selected;
            if (browsing && room_row_ > 0 && room_row_ <= static_cast<int>(room_directory_.rooms.size()))
                selected = key_of(room_directory_.rooms[room_row_ - 1]);
            if (result.error.empty()) room_directory_ = std::move(result);
            else room_directory_.error = std::move(result.error);
            room_directory_loaded_ = true;
            if (browsing) {
                const int count = static_cast<int>(room_directory_.rooms.size());
                room_row_ = std::min(room_row_, count);
                for (int i = 0; !selected.empty() && i < count; ++i) {
                    if (key_of(room_directory_.rooms[i]) == selected) { room_row_ = i + 1; break; }
                }
            }
        }
        if (room_directory_refresh_ && modal_ == Modal::multiplayer && room_page_ == RoomPage::public_rooms)
            refresh_public_rooms();
    }
    if (modal_ == Modal::multiplayer) {
        auto next = services_.room_status();
        if (next.phase != room_status_.phase) room_row_ = 0;
        room_status_ = std::move(next);
        if (room_status_.phase == Phase::Connected && room_page_ != RoomPage::choice)
            room_row_ = std::min(room_row_, std::max(0,
                static_cast<int>(room_status_.members.size()) - kVisibleRoomMembers));
    }
    if (!room_edit_.valid() || room_edit_.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
        return;
    std::optional<std::string> answer;
    try { answer = room_edit_.get(); }
    catch (const std::exception&) {
        room_password_join_ = false;
        say(tr("The keyboard could not be opened."), true);
        return;
    }
    if (!answer || room_edit_stop_ || modal_ != Modal::multiplayer) {
        room_password_join_ = false;
        return;
    }
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
    // Saved as typed, so the form survives closing the app before a join. A public room's
    // address and password are never saved: they would replace the private room.
    if (room_page_ == RoomPage::private_room) services_.remember_room(room_connection_, true);
    else if (room_edit_field_ == 2) services_.remember_room(room_connection_, false);
    if (room_password_join_) {
        room_password_join_ = false;
        if (!services_.connect_public_room(room_connection_))
            say(tr("Could not start the connection or save its settings."), true);
        room_connection_.password.clear();
    }
}

void Launcher::refresh_public_rooms() {
    if (room_directory_request_.valid()) {
        // Reopening while the previous cancellation completes must still fetch a fresh list.
        room_directory_refresh_ = room_directory_stop_.load();
        return;
    }
    room_directory_refresh_ = false;
    room_directory_stop_ = false;
    room_directory_.error.clear();
    message_.clear();
    try {
        room_directory_request_ = std::async(std::launch::async, [this] {
            return services_.public_rooms(room_directory_stop_);
        });
    } catch (const std::exception&) { room_directory_.error = "Could not start the public room request."; }
}

void Launcher::draw_public_rooms(Canvas& c) {
    const bool loading = room_directory_request_.valid() && !room_directory_stop_.load();
    text_fit(c, loading ? tr("Loading public rooms...") :
        fill(tr("{0} public rooms"), {std::to_string(room_directory_.rooms.size())}),
        542, 300, theme::kSmall, theme::kCopy, 836);
    const Rect nick{542, 322, 836, 64};
    if (room_row_ == 0) plate_focus(c, kRowPlate, nick, 1);
    else plate_rest(c, kRowPlate, nick);
    text_fit(c, fill(tr("Nickname: {0}"), {room_connection_.nickname}), 562, 363,
             theme::kSmall, theme::kTitle, 796);
    const int visible = room_status_.error.empty() ? 6 : 4;
    const int first = std::max(0, room_row_ - visible);
    for (int i = 0; i < visible && first + i < static_cast<int>(room_directory_.rooms.size()); ++i) {
        const auto& room = room_directory_.rooms[first + i];
        const float top = 400.0f + i * 64.0f;
        const Rect row{542, top, 836, 60};
        if (room_row_ == first + i + 1) plate_focus(c, kRowPlate, row, 1);
        else plate_rest(c, kRowPlate, row);
        text_fit(c, room.name, 560, top + 25, theme::kSmall, theme::kTitle, 580);
        const auto count = std::to_string(room.players) + " / " + std::to_string(room.capacity);
        text_fit(c, count, 1170, top + 25, theme::kSmall, theme::kCopy, 190);
        std::string detail = room.game;
        if (room.password) { detail += "  |  "; detail += tr("Password"); }
        if (!room.compatible) { detail += "  |  "; detail += tr("Incompatible"); }
        text_fit(c, detail, 560, top + 49, theme::kSmall, theme::kCopy, 796);
    }
    std::string notice;
    if (!message_.empty() && message_age_ < 6) notice = message_;
    else if (!room_directory_.error.empty()) {
        notice = room_directory_.error;
        if (!room_directory_.rooms.empty()) notice += " " + std::string{tr("Showing the previous list.")};
    }
    else if (!loading && room_directory_loaded_ && room_directory_.rooms.empty()) notice = tr("No public rooms are available.");
    else if (room_row_ > 0) notice = fill(tr("Room {0} of {1}"),
        {std::to_string(room_row_), std::to_string(room_directory_.rooms.size())});
    if (!room_status_.error.empty()) {
        text_fit(c, notice, 542, 682, theme::kSmall, theme::kCopy, 836);
        draw_room_error(c, room_status_.error);
    }
    else text_block(c, notice, 542, 814, theme::kSmall, 26, theme::kCopy, 836, 2);
    const Hint hints[]{{Pad::cross, TR("Select")}, {Pad::leftright, TR("Page")}, {Pad::triangle, TR("Refresh")},
                       {Pad::circle, TR("Back")}};
    draw_hints(c, hints, 4, 542, 890, theme::kCopy, 836);
}

void Launcher::draw_multiplayer(Canvas& c, float open) {
    c.list.push_opacity(open);
    glass(c, {500, 150, 920, 780}, 26, theme::kPanel.with_alpha(0.97f), theme::kPanelEdge, 1.6f);
    text(c, tr(room_page_ == RoomPage::choice ? TR("Multiplayer") :
               room_page_ == RoomPage::public_rooms ? TR("Public rooms") : TR("Private room")),
         542, 216, theme::kDisplay, theme::kTitle);
    const char* states[]{TR("Offline"), TR("Connecting..."), TR("Connected"), TR("Leaving..."), TR("Connection failed")};
    const auto state = states[static_cast<int>(room_status_.phase)];
    text_fit(c, tr(state), 542, 264, theme::kSmall, theme::kCopy, 836);
    if (room_page_ == RoomPage::choice) {
        const char* labels[]{TR("Public rooms"), TR("Private room")};
        const char* descriptions[]{TR("Browse the public directory and choose a room."),
                                   TR("Connect using a room address, port and password.")};
        for (int i = 0; i < 2; ++i) {
            const float top = 350.0f + i * 140.0f;
            const Rect row{542, top, 836, 120};
            if (room_row_ == i) plate_focus(c, kRowPlate, row, 1);
            else plate_rest(c, kRowPlate, row);
            text(c, tr(labels[i]), 568, top + 42, theme::kSmall, theme::kTitle);
            text_fit(c, tr(descriptions[i]), 568, top + 86, theme::kSmall, theme::kCopy, 784);
        }
        if (!room_status_.error.empty()) draw_room_error(c, room_status_.error);
        else text_block(c, tr("Join a room before launching a game, then choose Local Wireless in the game."),
                   542, 700, theme::kSmall, 28, theme::kCopy, 836, 3);
        const Hint hints[]{{Pad::cross, TR("Select")}, {Pad::square, TR("Go offline")}, {Pad::circle, TR("Back")}};
        draw_hints(c, hints, 3, 542, 878, theme::kCopy, 836);
        c.list.pop_opacity();
        return;
    }
    if (room_page_ == RoomPage::public_rooms && room_status_.phase != Phase::Connected &&
        room_status_.phase != Phase::Connecting && room_status_.phase != Phase::Disconnecting) {
        draw_public_rooms(c);
        c.list.pop_opacity();
        return;
    }
    const std::string notice = !message_.empty() && message_age_ < 6 ? message_ :
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
        const float top = 340.0f + 70.0f * row;
        const Rect rect{542, top, 836, 64};
        if (row == room_row_) plate_focus(c, kRowPlate, rect, 1);
        else plate_rest(c, kRowPlate, rect);
        text_fit(c, tr(labels[row]), 562, top + 28, theme::kSmall, theme::kTitle, 796);
        text_fit(c, values[row], 562, top + 55, theme::kSmall, theme::kCopy, 796);
    }
    if (!room_status_.error.empty()) draw_room_error(c, room_status_.error);
    const Hint hints[]{{Pad::cross, TR("Select")}, {Pad::square, TR("Go offline")}, {Pad::circle, TR("Back")}};
    draw_hints(c, hints, 3, 542, 878, theme::kCopy, 836);
    c.list.pop_opacity();
}
} // namespace pe::ui
