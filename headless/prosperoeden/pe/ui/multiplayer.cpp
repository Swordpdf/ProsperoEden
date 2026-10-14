// ProsperoEden - Launcher multiplayer: the public room directory, a private room, the room joined.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pe/ui/launcher.hpp"
#include <algorithm>
#include <charconv>

namespace pe::ui {
using Eden::Multiplayer::Phase;
using audio::Cue;
static constexpr int kVisibleRoomMembers = 8;
static constexpr int kVisibleRooms = 7;
// The pages at the left, the room joined at the right.
static constexpr Rect kPane{layout::kLeft, 152.0f, 980.0f, 820.0f};
static constexpr Rect kRoom{1204.0f, 152.0f, 620.0f, 820.0f};

static void draw_room_error(Canvas& c, const Rect& r, const std::string& error) {
    glass(c, r, th().radius_row, th().row, th().warning, 0.6f);
    notice(c, tr("Connection problem"), r.x + 24, r.y + 40, th().size_small, th().warning, r.w - 48, true);
    text_block(c, error, r.x + 24, r.y + 74, th().size_small, 27, th().title, r.w - 48, 3);
}

void Launcher::enter_multiplayer() {
    room_connection_ = services_.room_connection();
    room_status_ = services_.room_status();
    room_row_ = 0;
    room_page_ = RoomPage::choice;
}

bool Launcher::in_multiplayer() const {
    return screen_ == Screen::multiplayer && modal_ == Modal::none;
}

void Launcher::press_multiplayer(Key key) {
    if (key == Key::circle) {
        room_edit_stop_ = true;
        room_directory_stop_ = true;
        room_directory_refresh_ = false;
        room_password_join_ = false;
        room_connection_.password.clear();
        if (room_page_ == RoomPage::choice) go_section(0);
        else { room_page_ = RoomPage::choice; room_row_ = 0; message_.clear(); cue(Cue::back); }
        return;
    }
    if (room_edit_.valid()) return;
    if (key == Key::square) {
        services_.leave_room();
        room_connection_.password.clear();
        return;
    }
    if (room_page_ == RoomPage::choice) {
        if (key == Key::left) return focus_rail();
        if (key == Key::up || key == Key::down) {
            const int next = key == Key::down ? 1 : 0;
            if (next != room_row_) { room_row_ = next; cue(Cue::focus); }
        }
        if (key == Key::cross || key == Key::right) {
            press_ = 1.0f;
            room_page_ = room_row_ == 0 ? RoomPage::public_rooms : RoomPage::private_room;
            room_row_ = 0;
            room_connection_ = services_.room_connection();
            if (room_page_ == RoomPage::public_rooms) {
                room_connection_.password.clear();
                refresh_public_rooms();
            }
            cue(Cue::open);
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
            room_row_ = std::clamp(room_row_ + (key == Key::right ? kVisibleRooms : -kVisibleRooms), 0,
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
            const bool browsing = in_multiplayer() && room_page_ == RoomPage::public_rooms &&
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
        if (room_directory_refresh_ && in_multiplayer() && room_page_ == RoomPage::public_rooms)
            refresh_public_rooms();
    }
    if (screen_ == Screen::multiplayer) {
        auto next = services_.room_status();
        if (next.phase != room_status_.phase && room_page_ != RoomPage::choice) room_row_ = 0;
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
    if (!answer || room_edit_stop_ || screen_ != Screen::multiplayer) {
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

void Launcher::draw_public_rooms(Canvas& c, const Rect& pane) {
    const Theme& t = th();
    const bool loading = room_directory_request_.valid() && !room_directory_stop_.load();
    text_fit(c, loading ? tr("Loading public rooms...") :
        fill(tr("{0} public rooms"), {std::to_string(room_directory_.rooms.size())}),
        pane.x, baseline(pane.y, 30, t.size_small), t.size_small, t.accent_pale, pane.w);
    const Rect nick{pane.x, pane.y + 46, pane.w, 72};
    plate(c, row_plate(), nick, room_row_ == 0 ? 1.0f : 0.0f);
    icon(c, Icon::profile, nick.x + 40, nick.y + 36, 30, room_row_ == 0 ? t.accent_pale : t.label);
    text_fit(c, fill(tr("Nickname: {0}"), {room_connection_.nickname}), nick.x + 72, baseline(nick.y, 72, t.size_text),
             t.size_text, t.title, nick.w - 96);
    const int visible = room_status_.error.empty() ? kVisibleRooms : kVisibleRooms - 2;
    const int first = std::max(0, room_row_ - visible);
    for (int i = 0; i < visible && first + i < static_cast<int>(room_directory_.rooms.size()); ++i) {
        const auto& room = room_directory_.rooms[first + i];
        const float top = pane.y + 130.0f + i * 84.0f;
        const Rect row{pane.x, top, pane.w, 76};
        const bool on = room_row_ == first + i + 1;
        c.list.push_opacity(room.compatible ? 1.0f : 0.55f);
        plate(c, row_plate(), row, on ? 1.0f : 0.0f);
        if (room.password) icon(c, Icon::lock, row.x + 40, row.y + 38, 28, t.label);
        else icon(c, Icon::multiplayer, row.x + 40, row.y + 38, 28, on ? t.accent_pale : t.label);
        // Who is in it: a pip for each place, lit for each player (up to sixteen places shown).
        const int places = std::clamp(static_cast<int>(room.capacity), 0, 16);
        const float pips = places * 16.0f;
        const auto count = std::to_string(room.players) + " / " + std::to_string(room.capacity);
        const float counted = text_fit(c, count, row.x + row.w - 24, baseline(top, 76, t.size_small), t.size_small,
                                       t.value, 120, Align::right);
        const float pip_x = row.x + row.w - 24 - std::max(counted, 84.0f) - 20 - pips;
        for (int place = 0; place < places; ++place)
            c.list.circle(pip_x + place * 16.0f + 6, top + 38, 5,
                          place < static_cast<int>(room.players) ? t.accent : t.faint.with_alpha(0.3f));
        const float room_w = pip_x - 20 - (row.x + 72);
        text_fit(c, room.name, row.x + 72, baseline(top + 8, 34, t.size_text), t.size_text, t.title, room_w);
        std::string detail = room.game;
        if (room.password) { detail += "  /  "; detail += tr("Password"); }
        if (!room.compatible) { detail += "  /  "; detail += tr("Incompatible"); }
        text_fit(c, detail, row.x + 72, baseline(top + 40, 28, 18), 18, t.meta, room_w);
        c.list.pop_opacity();
    }
    std::string words;
    if (!message_.empty() && message_age_ < 6) words = message_;
    else if (!room_directory_.error.empty()) {
        words = room_directory_.error;
        if (!room_directory_.rooms.empty()) words += " " + std::string{tr("Showing the previous list.")};
    }
    else if (!loading && room_directory_loaded_ && room_directory_.rooms.empty()) words = tr("No public rooms are available.");
    else if (room_row_ > 0) words = fill(tr("Room {0} of {1}"),
        {std::to_string(room_row_), std::to_string(room_directory_.rooms.size())});
    const float foot = pane.y + 130.0f + visible * 84.0f + 8;
    text_block(c, words, pane.x, baseline(foot, 28, t.size_small), t.size_small, 28, t.copy, pane.w, 2);
    if (!room_status_.error.empty())
        draw_room_error(c, {pane.x, pane.y + pane.h - 164, pane.w, 152}, room_status_.error);
}

// The room joined, or that there is none: at the right of every page.
void Launcher::draw_room_panel(Canvas& c, const Rect& pane) {
    const Theme& t = th();
    glass(c, pane, t.radius_panel, t.panel.with_alpha(0.90f), t.panel_edge.with_alpha(0.42f), 1.2f);
    const float x = pane.x + 36;
    const float width = pane.w - 72;
    const bool connected = room_status_.phase == Phase::Connected;
    kicker(c, tr("MULTIPLAYER"), x, baseline(pane.y + 34, 30, t.size_small), connected ? t.accent : t.label, width);
    const char* states[]{TR("Offline"), TR("Connecting..."), TR("Connected"), TR("Leaving..."), TR("Connection failed")};
    {
        const Heavy heavy(c);
        text_fit(c, connected ? room_status_.room : std::string{tr(states[static_cast<int>(room_status_.phase)])},
                 x, baseline(pane.y + 72, 52, t.size_lead), t.size_lead,
                 room_status_.phase == Phase::Failed ? t.warning : t.title, width);
    }
    text_fit(c, fill(tr("Nickname: {0}"), {room_connection_.nickname}), x, baseline(pane.y + 130, 30, 22), 22,
             t.meta, width);
    if (connected) {
        // The room's members, a few at a time.
        const int total = static_cast<int>(room_status_.members.size());
        const int first = room_page_ == RoomPage::choice ? 0 : std::clamp(room_row_, 0, std::max(0, total - kVisibleRoomMembers));
        for (int row = 0; row < kVisibleRoomMembers && first + row < total; ++row) {
            const std::string& member = room_status_.members[first + row];
            const float top = pane.y + 188.0f + 66.0f * row;
            c.list.circle(x + 24, top + 28, 24, gfx::mix(t.accent_deep, t.row, 0.5f));
            icon(c, Icon::profile, x + 24, top + 28, 24, t.accent_pale);
            text_fit(c, member, x + 68, baseline(top, 56, t.size_text), t.size_text, t.body, width - 68);
            c.list.rounded_rect({x, top + 61, width, 1}, 0, t.rule.with_alpha(0.4f));
        }
        if (total > kVisibleRoomMembers)
            text_fit(c, std::to_string(first + 1) + " - " + std::to_string(std::min(first + kVisibleRoomMembers, total)) +
                " / " + std::to_string(total), x + width, baseline(pane.y + pane.h - 64, 30, t.size_small),
                t.size_small, t.copy, width, Align::right);
        return;
    }
    text_block(c, tr("Join a room before launching a game, then choose Local Wireless in the game."), x,
               baseline(pane.y + 196, 34, 22), 22, 34, t.copy, width, 4, kShrink);
    if (!room_status_.error.empty() && room_page_ != RoomPage::public_rooms)
        draw_room_error(c, {x, pane.y + pane.h - 188, width, 152}, room_status_.error);
}

void Launcher::draw_multiplayer(Canvas& c) {
    const Theme& t = th();
    const char* states[]{TR("Offline"), TR("Connecting..."), TR("Connected"), TR("Leaving..."), TR("Connection failed")};
    draw_frame(c, tr(room_page_ == RoomPage::choice ? TR("Multiplayer") :
                     room_page_ == RoomPage::public_rooms ? TR("Public rooms") : TR("Private room")),
               tr(states[static_cast<int>(room_status_.phase)]));
    draw_room_panel(c, kRoom);
    const float x = kPane.x;
    const bool own = !rail_focus_;
    if (room_page_ == RoomPage::choice) {
        const char* labels[]{TR("Public rooms"), TR("Private room")};
        const char* descriptions[]{TR("Browse the public directory and choose a room."),
                                   TR("Connect using a room address, port and password.")};
        const Icon icons[]{Icon::multiplayer, Icon::lock};
        for (int i = 0; i < 2; ++i) {
            const Rect row{x, kPane.y + i * 196.0f, kPane.w, 172};
            const float f = own && room_row_ == i ? 1.0f : 0.0f;
            const float scale = 1.0f + (0.015f * f - 0.02f * press_ * f) * motion();
            c.list.push_transform(scale, row.x + row.w * 0.5f, row.y + row.h * 0.5f, 0, 0);
            plate(c, tile_plate(), row, f);
            icon(c, icons[i], row.x + 76, row.y + 86, 64, f > 0 ? t.accent_pale : t.accent);
            {
                const Heavy heavy(c);
                text_shrink(c, tr(labels[i]), row.x + 148, baseline(row.y + 40, 44, t.size_heading), t.size_heading,
                            t.title, row.w - 148 - 40);
            }
            text_shrink(c, tr(descriptions[i]), row.x + 148, baseline(row.y + 94, 32, 22), 22, t.copy, row.w - 148 - 40);
            c.list.pop_transform();
        }
        static constexpr Hint kHints[]{{Pad::cross, TR("Select")}, {Pad::square, TR("Go offline")},
                                       {Pad::circle, TR("Back")}, {Pad::l1, TR("Sections"), Pad::r1}};
        draw_footer(c, kHints, 4);
        return;
    }
    if (room_status_.phase == Phase::Connected) {
        // In a room: its members are at the right; here, the way out.
        const Rect row{x, kPane.y, kPane.w, 172};
        plate(c, tile_plate(), row, 1.0f);
        icon(c, Icon::multiplayer, row.x + 76, row.y + 86, 64, t.accent_pale);
        {
            const Heavy heavy(c);
            text_fit(c, room_status_.room, row.x + 148, baseline(row.y + 40, 44, t.size_heading), t.size_heading,
                     t.title, row.w - 148 - 40);
        }
        text_shrink(c, tr("Leave room"), row.x + 148, baseline(row.y + 94, 32, 22), 22, t.accent_pale, row.w - 148 - 40);
        static constexpr Hint kHints[]{{Pad::updown, TR("Browse")}, {Pad::cross, TR("Leave room")},
                                       {Pad::circle, TR("Back")}};
        draw_footer(c, kHints, 3);
        return;
    }
    if (room_page_ == RoomPage::public_rooms && room_status_.phase != Phase::Connecting &&
        room_status_.phase != Phase::Disconnecting) {
        draw_public_rooms(c, kPane);
        static constexpr Hint kHints[]{{Pad::cross, TR("Select")}, {Pad::leftright, TR("Page")},
                                       {Pad::triangle, TR("Refresh")}, {Pad::circle, TR("Back")}};
        draw_menu_hints(c, kHints, 4, layout::kLeft, layout::kFooter, t.copy, layout::kWidth);
        return;
    }
    // The private room's form (also what shows while a room is being joined or left).
    const bool idle = room_status_.phase == Phase::Idle || room_status_.phase == Phase::Failed;
    const char* labels[]{TR("Room address"), TR("UDP port"), TR("Nickname"), TR("Password"),
                         idle ? TR("Join room") : TR("Leave / cancel")};
    const std::string values[]{room_connection_.host, std::to_string(room_connection_.port),
        room_connection_.nickname, room_connection_.password.empty() ? "" : "********", ""};
    for (int row = 0; row < 5; ++row) {
        const Rect rect{x, kPane.y + 94.0f * row, kPane.w, 84};
        const float f = row == room_row_ ? 1.0f : 0.0f;
        if (row == 4) {
            // The form's action, as a button.
            if (idle) primary_button(c, {rect.x, rect.y + 8, 420, 76}, tr(labels[row]), translate_hint(Pad::cross), f, press_);
            else secondary_button(c, {rect.x, rect.y + 8, 420, 76}, tr(labels[row]), translate_hint(Pad::cross), f, press_);
            continue;
        }
        plate(c, row_plate(), rect, f);
        const float shown = text_fit(c, values[row], rect.x + rect.w - 28, baseline(rect.y, 84, t.size_text),
                                     t.size_text, t.accent_pale, rect.w * 0.6f, Align::right);
        text_shrink(c, tr(labels[row]), rect.x + 28, baseline(rect.y, 84, t.size_text), t.size_text,
                    f > 0 ? t.title : t.value, rect.w - 56 - shown - 24);
    }
    const std::string words = !message_.empty() && message_age_ < 6 ? message_ : std::string{};
    if (!words.empty())
        notice_block(c, words, x, baseline(kPane.y + 94.0f * 5 + 16, 28, t.size_small), t.size_small, 28,
                     message_warning_ ? t.warning : t.accent_pale, kPane.w, 2, message_warning_);
    static constexpr Hint kHints[]{{Pad::cross, TR("Select")}, {Pad::square, TR("Go offline")},
                                   {Pad::circle, TR("Back")}};
    draw_menu_hints(c, kHints, 3, layout::kLeft, layout::kFooter, t.copy, layout::kWidth);
}
} // namespace pe::ui
