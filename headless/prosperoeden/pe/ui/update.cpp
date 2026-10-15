// ProsperoEden - Launcher: a newer release of the app. The offer (Update now / What's new / Skip)
// each time the app opens while one is listed; the release notes in a view of their own; then a
// ring fills while it downloads and unpacks, and ProsperoEden closes for the update helper to
// replace its files.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace pe::ui
{

using audio::Cue;

namespace
{

// The panel with buttons; while the update works it is shorter, and with the notes taller; it
// stays centred.
constexpr Rect kPanel{560.0f, 196.0f, 800.0f, 688.0f};
constexpr float kWorkingHeight = 604.0f;
constexpr float kNotesHeight = 864.0f;
constexpr float kCenterX = 960.0f;
// The badge and the ring, under the top of the panel.
constexpr float kRingY = 384.0f;
constexpr float kRingRadius = 96.0f;
constexpr float kRingWidth = 10.0f;
// The buttons, side by side across the same width, two or three of them.
constexpr float kButtonsTop = 704.0f;
constexpr float kButtonHeight = 76.0f;
constexpr float kButtonsLeft = 612.0f;
constexpr float kButtonsWidth = 696.0f;
constexpr float kButtonGap = 16.0f;
// The notes view: the text's window, under the title and above the buttons.
constexpr float kNotesLeft = kPanel.x + 64.0f;
constexpr float kNotesWidth = kPanel.w - 128.0f - 18.0f; // room for the scrollbar
constexpr float kNotesTop = kPanel.y + 132.0f;
constexpr float kNotesBottomRoom = 214.0f;               // buttons and hints under the window
// How long "closes now" shows before the app closes.
constexpr float kClosingSeconds = 3.0f;
constexpr float kPi = 3.14159265f;

std::string fill_text(std::string_view pattern, const std::string &value)
{
    return fill(pattern, {value});
}

std::string megabytes(std::uint64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

// One button's place when `count` share the row.
Rect button_rect(int count, float index, float top)
{
    const float width = (kButtonsWidth - kButtonGap * static_cast<float>(count - 1)) / static_cast<float>(count);
    return {kButtonsLeft + (width + kButtonGap) * index, top, width, kButtonHeight};
}

// The UTF-8 sequence starting at text[at]: its length in bytes.
std::size_t glyph_length(std::string_view text, std::size_t at)
{
    const auto lead = static_cast<unsigned char>(text[at]);
    const std::size_t length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return std::min(length, text.size() - at);
}

// Text broken into lines no wider than `width`: at spaces, and inside a word (between whole
// characters) only when the word alone is wider, as a long link or text without spaces is.
std::vector<std::string> wrap(Canvas &c, std::string_view text, float size, float width)
{
    std::vector<std::string> lines;
    std::string line;
    std::size_t at = 0;
    while (at < text.size())
    {
        const std::size_t space = text.find(' ', at);
        const std::string_view word = text.substr(at, space == std::string_view::npos ? std::string_view::npos : space - at);
        at = space == std::string_view::npos ? text.size() : space + 1;
        if (word.empty())
            continue;
        const std::string joined = line.empty() ? std::string{word} : line + " " + std::string{word};
        if (text_width(c, joined, size) <= width)
        {
            line = joined;
            continue;
        }
        if (!line.empty())
            lines.push_back(std::move(line));
        line.clear();
        if (text_width(c, word, size) <= width)
        {
            line = std::string{word};
            continue;
        }
        for (std::size_t i = 0; i < word.size();)
        {
            const std::size_t length = glyph_length(word, i);
            const std::string longer = line + std::string{word.substr(i, length)};
            if (!line.empty() && text_width(c, longer, size) > width)
            {
                lines.push_back(std::move(line));
                line = std::string{word.substr(i, length)};
            }
            else
            {
                line = longer;
            }
            i += length;
        }
    }
    if (!line.empty())
        lines.push_back(std::move(line));
    return lines;
}

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

void Launcher::layout_notes(Canvas &c)
{
    // Laid out once per text size (Larger text changes it).
    if (notes_laid_out_ && notes_large_ == look().large_text)
        return;
    notes_laid_out_ = true;
    notes_large_ = look().large_text;
    notes_lines_.clear();
    notes_boxes_.clear();

    float y = 0.0f;
    bool gap_before = false;
    const std::string &all = update_.notes;
    std::size_t at = 0;
    while (at <= all.size())
    {
        const std::size_t end = std::min(all.find('\n', at), all.size());
        std::string_view line = std::string_view{all}.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r'))
            line.remove_suffix(1);
        if (line.empty())
        {
            gap_before = !notes_lines_.empty();
            if (end >= all.size())
                break;
            continue;
        }
        // What the line is: a list item, a callout (the catalog turns GitHub's "> [!WARNING]"
        // boxes into "Warning: ..."), a heading (short, without closing punctuation) or text.
        const bool bullet = starts_with(line, "- ");
        if (bullet)
            line.remove_prefix(2);
        const bool warning = !bullet && (starts_with(line, "Warning:") || starts_with(line, "Caution:") ||
                                         starts_with(line, "Important:"));
        const bool note = !bullet && (starts_with(line, "Note:") || starts_with(line, "Tip:"));
        const char last = line.back();
        const bool heading = !bullet && !warning && !note && line.size() <= 48 && last != '.' && last != ':' &&
                             last != '!' && last != '?' && last != ',' && last != ';' && last != ')';

        const float size = heading ? th().size_text : th().size_small;
        const float pitch = std::round(text_size(size) * (heading ? 1.45f : 1.6f));
        const Color color = heading ? th().title : th().body;
        const bool boxed = warning || note;
        const float indent = bullet ? 30.0f : boxed ? 26.0f : 0.0f;
        const float width = kNotesWidth - indent - (boxed ? 22.0f : 0.0f);

        if (!notes_lines_.empty())
            y += heading ? 22.0f : boxed ? 18.0f : gap_before ? 14.0f : bullet ? 4.0f : 8.0f;
        gap_before = false;
        const float block_top = y;
        if (boxed)
            y += 14.0f;
        const std::vector<std::string> pieces = wrap(c, line, size, width);
        for (std::size_t i = 0; i < pieces.size(); ++i)
        {
            NoteLine out;
            out.text = pieces[i];
            out.y = y;
            out.height = pitch;
            out.size = size;
            out.color = color;
            out.indent = indent;
            out.bullet = bullet && i == 0;
            notes_lines_.push_back(std::move(out));
            y += pitch;
        }
        if (boxed)
        {
            y += 14.0f;
            notes_boxes_.push_back({block_top, y, warning});
        }
        if (end >= all.size())
            break;
    }
    if (update_.notes_truncated)
    {
        y += 20.0f;
        NoteLine out;
        out.text = tr("The rest is on the app's page on homebrew.page.");
        out.y = y;
        out.height = std::round(text_size(th().size_small) * 1.6f);
        out.size = th().size_small;
        out.color = th().meta;
        notes_lines_.push_back(out);
        y += out.height;
    }
    notes_height_ = y;
}

float Launcher::notes_window() const
{
    return std::max(120.0f, kNotesHeight - (kNotesTop - kPanel.y) - kNotesBottomRoom);
}

float Launcher::notes_max_scroll() const
{
    return std::max(0.0f, notes_height_ - notes_window());
}

void Launcher::scroll_notes(float by)
{
    const float target = std::clamp(notes_target_ + by, 0.0f, notes_max_scroll());
    if (target == notes_target_)
    {
        // Already at that end: the text gives a little and comes back.
        notes_bounce_.value = by > 0.0f ? 18.0f : -18.0f;
        notes_bounce_.velocity = 0.0f;
        return;
    }
    notes_target_ = target;
    cue(Cue::focus);
}

void Launcher::begin_update()
{
    update_status_ = {};
    update_fraction_.snap(0.0f);
    update_rate_ = 0.0f;
    update_rate_done_ = 0;
    update_rate_wait_ = 0.0f;
    update_stage_time_ = 0.0f;
    if (services_.start_update())
    {
        update_stage_ = UpdateStage::working;
        cue(Cue::select);
        return;
    }
    update_stage_ = UpdateStage::failed;
    update_status_.error = "The update helper could not start";
    update_choice_ = 0;
    update_choice_x_.snap(0.0f);
    cue(Cue::error);
}

void Launcher::open_notes()
{
    update_stage_ = UpdateStage::notes;
    update_stage_time_ = 0.0f;
    notes_target_ = 0.0f;
    notes_scroll_.snap(0.0f);
    notes_bounce_.snap(0.0f);
    update_choice_ = 0;
    update_choice_x_.snap(0.0f);
    cue(Cue::open);
}

void Launcher::close_notes()
{
    update_stage_ = UpdateStage::offer;
    // Back on the offer, with the ring already nearly full and the highlight on What's new.
    update_stage_time_ = 0.6f;
    update_choice_ = 1;
    update_choice_x_.snap(1.0f);
    cue(Cue::back);
}

void Launcher::update_install(float dt)
{
    update_choice_x_.target = static_cast<float>(update_choice_);
    update_choice_x_.update(dt, th().cursor_spring);
    if (modal_shown_ != Modal::update)
        return;
    const bool buttons = update_stage_ == UpdateStage::offer || update_stage_ == UpdateStage::failed;
    update_height_.target = update_stage_ == UpdateStage::notes ? kNotesHeight : buttons ? kPanel.h : kWorkingHeight;
    update_height_.update(dt, 13.0f);
    update_stage_time_ += dt;
    update_spin_ += dt;
    notes_scroll_.target = notes_target_;
    notes_scroll_.update(dt, look().reduce_motion ? 60.0f : 15.0f);
    notes_bounce_.target = 0.0f;
    notes_bounce_.update(dt, 16.0f);

    if (update_stage_ == UpdateStage::working || update_stage_ == UpdateStage::cancelling)
    {
        update_status_ = services_.update_status();
        const UpdatePhase phase = update_status_.phase;
        if (phase == UpdatePhase::cancelled)
        {
            services_.finish_update();
            modal_ = Modal::none;
            cue(Cue::modal_close);
            return;
        }
        if (phase == UpdatePhase::failed)
        {
            services_.finish_update();
            update_stage_ = UpdateStage::failed;
            update_stage_time_ = 0.0f;
            update_choice_ = 0;
            update_choice_x_.snap(0.0f);
            cue(Cue::error);
            return;
        }
        if (phase == UpdatePhase::ready && update_stage_ == UpdateStage::working)
        {
            if (services_.apply_update())
            {
                update_stage_ = UpdateStage::closing;
                update_stage_time_ = 0.0f;
                update_fraction_.target = 1.0f;
                cue(Cue::saved);
            }
            else
            {
                services_.finish_update();
                update_stage_ = UpdateStage::failed;
                update_stage_time_ = 0.0f;
                update_status_.error = "The update helper did not answer";
                update_choice_ = 0;
                update_choice_x_.snap(0.0f);
                cue(Cue::error);
            }
            return;
        }
        // The share done, for the ring and the bar: the download, then unpacking from where it
        // stands (it has no share of its own when the helper does not say one).
        if (update_status_.total > 0)
            update_fraction_.target =
                static_cast<float>(static_cast<double>(update_status_.done) / static_cast<double>(update_status_.total));
        else if (phase == UpdatePhase::unpacking)
            update_fraction_.target = 1.0f;
        // The speed, for the time left: sampled twice a second, smoothed.
        update_rate_wait_ += dt;
        if (phase == UpdatePhase::downloading && update_rate_wait_ >= 0.5f)
        {
            const std::uint64_t done = update_status_.done;
            const float now = done >= update_rate_done_ ? static_cast<float>(done - update_rate_done_) / update_rate_wait_
                                                        : 0.0f;
            update_rate_ = update_rate_ <= 0.0f ? now : update_rate_ * 0.75f + now * 0.25f;
            update_rate_done_ = done;
            update_rate_wait_ = 0.0f;
        }
    }
    update_fraction_.update(dt, 9.0f);

    // Closing: once the message has been read, the app ends; the helper finishes the update.
    if (update_stage_ == UpdateStage::closing && update_stage_time_ >= kClosingSeconds)
        done_ = true;
}

void Launcher::press_update(Key key)
{
    const bool has_notes = !update_.notes.empty();
    switch (update_stage_)
    {
    case UpdateStage::offer:
    case UpdateStage::failed:
    {
        // The offer has What's new between its buttons when the release has notes.
        const bool notes_button = update_stage_ == UpdateStage::offer && has_notes;
        const int count = notes_button ? 3 : 2;
        if (key == Key::left || key == Key::right)
        {
            const int choice = std::clamp(update_choice_ + (key == Key::right ? 1 : -1), 0, count - 1);
            if (choice != update_choice_)
            {
                update_choice_ = choice;
                cue(Cue::focus);
            }
            return;
        }
        if (key == Key::triangle && notes_button)
            return open_notes();
        if (key == Key::cross && update_choice_ == 0)
            return begin_update();
        if (key == Key::cross && notes_button && update_choice_ == 1)
            return open_notes();
        if (key == Key::cross || key == Key::circle)
        {
            // Skipped: asked again the next time the app opens.
            modal_ = Modal::none;
            cue(Cue::modal_close);
        }
        return;
    }
    case UpdateStage::notes:
    {
        const float line = std::round(text_size(th().size_small) * 1.6f);
        switch (key)
        {
        case Key::up:
        case Key::down:
            return scroll_notes((key == Key::down ? 3.0f : -3.0f) * line);
        case Key::l1:
        case Key::r1:
            return scroll_notes((key == Key::r1 ? 1.0f : -1.0f) * (notes_window() - 2.0f * line));
        case Key::left:
        case Key::right:
        {
            const int choice = key == Key::right ? 1 : 0;
            if (choice != update_choice_)
            {
                update_choice_ = choice;
                cue(Cue::focus);
            }
            return;
        }
        case Key::cross:
            return update_choice_ == 0 ? begin_update() : close_notes();
        case Key::circle:
            return close_notes();
        default:
            return;
        }
    }
    case UpdateStage::working:
        if (key == Key::circle)
        {
            services_.cancel_update();
            update_stage_ = UpdateStage::cancelling;
            update_stage_time_ = 0.0f;
            cue(Cue::back);
        }
        return;
    case UpdateStage::cancelling:
    case UpdateStage::closing:
        return;
    }
}

void Launcher::draw_notes(Canvas &c, float height)
{
    gfx::DrawList &list = c.list;
    layout_notes(c);
    const float t = update_stage_time_;
    const float arrive = tween::cubic_out(t / 0.35f);

    // The title, with a small mark: a page with lines on it.
    list.push_opacity(arrive);
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (1.0f - arrive) * 10.0f * motion());
    const float mark_x = kNotesLeft + 22.0f;
    const float mark_y = kPanel.y + 66.0f;
    list.circle(mark_x, mark_y, 24.0f, th().accent.with_alpha(0.16f));
    list.ring(mark_x, mark_y, 24.0f, 2.0f, th().accent.with_alpha(0.55f));
    for (int i = 0; i < 3; ++i)
    {
        const float ly = mark_y - 8.0f + 8.0f * static_cast<float>(i);
        list.line(mark_x - 9.0f, ly, mark_x + (i == 2 ? 3.0f : 9.0f), ly, 2.6f, th().accent);
    }
    text_shrink(c, fill_text(tr("What's new in version {0}"), update_version_), kNotesLeft + 64.0f,
                baseline(kPanel.y + 42.0f, 48.0f, th().size_heading), th().size_heading, th().title,
                kPanel.w - 128.0f - 64.0f);
    list.rounded_rect({kNotesLeft, kPanel.y + 112.0f, kPanel.w - 128.0f, 1.0f}, 0.0f, th().rule.with_alpha(0.9f));
    list.pop_transform();
    list.pop_opacity();

    // The text, in its window, moved by the scroll (and a little more at the ends).
    const float window = notes_window();
    const Rect area{kNotesLeft - 6.0f, kNotesTop, kNotesWidth + 12.0f, window};
    const float scroll = notes_scroll_.value + notes_bounce_.value * 0.6f;
    list.push_clip(area);
    for (const NoteBox &box : notes_boxes_)
    {
        const float top = kNotesTop + box.top - scroll;
        const float bottom = kNotesTop + box.bottom - scroll;
        if (bottom < area.y || top > area.y + area.h)
            continue;
        const Color accent = box.warning ? th().warning : th().accent;
        list.push_opacity(arrive);
        list.rounded_rect({kNotesLeft, top, kNotesWidth, bottom - top}, 14.0f, accent.with_alpha(0.09f));
        list.rounded_rect({kNotesLeft, top + 10.0f, 4.0f, bottom - top - 20.0f}, 2.0f, accent.with_alpha(0.85f));
        list.pop_opacity();
    }
    int shown = 0;
    for (const NoteLine &line : notes_lines_)
    {
        const float top = kNotesTop + line.y - scroll;
        if (top + line.height < area.y || top > area.y + area.h)
            continue;
        // The lines first shown come in one after another.
        const float delay = 0.10f + 0.03f * static_cast<float>(std::min(shown++, 14));
        const float in = t > 1.2f ? 1.0f : tween::cubic_out((t - delay) / 0.32f);
        if (in <= 0.0f)
            continue;
        list.push_opacity(in);
        list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (1.0f - in) * 14.0f * motion());
        if (line.bullet)
            list.circle(kNotesLeft + 11.0f, top + line.height * 0.5f, 3.6f, th().accent.with_alpha(0.9f));
        text(c, line.text, kNotesLeft + line.indent, baseline(top, line.height, line.size), line.size, line.color);
        list.pop_transform();
        list.pop_opacity();
    }
    list.pop_clip();

    // Soft edges where more text is above or below.
    const float max_scroll = notes_max_scroll();
    const Color panel = th().panel;
    const float above = tween::clamp01(scroll / 40.0f);
    const float below = tween::clamp01((max_scroll - scroll) / 40.0f);
    if (above > 0.0f)
        list.gradient_rect({area.x, area.y, area.w, 36.0f}, 0.0f, panel.with_alpha(0.96f * above), panel.with_alpha(0.0f));
    if (below > 0.0f)
        list.gradient_rect({area.x, area.y + area.h - 44.0f, area.w, 44.0f}, 0.0f, panel.with_alpha(0.0f),
                           panel.with_alpha(0.96f * below));

    // The scrollbar: a track and a thumb that follows the scroll.
    if (max_scroll > 0.0f)
    {
        const float track_x = kNotesLeft + kNotesWidth + 14.0f;
        const float thumb = std::max(48.0f, window * window / notes_height_);
        const float place = tween::clamp01(scroll / max_scroll);
        list.push_opacity(arrive);
        list.rounded_rect({track_x, area.y, 4.0f, window}, 2.0f, th().panel_edge.with_alpha(0.22f));
        list.rounded_rect({track_x - 1.0f, area.y + (window - thumb) * place, 6.0f, thumb}, 3.0f,
                          th().accent.with_alpha(0.85f));
        list.pop_opacity();
    }

    // The buttons and the hints under the window.
    const float buttons_top = kPanel.y + height - 178.0f;
    list.push_opacity(arrive);
    for (int i = 0; i < 2; ++i)
        plate_rest(c, row_plate(), button_rect(2, static_cast<float>(i), buttons_top));
    plate_focus(c, row_plate(), button_rect(2, update_choice_x_.value, buttons_top), 1.0f - 0.25f * press_);
    static constexpr const char *kLabels[] = {TR("Update now"), TR("Back")};
    for (int i = 0; i < 2; ++i)
    {
        const Rect r = button_rect(2, static_cast<float>(i), buttons_top);
        text_shrink(c, tr(kLabels[i]), r.x + r.w * 0.5f, baseline(r.y, r.h, th().size_text), th().size_text,
                    i == update_choice_ ? th().title : th().value, r.w - 40.0f, Align::center);
    }
    static constexpr Hint kNotesHints[] = {{Pad::updown, TR("Scroll")},
                                           {Pad::l1, TR("Page"), Pad::r1},
                                           {Pad::cross, TR("Select")},
                                           {Pad::circle, TR("Back")}};
    draw_menu_hints(c, kNotesHints, 4, kPanel.x + 52.0f, kPanel.y + height - 50.0f, th().copy, kPanel.w - 104.0f);
    list.pop_opacity();
}

void Launcher::draw_update(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(open);
    list.push_transform(1.0f - 0.04f * (1.0f - open) * motion(), kCenterX, 540.0f, 0.0f,
                        (1.0f - open) * 30.0f * motion());
    // Shorter while it works, taller with the notes: the content moves by half of the difference,
    // so the panel stays centred.
    const float height = std::clamp(update_height_.value, kWorkingHeight - 20.0f, kNotesHeight + 20.0f);
    const float lowered = (kPanel.h - height) * 0.5f;
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, lowered);
    glass(c, {kPanel.x, kPanel.y, kPanel.w, height}, 28.0f, th().panel.with_alpha(0.97f),
          th().accent.with_alpha(0.38f), 1.8f);
    const float hints = kPanel.y + height - 54.0f;

    if (update_stage_ == UpdateStage::notes)
    {
        draw_notes(c, height);
        list.pop_transform();
        list.pop_transform();
        list.pop_opacity();
        return;
    }

    const float t = update_stage_time_;
    const float breathe = 0.5f + 0.5f * std::sin(c.time * 2.4f);
    const bool failed = update_stage_ == UpdateStage::failed;
    const Color accent = failed ? th().warning : th().accent;

    // A soft glow behind the ring, breathing while it waits.
    list.shadow({kCenterX - kRingRadius, kRingY - kRingRadius, kRingRadius * 2.0f, kRingRadius * 2.0f},
                kRingRadius, 60.0f, accent.with_alpha(0.10f + 0.08f * breathe));
    list.circle(kCenterX, kRingY, kRingRadius - kRingWidth, th().base.with_alpha(0.55f));
    list.ring(kCenterX, kRingY, kRingRadius, kRingWidth, th().panel_edge.with_alpha(0.22f));

    const auto centred = [&](std::string_view value, float top, float line, float size, Color color)
    { text_shrink(c, value, kCenterX, baseline(top, line, size), size, color, kPanel.w - 96.0f, Align::center); };
    const auto buttons = [&](std::initializer_list<const char *> labels)
    {
        const int count = static_cast<int>(labels.size());
        for (int i = 0; i < count; ++i)
            plate_rest(c, row_plate(), button_rect(count, static_cast<float>(i), kButtonsTop));
        plate_focus(c, row_plate(), button_rect(count, update_choice_x_.value, kButtonsTop), 1.0f - 0.25f * press_);
        int i = 0;
        for (const char *label : labels)
        {
            const Rect r = button_rect(count, static_cast<float>(i), kButtonsTop);
            text_shrink(c, tr(label), r.x + r.w * 0.5f, baseline(r.y, r.h, th().size_text), th().size_text,
                        i == update_choice_ ? th().title : th().value, r.w - 32.0f, Align::center);
            ++i;
        }
    };

    switch (update_stage_)
    {
    case UpdateStage::offer:
    {
        // The ring fills once as the dialog rises, around an arrow that settles into its tray.
        const float fill = tween::cubic_out(t / 0.9f);
        arc(list, kCenterX, kRingY, kRingRadius, kRingWidth, -kPi * 0.5f, 2.0f * kPi * fill, accent);
        const float drop = (1.0f - tween::back_out(t / 0.7f)) * -26.0f * motion();
        const float bob = std::sin(c.time * 2.2f) * 3.0f * motion();
        const float ay = kRingY - 4.0f + drop + bob;
        list.line(kCenterX, ay - 30.0f, kCenterX, ay + 16.0f, 6.0f, accent);
        list.line(kCenterX - 18.0f, ay - 2.0f, kCenterX, ay + 16.0f, 6.0f, accent);
        list.line(kCenterX + 18.0f, ay - 2.0f, kCenterX, ay + 16.0f, 6.0f, accent);
        list.line(kCenterX - 30.0f, kRingY + 38.0f, kCenterX + 30.0f, kRingY + 38.0f, 6.0f, accent.with_alpha(0.85f));

        centred(tr("Update available"), 506.0f, 60.0f, th().size_display, th().title);
        centred(fill_text(tr("Version {0} is ready to install."), update_version_), 570.0f, 34.0f, th().size_text,
                th().value);
        if (update_.size > 0)
            centred(fill_text(tr("Download size: {0}"), megabytes(update_.size)), 606.0f, 30.0f, th().size_small,
                    th().accent_pale);
        centred(tr("Your games, saves and settings are kept."), 640.0f, 28.0f, th().size_small, th().copy);
        centred(tr("ProsperoEden closes to finish the update."), 668.0f, 28.0f, th().size_small, th().copy);
        if (update_.notes.empty())
            buttons({TR("Update now"), TR("Skip")});
        else
            buttons({TR("Update now"), TR("What's new"), TR("Skip")});
        static constexpr Hint kOfferHints[] = {
            {Pad::leftright, TR("Navigate")}, {Pad::cross, TR("Select")}, {Pad::circle, TR("Skip")}};
        draw_menu_hints(c, kOfferHints, 3, kPanel.x + 52.0f, hints, th().copy, kPanel.w - 104.0f);
        break;
    }
    case UpdateStage::working:
    case UpdateStage::cancelling:
    {
        const UpdatePhase phase = update_status_.phase;
        const bool measured = phase == UpdatePhase::downloading && update_status_.total > 0 &&
                              update_stage_ == UpdateStage::working;
        const float share = tween::clamp01(update_fraction_.value);
        if (measured)
        {
            arc(list, kCenterX, kRingY, kRingRadius, kRingWidth, -kPi * 0.5f, 2.0f * kPi * share, accent);
            // A bright head at the arc's end.
            const float a = -kPi * 0.5f + 2.0f * kPi * share;
            list.circle(kCenterX + kRingRadius * std::cos(a), kRingY + kRingRadius * std::sin(a), kRingWidth * 0.9f,
                        th().accent_pale.with_alpha(0.9f));
            const int percent = static_cast<int>(share * 100.0f + 0.5f);
            text(c, fill_text(tr("{0}%"), std::to_string(std::min(percent, 100))), kCenterX,
                 baseline(kRingY - 30.0f, 60.0f, th().size_display), th().size_display, th().title, Align::center);
        }
        else
        {
            // Waiting without a share: an arc that turns and breathes, and three dots.
            const float turn = update_spin_ * 4.2f;
            const float sweep = kPi * (0.55f + 0.45f * std::sin(update_spin_ * 2.1f));
            arc(list, kCenterX, kRingY, kRingRadius, kRingWidth, turn, sweep,
                update_stage_ == UpdateStage::cancelling ? th().warning : accent);
            for (int i = 0; i < 3; ++i)
            {
                const float wave = 0.5f + 0.5f * std::sin(update_spin_ * 6.0f - static_cast<float>(i) * 0.9f);
                list.circle(kCenterX - 26.0f + 26.0f * static_cast<float>(i), kRingY - wave * 8.0f * motion(), 7.0f,
                            th().accent_pale.with_alpha(0.35f + 0.6f * wave));
            }
        }

        const char *headline = update_stage_ == UpdateStage::cancelling ? TR("Cancelling")
                               : phase == UpdatePhase::downloading      ? TR("Downloading")
                               : phase == UpdatePhase::unpacking        ? TR("Unpacking")
                                                                        : TR("Preparing");
        centred(tr(headline), 512.0f, 46.0f, th().size_heading, th().title);
        centred(fill_text(tr("Version {0}"), update_version_), 560.0f, 30.0f, th().size_small, th().copy);

        // The bar: the share, or a light running along it while there is none.
        const Rect bar{kPanel.x + 96.0f, 618.0f, kPanel.w - 192.0f, 8.0f};
        list.rounded_rect(bar, 4.0f, th().panel_edge.with_alpha(0.22f));
        if (measured || phase == UpdatePhase::unpacking)
        {
            list.rounded_rect({bar.x, bar.y, std::max(bar.h, bar.w * share), bar.h}, 4.0f, accent);
            // A sheen sliding over the filled part.
            const float sheen = std::fmod(update_spin_ * 0.6f, 1.0f);
            const float sx = bar.x + bar.w * share * sheen;
            list.push_clip({bar.x, bar.y, bar.w * share, bar.h});
            list.rounded_rect({sx - 40.0f, bar.y, 80.0f, bar.h}, 4.0f, th().accent_pale.with_alpha(0.45f));
            list.pop_clip();
        }
        else
        {
            const float run = std::fmod(update_spin_ * 0.8f, 1.4f) - 0.2f;
            list.push_clip(bar);
            list.rounded_rect({bar.x + bar.w * run - 90.0f, bar.y, 180.0f, bar.h}, 4.0f, accent.with_alpha(0.8f));
            list.pop_clip();
        }

        // Bytes, and the time left once the speed is known.
        if (measured)
        {
            std::string line = megabytes(update_status_.done) + "  /  " + megabytes(update_status_.total);
            if (update_rate_ > 1.0f && update_stage_time_ > 1.5f && update_status_.total > update_status_.done)
            {
                const float seconds =
                    static_cast<float>(update_status_.total - update_status_.done) / update_rate_;
                const int whole = std::max(1, static_cast<int>(std::ceil(seconds)));
                line += "  ·  ";
                line += whole < 90 ? fill_text(tr("About {0} s left"), std::to_string(whole))
                                   : fill_text(tr("About {0} min left"), std::to_string((whole + 59) / 60));
            }
            centred(line, 646.0f, 32.0f, th().size_small, th().accent_pale);
        }
        if (update_stage_ == UpdateStage::working)
        {
            static constexpr Hint kWorkingHints[] = {{Pad::circle, TR("Cancel")}};
            draw_menu_hints(c, kWorkingHints, 1, kPanel.x + 52.0f, hints, th().copy, kPanel.w - 104.0f);
        }
        break;
    }
    case UpdateStage::closing:
    {
        // The ring closes, then a tick draws itself and the badge pops.
        arc(list, kCenterX, kRingY, kRingRadius, kRingWidth, -kPi * 0.5f, 2.0f * kPi, accent);
        const float pop = tween::back_out(t / 0.5f);
        list.push_transform(0.6f + 0.4f * pop, kCenterX, kRingY, 0.0f, 0.0f);
        list.circle(kCenterX, kRingY, kRingRadius - kRingWidth - 10.0f, accent.with_alpha(0.16f));
        const float stroke = tween::cubic_out((t - 0.15f) / 0.45f);
        const float x0 = kCenterX - 34.0f, y0 = kRingY + 2.0f;
        const float x1 = kCenterX - 10.0f, y1 = kRingY + 26.0f;
        const float x2 = kCenterX + 38.0f, y2 = kRingY - 26.0f;
        const float first = tween::clamp01(stroke / 0.4f);
        const float second = tween::clamp01((stroke - 0.4f) / 0.6f);
        if (first > 0.0f)
            list.line(x0, y0, x0 + (x1 - x0) * first, y0 + (y1 - y0) * first, 9.0f, accent);
        if (second > 0.0f)
            list.line(x1, y1, x1 + (x2 - x1) * second, y1 + (y2 - y1) * second, 9.0f, accent);
        list.pop_transform();

        centred(tr("Update ready"), 506.0f, 60.0f, th().size_display, th().title);
        centred(tr("ProsperoEden closes now."), 568.0f, 34.0f, th().size_text, th().value);
        centred(fill_text(tr("Open it again to use version {0}."), update_version_), 602.0f, 30.0f, th().size_small,
                th().copy);
        // The time until it closes.
        const float left = 1.0f - tween::clamp01(t / kClosingSeconds);
        list.rounded_rect({kPanel.x + 96.0f, 640.0f, (kPanel.w - 192.0f) * left, 4.0f}, 2.0f, accent.with_alpha(0.7f));
        break;
    }
    case UpdateStage::failed:
    {
        arc(list, kCenterX, kRingY, kRingRadius, kRingWidth, -kPi * 0.5f, 2.0f * kPi * tween::cubic_out(t / 0.6f),
            accent);
        // A gentle shake as it arrives, and an exclamation mark.
        const float shake = std::sin(t * 38.0f) * 10.0f * (1.0f - tween::clamp01(t / 0.45f)) * motion();
        list.line(kCenterX + shake, kRingY - 40.0f, kCenterX + shake, kRingY + 12.0f, 10.0f, accent);
        list.circle(kCenterX + shake, kRingY + 38.0f, 7.0f, accent);

        centred(tr("The update could not finish"), 506.0f, 60.0f, th().size_heading, th().title);
        centred(tr("ProsperoEden was not changed."), 570.0f, 34.0f, th().size_text, th().value);
        if (!update_status_.error.empty())
            centred(update_status_.error, 612.0f, 30.0f, th().size_small, th().copy.with_alpha(0.8f));
        buttons({TR("Try again"), TR("Close")});
        static constexpr Hint kFailedHints[] = {
            {Pad::leftright, TR("Navigate")}, {Pad::cross, TR("Select")}, {Pad::circle, TR("Close")}};
        draw_menu_hints(c, kFailedHints, 3, kPanel.x + 52.0f, hints, th().copy, kPanel.w - 104.0f);
        break;
    }
    case UpdateStage::notes:
        break;
    }
    list.pop_transform();
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
