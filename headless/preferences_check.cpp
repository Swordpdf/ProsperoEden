// Host check of the JSON settings store (settings_store.h) and its migration from the earlier
// text files. Build: c++ -std=c++20 -I<nlohmann include> preferences_check.cpp && ./a.out
#include "game_name.h"
#include "preferences.h"
#include <cassert>
#include <fstream>
#include <filesystem>
#include <unistd.h>

static std::string Read(const std::string& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), {}};
}

// A game's name as the Library shows it (game_name.h).
static void CheckGameNames() {
    assert(Eden::UsableName("Sky Shepherds") && Eden::UsableName("\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0"));
    // Not names: nothing, blanks, a question mark, a private-use character, bytes that are not text.
    assert(!Eden::UsableName("") && !Eden::UsableName("  ") && !Eden::UsableName("?") && !Eden::UsableName("\xef\xa3\xbf"));
    assert(!Eden::UsableName("\x83\x51\x81\x5b\x83\x80") && !Eden::UsableName("Ember \xff Knights") &&
           !Eden::UsableName("\xc0\xaf") && !Eden::UsableName("\xed\xa0\x80") && !Eden::UsableName("\xe2\x85"));
    assert(!Eden::UsableName("\xef\xbf\xbd") && !Eden::UsableName("\xe3\x80\x80"));
    // Look-alike characters become the plain letters: a sequel's Roman numeral, full-width Latin.
    assert(Eden::PlainName("EMBER KNIGHTS \xe2\x85\xa1") == "EMBER KNIGHTS II");
    assert(Eden::PlainName("Part \xe2\x85\xab, \xe2\x85\xb3") == "Part XII, iv");
    assert(Eden::PlainName("\xef\xbc\xa1\xef\xbd\x82\xef\xbc\x91\xe3\x80\x80\xef\xbc\x81") == "Ab1 !");
    assert(Eden::PlainName("  Moss & Lantern\xe2\x84\xa2 ") == "Moss & Lantern\xe2\x84\xa2");
    assert(Eden::PlainName("\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0") == "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0");
    assert(Eden::PlainName("\x83\x51") == "\x83\x51");  // not text: left as it is
    std::puts("Game names: text or not, and look-alike characters as plain letters PASS");
}

int main() {
    CheckGameNames();
    char directory[] = "/tmp/eden-settings-XXXXXX";
    assert(mkdtemp(directory));
    const std::string file = std::string(directory) + "/prosperoeden.json";

    // Defaults with no file.
    assert(Eden::LoadPreferences(file).volume == 100);
    assert(Eden::LoadPreferences(file).backend == Eden::GraphicsBackend::Vulkan);
    assert(Eden::LoadPreferences(file).hud);

    // Preferences round trip and validation.
    assert(Eden::SavePreferences({false, 40, true, true, Eden::GraphicsBackend::OpenGL}, file));
    auto saved = Eden::LoadPreferences(file);
    assert(!saved.hud && saved.volume == 40 && saved.mute && saved.detailed_logging &&
           saved.backend == Eden::GraphicsBackend::OpenGL);
    assert(!saved.immediate_logs);
    saved.immediate_logs = true;
    assert(Eden::SavePreferences(saved, file) && Eden::LoadPreferences(file).immediate_logs);
    assert(!Eden::SavePreferences({true, 101, false, false}, file));
    assert(Eden::LoadPreferences(file).volume == 40);
    assert(Read(file).find("\"renderer\": \"opengl\"") != std::string::npos);

    const Eden::Multiplayer::Connection room{"127.0.0.1", 24872, "PlayerOne", "never-store-this-password"};
    assert(Eden::SaveRoomConnection(room, file));
    const auto stored_room = Eden::LoadRoomConnection(file);
    assert(stored_room.host == room.host && stored_room.port == room.port && stored_room.nickname == room.nickname);
    assert(stored_room.password.empty());
    assert(Read(file).find("never-store-this-password") == std::string::npos);
    assert(Eden::LoadPreferences(file).volume == 40);

    // Language: English (US) by default, saved by code, invalid values rejected or ignored.
    assert(saved.language == 0);
    saved.language = 13;
    assert(Eden::SavePreferences(saved, file));
    assert(Eden::LoadPreferences(file).language == 13 && std::string(Eden::kLanguageLabels[13]) == "Japanese");
    assert(Read(file).find("\"language\": \"ja\"") != std::string::npos);
    saved.language = int(std::size(Eden::kLanguageKeys));
    assert(!Eden::SavePreferences(saved, file));
    const std::string unknown_language = std::string(directory) + "/unknown-language.json";
    std::ofstream(unknown_language) << R"({"system": {"language": "xx"}})";
    assert(Eden::LoadPreferences(unknown_language).language == 0);

    // Last and recent games.
    assert(Eden::SaveLastGame("Sample Quest [id].nsp", file));
    assert(Eden::LoadLastGame(file) == "Sample Quest [id].nsp");
    for (const char* invalid : {"../escape.nsp", "game.zip", "a/b.nsp"}) assert(!Eden::SaveLastGame(invalid, file));
    assert(Eden::LoadLastGame(file) == "Sample Quest [id].nsp");
    assert(Eden::SaveRecentGame("Sample Quest.nsp", file));
    assert(Eden::SaveRecentGame("Demo Racer.xci", file));
    assert(Eden::SaveRecentGame("Sample Quest.nsp", file));
    assert((Eden::LoadRecentGames(file) == std::vector<std::string>{"Sample Quest.nsp", "Demo Racer.xci"}));
    for (int i = 0; i < 5; ++i) assert(Eden::SaveRecentGame("Game" + std::to_string(i) + ".nsp", file));
    assert((Eden::LoadRecentGames(file) == std::vector<std::string>{"Game4.nsp", "Game3.nsp", "Game2.nsp", "Game1.nsp"}));

    // Console mode per title.
    constexpr uint64_t racer = 0x0100000000010000, quest = 0x0100000000030000;
    assert(Eden::LoadGameDocked(racer, file) && Eden::LoadGameDocked(quest, file));
    assert(Eden::SaveGameDocked(racer, false, file));
    assert(!Eden::LoadGameDocked(racer, file) && Eden::LoadGameDocked(quest, file));
    assert(!Eden::SaveGameDocked(0, true, file));
    assert(Read(file).find("\"0100000000010000\"") != std::string::npos);

    // Resolution and upscaling filter (Settings > Video).
    auto video = Eden::LoadPreferences(file);
    assert(video.resolution == Eden::kNativeResolution && video.upscaling_filter == 0);
    video.resolution = 1;
    video.upscaling_filter = 1;
    assert(Eden::SavePreferences(video, file));
    video = Eden::LoadPreferences(file);
    assert(video.resolution == 1 && video.upscaling_filter == 1 && video.volume == 40);
    assert(Read(file).find("\"resolution\": \"0.75x\"") != std::string::npos);
    assert(Read(file).find("\"upscaling_filter\": \"fsr\"") != std::string::npos);
    // The largest scale is 4x; nothing beyond the list is saved.
    video.resolution = 6;
    assert(Eden::SavePreferences(video, file) && Eden::LoadPreferences(file).resolution == 6);
    assert(Read(file).find("\"resolution\": \"4x\"") != std::string::npos);
    video.resolution = int(std::size(Eden::kResolutionKeys));
    assert(!Eden::SavePreferences(video, file));
    video.resolution = 1;
    assert(Eden::SavePreferences(video, file));

    // Refresh rate (Settings > Video): 60 Hz unless 120 Hz is chosen; other values are refused or
    // read as 60 Hz.
    video = Eden::LoadPreferences(file);
    assert(video.refresh == 0 && Eden::kRefreshHz[video.refresh] == 60);
    video.refresh = 1;
    assert(Eden::SavePreferences(video, file));
    assert(Eden::LoadPreferences(file).refresh == 1 && Eden::kRefreshHz[1] == 120);
    assert(Read(file).find("\"refresh_rate\": \"120\"") != std::string::npos);
    video.refresh = 2;
    assert(!Eden::SavePreferences(video, file));
    const std::string unknown_refresh = std::string(directory) + "/unknown-refresh.json";
    std::ofstream(unknown_refresh) << R"({"video": {"refresh_rate": "144"}})";
    assert(Eden::LoadPreferences(unknown_refresh).refresh == 0);

    // Output resolution (Settings > Video): 1080p unless another size is chosen; other values are
    // refused or read as 1080p.
    video = Eden::LoadPreferences(file);
    assert(video.output == 0 && Eden::kOutputWidth[video.output] == 1920 && Eden::kOutputHeight[video.output] == 1080);
    video.output = 2;
    assert(Eden::SavePreferences(video, file));
    video = Eden::LoadPreferences(file);
    assert(video.output == 2 && Eden::kOutputWidth[2] == 3840 && Eden::kOutputHeight[2] == 2160 && video.refresh == 1);
    assert(Read(file).find("\"output_resolution\": \"2160p\"") != std::string::npos);
    video.output = 3;
    assert(!Eden::SavePreferences(video, file));
    const std::string unknown_output = std::string(directory) + "/unknown-output.json";
    std::ofstream(unknown_output) << R"({"video": {"output_resolution": "720p"}})";
    assert(Eden::LoadPreferences(unknown_output).output == 0);

    // Frame generation (Settings > Video): off unless chosen, and a switch, so the file holds a
    // boolean. Vulkan only, but the setting itself is stored like any other.
    video = Eden::LoadPreferences(file);
    assert(!video.frame_gen);
    video.frame_gen = true;
    assert(Eden::SavePreferences(video, file) && Eden::LoadPreferences(file).frame_gen);
    assert(Read(file).find("\"frame_gen\": true") != std::string::npos);
    video.frame_gen = false;
    assert(Eden::SavePreferences(video, file) && !Eden::LoadPreferences(file).frame_gen);
    // The target rate and the multiplier are stored as their keys, and "auto" is the default:
    // Eden leaves the ratio in charge unless a target is asked for (settings_store.h).
    assert(video.frame_gen_target == 0 && video.frame_gen_multiplier == 0);
    assert(Read(file).find("\"frame_gen_target\": \"auto\"") != std::string::npos);
    assert(Read(file).find("\"frame_gen_multiplier\": \"2\"") != std::string::npos);
    video.frame_gen_target = 3;      // "120"
    video.frame_gen_multiplier = 2;  // "4"
    assert(Eden::SavePreferences(video, file));
    assert(Eden::LoadPreferences(file).frame_gen_target == 3);
    assert(Eden::LoadPreferences(file).frame_gen_multiplier == 2);
    assert(Read(file).find("\"frame_gen_target\": \"120\"") != std::string::npos);
    assert(Read(file).find("\"frame_gen_multiplier\": \"4\"") != std::string::npos);
    assert(Eden::kFrameGenTargetHz[Eden::LoadPreferences(file).frame_gen_target] == 120);
    assert(Eden::kFrameGenMultiplierValue[Eden::LoadPreferences(file).frame_gen_multiplier] == 4);
    video.frame_gen_target = 0;
    video.frame_gen_multiplier = 0;
    assert(Eden::SavePreferences(video, file));

    // Vibration (Settings > Controls): on unless turned off.
    auto controls = Eden::LoadPreferences(file);
    assert(controls.vibration);
    controls.vibration = false;
    assert(Eden::SavePreferences(controls, file));
    assert(!Eden::LoadPreferences(file).vibration);
    assert(Read(file).find("\"vibration\": false") != std::string::npos);

    // The launcher's look (Settings > Accessibility): all off unless turned on.
    auto look = Eden::LoadPreferences(file);
    assert(!look.large_text && !look.high_contrast && !look.reduce_motion);
    look.large_text = look.reduce_motion = true;
    assert(Eden::SavePreferences(look, file));
    look = Eden::LoadPreferences(file);
    assert(look.large_text && !look.high_contrast && look.reduce_motion && !look.vibration);
    assert(Read(file).find("\"reduce_motion\": true") != std::string::npos);

    // Renderer, resolution and filter per title (Library > Game settings); -1 = Settings default.
    auto game = Eden::LoadGameSettings(racer, file);
    assert(game.renderer == -1 && game.resolution == -1 && game.upscaling_filter == -1);
    assert(Eden::SaveGameSettings(racer, {0, 4, 3}, file));
    game = Eden::LoadGameSettings(racer, file);
    assert(game.renderer == 0 && game.resolution == 4 && game.upscaling_filter == 3);
    assert(!Eden::LoadGameDocked(racer, file));  // the title's console mode stays
    assert(Eden::LoadGameSettings(quest, file).renderer == -1);
    assert(Eden::SaveGameSettings(racer, {-1, 4, -1}, file));
    game = Eden::LoadGameSettings(racer, file);
    assert(game.renderer == -1 && game.resolution == 4 && game.upscaling_filter == -1);
    assert(!Eden::SaveGameSettings(0, {}, file) && !Eden::SaveGameSettings(racer, {2, -1, -1}, file));
    // The same for the refresh rate.
    assert(game.refresh == -1);
    assert(Eden::SaveGameSettings(racer, {-1, 4, -1, 1}, file));
    game = Eden::LoadGameSettings(racer, file);
    assert(game.refresh == 1 && game.resolution == 4 && Eden::LoadGameSettings(quest, file).refresh == -1);
    assert(Eden::SaveGameSettings(racer, {-1, 4, -1, -1}, file) && Eden::LoadGameSettings(racer, file).refresh == -1);
    assert(!Eden::SaveGameSettings(racer, {-1, -1, -1, 2}, file));
    // The same for frame generation (Library > Triangle > Video): a game's own choice, on top of
    // Settings', reaching the session through PreferencesFor. SaveGameSettings replaces the whole
    // record, so this reads it back, changes one field and saves it again, as the launcher does --
    // a partial initializer here would drop the title's other settings.
    assert(Eden::LoadGameSettings(racer, file).frame_gen == -1);
    game = Eden::LoadGameSettings(racer, file);
    game.frame_gen = 1;
    assert(Eden::SaveGameSettings(racer, game, file));
    game = Eden::LoadGameSettings(racer, file);
    assert(game.frame_gen == 1 && game.resolution == 4 &&
           Eden::LoadGameSettings(quest, file).frame_gen == -1);
    assert(Eden::PreferencesFor(racer, file).frame_gen);
    assert(!Eden::PreferencesFor(quest, file).frame_gen);
    game.frame_gen = -1;
    assert(Eden::SaveGameSettings(racer, game, file) &&
           Eden::LoadGameSettings(racer, file).frame_gen == -1);
    game.frame_gen = 2;
    assert(!Eden::SaveGameSettings(racer, game, file));

    // A game's own target rate and multiplier ride along the same path, and an index outside the
    // tables is refused the way the boolean above is.
    assert(Eden::LoadGameSettings(racer, file).frame_gen_target == -1);
    assert(Eden::LoadGameSettings(racer, file).frame_gen_multiplier == -1);
    assert(!Eden::PreferencesFor(racer, file).frame_gen_target &&
           !Eden::PreferencesFor(racer, file).frame_gen_multiplier);
    game = Eden::LoadGameSettings(racer, file);
    game.frame_gen_target = 4;      // "144"
    game.frame_gen_multiplier = 1;  // "3"
    assert(Eden::SaveGameSettings(racer, game, file));
    game = Eden::LoadGameSettings(racer, file);
    assert(game.frame_gen_target == 4 && game.frame_gen_multiplier == 1);
    assert(Read(file).find("\"frame_gen_target\": \"144\"") != std::string::npos);
    assert(Read(file).find("\"frame_gen_multiplier\": \"3\"") != std::string::npos);
    assert(Eden::kFrameGenTargetHz[Eden::PreferencesFor(racer, file).frame_gen_target] == 144);
    assert(Eden::kFrameGenMultiplierValue[Eden::PreferencesFor(racer, file).frame_gen_multiplier] == 3);
    assert(Eden::LoadGameSettings(quest, file).frame_gen_target == -1 &&
           Eden::LoadGameSettings(quest, file).frame_gen_multiplier == -1);
    game.frame_gen_target = -1;
    game.frame_gen_multiplier = -1;
    assert(Eden::SaveGameSettings(racer, game, file) &&
           Eden::LoadGameSettings(racer, file).frame_gen_target == -1 &&
           Eden::LoadGameSettings(racer, file).frame_gen_multiplier == -1);
    game.frame_gen_target = 6;  // one past the last target
    assert(!Eden::SaveGameSettings(racer, game, file));
    game.frame_gen_target = -1;
    game.frame_gen_multiplier = 3;  // one past the last multiplier
    assert(!Eden::SaveGameSettings(racer, game, file));
    game.frame_gen_multiplier = -1;
    assert(Eden::SaveGameSettings(racer, game, file));

    // Performance ("performance", and one per title): the block list and the trade-offs off
    // unless chosen. A title's own values go before the general ones, one value at a time.
    auto speed = Eden::LoadPerformance(racer, file);
    assert(!speed.block_list && !speed.async_shaders && !speed.fast_gpu && !speed.unsafe_cpu && !speed.unsafe_dma);
    assert(Eden::SavePerformance(0, {true, true, false, false, false}, file));  // title 0: general
    speed = Eden::LoadPerformance(racer, file);
    assert(speed.block_list && speed.async_shaders && !speed.fast_gpu);
    assert(Read(file).find("\"async_shaders\": true") != std::string::npos);
    assert(Eden::SavePerformance(racer, {false, false, true, true, true}, file));
    speed = Eden::LoadPerformance(racer, file);
    assert(!speed.block_list && !speed.async_shaders && speed.fast_gpu && speed.unsafe_cpu && speed.unsafe_dma);
    speed = Eden::LoadPerformance(quest, file);  // another title keeps the general values
    assert(speed.block_list && speed.async_shaders && !speed.fast_gpu && !speed.unsafe_cpu && !speed.unsafe_dma);
    assert(Eden::LoadPerformance(0, file).async_shaders);
    assert(Eden::LoadGameSettings(racer, file).resolution == 4);  // the title's other settings stay
    // A file written by hand: a title names one value, the rest are the general ones; a value of
    // the wrong type reads as if it were absent.
    const std::string by_hand = std::string(directory) + "/performance.json";
    std::ofstream(by_hand) << R"({"performance": {"fast_gpu": true, "unsafe_cpu": "yes"},
        "games": {"0100000000010000": {"performance": {"block_list": true}}}})";
    speed = Eden::LoadPerformance(racer, by_hand);
    assert(speed.block_list && speed.fast_gpu && !speed.unsafe_cpu && !speed.async_shaders);
    assert(!Eden::LoadPerformance(quest, by_hand).block_list);
    // Reactive flushing is on and skipping the CPU's invalidation off unless chosen; both are
    // saved with the rest and a title can have its own.
    speed = Eden::LoadPerformance(quest, by_hand);
    assert(speed.reactive_flushing && !speed.skip_invalidation);
    speed.reactive_flushing = false;
    speed.skip_invalidation = true;
    assert(Eden::SavePerformance(0, speed, by_hand));
    speed = Eden::LoadPerformance(quest, by_hand);
    assert(!speed.reactive_flushing && speed.skip_invalidation && speed.fast_gpu);
    assert(Read(by_hand).find("\"reactive_flushing\": false") != std::string::npos);
    assert(Read(by_hand).find("\"skip_invalidation\": true") != std::string::npos);
    std::ofstream(by_hand) << R"({"performance": {"skip_invalidation": true},
        "games": {"0100000000010000": {"performance": {"skip_invalidation": false, "reactive_flushing": false}}}})";
    speed = Eden::LoadPerformance(racer, by_hand);
    assert(!speed.skip_invalidation && !speed.reactive_flushing);
    assert(Eden::LoadPerformance(quest, by_hand).skip_invalidation && Eden::LoadPerformance(quest, by_hand).reactive_flushing);

    // Button mapping (button_mapping.h): the usual one, a changed one, and one the file names
    // badly. Choosing a button another game button has swaps the two.
    {
        auto prefs = Eden::LoadPreferences(file);
        assert(prefs.mapping == Eden::kDefaultMapping);
        prefs.mapping = Eden::Assign(prefs.mapping, Eden::game_a, Eden::pad_cross);
        assert(prefs.mapping[Eden::game_a] == Eden::pad_cross && prefs.mapping[Eden::game_b] == Eden::pad_circle);
        assert(Eden::ValidMapping(prefs.mapping) && Eden::Assign(prefs.mapping, Eden::game_a, Eden::pad_cross) == prefs.mapping);
        assert(Eden::SavePreferences(prefs, file));
        assert(Eden::LoadPreferences(file).mapping == prefs.mapping);
        assert(Read(file).find("\"mapping\": {") != std::string::npos &&
               Read(file).find("\"a\": \"cross\"") != std::string::npos &&
               Read(file).find("\"x\":") == std::string::npos);  // only what differs is written
        auto twice = prefs;
        twice.mapping[Eden::game_x] = twice.mapping[Eden::game_y];
        assert(!Eden::ValidMapping(twice.mapping) && !Eden::SavePreferences(twice, file));
        prefs.mapping = Eden::kDefaultMapping;
        assert(Eden::SavePreferences(prefs, file) && Read(file).find("\"mapping\"") == std::string::npos);
        const std::string by_hand = std::string(directory) + "/mapping.json";
        std::ofstream(by_hand) << R"({"controls": {"mapping": {"a": "cross", "b": "cross"}}})";
        assert(Eden::LoadPreferences(by_hand).mapping == Eden::kDefaultMapping);  // Cross twice
        std::ofstream(by_hand) << R"({"controls": {"mapping": {"a": "cross", "b": "circle", "zl": "touchpad", "minus": "l2"}}})";
        const auto read = Eden::LoadPreferences(by_hand).mapping;
        assert(read[Eden::game_a] == Eden::pad_cross && read[Eden::game_zl] == Eden::pad_touchpad &&
               read[Eden::game_minus] == Eden::pad_l2 && read[Eden::game_x] == Eden::pad_triangle);
        std::ofstream(by_hand) << R"({"controls": {"mapping": {"a": "share"}}})";
        assert(Eden::LoadPreferences(by_hand).mapping == Eden::kDefaultMapping);  // no such button
    }

    // A game's own settings over Settings: video, audio, controls, language and Performance,
    // each one on its own, and back to Settings.
    {
        Eden::GameSettings game = Eden::LoadGameSettings(quest, file);
        game.hud = 0;
        game.volume = 40;
        game.mute = 1;
        game.vibration = 0;
        game.language = 13;
        game.controller = 3;  // left Joy-Con
        game.own_mapping = true;
        game.mapping = Eden::Assign(Eden::kDefaultMapping, Eden::game_a, Eden::pad_triangle);
        game.performance[2] = 1;  // fast GPU
        assert(Eden::SaveGameSettings(quest, game, file));
        const auto back = Eden::LoadGameSettings(quest, file);
        assert(back.controller == 3 && Eden::PreferencesFor(quest, file).controller == 3 &&
               Eden::PreferencesFor(racer, file).controller == -1 && Eden::LoadPreferences(file).controller == -1);
        assert(back.hud == 0 && back.volume == 40 && back.mute == 1 && back.vibration == 0 && back.language == 13 &&
               back.own_mapping && back.mapping == game.mapping && back.performance[2] == 1 &&
               back.performance[0] == -1);
        const auto general = Eden::LoadPreferences(file);
        const auto mine = Eden::PreferencesFor(quest, file);
        assert(!mine.hud && mine.volume == 40 && mine.mute && !mine.vibration && mine.language == 13 &&
               mine.mapping == game.mapping && mine.resolution == general.resolution);
        assert(Eden::LoadPerformance(quest, file).fast_gpu);
        const auto theirs = Eden::PreferencesFor(racer, file);
        assert(theirs.volume == general.volume && theirs.mapping == general.mapping && theirs.language == general.language);
        assert(Eden::PreferencesFor(0, file).volume == general.volume);
        auto wrong = game;
        wrong.volume = 101;
        assert(!Eden::SaveGameSettings(quest, wrong, file));
        wrong = game;
        wrong.mute = 2;
        assert(!Eden::SaveGameSettings(quest, wrong, file));
        wrong = game;
        wrong.mapping[Eden::game_b] = wrong.mapping[Eden::game_a];
        assert(!Eden::SaveGameSettings(quest, wrong, file));
        assert(Eden::SaveGameSettings(quest, Eden::GameSettings{}, file));
        const auto plain = Eden::LoadGameSettings(quest, file);
        assert(plain.hud < 0 && plain.volume < 0 && plain.language < 0 && !plain.own_mapping &&
               plain.performance[2] < 0 && Eden::PreferencesFor(quest, file).volume == general.volume);
    }

    // Game files folder.
    assert(Eden::LoadSavedAssetsDir(file).empty());
    assert(Eden::SaveAssetsDir("/mnt/ext1/eden", file));
    assert(Eden::LoadSavedAssetsDir(file) == "/mnt/ext1/eden");
    assert(!Eden::SaveAssetsDir("relative/path", file) && !Eden::SaveAssetsDir("/a/../b", file));

    // Everything saved so far survives in one document.
    assert(Eden::LoadPreferences(file).volume == 40 && Eden::LoadLastGame(file) == "Sample Quest [id].nsp");

    // A damaged file reads as defaults and is replaced by the next save.
    { std::ofstream out(file); out << "{ not json"; }
    assert(Eden::LoadPreferences(file).volume == 100 && Eden::LoadLastGame(file).empty());
    assert(Eden::SaveLastGame("Game.XCI", file) && Eden::LoadLastGame(file) == "Game.XCI");

    // Migration from the text files of earlier versions, once, leaving them in place.
    char legacy[] = "/tmp/eden-legacy-XXXXXX";
    assert(mkdtemp(legacy));
    const std::string folder = legacy, migrated = folder + "/prosperoeden.json";
    { std::ofstream(folder + "/settings.txt") << "2 0 70 0 1 0\n"; }
    { std::ofstream(folder + "/last-game.txt") << "Demo Racer [0100000000010000].nsp"; }
    { std::ofstream(folder + "/recent-games.txt") << "Demo Racer [0100000000010000].nsp\nTest Platformer.nsp\n"; }
    { std::ofstream(folder + "/assets-dir.txt") << "/mnt/ext1/eden\n"; }
    { std::ofstream(folder + "/game-0100000000030000-mode.txt") << "1 handheld\n"; }
    const auto old = Eden::LoadPreferences(migrated);
    assert(!old.hud && old.volume == 70 && !old.mute && old.detailed_logging &&
           old.backend == Eden::GraphicsBackend::OpenGL);
    assert(std::filesystem::exists(migrated));
    assert(Eden::LoadLastGame(migrated) == "Demo Racer [0100000000010000].nsp");
    assert((Eden::LoadRecentGames(migrated) == std::vector<std::string>{"Demo Racer [0100000000010000].nsp", "Test Platformer.nsp"}));
    assert(Eden::LoadSavedAssetsDir(migrated) == "/mnt/ext1/eden");
    assert(!Eden::LoadGameDocked(quest, migrated) && Eden::LoadGameDocked(racer, migrated));
    assert(std::filesystem::exists(folder + "/settings.txt"));

    std::filesystem::remove_all(directory);
    std::filesystem::remove_all(legacy);
    std::puts("settings store: all checks passed");
}
