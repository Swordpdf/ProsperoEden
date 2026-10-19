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

    // Appearance survives reopening and an unrelated settings write.
    {
        Eden::Preferences value;
        value.theme = "slate"; value.accent = "ice";
        assert(Eden::SavePreferences(value, file));
        auto reopened = Eden::LoadPreferences(file);
        assert(reopened.theme == "slate" && reopened.accent == "ice");
        reopened.volume = 75;
        assert(Eden::SavePreferences(reopened, file));
        assert(Eden::LoadPreferences(file).accent == "ice");
    }

    // A wallpaper (docs/WALLPAPERS.md) survives reopening; a name that could leave the folder
    // is neither read nor saved.
    {
        Eden::Preferences value;
        value.wallpaper = "Sky Pond.png";
        assert(Eden::SavePreferences(value, file));
        assert(Eden::LoadPreferences(file).wallpaper == "Sky Pond.png");
        const std::string by_name = std::string(directory) + "/wallpaper.json";
        std::ofstream(by_name) << R"({"launcher": {"wallpaper": "beach.png", "wallpaper_blur": true}})";
        // A "wallpaper_blur" an older build wrote reads as nothing and leaves on the next save.
        assert(Eden::LoadPreferences(by_name).wallpaper == "beach.png");
        assert(Eden::SavePreferences(Eden::LoadPreferences(by_name), by_name));
        assert(Read(by_name).find("wallpaper_blur") == std::string::npos);
        std::ofstream(by_name) << R"({"launcher": {"wallpaper": "../secrets.png"}})";
        assert(Eden::LoadPreferences(by_name).wallpaper.empty());
        Eden::Preferences named;
        named.wallpaper = "a/b.png"; // the folder is not part of what is saved
        assert(!Eden::SavePreferences(named, by_name));
    }

    // Preferences round trip and validation.
    assert(Eden::SavePreferences({false, 40, true, true, Eden::GraphicsBackend::OpenGL}, file));
    auto saved = Eden::LoadPreferences(file);
    assert(!saved.hud && saved.volume == 40 && saved.mute && saved.detailed_logging &&
           saved.backend == Eden::GraphicsBackend::OpenGL);
    assert(!saved.immediate_logs);
    saved.immediate_logs = true;
    assert(Eden::SavePreferences(saved, file) && Eden::LoadPreferences(file).immediate_logs);
    // Update notices (Settings > Diagnostics): on until switched off, and kept under "system".
    assert(saved.update_notices);
    saved.update_notices = false;
    assert(Eden::SavePreferences(saved, file) && !Eden::LoadPreferences(file).update_notices);
    assert(Read(file).find("\"update_notices\": false") != std::string::npos);
    saved.update_notices = true;
    assert(Eden::SavePreferences(saved, file) && Eden::LoadPreferences(file).update_notices);
    assert(!Eden::SavePreferences({true, 101, false, false}, file));
    assert(Eden::LoadPreferences(file).volume == 40);
    assert(Read(file).find("\"renderer\": \"opengl\"") != std::string::npos);

    Eden::Multiplayer::Connection room{"127.0.0.1", 24872, "PlayerOne", "saved-room-password"};
    assert(Eden::SaveRoomConnection(room, file));
    const auto stored_room = Eden::LoadRoomConnection(file);
    assert(stored_room.host == room.host && stored_room.port == room.port && stored_room.nickname == room.nickname);
    assert(stored_room.password == room.password);
    room.password.assign(129, 'x');
    assert(!Eden::SaveRoomConnection(room, file));
    assert(Eden::LoadRoomConnection(file).password == stored_room.password);
    room.password.clear();
    assert(Eden::SaveRoomConnection(room, file));
    assert(Eden::LoadRoomConnection(file).password.empty());
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
    for (const char* invalid : {"../escape.nsp", "game.zip", "a/b/c.nsp", "/game.nsp", ".hidden/game.nsp", "a/",
                                "a/.nsp", "a\b.nsp", "Game/game.zip"})
        assert(!Eden::SaveLastGame(invalid, file));
    assert(Eden::LoadLastGame(file) == "Sample Quest [id].nsp");
    // A game in its own folder in roms/ (issue 168), and back to one directly in it.
    assert(Eden::SaveLastGame("Sample Quest/Sample Quest [id].nsp", file));
    assert(Eden::LoadLastGame(file) == "Sample Quest/Sample Quest [id].nsp");
    assert(Eden::SaveLastGame("Sample Quest [id].nsp", file));
    assert(Eden::SaveRecentGame("Sample Quest.nsp", file));
    assert(Eden::SaveRecentGame("Demo Racer.xci", file));
    assert(Eden::SaveRecentGame("Sample Quest.nsp", file));
    assert((Eden::LoadRecentGames(file) == std::vector<std::string>{"Sample Quest.nsp", "Demo Racer.xci"}));
    for (int i = 0; i < 5; ++i) assert(Eden::SaveRecentGame("Game" + std::to_string(i) + ".nsp", file));
    assert((Eden::LoadRecentGames(file) == std::vector<std::string>{"Game4.nsp", "Game3.nsp", "Game2.nsp", "Game1.nsp",
                                                                    "Game0.nsp", "Sample Quest.nsp", "Demo Racer.xci"}));

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
    // ASTC textures: compressed unless the file says full; nothing beyond the list is saved.
    assert(video.astc_textures == 0);
    assert(Read(file).find("\"astc_textures\": \"compressed\"") != std::string::npos);
    video.astc_textures = 1;
    assert(Eden::SavePreferences(video, file));
    assert(Eden::LoadPreferences(file).astc_textures == 1);
    assert(Read(file).find("\"astc_textures\": \"full\"") != std::string::npos);
    video.astc_textures = 2;
    assert(!Eden::SavePreferences(video, file));
    video.astc_textures = 1;
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
    // The frame generation method defaults to LSFG (index 0) and round-trips as its key.
    assert(video.frame_gen_method == 0);
    assert(Read(file).find("\"frame_gen_method\": \"lsfg\"") != std::string::npos);
    video.frame_gen_method = 1;  // "mihawkgen"
    assert(Eden::SavePreferences(video, file));
    assert(Eden::LoadPreferences(file).frame_gen_method == 1);
    assert(Read(file).find("\"frame_gen_method\": \"mihawkgen\"") != std::string::npos);
    video.frame_gen_method = 0;
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
    // The same for the ASTC textures: a game's own choice wins over Settings (full, saved above).
    assert(game.astc_textures == -1 && Eden::PreferencesFor(racer, file).astc_textures == 1);
    game.astc_textures = 0;
    assert(Eden::SaveGameSettings(racer, game, file));
    assert(Eden::LoadGameSettings(racer, file).astc_textures == 0);
    assert(Eden::PreferencesFor(racer, file).astc_textures == 0);
    assert(Eden::PreferencesFor(quest, file).astc_textures == 1);
    game.astc_textures = 2;
    assert(!Eden::SaveGameSettings(racer, game, file));
    game.astc_textures = -1;
    assert(Eden::SaveGameSettings(racer, game, file));
    assert(Read(file).find("\"astc_textures\": \"compressed\"") == std::string::npos);
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

    // A game's own frame gen method: -1 follows Settings, 0 is LSFG, 1 is MihawkGen, 2+ refused.
    assert(Eden::LoadGameSettings(racer, file).frame_gen_method == -1);
    game = Eden::LoadGameSettings(racer, file);
    game.frame_gen_method = 1;  // "mihawkgen"
    assert(Eden::SaveGameSettings(racer, game, file));
    assert(Eden::LoadGameSettings(racer, file).frame_gen_method == 1);
    assert(Eden::PreferencesFor(racer, file).frame_gen_method == 1);
    game.frame_gen_method = -1;
    assert(Eden::SaveGameSettings(racer, game, file));
    game.frame_gen_method = 2;  // one past the last method
    assert(!Eden::SaveGameSettings(racer, game, file));
    game.frame_gen_method = -1;

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
    // Console memory ("performance/memory_layout", settings_store.h): the Switch's own 4 GiB unless
    // named, a title's own value before the general one, and a name that is not one of the three
    // reads as if it were absent. The launcher's game settings write the switches by name, so a
    // layout saved here stays with the title.
    assert(Eden::LoadPerformance(racer, by_hand).memory_layout == 0);
    std::ofstream(by_hand) << R"({"performance": {"memory_layout": "6Gb"},
        "games": {"0100000000010000": {"performance": {"memory_layout": "8Gb"}}}})";
    assert(Eden::LoadPerformance(racer, by_hand).memory_layout == 2);
    assert(Eden::LoadPerformance(quest, by_hand).memory_layout == 1);
    std::ofstream(by_hand) << R"({"performance": {"memory_layout": "16Gb"},
        "games": {"0100000000010000": {"performance": {"memory_layout": 6}}}})";
    assert(Eden::LoadPerformance(racer, by_hand).memory_layout == 0 &&
           Eden::LoadPerformance(quest, by_hand).memory_layout == 0);
    speed = Eden::LoadPerformance(quest, by_hand);
    speed.memory_layout = 1;
    assert(Eden::SavePerformance(0, speed, by_hand));
    assert(Read(by_hand).find("\"memory_layout\": \"6Gb\"") != std::string::npos);
    assert(Eden::LoadPerformance(0, by_hand).memory_layout == 1);
    // A layout out of the list is refused rather than written.
    speed.memory_layout = int(std::size(Eden::kMemoryLayoutKeys));
    assert(!Eden::SavePerformance(0, speed, by_hand));
    {
        const std::string layout_file = std::string(directory) + "/layout.json";
        std::ofstream(layout_file) << R"({"games": {"0100000000010000": {"performance": {"memory_layout": "6Gb"}}}})";
        assert(Eden::LoadPerformance(racer, layout_file).memory_layout == 1);
        auto saved_game = Eden::LoadGameSettings(racer, layout_file);
        saved_game.performance[2] = 1;  // Settings > Performance > fast GPU, the launcher's write
        assert(Eden::SaveGameSettings(racer, saved_game, layout_file));
        assert(Eden::LoadPerformance(racer, layout_file).memory_layout == 1);
    }
    // Speed limit ("performance/speed_limit"): 100% without a file, a game's own before the general
    // one, a value that is not one of the steps read as 100%, and both kinds of write keep it.
    {
        const std::string speed_file = std::string(directory) + "/speed.json";
        assert(Eden::LoadPerformance(racer, speed_file).speed_limit == 0);
        std::ofstream(speed_file) << R"({"performance": {"speed_limit": "200"},
            "games": {"0100000000010000": {"performance": {"speed_limit": "unlimited"}}}})";
        assert(Eden::LoadPerformance(racer, speed_file).speed_limit == 4);
        assert(Eden::LoadPerformance(quest, speed_file).speed_limit == 2);
        assert(Eden::LoadGameSettings(racer, speed_file).speed_limit == 4);
        assert(Eden::LoadGameSettings(quest, speed_file).speed_limit == -1);
        std::ofstream(speed_file) << R"({"performance": {"speed_limit": 200}})";
        assert(Eden::LoadPerformance(quest, speed_file).speed_limit == 0);
        auto general = Eden::LoadPerformance(0, speed_file);
        general.speed_limit = 3;
        assert(Eden::SavePerformance(0, general, speed_file));
        assert(Read(speed_file).find("\"speed_limit\": \"300\"") != std::string::npos);
        general.speed_limit = int(std::size(Eden::kSpeedLimitKeys));
        assert(!Eden::SavePerformance(0, general, speed_file));
        auto own = Eden::LoadGameSettings(racer, speed_file);
        own.speed_limit = 1;
        assert(Eden::SaveGameSettings(racer, own, speed_file));
        assert(Eden::LoadPerformance(racer, speed_file).speed_limit == 1 &&
               Eden::LoadPerformance(quest, speed_file).speed_limit == 3);
        own.speed_limit = -1;  // back to Settings
        assert(Eden::SaveGameSettings(racer, own, speed_file));
        assert(Eden::LoadPerformance(racer, speed_file).speed_limit == 3);
        own.speed_limit = int(std::size(Eden::kSpeedLimitKeys));
        assert(!Eden::SaveGameSettings(racer, own, speed_file));
    }

    // Guest clock adjustment is off unless explicitly chosen for this title. Launcher saves
    // preserve the advanced option without applying it to another game or accepting bad types.
    {
        const std::string clock_file = std::string(directory) + "/clock.json";
        assert(!Eden::LoadAdaptiveClock(racer, clock_file));
        std::ofstream(clock_file) << R"({"adaptive_clock": true,
            "games": {"0100000000010000": {"adaptive_clock": true},
                      "0100000000020000": {"adaptive_clock": "true"}}})";
        assert(Eden::LoadAdaptiveClock(racer, clock_file));
        assert(!Eden::LoadAdaptiveClock(quest, clock_file));
        assert(!Eden::LoadAdaptiveClock(0, clock_file));
        assert(Eden::SavePreferences(Eden::LoadPreferences(clock_file), clock_file));
        assert(Eden::SavePerformance(racer, Eden::LoadPerformance(racer, clock_file), clock_file));
        assert(Eden::SaveGameSettings(racer, Eden::LoadGameSettings(racer, clock_file), clock_file));
        assert(Eden::LoadAdaptiveClock(racer, clock_file));
        assert(!Eden::LoadAdaptiveClock(quest, clock_file));
        std::ofstream(clock_file) << R"({"games": {"0100000000010000": {"adaptive_clock": false}}})";
        assert(!Eden::LoadAdaptiveClock(racer, clock_file));
    }

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

    // Post-processing (settings_store.h): a preset by name for every game, a game's own over it
    // ("" is off for that game), and back to Settings; a name with a path in it is refused.
    {
        auto prefs = Eden::LoadPreferences(file);
        assert(prefs.post_processing.empty());
        prefs.post_processing = "Cinematic";
        assert(Eden::SavePreferences(prefs, file) && Eden::LoadPreferences(file).post_processing == "Cinematic");
        constexpr uint64_t pp_game = 0x0100000000077000;
        auto game = Eden::LoadGameSettings(pp_game, file);
        assert(!game.post_processing && Eden::PreferencesFor(pp_game, file).post_processing == "Cinematic");
        game.post_processing = "";
        assert(Eden::SaveGameSettings(pp_game, game, file));
        assert(Eden::LoadGameSettings(pp_game, file).post_processing == std::string{});
        assert(Eden::PreferencesFor(pp_game, file).post_processing.empty());
        game.post_processing = "My look";
        assert(Eden::SaveGameSettings(pp_game, game, file) && Eden::PreferencesFor(pp_game, file).post_processing == "My look");
        game.post_processing.reset();
        assert(Eden::SaveGameSettings(pp_game, game, file) && !Eden::LoadGameSettings(pp_game, file).post_processing);
        assert(Eden::PreferencesFor(pp_game, file).post_processing == "Cinematic");
        prefs.post_processing = "../escape";
        assert(!Eden::SavePreferences(prefs, file));
        prefs.post_processing = "";
        assert(Eden::SavePreferences(prefs, file) && Read(file).find("post_processing") == std::string::npos);
    }

    // Menu keybinds (button_mapping.h): the face button that presses each core menu action. A
    // binding names each of the four face buttons once; one that names one twice, or a button that
    // is not a face button, is refused or read as the default.
    {
        auto prefs = Eden::LoadPreferences(file);
        assert(prefs.menu_bind == Eden::kDefaultMenuBinding);
        // Swap Select and Back: Cross presses Back, Circle presses Select.
        prefs.menu_bind[Eden::menu_select] = Eden::pad_circle;
        prefs.menu_bind[Eden::menu_back] = Eden::pad_cross;
        assert(Eden::ValidMenuBinding(prefs.menu_bind));
        assert(Eden::SavePreferences(prefs, file));
        const auto read = Eden::LoadPreferences(file).menu_bind;
        assert(read == prefs.menu_bind && read[Eden::menu_select] == Eden::pad_circle &&
               read[Eden::menu_back] == Eden::pad_cross && read[Eden::menu_secondary] == Eden::pad_square);
        assert(Read(file).find("\"menu_mapping\": {") != std::string::npos &&
               Read(file).find("\"select\": \"circle\"") != std::string::npos &&
               Read(file).find("\"back\": \"cross\"") != std::string::npos &&
               Read(file).find("\"secondary\":") == std::string::npos);  // only what differs is written
        // Two actions on one face button is refused.
        auto twice = prefs;
        twice.menu_bind[Eden::menu_secondary] = twice.menu_bind[Eden::menu_select];
        assert(!Eden::ValidMenuBinding(twice.menu_bind) && !Eden::SavePreferences(twice, file));
        // Back to the default: nothing is written.
        prefs.menu_bind = Eden::kDefaultMenuBinding;
        assert(Eden::SavePreferences(prefs, file) && Read(file).find("\"menu_mapping\"") == std::string::npos);
        // A file written by hand: a valid swap, a button that is not a face button, one named twice.
        const std::string by_hand = std::string(directory) + "/menu-mapping.json";
        std::ofstream(by_hand) << R"({"controls": {"menu_mapping": {"select": "circle", "back": "cross"}}})";
        const auto ok = Eden::LoadPreferences(by_hand).menu_bind;
        assert(ok[Eden::menu_select] == Eden::pad_circle && ok[Eden::menu_back] == Eden::pad_cross &&
               ok[Eden::menu_secondary] == Eden::pad_square && ok[Eden::menu_settings] == Eden::pad_triangle);
        std::ofstream(by_hand) << R"({"controls": {"menu_mapping": {"select": "l1"}}})";
        assert(Eden::LoadPreferences(by_hand).menu_bind == Eden::kDefaultMenuBinding);  // not a face button
        std::ofstream(by_hand) << R"({"controls": {"menu_mapping": {"select": "circle", "back": "circle"}}})";
        assert(Eden::LoadPreferences(by_hand).menu_bind == Eden::kDefaultMenuBinding);  // Circle twice
    }

    // Settings > Theme > Colour: the accent preset within a theme (settings_store.h, THEMES.md).
    // "" is the theme's own colours and nothing is written for it; a preset id is kept as chosen;
    // an id written by hand is read only when it is lowercase letters, digits or '-' and short.
    {
        auto prefs = Eden::LoadPreferences(file);
        assert(prefs.accent.empty());
        prefs.accent = "ice";
        assert(Eden::SavePreferences(prefs, file));
        assert(Eden::LoadPreferences(file).accent == "ice");
        assert(Read(file).find("\"accent\": \"ice\"") != std::string::npos);
        // The theme's own colours: nothing is written.
        prefs.accent.clear();
        assert(Eden::SavePreferences(prefs, file) && Read(file).find("\"accent\"") == std::string::npos);
        // A file written by hand: a preset id, an id that is not lowercase, one with other characters.
        const std::string by_hand = std::string(directory) + "/accent.json";
        std::ofstream(by_hand) << R"({"launcher": {"theme": "slate", "accent": "ember"}})";
        assert(Eden::LoadPreferences(by_hand).accent == "ember");
        std::ofstream(by_hand) << R"({"launcher": {"accent": "ICE"}})";
        assert(Eden::LoadPreferences(by_hand).accent.empty());  // not lowercase
        std::ofstream(by_hand) << R"({"launcher": {"accent": "../../secret"}})";
        assert(Eden::LoadPreferences(by_hand).accent.empty());  // not an id's shape
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
               back.performance[0] == -1 && back.rng_seed == -1);
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
        wrong.rng_seed = 0xffffffffLL + 1;
        assert(!Eden::SaveGameSettings(quest, wrong, file));
        game.rng_seed = 0;
        assert(Eden::SaveGameSettings(quest, game, file));
        assert(Eden::LoadGameSettings(quest, file).rng_seed == 0);
        game.rng_seed = -1;
        // A typed seed: digits only, within 32 bits.
        assert(Eden::RngSeedFromText("0") == 0 && Eden::RngSeedFromText("1234") == 1234 &&
               Eden::RngSeedFromText("007") == 7 && Eden::RngSeedFromText("4294967295") == 0xffffffffLL);
        assert(Eden::RngSeedFromText("") == -1 && Eden::RngSeedFromText("4294967296") == -1 &&
               Eden::RngSeedFromText("99999999999") == -1 && Eden::RngSeedFromText("-1") == -1 &&
               Eden::RngSeedFromText("+5") == -1 && Eden::RngSeedFromText("12a") == -1 &&
               Eden::RngSeedFromText(" 12") == -1 && Eden::RngSeedFromText("1.5") == -1);
        // Every seed a text can name is one the settings take.
        game.rng_seed = Eden::RngSeedFromText("4294967295");
        assert(Eden::SaveGameSettings(quest, game, file) &&
               Eden::LoadGameSettings(quest, file).rng_seed == 0xffffffffLL);
        game.rng_seed = -1;
        assert(Eden::SaveGameSettings(quest, game, file) && Eden::LoadGameSettings(quest, file).rng_seed == -1);
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
