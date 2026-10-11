// SPDX-License-Identifier: GPL-3.0-or-later
#include "system_keyboard.h"

#ifdef PS5_NATIVE
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "diagnostics.h"

namespace {
// The text entry dialog's parameter and result as the system library takes them.
struct ImeParam {
    std::int32_t user_id, type;
    std::uint64_t supported_languages;
    std::int32_t enter_label, input_method;
    void* filter;
    std::uint32_t option, max_text_length;
    std::uint16_t* input_text_buffer;
    float pos_x, pos_y;
    std::int32_t horizontal_alignment, vertical_alignment;
    const std::uint16_t* placeholder;
    const std::uint16_t* title;
    std::int8_t reserved[16];
};
struct ImeResult {
    std::int32_t outcome;
    std::int8_t reserved[12];
};
static_assert(sizeof(ImeParam) == 96);
static_assert(offsetof(ImeParam, input_text_buffer) == 40);
static_assert(offsetof(ImeParam, title) == 72);
static_assert(sizeof(ImeResult) == 16);

constexpr std::uint16_t kImeDialogModule = 0x0096;
constexpr std::uint32_t kCommonDialogAlreadyInitialized = 0x80b80002u;
constexpr std::int32_t kTypeBasicLatin = 1, kTypeNumber = 4;
constexpr std::uint32_t kOptionPassword = 0x4;
constexpr std::int32_t kAlignCenter = 1;
constexpr int kStatusNone = 0, kStatusRunning = 1, kStatusFinished = 2;
constexpr std::size_t kLongestText = 1023;
constexpr std::size_t kUnlimitedText = 500;

std::vector<std::uint16_t> Units(const std::u16string& text, std::size_t limit) {
    std::vector<std::uint16_t> units(limit + 1);
    std::size_t length = std::min(text.size(), limit);
    // Not half of a character outside the basic plane.
    if (length > 0 && text[length - 1] >= 0xd800 && text[length - 1] <= 0xdbff) --length;
    std::copy_n(text.begin(), length, units.begin());
    return units;
}

void Note(const char* what, int code) {
    char line[160];
    std::snprintf(line, sizeof(line), "%s (%#x); the game gets its default text", what, static_cast<unsigned>(code));
    Eden::Report("keyboard", line);
}
}  // namespace

extern "C" {
int sceCommonDialogInitialize();
int sceImeDialogAbort();
int sceImeDialogGetResult(ImeResult* result);
int sceImeDialogGetStatus();
int sceImeDialogInit(const ImeParam* param, const void* extended);
int sceImeDialogTerm();
int sceSysmoduleLoadModule(std::uint16_t module_id);
int sceUserServiceGetForegroundUser(std::int32_t* user_id);
}

namespace Eden {
int PrepareSystemKeyboard() {
    static std::mutex once;
    static int answer = -1;
    static bool asked = false;
    const std::lock_guard lock(once);
    if (!asked || answer < 0) answer = sceSysmoduleLoadModule(kImeDialogModule);
    asked = true;
    return answer;
}

TextAnswer AskSystemKeyboard(const TextRequest& request, const std::atomic<bool>& stop) {
    using namespace std::chrono_literals;
    static std::mutex one_at_a_time;
    static bool loaded = false;
    const std::lock_guard lock(one_at_a_time);
    TextAnswer answer;
    if (stop.load()) {
        answer.outcome = TextOutcome::cancelled;
        return answer;
    }
    if (!loaded) {
        // The keyboard's module (asked for when the app started, PrepareSystemKeyboard): without
        // it its functions are not there to call.
        if (const int module = PrepareSystemKeyboard(); module < 0) {
            Note("The PS5 keyboard's module is not loaded", module);
            return answer;
        }
        // The system's dialogs, then the keyboard.
        if (const int dialogs = sceCommonDialogInitialize();
            dialogs < 0 && static_cast<std::uint32_t>(dialogs) != kCommonDialogAlreadyInitialized) {
            Note("The PS5's dialogs did not start", dialogs);
            return answer;
        }
        loaded = true;
    }
    ImeParam param{};
    if (const int user = sceUserServiceGetForegroundUser(&param.user_id); user < 0) {
        Note("No PS5 user for the keyboard", user);
        return answer;
    }
    const std::size_t limit = std::clamp<std::size_t>(request.max_length ? request.max_length : kUnlimitedText, 1,
                                                      kLongestText);
    std::vector<std::uint16_t> text = Units(request.initial, limit);
    const std::vector<std::uint16_t> title = Units(request.title, 127);
    const std::vector<std::uint16_t> placeholder = Units(request.placeholder, 127);
    // Password masking is invalid with the default multilingual keyboard type.
    param.type = request.numbers ? kTypeNumber : request.password ? kTypeBasicLatin : 0;
    param.option = request.password ? kOptionPassword : 0;
    param.max_text_length = static_cast<std::uint32_t>(limit);
    param.input_text_buffer = text.data();
    param.horizontal_alignment = param.vertical_alignment = kAlignCenter;
    param.placeholder = placeholder.data();
    param.title = title.data();
    if (const int opened = sceImeDialogInit(&param, nullptr); opened != 0) {
        Note("The PS5 keyboard did not open", opened);
        return answer;
    }
    Report("keyboard", "The PS5 keyboard is open");
    const auto since = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(50ms);
        if (stop.load()) {
            (void)sceImeDialogAbort();
            answer.outcome = TextOutcome::cancelled;
            break;
        }
        const int status = sceImeDialogGetStatus();
        // The status is "none" for a moment after the dialog was asked for.
        if (status == kStatusRunning || (status == kStatusNone && std::chrono::steady_clock::now() - since < 1s))
            continue;
        ImeResult result{};
        if (status == kStatusFinished && sceImeDialogGetResult(&result) >= 0) {
            answer.outcome = result.outcome == 0 ? TextOutcome::accepted : TextOutcome::cancelled;
            if (answer.outcome == TextOutcome::accepted)
                answer.text.assign(text.begin(), std::find(text.begin(), text.end(), 0));
        } else {
            Note("The PS5 keyboard closed without an answer", status);
        }
        break;
    }
    // The system owns the dialog until it has let go of it.
    for (int tries = 0; tries < 100 && sceImeDialogTerm() < 0; ++tries) std::this_thread::sleep_for(50ms);
    Report("keyboard", answer.outcome == TextOutcome::accepted ? "The PS5 keyboard closed: text entered" :
                       answer.outcome == TextOutcome::cancelled ? "The PS5 keyboard closed: cancelled" :
                                                                  "The PS5 keyboard closed");
    return answer;
}
}  // namespace Eden

#else

namespace Eden {
int PrepareSystemKeyboard() {
    return -1;
}

TextAnswer AskSystemKeyboard(const TextRequest&, const std::atomic<bool>&) {
    return {};
}
}  // namespace Eden

#endif
