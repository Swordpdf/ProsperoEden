// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise native parameter construction against the system dialog's password/type rule.
#define PS5_NATIVE
#include "system_keyboard.cpp"
#include <cassert>

static int expected_type;
static bool expected_password;
extern "C" {
int sceSysmoduleLoadModule(std::uint16_t) { return 0; }
int sceCommonDialogInitialize() { return 0; }
int sceUserServiceGetForegroundUser(std::int32_t* user) { *user = 1; return 0; }
int sceImeDialogInit(const ImeParam* param, const void*) {
    assert(param->type == expected_type);
    assert(bool(param->option & 4) == expected_password);
    if ((param->option & 4) && param->type != 1 && param->type != 4)
        return static_cast<int>(0x80bc0030u);
    assert(param->max_text_length == 128);
    assert(param->input_text_buffer[0] == u't');
    return 0;
}
int sceImeDialogGetStatus() { return 2; }
int sceImeDialogGetResult(ImeResult* result) { result->outcome = 0; return 0; }
int sceImeDialogTerm() { return 0; }
int sceImeDialogAbort() { return 0; }
}

int main() {
    std::atomic<bool> stop{};
    for (bool password : {false, true}) {
        for (bool numbers : {false, true}) {
            Eden::TextRequest request;
            request.initial = u"test";
            request.max_length = 128;
            request.password = expected_password = password;
            request.numbers = numbers;
            expected_type = numbers ? 4 : password ? 1 : 0;
            const auto answer = Eden::AskSystemKeyboard(request, stop);
            assert(answer.outcome == Eden::TextOutcome::accepted);
            assert(answer.text == request.initial);
        }
    }
}
