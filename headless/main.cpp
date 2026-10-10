#ifdef EDEN_DEV_ROM_ID
#include "development_input.h"
#endif
#include <utility>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <algorithm>
#include <exception>
#include "assets_dir.h"
#include "forwarded_launch.h"
#include "gpu_failure.h"
#include "guest_fault.h"
#include "jit_list.h"
#include "boot_trace.h"
#ifdef PS5_NATIVE
#include "elevation/elevation.hpp"
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#endif
#ifdef EDEN_DEV_VULKAN
#include "sdk_audit.h"
#endif
#include <filesystem>
#include <unistd.h>
#include <condition_variable>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <new>
#include <string_view>
#include <stdexcept>
#include <fstream>
#include <nlohmann/json.hpp>
#include "devices.h"
#include "diagnostics.h"
#include "hud.h"
#include "display_refresh.h"
#include "log_flusher.h"
#include "mods.h"
#include "controller_applet.h"
#include "error_applet.h"
#include "keyboard_applet.h"
#include "preferences.h"
#include "metadata_bridge.h"
#ifdef EDEN_PS5_OPENGL
#include "graphics.h"
#include "video_core/renderer_base.h"
#include "video_core/rasterizer_interface.h"
#endif
#ifdef PS5_NATIVE
#include "native_directory.h"
#include "cache_budget.h"
#include "performance.h"
#include "stall_watchdog.h"
#include "dev_vulkan.h"
#include "../src/fastmem.h"
#include "crash_report.h"
#include "prosperoeden/frontend.h"
#include "prosperoeden/version.h"
extern "C" void ps5_opengl_heap_snapshot(const char*, unsigned);
extern "C" std::int64_t sceKernelGetDirectMemorySize();
#else
#include <malloc.h>
#include "mock_devices.h"
#endif
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/file_sys/registered_cache.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/set/settings_types.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
// The game's Joy-Con hold type (UpdateGrips) is asked of the HID service; its headers bring kernel
// ones that do not pass this file's warnings.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-private-field"
#include "core/hle/service/hid/hid_server.h"
#include "core/hle/service/sm/sm.h"
#include "hid_core/resource_manager.h"
#include "hid_core/resources/applet_resource.h"
#include "hid_core/resources/npad/npad.h"
#pragma GCC diagnostic pop
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-private-field"
#include "core/hle/kernel/k_process.h"  // every build: the game's code address (jit_list.h)
#ifdef PS5_NATIVE
#include "core/hle/kernel/k_thread.h"
#endif
#pragma clang diagnostic pop
#ifdef EDEN_DEV_PROFILE
#include "crash_trigger.h"
#include "watch.h"
#include "core/arm/debug.h"
#include "core/memory.h"
extern "C" bool eden_jit_shared;  // headless/dynarmic/jit_group_support.inc
#endif
#include "video_core/gpu.h"
#ifdef EDEN_PS5_FRAMEGEN
#include "video_core/frame_gen/lossless_dll.h"
#endif
#include "profiles.h"
#include "stop_limit.h"
namespace Common {
bool SparseTablesAvailable() noexcept; // src/memory_pages.cpp
}
#if defined(EDEN_DEV_PROFILE) || defined(EDEN_DEV_ROM_ID)
static bool dev_stop_hang = false; // dev-settings stop_hang=on
static bool dev_load_hang = false; // dev-settings load_hang=on
#endif

class HeadlessWindow final : public Core::Frontend::EmuWindow {
public:
    HeadlessWindow() { UpdateCurrentFramebufferLayout(1280, 720); }
    ~HeadlessWindow() override = default;
    bool IsShown() const override { return false; }
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<Core::Frontend::GraphicsContext>();
    }
};

#ifdef PS5_NATIVE
// First start with filesystem access: copy what the sandbox kept (settings, covers, Eden's saves
// and caches) into /data/prosperoeden. Only folders that do not exist yet are filled, and the
// sandbox copy stays, so an older ProsperoEden still finds its data.
static void MigrateSandboxData() {
    const std::filesystem::path sandbox{"/mnt/sandbox/PPSA99008_000/download0"};
    const std::pair<std::filesystem::path, std::string> moves[] = {
        {sandbox / "eden-headless-g7/user", Eden::UserDir()},
        {sandbox / "prosperoeden/covers", Eden::CoversDir()},
    };
    const auto options = std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing;
    for (const auto& [from, to] : moves) {
        std::error_code error;
        if (Eden::DirectoryExists(to) || !Eden::DirectoryExists(from.string())) continue;
        std::filesystem::copy(from, to, options, error);
        const std::string detail = from.string() + " -> " + to + (error ? " failed: " + error.message() : "");
        Eden::Report("data migration", detail.c_str());
    }
    // Settings files (not the covers folder) go to config/.
    if (!Eden::DirectoryExists(Eden::ConfigDir()) && Eden::DirectoryExists((sandbox / "prosperoeden").string())) {
        std::error_code error;
        std::filesystem::create_directories(Eden::ConfigDir(), error);
        for (std::filesystem::directory_iterator it{sandbox / "prosperoeden", error}, end; !error && it != end; it.increment(error))
            if (it->is_regular_file())
                std::filesystem::copy_file(it->path(), std::filesystem::path{Eden::ConfigDir()} / it->path().filename(),
                                           std::filesystem::copy_options::skip_existing, error);
        Eden::Report("data migration", error ? ("settings failed: " + error.message()).c_str() : "settings copied");
    }
}
#endif

#ifdef PS5_NATIVE
// Game files an early version kept inside the app's folder (assets/ with keys, firmware and
// roms) move to /data/prosperoeden, where replacing the app cannot take them away: an update
// that replaces the whole folder left the app without its keys and games. Both are on /data, so
// it is a rename of each folder. Nothing moves unless all of it can: a folder chosen in Settings,
// or anything already in /data/prosperoeden, leaves everything where it is.
static void MoveGameFilesOutOfTheApp() {
    const std::string from = std::string{Eden::kInstallDir} + "/assets", to = Eden::kDefaultAssetsDir;
    const std::string saved = Eden::LoadSavedAssetsDir();
    if ((!saved.empty() && saved != from) || !Eden::FileExists(from + "/keys/prod.keys") ||
        Eden::FileExists(to + "/keys/prod.keys"))
        return;
    static constexpr const char* kFolders[] = {"keys", "firmware", "roms", "updates", "mods", "ryujinx", "save-import"};
    (void)mkdir(to.c_str(), 0777);
    for (const char* folder : kFolders) {
        const std::string target = to + "/" + folder;
        if (Eden::DirectoryExists(from + "/" + folder) && rmdir(target.c_str()) != 0 && errno != ENOENT) {
            Eden::Report("game files", ("Left in the app's folder: " + target + " already holds files").c_str());
            return;
        }
    }
    int moved = 0;
    for (const char* folder : kFolders) {
        const std::string source = from + "/" + folder, target = to + "/" + folder;
        if (!Eden::DirectoryExists(source)) continue;
        if (std::rename(source.c_str(), target.c_str()) != 0) {
            Eden::Report("game files", ("Could not move " + source + " to " + target + ": " + std::strerror(errno)).c_str());
            continue;
        }
        ++moved;
    }
    if (!saved.empty()) (void)Eden::SaveAssetsDir(to);
    Eden::Report("game files", (std::to_string(moved) + " folders moved from " + from + " to " + to +
                                ", where an update of the app leaves them alone").c_str());
}
#endif

// A game that has single Joy-Cons held sideways (their hold type, set by the game) is played on a
// DualSense held as usual: the pad then turns the stick, the button places and the motion by a
// quarter for each player who has such a Joy-Con (Pad::Grip). Asked a few times a second.
static void UpdateGrips(Core::System& system, Eden::Pad& pad) {
    using Grip = Eden::Pad::Grip;
    static unsigned calls = 0;
    static std::array<Grip, Eden::Pad::kMaxPlayers> last{};
    if (calls++ % 15 != 0) return;
    bool sideways = false;
    if (const auto hid = system.ServiceManager().GetService<Service::HID::IHidServer>("hid")) {
        const auto resources = hid->GetResourceManager();
        const auto npad = resources ? resources->GetNpad() : nullptr;
        const auto applets = resources ? resources->GetAppletResource() : nullptr;
        auto hold = Service::HID::NpadJoyHoldType::Vertical;
        sideways = npad && applets && npad->GetNpadJoyHoldType(applets->GetActiveAruid(), hold).IsSuccess() &&
                   hold == Service::HID::NpadJoyHoldType::Horizontal;
    }
    for (std::size_t index = 0; index < Eden::Pad::kMaxPlayers; ++index) {
        const auto style = system.HIDCore().GetEmulatedControllerByIndex(index)->GetNpadStyleIndex();
        const Grip grip = !sideways ? Grip::usual :
                          style == Core::HID::NpadStyleIndex::JoyconLeft ? Grip::sideways_left :
                          style == Core::HID::NpadStyleIndex::JoyconRight ? Grip::sideways_right : Grip::usual;
        pad.SetGrip(index, grip);
        if (grip == last[index]) continue;
        last[index] = grip;
        Eden::Report("controllers", ("Player " + std::to_string(index + 1) +
                                     (grip == Grip::usual ? ": controller as usual" :
                                      grip == Grip::sideways_left ? ": a left Joy-Con held sideways, turned for a DualSense held as usual" :
                                                                    ": a right Joy-Con held sideways, turned for a DualSense held as usual")).c_str());
    }
}

int main(int argc, char** argv) {
    try {
        std::FILE* report = stdout;
        SCOPE_EXIT { if (report != stdout) std::fclose(report); };
        std::setvbuf(report, nullptr, _IONBF, 0);
#ifdef PS5_NATIVE
        Eden::BootTrace::Begin(Eden::kAppVersion, __DATE__ " " __TIME__);
        // The PS5 keyboard's module, for a game's text entry: asked for before anything else
        // changes what the app may load (system_keyboard.h).
        Eden::BootTrace::Line("PS5 keyboard module: %#x", static_cast<unsigned>(Eden::PrepareSystemKeyboard()));
        Eden::BootTrace::Line("requesting filesystem access");
        // Filesystem access beyond the sandbox, first: every path below depends on it
        // (assets_dir.h). A resident upstream Lapy service gets the first opportunity; otherwise
        // the packaged exact-title upstream helper is sent to the local elfldr. ProsperoEden
        // contains no locally implemented kernel mutation code. If neither path works, the app
        // keeps its sandbox paths.
#if defined(EDEN_DEV_PROFILE)
        // Development: a start without access, to see what the launcher says then.
        if (access("/app0/no-filesystem-access.txt", F_OK) == 0) Eden::FilesystemAccessStatus() = 99;
        else
#endif
        Eden::FilesystemAccessStatus() = static_cast<int>(elevation::request(elevation::Capability::filesystem));
        Eden::BootTrace::Line("filesystem access returned status=%d, uid %d/%d gid %d/%d", Eden::FilesystemAccessStatus(),
                              static_cast<int>(getuid()), static_cast<int>(geteuid()), static_cast<int>(getgid()),
                              static_cast<int>(getegid()));
#if defined(EDEN_DEV_PROFILE)
        // Upstream Lapy deliberately accepts only a single-threaded target. Keep every
        // development worker behind the completed elevation exchange.
        Eden::Stall::Start();
#endif
        // Elevation leaves the effective group (1) apart from the real one (0), and Mesa turns
        // RADV's disk cache off for a process whose real and effective ids differ, so no
        // compiled shader was ever kept between sessions. Match them; the effective user is
        // root, which may set its group.
        if (getegid() != getgid() && setegid(getgid()) != 0)
            Eden::Report("filesystem access", "Could not match the effective group; RADV's disk cache stays off");
        if (Eden::FilesystemAccess()) {
            MigrateSandboxData();
            MoveGameFilesOutOfTheApp();
        }
        for (const auto& folder : {Eden::UserDir(), Eden::ConfigDir(), Eden::CoversDir(), Eden::LogsDir()}) {
            std::error_code folder_error;
            std::filesystem::create_directories(folder, folder_error);
        }
        // Keep the previous session's logs: a freeze is diagnosed after the app is reopened.
        for (const char* name : {"stderr.log", "heap.log"}) {
            const std::string log = Eden::LogFile(name);
            std::rename(log.c_str(), (log.substr(0, log.size() - 4) + ".prev.log").c_str());
        }
        // A crash report the previous run left: that run's logs move beside it, and the launcher
        // says where it is (crash_report.h).
        const Eden::Crash::Last last_crash = Eden::Crash::TakeLast(Eden::LogsDir(), Eden::UserDir() + "/log/eden_log.txt");
#ifdef EDEN_DEV_PROFILE
        // UI inspection captures are disposable; keep saves, settings and shader caches.
        for (const char* name : {"ui-preview.bmp", "ui-main.bmp", "ui-nav.bmp",
                                 "ui-library.bmp", "ui-about.bmp"})
            std::filesystem::remove(std::filesystem::path{Eden::ConfigDir()} / name);
        for (const char* name : {"eden_log.txt", "eden_log.txt.old.txt"})
            std::filesystem::remove(std::filesystem::path{Eden::UserDir()} / "log" / name);
#endif
        if (!std::freopen(Eden::LogFile("stderr.log").c_str(), "w", stderr) ||
            !std::freopen(Eden::LogFile("heap.log").c_str(), "w", stdout)) return 2;
        // Console storage writes take ~25 ms each; a background thread writes both streams out,
        // unless Settings > Diagnostics asks for every line at once (to find a crash).
        static Eden::LogFlusher log_flusher;
        if (!log_flusher.Start(Eden::LoadPreferences().immediate_logs)) return 2;
        Eden::Crash::Install(Eden::LogsDir(), Eden::kAppVersion, last_crash.restarted);
        Eden::BootTrace::Ready(Eden::LogsDir(), Eden::FilesystemAccess());
        Eden::BootTrace::Line("logs and crash handler ready (%s)", Eden::LogsDir().c_str());
        for (const char* folder : {"/app0", Eden::kMountedAppDir, Eden::kInstallDir, "/mnt/sandbox/PPSA99008_000/app0"})
            Eden::BootTrace::Line("app folder candidate %s: eboot %s, font %s", folder,
                                  Eden::FileExists(std::string{folder} + "/eboot.bin") ? "yes" : "no",
                                  Eden::FileExists(std::string{folder} + "/ui/fonts/montserrat-medium.pefont") ? "yes" : "no");
        Eden::BootTrace::Line("app folder: %s", Eden::AppDir().c_str());
        std::set_new_handler([] {
            ps5_opengl_heap_snapshot("allocation_failure", 0);
            std::fflush(stdout);
            Eden::Report("allocation failure", "operator new: heap exhausted");
            throw std::bad_alloc{};
        });
        // Name the exception in klog, then the crash report (which starts the app again).
        std::set_terminate([] {
            const char* detail = "no active exception";
            if (const auto current = std::current_exception()) {
                try {
                    std::rethrow_exception(current);
                } catch (const std::exception& error) {
                    detail = error.what();
                } catch (...) {
                    detail = "non-standard exception";
                }
            }
            Eden::Report("terminate", detail);
            Eden::Crash::Fail(detail);
        });
        {
            // The ids too: Mesa's disk cache turns itself off when the effective and real ids differ.
            const std::string access = "status=" + std::to_string(Eden::FilesystemAccessStatus()) +
                " elevation=" + elevation::path() +
                " app=" + Eden::AppDir() + " data=" + Eden::UserDir() + " game_files=" + Eden::AssetsDir() +
                " uid=" + std::to_string(getuid()) + "/" + std::to_string(geteuid()) +
                " gid=" + std::to_string(getgid()) + "/" + std::to_string(getegid());
            Eden::Report("filesystem access", access.c_str());
            if (Eden::FilesystemAccess() && Eden::AssetsDir() == Eden::kDefaultAssetsDir)
                for (const char* folder : {"/keys", "/firmware", "/roms", "/updates", "/mods", "/ryujinx"})
                    (void)mkdir((std::string{Eden::kDefaultAssetsDir} + folder).c_str(), 0777);
            // RADV's shader cache is kept with the app's data (its own default is /app0), so it also
            // works when the app is installed as a read-only package image. The cache an earlier
            // version kept in the app folder moves over.
            if (Eden::FilesystemAccess()) {
                const std::string cache = std::string{Eden::kDataDir} + "/cache/radv";
                const std::string before = Eden::AppFile("radv-shader-cache");
                (void)mkdir((std::string{Eden::kDataDir} + "/cache").c_str(), 0777);
                if (!Eden::DirectoryExists(cache) && Eden::DirectoryExists(before) &&
                    std::rename(before.c_str(), cache.c_str()) != 0)
                    Eden::Report("cache", "The driver's shader cache could not move to the data folder; it starts empty");
                (void)mkdir(cache.c_str(), 0777);
                setenv("MESA_SHADER_CACHE_DIR", cache.c_str(), 1);
            }
        }
        // Whether Eden's large tables can be sparse on this console (src/memory_pages.cpp),
        // decided now: every session's log says it, with or without a game.
        (void)Common::SparseTablesAvailable();
        {
            // A game that would not stop made the app start again (stop_limit.h): this log says so.
            const std::string note = Eden::LogFile("stop-limit.txt");
            if (std::remove(note.c_str()) == 0)
                Eden::Report("exit", "The last game did not stop (or finish loading) within ten seconds of being "
                                     "asked to, so ProsperoEden started again");
            Eden::StopLimit::Start(note);
        }
        report = std::fopen(Eden::LogFile("result.tsv").c_str(), "w");
        if (!report) { report = stdout; return 2; }
        std::puts("[headless-startup] directories_ready");
#ifdef EDEN_DEV_VULKAN
        if (std::filesystem::exists(Eden::AppFile("sdk-audit.txt"))) {
            Eden::AuditSdk();
            std::puts("EDEN_SDK_AUDIT_COMPLETE");
            std::fflush(stdout);
            // Keep the sandbox mounted for the existing bounded FTP collector.
            std::this_thread::sleep_for(std::chrono::seconds(30));
            return 0;
        }
#endif
        const std::string user_dir = Eden::UserDir();
#ifdef EDEN_PS5_OPENGL
        const auto native_shader_cache = std::filesystem::path{user_dir} / "cache/native-opengl";
        if (setenv("PS5_GLTHREAD", "1", 1) != 0)
            throw std::runtime_error("Cannot configure GL worker");
#if defined(EDEN_DEV_PROFILE) && !defined(EDEN_DEV_VULKAN)
        Eden::Performance::BeginPcSampling();
        // A driver rebuild already invalidates these compiler records. Drop its
        // obsolete slots so repeated development builds fit the 256 MiB sandbox.
        std::string cached_runtime;
        std::ifstream(native_shader_cache / "development-runtime") >> cached_runtime;
        if (cached_runtime != EDEN_DEV_GL_VERSION) {
            std::filesystem::create_directories(native_shader_cache);
            std::error_code directory_error;
            size_t removed = 0;
            for (const auto& entry : Eden::ReadNativeDirectory(native_shader_cache, directory_error)) {
                const auto name = entry.path().filename().string();
                const auto dot = name.find(".bin");
                if ((dot == 3 || dot == 4) &&
                    name.find_first_not_of("0123456789abcdef", 0) == dot)
                    removed += std::filesystem::remove(entry.path());
            }
            if (directory_error) throw std::system_error(directory_error, "Read native compiler cache");
            std::ofstream(native_shader_cache / "development-runtime") << EDEN_DEV_GL_VERSION;
            std::printf("EDEN_DEV_CACHE removed_obsolete_files=%zu\n", size_t(removed));
        }
        for (unsigned i = 0; i < 8; ++i)
            std::filesystem::remove(std::filesystem::path{Eden::LogsDir()} /
                                    ("pass-" + std::to_string(i) + ".ppm"));
#endif
        std::error_code cache_error;
        std::filesystem::create_directories(native_shader_cache, cache_error);
        if (!cache_error && setenv("PS5_SHADER_CACHE_DIR", native_shader_cache.c_str(), 1) != 0)
            throw std::runtime_error("Cannot configure native shader cache");
#endif
        std::string launch_error;
        // The previous run crashed: the launcher says where its report is.
        if (!last_crash.report.empty()) launch_error = std::string{Eden::Crash::kNotice} + last_crash.report;
        const Eden::ForwardedArgs forwarded = Eden::ParseForwardedArgs(argc, argv);
        std::string forwarded_game;
        bool forwarded_session = false;
        if (!forwarded.rom.empty() && last_crash.report.empty()) {
            const std::string path = Eden::ResolveForwardedRom(forwarded.rom, Eden::AssetsPath("roms"));
            if (!path.empty() && Eden::FileExists(path)) forwarded_game = path;
            else launch_error = "Forwarded game not found: " + (path.empty() ? forwarded.rom : path);
            Eden::BootTrace::Line("forwarded game: %s", forwarded_game.empty() ? "not found" : "found");
        }
        // A game that faulted early in its boot is restarted (at most four times per launch).
        std::string relaunch_game;
        unsigned guest_fault_retries = 0;
#ifdef EDEN_DEV_VULKAN
        std::string recovery_mode;
        std::ifstream(Eden::AppFile("backend-recovery.txt")) >> recovery_mode;
        const bool check_backend_recovery = !recovery_mode.empty();
        bool recovery_opengl = false;
#endif
#if defined(EDEN_DEV_PROFILE) || defined(EDEN_DEV_ROM_ID)
        bool autoboot_pending = true;
        // dev-settings rom=TITLEID boots another title with the same development package
        // (tests on two consoles); the build's development title is the default.
#ifdef EDEN_DEV_ROM_ID
        std::string development_id = EDEN_DEV_ROM_ID;
#else
        std::string development_id = EDEN_DEV_PROFILE_TITLE;
#endif
        {
            std::ifstream dev_settings(Eden::AppFile("dev-settings.txt"));
            for (std::string entry; dev_settings >> entry;) {
                if (entry.starts_with("rom=") && entry.size() == 20) development_id = entry.substr(4);
                // dev-settings launcher=first opens the launcher instead of the development title
                // (launcher work: its captures and file-driven input need a development build).
                if (entry == "launcher=first") autoboot_pending = false;
            }
            // After a crash the launcher opens with its notice, not the development title again.
            if (!last_crash.report.empty()) autoboot_pending = false;
        }
#ifdef EDEN_DEV_ROM_ID
        {
            // A scripted input left by an earlier run is not for this process.
            std::ifstream left_behind(Eden::AppFile("compat-input.txt"));
            Eden::DevelopmentInput::IgnoreExisting(left_behind);
        }
#endif
#endif
        Common::FS::CreateEdenPaths();
        Eden::Performance::PlatformChecks();
        Common::Log::Initialize();
        Eden::Multiplayer::RoomClient rooms;
        for (;;) {
        Eden::StopLimit::End();  // whatever game ran before is gone
#ifdef EDEN_PS5_OPENGL
        std::error_code trim_error;
        const auto cache_entries = Eden::ReadNativeDirectory(native_shader_cache, trim_error);
        if (!trim_error) {
            const auto removed = Eden::TrimShaderCache(cache_entries);
            if (removed) Eden::Report("cache", "Removed old shader cache records to reserve writable storage");
        }
#endif
        std::string selected_game;
        Eden::Crash::SetSession("launcher", false);
#ifdef PS5_NATIVE
        // A Vulkan session at 120 Hz handed the output back at 60 Hz as it closed; the launcher
        // opens it once the display has had time to follow (display_refresh.h).
        if (Eden::Display::settle.exchange(false)) {
            Eden::Report("display", "Back to 60 Hz after a 120 Hz session");
            std::this_thread::sleep_for(std::chrono::seconds(Eden::Display::kSettleSeconds));
        }
#endif
#ifdef EDEN_DEV_VULKAN
        if (check_backend_recovery && !recovery_opengl && !launch_error.empty()) {
            recovery_opengl = true;
            autoboot_pending = true;
            Eden::Report("recovery check", "Starting OpenGL after Vulkan session failure");
        }
        const bool automatic_launch = autoboot_pending;
#endif
        if (!relaunch_game.empty()) {
            selected_game = std::exchange(relaunch_game, {});
        } else if (!forwarded_game.empty()) {
            guest_fault_retries = 0;
            forwarded_session = true;
            selected_game = std::exchange(forwarded_game, {});
            Eden::BootTrace::Line("starting the forwarded game");
        } else if (forwarded_session && forwarded.exit_after_game && launch_error.empty()) {
            Eden::Report("exit", "Forwarded game ended");
            return 0;
        } else {
        guest_fault_retries = 0;
        forwarded_session = false;
#if defined(EDEN_DEV_PROFILE) || defined(EDEN_DEV_ROM_ID)
        if (std::exchange(autoboot_pending, false)) {
        // Match the title ID in the file name, else in the ROM's own metadata; the game files
        // folder first, then the pre-1.000.020 assets folder that still holds other test titles.
        const auto development_title = std::strtoull(development_id.c_str(), nullptr, 16);
        for (const std::string& folder : {Eden::AssetsPath("roms"), std::string{"/data/assets/roms"}}) {
            std::error_code rom_error;
            for (const auto& entry : Eden::ReadNativeDirectory(folder, rom_error)) {
                const auto filename = entry.path().filename().string();
                const auto extension = entry.path().extension();
                if (extension != ".nsp" && extension != ".xci") continue;
                const auto path = folder + "/" + filename;
                if (filename.find(development_id) != std::string::npos ||
                    eden_game_title_id(path.c_str()) == development_title) {
                    selected_game = path;
                    break;
                }
            }
            if (!selected_game.empty()) break;
        }
        if (selected_game.empty())
            throw std::runtime_error("Development ROM not found");
        } else {
            Eden::BootTrace::Line("opening the launcher");
            selected_game = SelectProsperoEdenGame(rooms, launch_error);
        }
#else
        Eden::BootTrace::Line("opening the launcher");
        selected_game = SelectProsperoEdenGame(rooms, launch_error);
#endif
        Eden::BootTrace::Line("launcher closed: %s", selected_game.empty() ? "no game (quit)" : "a game was chosen");
        }
        if (selected_game.empty()) {
            Eden::Report("exit", "Launcher closed");
#ifdef EDEN_DEV_ROM_ID
            // Development runs end here (EDEN_DEV_QUIT): flush the logs and let the log pipes copy
            // them out. Returning asks the system to end the title (src/lifecycle.c); the C library's
            // own exit here made every development run end as an app crash.
            std::fflush(nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
#endif
            return 0;
        }
        try {
        launch_error.clear();
        if (!rooms.BeginGame()) throw std::runtime_error("Join a room successfully or leave it before starting a game.");
        SCOPE_EXIT { rooms.EndGame(); };
#ifdef EDEN_DEV_VULKAN
        Eden::Performance::vulkan_cost_enabled = std::filesystem::exists(Eden::AppFile("cost-run.txt"));
        const bool performance_run = std::filesystem::exists(Eden::AppFile("performance-run.txt"));
#ifdef EDEN_DEV_PROFILE
        Eden::Performance::texture_budget_log = true;  // EDEN_VULKAN_TEXTURE_BUDGET every 300 frames
        // Optional 20 Hz GPU-thread PC samples. The handler must exist before the
        // GPU thread registers, which unblocks SIGUSR2 only while sampling is on.
        const bool pc_sample_run = std::filesystem::exists(Eden::AppFile("pc-sample.txt"));
        static bool pc_sampler_installed = false;
        if (pc_sample_run && !pc_sampler_installed) {
            Eden::Performance::BeginPcSampling();
            pc_sampler_installed = true;
        }
#endif
        setenv("PS5VK_QUIET_LOG", performance_run ? "1" : "0", 1);
        // Bounded development cost breakdowns; never enable per-draw tracing.
        setenv("PS5VK_COST_LOG", std::filesystem::exists(Eden::AppFile("cost-run.txt")) ? "1" : "0", 1);
        if (performance_run) unsetenv("PS5VK_CAPTURE_SCANOUT");
        else setenv("PS5VK_CAPTURE_SCANOUT", "1", 1);
        std::printf("EDEN_VULKAN_MEASUREMENT quiet=%d captures=%d\n",
                    performance_run, !performance_run);
        const auto game_video = Eden::LoadGameSettings(eden_game_title_id(selected_game.c_str()));
        const auto backend = automatic_launch ?
            (recovery_opengl ? Eden::GraphicsBackend::OpenGL : Eden::GraphicsBackend::Vulkan) :
            game_video.renderer >= 0 ?
            (game_video.renderer == 0 ? Eden::GraphicsBackend::OpenGL : Eden::GraphicsBackend::Vulkan) :
            Eden::LoadPreferences().backend;
#else
        // Library > Game settings take precedence over Settings > Video.
        const auto game_video = Eden::LoadGameSettings(eden_game_title_id(selected_game.c_str()));
        const auto backend = game_video.renderer >= 0 ?
            (game_video.renderer == 0 ? Eden::GraphicsBackend::OpenGL : Eden::GraphicsBackend::Vulkan) :
            Eden::LoadPreferences().backend;
#ifdef EDEN_PS5_VULKAN
        // The user-facing diagnostics option controls Eden logs, not synchronous
        // driver traces for every draw. Keep those in explicit development probes.
        setenv("PS5VK_QUIET_LOG", "1", 1);
#endif
#endif
#ifndef EDEN_PS5_VULKAN
        if (backend == Eden::GraphicsBackend::Vulkan)
            throw std::runtime_error("Vulkan is not available in this build yet. Select OpenGL in Settings > Video to play.");
#endif
        Eden::Report("launch", Eden::BackendName(backend));
        const bool game = std::filesystem::is_regular_file(selected_game);
        if (!game) throw std::runtime_error("Selected ROM is no longer available");
        const char* guest = selected_game.c_str();
        // The audio and the renderer read this game's own settings (Library > Game settings).
#ifdef PS5_NATIVE
        const uint64_t game_title = game ? eden_game_title_id(guest) : 0;
#else
        const uint64_t game_title = 0;
#endif
        Eden::session_title.store(game_title);
        const unsigned cycles = game ? 1 : 3;
        const bool devices = EDEN_DEVICE_FRONTEND;
        if (game) {
            for (const char* capture : {"game-frame.ppm", "scanout.ppm", "game-hud.ppm", "game-hud-steady.ppm",
                                        "game-hud-late.ppm"})
                std::filesystem::remove(std::filesystem::path{Eden::LogsDir()} / capture);
            // Firmware and keys stay in the user-provided read-only assets tree.
            std::puts("EDEN_GAME_ASSETS_READY");
        }
        constexpr bool cpu_pressure = false;
        constexpr bool shutdown_sweep = false;
#else
        const char* guest = argc >= 2 ? argv[1] : nullptr;
        const uint64_t game_title = 0;  // the host build has no game's own settings
        unsigned cycles = 1;
        bool devices = false;
        bool game = false;
        bool cpu_pressure = false;
        bool shutdown_sweep = false;
        for (int i = 2; i < argc; ++i) {
            if (std::string_view{argv[i]} == "--repeat" && cycles == 1) cycles = 3;
            else if (std::string_view{argv[i]} == "--soak" && cycles == 1) cycles = 20;
            else if (std::string_view{argv[i]} == "--devices" && !devices) devices = true;
            else if (std::string_view{argv[i]} == "--game" && !game) game = true;
            else if (std::string_view{argv[i]} == "--cpu-pressure" && !cpu_pressure) cpu_pressure = true;
            else if (std::string_view{argv[i]} == "--shutdown-sweep" && !shutdown_sweep) shutdown_sweep = true;
            else return 2;
        }
        // Device mock assertions describe the homebrew fixture, not arbitrary games.
        if (game && devices) return 2;
        if (cpu_pressure && (game || devices || cycles == 20)) return 2;
        if (shutdown_sweep) {
            if (game || devices || cpu_pressure || cycles != 1) return 2;
            game = true;
            cycles = 6;
        }
        const char* user_dir = "user";
#endif
        const auto passed = [&](const char* name) {
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
            Eden::Stall::Progress(name);
#endif
            std::fprintf(report, "%s\tPASS\n", name);
#ifdef PS5_NATIVE
            const auto mono_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            std::printf("EDEN_STAGE backend=%s phase=%s mono_ns=%lld\n",
                        Eden::BackendName(backend), name, static_cast<long long>(mono_ns));
            ps5_opengl_heap_snapshot(name, 0);
            std::fflush(stdout);
#else
            const auto heap = mallinfo2();
            std::fprintf(stderr, "host_heap phase=%s allocated_bytes=%zu arena_bytes=%zu mapped_bytes=%zu free_bytes=%zu\n",
                         name, heap.uordblks + heap.hblkhd, heap.arena, heap.hblkhd, heap.fordblks);
#endif
        };
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
        Eden::Stall::Arm();
#endif
        passed("session_start");
        if (!std::filesystem::is_directory(user_dir)) {
            std::fputs("Run from the isolated directory containing user/.\n", stderr);
            return 2;
        }
        Common::FS::CreateEdenPaths();
#ifdef PS5_NATIVE
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, Eden::AssetsPath("keys"));
        // Mods come from the game files folder's mods/ (mods.h). Without that folder Eden's own
        // load folder stays, where nothing is expected.
        if (const std::string mods = Eden::AssetsPath("mods"); Eden::DirectoryExists(mods))
            Common::FS::SetEdenPath(Common::FS::EdenPath::LoadDir, mods);
#endif
#ifdef PS5_NATIVE
        if (Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) != user_dir) return 2;
        std::puts("[headless-startup] absolute_paths_ready");
#endif
#ifndef PS5_NATIVE
        Common::Log::Initialize();
#endif
        if (game) {
            Common::Log::Filter filter;
            filter.SetClassLevel(Common::Log::Class::Service_FS, Common::Log::Level::Info);
#if defined(PS5_NATIVE) && !defined(EDEN_DEV_PROFILE) && !defined(EDEN_DEV_ROM_ID)
            if (Eden::LoadPreferences().detailed_logging) filter.ParseFilterString("*:Debug");
#endif
            Common::Log::SetGlobalFilter(filter);
        }
        Common::Log::Start();
        SCOPE_EXIT { Common::Log::Stop(); };
        #ifdef EDEN_PS5_OPENGL
        Settings::values.renderer_backend = backend == Eden::GraphicsBackend::Vulkan ?
            Settings::RendererBackend::Vulkan : Settings::RendererBackend::OpenGL_GLSL;
        // Keep emulator threads off guest cores 0-2 and their SMT siblings (Vulkan):
        // +17% in the heaviest measured window of a racing game.
        Eden::Performance::SetSecondaryPlacement(backend == Eden::GraphicsBackend::Vulkan);
        // A32 fastmem window (direct JIT accesses to aliased guest memory): opt-in with dev-settings
        // fastmem=on; on the console it is slower than the page-table path.
        Eden::Fastmem::Request(false);
#ifdef EDEN_DEV_VULKAN
        Settings::values.async_presentation = backend == Eden::GraphicsBackend::Vulkan;
        // Exercise upstream's checked initialization error, never an invalid pointer.
        Settings::values.vulkan_device = recovery_mode == "init-failure" && !recovery_opengl ?
            0xffffffffu : 0u;
#endif
        Settings::values.renderer_debug = false;
        // RADV presents the console's 12 GiB of direct memory as an integrated GPU, for which
        // Eden budgets 4 GiB: a game using ~4.4 GB of Vulkan memory then ran the texture GC
        // every frame (20-25 FPS). Eden's larger integrated budget (6 GiB) holds it at 30 FPS
        // with ~0.9 GB of direct memory to spare.
        Settings::values.vram_usage_mode.SetValue(Settings::VramUsageMode::Aggressive);
#else
        Settings::values.renderer_backend = Settings::RendererBackend::Null;
#endif
        Settings::values.cpu_accuracy = Settings::CpuAccuracy::Auto;
        // The exact GPU and DMA levels too, every session: one app run starts many games, and a
        // level the game before lowered ("performance" in the settings file, below) would stay.
        Settings::values.gpu_accuracy.SetValue(Settings::GpuAccuracy::High);
        Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::High;
        Settings::values.dma_accuracy.SetValue(Settings::DmaAccuracy::Default);
        Settings::values.memory_layout_mode = Settings::MemoryLayout::Memory_4Gb;
        // A development run that names jit_list in dev-settings decides the block list by itself.
        [[maybe_unused]] bool dev_block_list = false;  // read where a session applies its settings
#ifdef EDEN_DEV_PROFILE
        // One-run A/B switches written by the development runner; absent = defaults.
        {
            std::ifstream dev_settings(Eden::AppFile("dev-settings.txt"));
            for (std::string entry; dev_settings >> entry;) {
                if (entry == "dma_accuracy=unsafe") {
                    Settings::values.dma_accuracy.SetValue(Settings::DmaAccuracy::Unsafe);
                } else if (entry == "placement=off") {
                    Eden::Performance::SetSecondaryPlacement(false);
                } else if (entry == "fastmem=off" || entry == "fastmem=on") {
                    Eden::Fastmem::Request(entry.ends_with("on"));
                } else if (entry.starts_with("cache_spin=")) {
                    // try_lock retries before a guest core sleeps on a GPU cache lock (0 = upstream).
                    Eden::Performance::cache_lock_spins = static_cast<unsigned>(std::strtoul(entry.c_str() + 11, nullptr, 10));
                } else if (entry == "gc_dirty=upstream") {
                    // Eden's rule: evict GPU-written textures from the "expected" memory mark on.
                    Eden::Performance::gc_keep_dirty = false;
                } else if (entry.starts_with("dispatch_draws=")) {
                    // Draws between Vulkan worker hand-offs: a power of two from 8 (upstream) to 512.
                    const unsigned long draws = std::strtoul(entry.c_str() + 15, nullptr, 10);
                    if (draws >= 8 && draws <= 512 && (draws & (draws - 1)) == 0)
                        Eden::Performance::dispatch_mask = static_cast<unsigned>(draws - 1);
                } else if (entry.starts_with("idle_spin_us=")) {
                    // Guest cores spin this long on their interrupt flag before sleeping (W1).
                    Eden::Performance::idle_spin_iterations = static_cast<unsigned>(std::stoul(entry.substr(13)) * 50);
                } else if (entry.starts_with("pc_core=") && entry.size() == 9 && entry[8] >= '0' && entry[8] <= '3') {
                    // Host PC samples from this guest core instead of core 0 (with --pc-sample).
                    Eden::Performance::pc_sample_core = static_cast<unsigned>(entry[8] - '0');
                } else if (entry == "pc_fast=on") {
                    Eden::Performance::pc_fast = true;
                } else if (entry == "capture=early") {
                    Eden::Performance::capture_early = true;
                } else if (entry == "jit_dups=on") {
                    // Which cores compiled each A64 block, and when (EDEN_PERF_DUPLICATES).
                    Eden::Performance::jit_duplicate_tracking = true;
                } else if (entry == "applets=firmware") {
                    // Eden's default library applets: several run from the firmware as guest programs.
                    Eden::Performance::firmware_applets = true;
                } else if (entry == "jit_list=on" || entry == "jit_list=off") {
                    // The saved block list (jit_list.h), whatever the settings file says.
                    Eden::JitList::enabled = entry.ends_with("on");
                    dev_block_list = true;
                } else if (entry == "jit_shared=off") {
                    // Every guest core keeps its own compiled blocks (headless/dynarmic/jit_group.h).
                    eden_jit_shared = false;
                } else if (entry == "cpu_accuracy=unsafe") {
                    // Dynarmic's unsafe FP shortcuts on top of Auto (reduced-error estimates, inaccurate NaN).
                    Settings::values.cpu_accuracy = Settings::CpuAccuracy::Unsafe;
                } else if (entry == "replay=off") {
                    // The profile title takes controller and runner input instead of the timed replay.
                } else if (entry == "large_pages=off" || entry == "sparse_tables=off" || entry == "heap=whole") {
                    // Read directly by the page allocator (src/memory_pages.cpp) before this parse:
                    // 16 KiB pages only; Eden's large tables dense; the heap's 3 GiB taken at start.
                } else if (entry == "stop_hang=on") {
                    dev_stop_hang = true;
                } else if (entry == "load_hang=on") {
                    dev_load_hang = true;
                } else if (entry == "graphics_usage=driver") {
                    // The caches' "memory in use" as the driver counts it (performance.h).
                    Eden::Performance::graphics_usage_from_pool = false;
                } else if (entry == "gpu_accuracy=low") {
                    // Nothing calls UpdateGPUAccuracy() here; set the live value too.
                    Settings::values.gpu_accuracy.SetValue(Settings::GpuAccuracy::Low);
                    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::Low;
                } else if (entry == "null_descriptor=off") {
                    Eden::DevVulkan::disable_null_descriptor = true;
                } else if (entry == "descriptor_buffer=off") {
                    Eden::DevVulkan::disable_descriptor_buffer = true;
                } else if (entry == "robustness2=on") {
                    Eden::DevVulkan::robustness2 = true;
                } else if (entry == "vertex_input_dynamic=off") {
                    Settings::values.vertex_input_dynamic_state.SetValue(false);
                } else if (entry == "pipeline_trace=on") {
                    Eden::DevVulkan::trace_pipelines = true;
                } else if (entry.starts_with("radv_debug=")) {
                    // Read by RADV at instance creation, which happens after this parse.
                    setenv("RADV_DEBUG", entry.c_str() + 11, 1);
                } else if (entry.starts_with("env=") && entry.find('=', 4) != std::string::npos) {
                    // Other driver switches read at device creation, e.g. env=RADV_PS5_BORDER_REBIND=1.
                    const auto split = entry.find('=', 4);
                    setenv(entry.substr(4, split - 4).c_str(), entry.c_str() + split + 1, 1);
                } else if (entry == "sparse=off") {
                    Eden::DevVulkan::disable_sparse = true;
                } else if (entry == "multirange=off") {
                    Eden::DevVulkan::disable_multi_range = true;
                } else if (entry == "custom_border=off") {
                    Eden::DevVulkan::disable_custom_border = true;
                } else if (entry == "custom_border=on") {
                    Eden::DevVulkan::disable_custom_border = false;
                } else if (entry == "vram=aggressive") {
                    // Eden's own larger integrated-GPU budget (6 GiB instead of 4 GiB), the default.
                    Settings::values.vram_usage_mode.SetValue(Settings::VramUsageMode::Aggressive);
                } else if (entry == "vram=conservative") {
                    Settings::values.vram_usage_mode.SetValue(Settings::VramUsageMode::Conservative);
                } else if (entry == "conditional_rendering=off") {
                    Eden::DevVulkan::disable_conditional_rendering = true;
                } else if (entry == "conditional_rendering=on") {
                    Eden::DevVulkan::disable_conditional_rendering = false;
                } else if (entry == "gpu_time=on") {
                    Eden::DevVulkan::gpu_time = true;
                } else if (entry == "submit_sync=on") {
                    Eden::DevVulkan::sync_submissions = true;
                } else if (entry == "astc=cpu") {
                    Settings::values.accelerate_astc.SetValue(Settings::AstcDecodeMode::Cpu);
                } else if (entry == "log=render") {
                    // Renderer debug logging for crash probes (fetched every few seconds).
                    Common::Log::Filter filter;
                    filter.ParseFilterString("*:Info Render:Debug Render.Vulkan:Debug HW.GPU:Debug Shader:Debug");
                    Common::Log::SetGlobalFilter(filter);
                } else if (entry.starts_with("boot_trace=") && entry.find(':') != std::string::npos) {
                    // Individual guest GPU submissions/dispatches from START to END ms after now.
                    const auto colon = entry.find(':');
                    const long long now = Eden::Performance::NowNs();
                    Eden::Performance::boot_trace_begin = now + std::stoll(entry.substr(11, colon - 11)) * 1000000;
                    Eden::Performance::boot_trace_end = now + std::stoll(entry.substr(colon + 1)) * 1000000;
                } else if (entry == "hcr=eden" || entry == "hcr=exact" || entry == "hcr=cpu") {
                    Eden::DevVulkan::hcr_mode = entry == "hcr=eden" ? 0 : entry == "hcr=exact" ? 1 : 2;
                } else if (entry == "compute_barriers=off") {
                    Eden::DevVulkan::compute_barriers = false;
                } else if (entry == "gpu_clock=normal" || entry == "gpu_clock=boost" || entry == "gpu_clock=overclock") {
                    // Guest-visible GPU timestamp scale (Eden's fast_gpu_time; Boost is the default).
                    Settings::values.gpu_clock.SetValue(entry.ends_with("normal") ? Settings::GpuClock::Normal :
                        entry.ends_with("boost") ? Settings::GpuClock::Boost : Settings::GpuClock::Overclock);
                } else if (entry.starts_with("rom=")) {
                    // Development title override, applied when the title is selected.
                } else if (entry == "fs_callers=on") {
                    Eden::Performance::trace_fs_callers = true;
                } else if (entry.starts_with("log_filter=")) {
                    // Any Eden log filter for crash probes, '+' for spaces,
                    // e.g. log_filter=*:Info+Service:Debug+Service.HID:Info.
                    std::string text = entry.substr(11);
                    std::replace(text.begin(), text.end(), '+', ' ');
                    Common::Log::Filter filter;
                    filter.ParseFilterString(text);
                    Common::Log::SetGlobalFilter(filter);
                } else if (Eden::Watch::Parse(entry)) {
                    // Write watch / code dump on the main module, armed after load (watch.h).
                } else if (entry.starts_with("dyna_state=") && entry.size() == 12 &&
                           entry[11] >= '0' && entry[11] <= '3') {
                    Settings::values.dyna_state.SetValue(
                        static_cast<Settings::ExtendedDynamicState>(entry[11] - '0'));
                } else {
                    std::printf("EDEN_DEV_SETTINGS unknown=%s\n", entry.c_str());
                }
            }
            std::printf("EDEN_DEV_SETTINGS dma_accuracy=%u gpu_accuracy=%u null_descriptor=%u "
                        "descriptor_buffer=%u robustness2=%u vertex_input_dynamic=%u dyna_state=%u "
                        "sparse=%u multirange=%u custom_border=%u submit_sync=%u fastmem=%u\n",
                        unsigned(Settings::values.dma_accuracy.GetValue()),
                        unsigned(Settings::values.current_gpu_accuracy),
                        unsigned(!Eden::DevVulkan::disable_null_descriptor),
                        unsigned(!Eden::DevVulkan::disable_descriptor_buffer),
                        unsigned(Eden::DevVulkan::robustness2),
                        unsigned(Settings::values.vertex_input_dynamic_state.GetValue()),
                        unsigned(Settings::values.dyna_state.GetValue()),
                        unsigned(!Eden::DevVulkan::disable_sparse),
                        unsigned(!Eden::DevVulkan::disable_multi_range),
                        unsigned(!Eden::DevVulkan::disable_custom_border),
                        unsigned(Eden::DevVulkan::sync_submissions),
                        unsigned(Eden::Fastmem::Requested()));
            std::printf("EDEN_DEV_MEMORY direct_memory=%lld vram_mode=%u\n",
                        static_cast<long long>(sceKernelGetDirectMemorySize()),
                        unsigned(Settings::values.vram_usage_mode.GetValue()));
        }
        Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Docked);
        std::puts("[headless-startup] console_mode=docked");
#else
        Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
#if defined(PS5_NATIVE) && defined(EDEN_PS5_OPENGL)
        const bool docked = Eden::LoadGameDocked(eden_game_title_id(guest));
        Settings::values.use_docked_mode.SetValue(docked ? Settings::ConsoleMode::Docked
                                                       : Settings::ConsoleMode::Handheld);
        Eden::Report("launch", docked ? "Console mode: Docked" : "Console mode: Handheld");
#endif
#endif
#if defined(PS5_NATIVE) && defined(EDEN_PS5_OPENGL)
        {
            // Settings > Video: internal resolution and the filter scaling it to the output.
            static constexpr Settings::ResolutionSetup resolutions[] = {
                Settings::ResolutionSetup::Res1_2X, Settings::ResolutionSetup::Res3_4X, Settings::ResolutionSetup::Res1X,
                Settings::ResolutionSetup::Res3_2X, Settings::ResolutionSetup::Res2X, Settings::ResolutionSetup::Res3X,
                Settings::ResolutionSetup::Res4X};
            static_assert(std::size(resolutions) == std::size(Eden::kResolutionKeys));
            static constexpr Settings::ScalingFilter filters[] = {
                Settings::ScalingFilter::Bilinear, Settings::ScalingFilter::Fsr, Settings::ScalingFilter::Bicubic,
                Settings::ScalingFilter::NearestNeighbor};
            // Settings > Video, with what the game does differently (Library > Game settings > Video).
            const auto video = Eden::PreferencesFor(eden_game_title_id(guest));
            const int resolution = video.resolution;
            const int filter = video.upscaling_filter;
            Settings::values.resolution_setup.SetValue(resolutions[resolution]);
            Settings::values.scaling_filter.SetValue(filters[filter]);
            Settings::UpdateRescalingInfo();
#ifdef EDEN_PS5_FRAMEGEN
            // Lossless Scaling frame generation (EDEN_PS5_FRAMEGEN, headless/vulkan.cmake).
            // Settings > Video's switch, or the game's own in Library > Triangle > Video, asks
            // for it. The app folder's framegen.txt overrides all of it, the way block-list.txt
            // turns the block list on: its first number is the target rate in Hz, its second the
            // multiplier.
            //
            // Eden's two controls work the way Eden defines them (settings.cpp). A target rate
            // makes the pacer add as many frames as that rate needs, up to Eden's own maximum;
            // "auto" passes 0 and leaves the multiplier in charge, which is the ratio Eden uses
            // when no target is asked for. Per session, and before the renderer starts: the
            // swapchain and the presentation manager read them as they are created. Vulkan only,
            // and only when the build has the feature at all (it needs the user's own
            // Lossless.dll).
            {
                const auto at = [](int index, auto count) {
                    return std::clamp(index, 0, static_cast<int>(count) - 1);
                };
                const int target_index =
                    at(video.frame_gen_target, std::size(Eden::kFrameGenTargetHz));
                const int multiplier_index =
                    at(video.frame_gen_multiplier, std::size(Eden::kFrameGenMultiplierValue));
                int target = Eden::kFrameGenTargetHz[target_index];
                int multiplier = Eden::kFrameGenMultiplierValue[multiplier_index];
                int override_rate = -1;
                int override_multiplier = -1;
                std::ifstream frame_gen_file(Eden::AppFile("framegen.txt"));
                (void)(frame_gen_file >> override_rate);
                (void)(frame_gen_file >> override_multiplier);
                if (override_rate > 0) target = override_rate;
                if (override_multiplier >= 2) multiplier = override_multiplier;
                const bool active = (override_rate >= 0 || override_multiplier >= 0 || video.frame_gen) &&
                                    video.backend == Eden::GraphicsBackend::Vulkan;
                Settings::values.frame_gen.SetValue(active);
                Settings::values.frame_gen_target_rate.SetValue(
                    active ? static_cast<unsigned>(std::min(target, 240)) : 0u);
                Settings::values.frame_gen_multiplier.SetValue(
                    static_cast<unsigned>(std::clamp(multiplier, 2, 4)));
                if (active)
                {
                    // Both of frame generation's gates give up silently, so a session that
                    // cannot run it looks exactly like one that never asked: the shaders load
                    // only from the user's own Lossless Scaling library, and the shader stage
                    // needs Vulkan memory model plus robustBufferAccess2's null descriptors.
                    // Say what this session found, in the log and in the crash report, so the
                    // answer does not need another console round trip.
                    const auto folder = Common::FS::GetEdenPath(Common::FS::EdenPath::LosslessDir);
                    const auto status =
                        static_cast<unsigned>(VideoCore::FrameGen::GetInstalledLosslessStatus());
                    constexpr const char* kLosslessStates[] = {
                        "ok",         "Lossless.dll not installed", "Lossless.dll unreadable",
                        "not a PE",   "no LSFG shaders in it",      "shader translation failed",
                        "shader cache unusable"};
                    Eden::Report("launch",
                                 ("Frame generation: " +
                                  std::string(target > 0 ? std::to_string(target) + " Hz target"
                                                         : std::to_string(multiplier) + "x multiplier") +
                                  "; " + folder.string() +
                                  ": " + (status < std::size(kLosslessStates) ? kLosslessStates[status]
                                                                             : "unknown") +
                                  "; null descriptors " +
                                  (Eden::DevVulkan::disable_null_descriptor ? "off" : "on"))
                                     .c_str());
                }
            }
#endif
            // The output's refresh rate while the game runs (display_refresh.h): the renderer asks
            // for it as it opens the output.
            const int refresh = video.refresh;
            Eden::Display::requested_hz.store(Eden::kRefreshHz[refresh]);
            Eden::Display::output_millihertz.store(0);
            setenv(Eden::Display::kVulkanSwitch, refresh ? "1" : "0", 1);
            Eden::Display::game_millihertz.store(60000);
            Eden::Display::skipped_frames.store(0);
            // The size of the picture the session puts out (Settings > Video > Output resolution).
            Eden::Display::output_width.store(Eden::kOutputWidth[video.output]);
            Eden::Display::output_height.store(Eden::kOutputHeight[video.output]);
            Eden::Report("launch", (std::string("Resolution ") + Eden::kResolutionKeys[resolution] + ", " +
                                    Eden::kUpscalingFilterLabels[filter] + ", output " +
                                    Eden::kOutputKeys[video.output] + ", " + Eden::kRefreshKeys[refresh] +
                                    " Hz").c_str());
            // Speed against accuracy ("performance" in the settings file, settings_store.h): each
            // one is off unless chosen there, for every game or for this one. The levels start
            // every session at their exact values (above), so one that is not chosen is off.
            const auto speed = Eden::LoadPerformance(eden_game_title_id(guest));
            Settings::values.use_asynchronous_shaders = speed.async_shaders;
            if (speed.fast_gpu) {
                // Nothing calls UpdateGPUAccuracy() here; set the live value too.
                Settings::values.gpu_accuracy.SetValue(Settings::GpuAccuracy::Low);
                Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::Low;
            }
            if (speed.unsafe_cpu) Settings::values.cpu_accuracy = Settings::CpuAccuracy::Unsafe;
            if (speed.unsafe_dma) Settings::values.dma_accuracy.SetValue(Settings::DmaAccuracy::Unsafe);
            Settings::values.use_reactive_flushing.SetValue(speed.reactive_flushing);
            Settings::values.skip_cpu_inner_invalidation.SetValue(speed.skip_invalidation);
            // The block list is off unless the settings turn it on (block-list.txt in the app
            // folder still does, further down).
            if (!dev_block_list) Eden::JitList::enabled = speed.block_list;
            // The report reads the values back: what the emulator has, whoever set it.
            const auto on = [](bool value) { return value ? "on" : "off"; };
            Eden::Report("launch", (std::string("Performance: block list ") + on(Eden::JitList::enabled) +
                                    ", async shaders " + on(Settings::values.use_asynchronous_shaders.GetValue()) +
                                    ", fast GPU " + on(!Settings::IsGPULevelHigh()) +
                                    ", unsafe CPU " +
                                    on(Settings::values.cpu_accuracy.GetValue() == Settings::CpuAccuracy::Unsafe) +
                                    ", unsafe DMA " +
                                    on(Settings::values.dma_accuracy.GetValue() == Settings::DmaAccuracy::Unsafe) +
                                    ", reactive flushing " + on(Settings::values.use_reactive_flushing.GetValue()) +
                                    ", skip invalidation " +
                                    on(Settings::values.skip_cpu_inner_invalidation.GetValue())).c_str());
            // What a crash report says was running.
            char title_id[20];
            std::snprintf(title_id, sizeof(title_id), "%016llx",
                          static_cast<unsigned long long>(eden_game_title_id(guest)));
            // The game's mods, minus those switched off in the launcher (Library > Game settings >
            // Mods): Eden's patch manager skips the names in this list. With the Library's Mods
            // switch off for the game, every one of them is in it.
            const u64 title = eden_game_title_id(guest);
            const auto all_mods = Eden::Mods::List(Eden::AssetsPath("mods"), title);
            auto mods_off = Eden::LoadDisabledMods(title);
            if (!Eden::LoadModsEnabled(title)) {
                for (const auto& mod : all_mods)
                    if (std::find(mods_off.begin(), mods_off.end(), mod.name) == mods_off.end())
                        mods_off.push_back(mod.name);
            }
            // Of a mod that lists several cheats, only the ones chosen there run (cheats.h).
            const auto cheats_on = Eden::LoadChosenCheats(title);
            Eden::Cheats::Off() = Eden::Mods::CheatsOff(all_mods, cheats_on);
            const std::string mods = Eden::Mods::Summary(all_mods, mods_off, cheats_on);
            Settings::values.disabled_addons[title] = mods_off;
            Eden::Report("launch", ("Mods: " + mods).c_str());
            Eden::Crash::SetSession("game " + std::filesystem::path(guest).filename().string() + " (" + title_id +
                                    "), " + Eden::BackendName(backend) + ", resolution " +
                                    Eden::kResolutionKeys[resolution] + ", " + Eden::kUpscalingFilterLabels[filter] +
                                    ", output " + Eden::kOutputKeys[video.output] + ", " +
                                    Eden::kRefreshKeys[refresh] + " Hz, mods: " + mods,
                                    true);
        }
#endif
        Settings::values.sink_id = Settings::AudioEngine::Null;
        Settings::values.use_multi_core = true;
        // A game's library applets (its error dialog, profile picker, amiibo screen, Mii editor,
        // manual, album...) are Eden's built-in ones. Eden's defaults start several of them from
        // the firmware as a second guest program. Nothing can operate those here, and the error
        // applet started that way ran a game's session out of graphics memory on the console
        // (CreateBuffer: VK_ERROR_OUT_OF_DEVICE_MEMORY as the applet started). The built-in ones
        // answer at once: the error is logged (error_applet.h), the first profile is chosen, the
        // others close.
        // (dev-settings applets=firmware keeps Eden's defaults.)
#ifdef EDEN_DEV_PROFILE
        if (!Eden::Performance::firmware_applets)
#endif
        {
            Settings::values.cabinet_applet_mode = Settings::AppletMode::HLE;
            Settings::values.error_applet_mode = Settings::AppletMode::HLE;
            Settings::values.net_connect_applet_mode = Settings::AppletMode::HLE;
            Settings::values.player_select_applet_mode = Settings::AppletMode::HLE;
            Settings::values.mii_edit_applet_mode = Settings::AppletMode::HLE;
            Settings::values.photo_viewer_applet_mode = Settings::AppletMode::HLE;
            Settings::values.offline_web_applet_mode = Settings::AppletMode::HLE;
            Settings::values.my_page_applet_mode = Settings::AppletMode::HLE;
        }
        // The PS5 build bounds Eden's GPU command queue below, so the CPU and renderer can run in
        // parallel without accumulating the multi-frame controller lag of the upstream queue.
        Settings::values.use_asynchronous_gpu_emulation = true;
#ifdef EDEN_PS5_OPENGL
        Settings::values.use_disk_shader_cache = game;
#else
        Settings::values.use_disk_shader_cache = false;
#endif
        // The PS5 port has software FFmpeg decoders but no FFmpeg hardware-device adapter.
        Settings::values.nvdec_emulation = game ? Settings::NvdecEmulation::Cpu
                                                : Settings::NvdecEmulation::Off;
        Settings::values.use_gdbstub = false;
        // Settings > Language: the system language games see, with its console region.
        static_assert(static_cast<int>(Settings::Language::EnglishBritish) == 12 &&
                      static_cast<int>(Settings::Language::ChineseTraditional) == 16 &&
                      static_cast<int>(Settings::Language::Thai) == 19 &&
                      static_cast<int>(Settings::Region::Taiwan) == 6, "Eden's language or region order changed");
        {
            // Eden only finds a game's supported languages when an update supplies its control data.
            // Without them it hands the game the chosen language as it is, and a game that does not
            // know it (an older one given pt-BR) falls back to Japanese. So the game gets its own
            // closest language, the one the launcher shows for it (eden_game_language).
            const int choice = Eden::PreferencesFor(game_title).language;
            int language = Eden::kLanguageSettings[choice];
#ifdef PS5_NATIVE
            if (game && guest)
                language = eden_game_language(guest, Eden::AssetsPath("keys").c_str(), eden_game_title_id(guest), language);
#endif
            Settings::values.language_index.SetValue(static_cast<Settings::Language>(language));
            Settings::values.region_index.SetValue(static_cast<Settings::Region>(Eden::kLanguageRegions[choice]));
            const std::string code(reinterpret_cast<const char*>(
                &Service::Set::available_language_codes[static_cast<std::size_t>(language)]), 8);
            Eden::Report("launch", ("Language: " + std::string(code.c_str()) + " (chosen " +
                                    Eden::kLanguageKeys[choice] + ")").c_str());
        }
        std::unique_ptr<Eden::Pad> pad;
        bool return_to_menu = false;
        if (devices || game) {
            Eden::BootTrace::Line("opening the controllers");
            pad = std::make_unique<Eden::Pad>();
            if (!pad->Open()) throw std::runtime_error("PS5 controller initialization failed");
            Eden::BootTrace::Line("controllers open");
            Settings::values.audio_output_device_id = "ps5";
            // Settings > Controls, or the game's own (Library > Game settings > Controls).
            const auto controls = Eden::PreferencesFor(game_title);
            Settings::values.vibration_enabled.SetValue(controls.vibration);
            pad->SetMapping(controls.mapping);
            Eden::Report("launch", (std::string("Controls: vibration ") + (controls.vibration ? "on" : "off") +
                                    ", buttons " + (controls.mapping == Eden::kDefaultMapping ? std::string("as usual") :
                                    Eden::Settings::MappingJson(controls.mapping).dump())).c_str());
            // One Pro Controller per signed-in user's DualSense; later changes apply mid-game.
            const unsigned connected = pad->ConnectedPlayers();
            (void)pad->TakeConnectionChanges();
            // The game's own Controller type (Game settings > Controls), a Pro Controller without
            // one. The handheld is a controller of its own, for player 1 only.
            Eden::session_controller = controls.controller;
            const bool handheld = Eden::ControllerSetting(controls.controller) == Settings::ControllerType::Handheld;
            Eden::handheld_in_use = handheld;
            for (std::size_t index = 0; index < Eden::Pad::kMaxPlayers; ++index) {
                auto& player = Settings::values.players.GetValue()[index];
                player.connected = handheld ? false : index == 0 || (connected & (1u << index)) != 0;
                player.controller_type = handheld ? Settings::ControllerType::ProController :
                                                    Eden::ControllerSetting(controls.controller);
                // DualSense rumble (headless/pad.cpp) at Eden's full strength.
                player.vibration_enabled = true;
                player.vibration_strength = 100;
            }
            {
                auto& player = Settings::values.players.GetValue()[8];  // Eden's handheld controller
                player.connected = handheld;
                player.controller_type = Settings::ControllerType::Handheld;
                player.vibration_enabled = true;
                player.vibration_strength = 100;
            }
            if (controls.controller >= 0)
                Eden::Report("launch", (std::string("Controller type: ") +
                                        Eden::kControllerKeys[controls.controller]).c_str());
        }
        {
#ifdef EDEN_PS5_OPENGL
            Eden::BootTrace::Line("opening the graphics window (%s)", backend == Eden::GraphicsBackend::Vulkan ? "Vulkan" : "OpenGL");
            Eden::GraphicsWindow window(backend == Eden::GraphicsBackend::Vulkan);
            Eden::BootTrace::Line("graphics window open");
#ifdef EDEN_GPU_PROBE
            window.RunGpuProbe();
            passed("GPU_PROBE_COMPLETE");
            return 0;
#endif
#else
            HeadlessWindow window;
#endif
            // The running game's own contents, filled for each session below. The system keeps this
            // object's address, so it is declared first; its files belong to the system's file
            // system, so it is emptied before the system goes (a second launch crashed when it
            // released the first session's files after their file system).
            FileSys::ManualContentProvider game_contents;
#ifdef PS5_NATIVE
            if (game) {
                // Who is playing (profiles.h): Eden opens this user of its list, and the game's
                // save data is that user's.
                const auto who = Eden::Profiles::Resolve();
                Settings::values.current_user = who.current;
                if (!who.profiles.empty())
                    Eden::Report("launch", ("Profile: " + who.profiles[static_cast<std::size_t>(who.current)].name +
                                            " (" + std::to_string(who.current + 1) + " of " +
                                            std::to_string(who.profiles.size()) + ")").c_str());
            }
#endif
            Core::System system;
            SCOPE_EXIT { game_contents.ClearAllEntries(); };
            passed("core_constructed");
#ifdef EDEN_PS5_OPENGL
            window.SetSystem(system);
#endif
            system.Initialize();
            if (system.IsPoweredOn()) return 1;
            passed("core_initialized");
            for (unsigned cycle = 0; guest && cycle < cycles; ++cycle) {
                if (game) LOG_INFO(Frontend, "EDEN_GAME_SESSION_BEGIN {}", cycle + 1);
                struct Completion {
                    std::mutex mutex;
                    std::condition_variable wake;
                    bool exited = false;
                    bool captured = false;
                    bool return_to_menu = false;
                    bool room_lost = false;
                    std::exception_ptr failure;
                    std::string guest_fault;
                };
                auto completion = std::make_shared<Completion>();
                system.RegisterExitCallback([completion] {
                    std::lock_guard lock(completion->mutex);
                    completion->exited = true;
                    completion->wake.notify_one();
                });
                SCOPE_EXIT {
                    if (system.IsPoweredOn()) system.ShutdownMainProcess();
                    system.RegisterExitCallback({});
                };
                // Like Eden's Qt/Android frontends, reset shutdown state for each load.
                system.SetShuttingDown(false);
                // Update and DLC files (NSP or XCI) in the game files' updates folder apply to their
                // base game: Eden's ExternalContentProvider, which the patch manager checks first.
                Settings::values.external_content_dirs = {Eden::AssetsPath("updates")};
                system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
                // The game's own contents, registered as Eden's desktop frontends do before booting a
                // game (ConfigureFilesystemProvider). An update's data is a patch on the base game's:
                // the patch manager looks the base up here, and without it applied only the update's
                // code, which then ran against the old data. It also gives Eden the game's control data.
                game_contents.ClearAllEntries();
                if (game && guest && !game_contents.AddEntriesFromContainer(
                        system.GetFilesystem()->OpenFile(guest, FileSys::OpenMode::Read)))
                    Eden::Report("loader", "Game contents not registered; an update's data will not apply");
                system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, &game_contents);
                {
                    Service::AM::Frontend::FrontendAppletSet applets;
                    // A game's "connect controllers" screen: one player per PS5 controller in use.
                    if (pad) applets.controller = std::make_unique<Eden::PadControllerApplet>(system.HIDCore(), *pad);
                    // A game's error dialog: logged and closed, so the game carries on.
                    applets.error = std::make_unique<Eden::LoggedErrorApplet>();
                    // A game's text entry: the PS5's own on-screen keyboard.
                    applets.software_keyboard = std::make_unique<Eden::SystemKeyboardApplet>(Eden::AskSystemKeyboard);
                    system.GetFrontendAppletHolder().SetFrontendAppletSet(std::move(applets));
                }
                Service::AM::FrontendAppletParameters params{
                    .applet_id = Service::AM::AppletId::Application,
                };
#ifdef PS5_NATIVE
                // While a game loads nothing else reads the controller, so Touchpad + L1 did nothing
                // until the game ran: a load that hung could only be left by closing ProsperoEden.
                // Now it is heard here too. The stop limit starts at the press (stop_limit.h): a
                // load that finishes within it ends as a normal stop, one that does not ends with
                // the app starting again at the launcher.
                std::atomic<bool> left_while_loading{false};
                std::jthread load_watch;
                if (pad && game) load_watch = std::jthread([&](std::stop_token stop) {
                    while (!stop.stop_requested()) {
                        pad->Poll();
                        (void)pad->TakeHudToggle();
#if defined(EDEN_DEV_PROFILE) || defined(EDEN_DEV_ROM_ID)
                        // The runner's stand-in for the press: its input commands are read only
                        // once the game runs.
                        const bool asked = std::remove(Eden::AppFile("leave-loading.txt").c_str()) == 0;
#else
                        const bool asked = false;
#endif
                        if (pad->TakeReturnToMenu() || asked) {
                            Eden::Report("exit", "Touchpad + L1 while the game loads: leaving as soon as it can be stopped");
                            Eden::StopLimit::Begin();
                            left_while_loading = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(8));
                    }
                });
#endif
                Eden::Loading::Set(Eden::Loading::Step::game);
                Eden::BootTrace::Line("loading the game");
                Core::SystemResultStatus loaded;
                try {
                    loaded = system.Load(window, guest, params);
                } catch (const std::exception& error) {
                    // Preserve the original error before partial core teardown.
                    std::fprintf(stderr, "Game load failed: %s\n", error.what());
                    std::fflush(stdout);
                    throw;
                }
                if (loaded != Core::SystemResultStatus::Success) {
                    std::fprintf(stderr, "Core load failed: %u\n", static_cast<unsigned>(loaded));
                    if (loaded == Core::SystemResultStatus::ErrorVideoCore) {
                        throw std::runtime_error("Graphics backend initialization failed. Try another backend in Settings; see stderr.log and eden_log.txt for driver details.");
                    }
                    throw std::runtime_error("Loader status " + std::to_string(static_cast<unsigned>(loaded)) +
                        ". Check this ROM, its keys and firmware; see stderr.log.");
                }
#ifdef PS5_NATIVE
                if (game) {
                    const auto filename = std::filesystem::path(guest).filename().string();
                    const bool saved_last = Eden::SaveLastGame(filename);
                    const bool saved_recent = Eden::SaveRecentGame(filename);
                    if (!saved_last || !saved_recent)
                        Eden::Report("history", "Could not save recent game");
                }
#endif
                passed(game ? "game_loaded" : "nro_loaded");
#if defined(PS5_NATIVE) && (defined(EDEN_DEV_PROFILE) || defined(EDEN_DEV_ROM_ID))
                if (dev_load_hang) {
                    // dev-settings load_hang=on: a load that never finishes, to check leaving it.
                    Eden::Report("loader", "Development: holding the load to check leaving it");
                    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
                }
#endif
                Eden::BootTrace::Line("game loaded (status %u)", static_cast<unsigned>(loaded));
                Eden::Report("loader", "Game loaded; initializing renderer");
                Eden::Loading::Set(Eden::Loading::Step::graphics);
#ifdef EDEN_PS5_OPENGL
                // Retain the failure, then release CPU readiness and complete normal
                // shutdown before reporting it. Unwinding before OnGpuReady can hang.
                std::exception_ptr graphics_error;
#ifndef PS5_NATIVE
                std::shared_future<bool> game_capture;
                try { window.CheckPresentation(system.GPU().Renderer().Context()); }
                catch (...) { graphics_error = std::current_exception(); }
                if (game && !graphics_error)
                    game_capture = window.CaptureNextFrame(system.GPU().Renderer(), [completion] {
                        std::lock_guard lock(completion->mutex);
                        completion->captured = true;
                        completion->wake.notify_one();
                    });
#endif
#endif
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                Eden::Stall::Trace("main gpu_start");
#endif
                system.GPU().Start();
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                Eden::Stall::Trace("main gpu_started");
#endif
                system.GetCpuManager().OnGpuReady();
                passed("cpu_manager_ready");
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                Eden::Stall::Trace("main shader_cache");
#endif
#ifdef EDEN_PS5_OPENGL
                // Match yuzu_cmd's ordering: GPU/context ready, cache load, guest Run.
                if (Settings::values.use_disk_shader_cache.GetValue() && !graphics_error) {
                    // Every cached pipeline is built before the game starts; report progress
                    // every five seconds so a slow build can be told apart from a stalled one.
                    std::atomic<size_t> built{0}, total{0};
                    std::atomic<bool> counted{false};
                    Eden::Loading::Set(Eden::Loading::Step::shaders);
                    const auto load_start = std::chrono::steady_clock::now();
                    std::jthread reporter([&](std::stop_token stop) {
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                        Eden::Stall::Trace("reporter start");
#endif
                        for (unsigned tick = 1; !stop.stop_requested(); ++tick) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            if (tick % 50 != 0 || stop.stop_requested()) continue;
                            const std::string line = counted
                                ? fmt::format("Shader cache: {} of {} pipelines built ({} s)",
                                              built.load(), total.load(), tick / 10)
                                : fmt::format("Shader cache: reading ({} s)", tick / 10);
                            Eden::Report("loader", line.c_str());
                        }
                    });
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                    Eden::Stall::Trace("main cache_load");
#endif
                    system.Renderer().ReadRasterizer()->LoadDiskResources(
                        system.GetApplicationProcessProgramID(), std::stop_token{},
                        [&](VideoCore::LoadCallbackStage stage, size_t value, size_t count) {
                            if (stage != VideoCore::LoadCallbackStage::Build) return;
                            built = value;
                            Eden::Loading::Shaders(value, count);
                            total = count;
                            counted = true;
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                            Eden::Stall::Tick();
#endif
                        });
                    reporter.request_stop();
                    reporter.join();
                    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - load_start).count();
                    Eden::Report("loader", fmt::format("Shader cache ready: {} pipelines in {} ms",
                                                       total.load(), load_ms).c_str());
                    std::puts("EDEN_SHADER_CACHE_LOADED");
                    std::fflush(stdout);
                }
#endif
#ifndef PS5_NATIVE
                if (devices) Eden::Mock::BeginGuestCycle();
#endif
#ifdef EDEN_DEV_PROFILE
                if (auto* process = system.ApplicationProcess()) {
                    // P1: guest code reads for the core-0 profile's block dumps.
                    static Core::Memory::Memory* dev_guest_memory = nullptr;
                    dev_guest_memory = &process->GetMemory();
                    Eden::Performance::guest_read32 = [](unsigned long long address, unsigned& value) {
                        if (!dev_guest_memory || !dev_guest_memory->IsValidVirtualAddressRange(address, 4)) return false;
                        value = dev_guest_memory->Read32(address);
                        return true;
                    };
                    std::printf("EDEN_MAIN_BASE main=%llx\n",
                                static_cast<unsigned long long>(GetInteger(Core::FindMainModuleEntrypoint(process))));
                }
                if (auto* process = system.ApplicationProcess();
                    process && (Eden::Watch::watch_range.size || Eden::Watch::dump_range.size)) {
                    // Resolve dev-settings watch=/dump= against this boot's main module.
                    const u64 main_base = GetInteger(Core::FindMainModuleEntrypoint(process));
                    auto& memory = process->GetMemory();
                    if (const auto range = Eden::Watch::dump_range; range.size) {
                        for (u64 at = main_base + range.offset; at < main_base + range.offset + range.size; at += 0x40) {
                            std::string line = fmt::format("EDEN_CRASH_CODE dump ret={:016X} from={:016X}:", at, at);
                            for (u64 word = at; word < at + 0x40; word += 4)
                                line += fmt::format(" {:08X}", memory.IsValidVirtualAddressRange(word, 4) ? memory.Read32(word) : 0u);
                            LOG_CRITICAL(Core_ARM, "{}", line);
                        }
                    }
                    if (const auto range = Eden::Watch::watch_range; range.size) {
                        const u64 first = main_base + range.offset;
                        Eden::Watch::end = first + range.size;
                        Eden::Watch::begin = first;
                        memory.MarkRegionDebug(first, range.size, true);
                    }
                    LOG_CRITICAL(Core_ARM, "EDEN_WATCH main={:016X} watch=+{:#x}:{:#x} dump=+{:#x}:{:#x}", main_base,
                                 Eden::Watch::watch_range.offset, Eden::Watch::watch_range.size,
                                 Eden::Watch::dump_range.offset, Eden::Watch::dump_range.size);
                }
#endif
                // The blocks this game compiled in earlier sessions, compiled ahead on a spare CPU.
                if (std::filesystem::exists(Eden::AppFile("block-list.txt"))) Eden::JitList::enabled = true;
                Eden::JitList::Session jit_list;
                if (auto* process = system.ApplicationProcess()) {
                    Eden::JitList::BuildId build{};
                    const auto& id = system.GetApplicationProcessBuildID();
                    std::memcpy(build.data(), id.data(), std::min(build.size(), id.size()));
                    // The game's own modules: the run of code mappings from its entry point.
                    const u64 code_start = GetInteger(process->GetEntryPoint());
                    u64 code_end = code_start;
                    for (;;) {
                        Kernel::KMemoryInfo info{};
                        Kernel::Svc::PageInfo page{};
                        if (process->GetPageTable().QueryInfo(&info, &page, code_end).IsError() || info.m_size == 0 ||
                            (info.m_state != Kernel::KMemoryState::Code && info.m_state != Kernel::KMemoryState::CodeData))
                            break;
                        code_end = info.m_address + info.m_size;
                    }
                    jit_list.Start(system.GetApplicationProcessProgramID(), build, code_start, code_end - code_start);
                }
                Eden::TakeGuestFault(); // Nothing from an earlier session belongs to this one.
                Eden::Loading::Set(Eden::Loading::Step::starting);
                system.Run();
#if defined(EDEN_DEV_PROFILE) && defined(PS5_NATIVE)
                Eden::Stall::Trace("main running");
                Eden::Stall::Disarm();
#endif
#ifdef PS5_NATIVE
                // The controller goes to the session's own reader from here.
                load_watch.request_stop();
                if (load_watch.joinable()) load_watch.join();
                if (left_while_loading) {
                    std::lock_guard lock(completion->mutex);
                    completion->return_to_menu = true;
                }
#endif
                const auto session_start = std::chrono::steady_clock::now();
                [[maybe_unused]] double session_seconds = 0;  // guest-fault relaunch (PS5 only)
                std::jthread input_worker;
                if (pad) input_worker = std::jthread([&](std::stop_token stop) {
#ifdef EDEN_DEV_PROFILE
                    // The timed input replay drives the profile's own title (EDEN_DEV_PROFILE_TITLE);
                    // other development titles take runner commands (compat-input.txt).
                    bool replay_off = false;
                    {
                        std::ifstream dev_settings(Eden::AppFile("dev-settings.txt"));
                        for (std::string entry; dev_settings >> entry;) replay_off |= entry == "replay=off";
                    }
                    const bool timed_replay = development_id == EDEN_DEV_PROFILE_TITLE && !replay_off;
                    const auto replay_start = std::chrono::steady_clock::now();
#endif
#ifdef EDEN_DEV_ROM_ID
                    Eden::DevelopmentInput development_input;
                    unsigned command_poll = 0;
#endif
                    while (!stop.stop_requested()) {
                        if (auto error = Eden::TakeGpuFailure()) {
                            std::lock_guard lock(completion->mutex);
                            completion->failure = std::move(error);
                            completion->wake.notify_one();
                            break;
                        }
                        if (auto fault = Eden::TakeGuestFault(); !fault.empty()) {
                            std::lock_guard lock(completion->mutex);
                            completion->guest_fault = std::move(fault);
                            completion->wake.notify_one();
                            break;
                        }
#ifdef EDEN_DEV_PROFILE
                        if (!timed_replay) {
#endif
#ifdef EDEN_DEV_ROM_ID
                        if (!development_input.active) { UpdateGrips(system, *pad); pad->Poll(); }
#else
                        UpdateGrips(system, *pad);
                        pad->Poll();
#endif
#ifdef EDEN_DEV_ROM_ID
                        const auto command_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        if (++command_poll >= 25) {
                            command_poll = 0;
                            std::ifstream command(Eden::AppFile("compat-input.txt"));
                            if (development_input.Read(command, command_now))
                                std::printf("EDEN_DEV_INPUT sequence=%llu buttons=%x\n",
                                    static_cast<unsigned long long>(development_input.sequence), development_input.buttons);
                        }
                        if (const auto sample = development_input.Sample(command_now))
                            pad->Consume({&*sample, 1});
                        // The guest's Minus going down and up, with how long the state before it
                        // lasted: the touchpad's tap and long press (headless/pad.cpp) on real timing.
                        {
                            static bool minus_down = false;
                            static auto minus_since = std::chrono::steady_clock::now();
                            const bool down = pad->Engine().GetButton({}, static_cast<int>(
                                InputCommon::VirtualGamepad::VirtualButton::ButtonMinus));
                            if (down != minus_down) {
                                const auto changed = std::chrono::steady_clock::now();
                                std::printf("EDEN_DEV_MINUS state=%d after_ms=%lld\n", down,
                                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        changed - minus_since).count()));
                                minus_down = down;
                                minus_since = changed;
                            }
                        }
#endif
#ifdef EDEN_DEV_PROFILE
                        } else {
                        using Button = InputCommon::VirtualGamepad::VirtualButton;
                        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - replay_start).count();
                        // Development replay: enter Single Player/Grand Prix using defaults.
                        pad->Engine().SetButtonState(0, Button::TriggerL, seconds == 45);
                        pad->Engine().SetButtonState(0, Button::TriggerR, seconds == 45);
                        pad->Engine().SetButtonState(0, Button::ButtonA,
                            (seconds >= 50 && seconds < 200 && seconds % 5 == 0) || seconds >= 200);
                        }
#endif
                        if (const unsigned changed = pad->TakeConnectionChanges()) {
                            // A controller that came or went connects or disconnects its player.
                            const unsigned connected = pad->ConnectedPlayers();
                            for (std::size_t index = 1; index < Eden::Pad::kMaxPlayers; ++index) {
                                if (!(changed & (1u << index))) continue;
                                const bool present = (connected & (1u << index)) != 0;
                                Settings::values.players.GetValue()[index].connected = present;
                                auto* controller = system.HIDCore().GetEmulatedControllerByIndex(index);
                                // With the handheld chosen, the game has one player.
                                const auto chosen = Eden::ChosenStyle(Eden::session_controller.load());
                                if (present && chosen == Core::HID::NpadStyleIndex::Handheld) continue;
                                if (present) {
                                    controller->SetNpadStyleIndex(chosen.value_or(Core::HID::NpadStyleIndex::Fullkey));
                                    controller->Connect();
                                } else {
                                    controller->Disconnect();
                                }
                                LOG_INFO(Input, "EDEN_PLAYER player={} connected={}", index + 1, present);
                            }
                        }
#ifdef EDEN_PS5_OPENGL
                        if (pad->TakeHudToggle()) Eden::ToggleHud();
#else
                        (void)pad->TakeHudToggle();
#endif
                        bool room_lost = false;
#ifdef PS5_NATIVE
                        room_lost = rooms.GameConnectionLost();
#endif
                        if (pad->TakeReturnToMenu() || room_lost) {
                            // From here the player is waiting to leave (stop_limit.h).
                            Eden::StopLimit::Begin();
                            std::lock_guard lock(completion->mutex);
                            completion->return_to_menu = true;
                            completion->room_lost = room_lost;
                            completion->wake.notify_one();
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    }
                });
                {
                    std::unique_lock lock(completion->mutex);
                    constexpr unsigned stop_delays_ms[] = {0, 1, 10, 100, 1000, 30000};
                    const auto wait_ms = shutdown_sweep ? stop_delays_ms[cycle] :
                        1000u * (cpu_pressure ? 180u : game ? 600u : devices ? 20u : 5u);
                    if (shutdown_sweep) LOG_INFO(Frontend, "EDEN_SHUTDOWN_DELAY_MS {}", wait_ms);
                    const auto completed = [&] {
                        return completion->failure || completion->exited || completion->captured ||
                            completion->return_to_menu || !completion->guest_fault.empty();
                    };
#ifdef EDEN_DEV_PROFILE
                    // The 30-second segments a development session samples before it only waits.
                    // On Vulkan a picture can be asked for (capture-once.txt) for as long as the
                    // game runs.
#ifdef EDEN_DEV_VULKAN
                    [[maybe_unused]] constexpr unsigned segments = ~0u;
#else
                    [[maybe_unused]] constexpr unsigned segments = 12;
#endif
#endif
#ifdef PS5_NATIVE
                    if (game) {
#ifdef EDEN_DEV_PROFILE
                        for (unsigned segment = 0; segment < segments; ++segment) {
                            bool finished = false;
                            for (unsigned poll = 0; poll < 600; ++poll) {
                                if (completion->wake.wait_for(lock, std::chrono::milliseconds(50), completed)) {
                                    finished = true;
                                    break;
                                }
                                // The runner's crash request, to test the crash report in a game.
                                if (poll % 20 == 0) {
                                    lock.unlock();
                                    Eden::Crash::DevelopmentRequest(Eden::AppFile("crash-app.txt"));
                                    lock.lock();
                                }
#ifndef EDEN_DEV_VULKAN
                                if (segment >= 5) {
                                    lock.unlock();
                                    Eden::Performance::PollGpuPc();
                                    lock.lock();
                                }
#else
                                // Cover the heavy phase (wall ~90-180 s) when requested.
                                if (pc_sample_run && segment >= 3) {
                                    lock.unlock();
                                    Eden::Performance::PollGpuPc();
                                    lock.lock();
                                }
#endif
                            }
                            if (finished) break;
                            if (segment == segments - 1) {
                                completion->wake.wait(lock, completed);
                                break;
                            }
                            lock.unlock();
#ifndef EDEN_DEV_VULKAN
                            Eden::Performance::Snapshot();
                            window.CaptureNextFrame(system.GPU().Renderer(), [] {});
#else
                            std::error_code capture_error;
                            if ((segment >= 4 || Eden::Performance::capture_early) &&
                                std::filesystem::remove(Eden::AppFile("capture-once.txt"), capture_error))
                                window.CaptureNextFrame(system.GPU().Renderer(), [] {});
#endif
                            lock.lock();
                        }
#elif defined(EDEN_DEV_ROM_ID)
                        // Bounded compatibility probe (10 minutes; the runner usually closes
                        // the title first); use the ordinary teardown path.
                        constexpr unsigned observation_samples = 120;
                        for (unsigned sample = 0; sample < observation_samples; ++sample) {
                            if (completion->wake.wait_for(lock, std::chrono::seconds(5), completed)) break;
                            if (sample == observation_samples - 1) {
                                completion->return_to_menu = true;
                                break;
                            }
                            lock.unlock();
#ifdef EDEN_DEV_VULKAN
                            std::error_code capture_error;
                            const bool capture_requested = performance_run && sample >= 11 &&
                                std::filesystem::remove(Eden::AppFile("capture-once.txt"), capture_error);
                            if (!performance_run || capture_requested)
#endif
                            window.CaptureNextFrame(system.GPU().Renderer(), [] {});
                            lock.lock();
                        }
#else
                        completion->wake.wait(lock, completed);
#endif
                    } else
#endif
                    completion->wake.wait_for(lock, std::chrono::milliseconds(wait_ms), completed);
                    const bool guest_exited = completion->exited;
                    return_to_menu = completion->return_to_menu;
#ifdef PS5_NATIVE
                    if (completion->room_lost)
                        launch_error = "The room connection was lost. Rejoin from Multiplayer before launching again.";
#endif
                    session_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - session_start).count();
                    if (!guest_exited && !game) {
                        std::fputs("Guest exit timed out\n", stderr);
                        return 1;
                    }
                    passed(return_to_menu ? "shortcut_return_to_menu" :
                        guest_exited ? "guest_exit_callback" : !completion->guest_fault.empty() ? "guest_fault" :
                        completion->captured ? "game_capture_shutdown" : "game_observation_complete");
                }
                input_worker.request_stop();
                if (input_worker.joinable()) input_worker.join();
                if (pad) {
                    const auto neutral = ps5::pad::neutral_data();
                    pad->Consume({&neutral, 1});
                }
                // A game that ended itself or failed is being stopped too (stop_limit.h).
                Eden::StopLimit::Begin();
                jit_list.Finish();  // while the JITs still exist: their block list is saved
                // Shutdown requests cancellation before suspending cores; Pause can
                // block while a CPU producer is waiting on a full GPU queue.
                Eden::ReportStep("shutdown", "Stopping the game");
#if defined(EDEN_DEV_PROFILE) || defined(EDEN_DEV_ROM_ID)
                if (dev_stop_hang) {
                    // dev-settings stop_hang=on: a stop that never finishes, to check its limit.
                    Eden::Report("shutdown", "Development: holding the stop to check its limit");
                    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
                }
#endif
                system.ShutdownMainProcess();
                Eden::Report("shutdown", "Game stopped; releasing renderer");
#ifndef PS5_NATIVE
                if (devices) {
                    Eden::Mock::CheckGuestCycle();
                    LOG_INFO(Frontend, "EDEN_GUEST_AUDIO_OUTPUT_PASS");
                    LOG_INFO(Frontend, "EDEN_GUEST_RENDERER_PCM_PASS");
                }
#endif
                if (system.IsPoweredOn()) return 1;
                passed("core_shutdown");
                if (completion->failure) {
                    try { std::rethrow_exception(completion->failure); }
                    catch (const std::exception& error) {
                        // Out of graphics memory: seen with the resolution above 1x in a game that
                        // runs at 1x. The launcher shows this sentence in the player's language
                        // (prosperoeden/eden_services.cpp, kLaunchErrors).
                        const std::string what = error.what();
                        if (what.find("OUT_OF_DEVICE_MEMORY") != std::string::npos ||
                            what.find("OUT_OF_HOST_MEMORY") != std::string::npos)
                            throw std::runtime_error("The game ran out of graphics memory. Lower the resolution in "
                                                     "Settings, Video (or in the game's own settings) and start it again.");
                        throw std::runtime_error("Rendering failed: " + what +
                            ". Try another graphics backend in Settings, then reopen the game.");
                    }
                }
                if (!completion->guest_fault.empty()) {
#ifdef PS5_NATIVE
                    // A game can run its save-load completion before its own callback exists
                    // (a boot race between two guest threads); a fresh boot normally passes. Retry early faults, report others.
                    // Four retries: two faults in a row were seen with slower GPU synchronization
                    // (RADV_DEBUG=syncshaders), so the per-boot rate can exceed the usual ~1 in 5.
                    if (game && session_seconds < 60 && guest_fault_retries < 4) {
                        ++guest_fault_retries;
                        relaunch_game = selected_game;
                        return_to_menu = true;
                        LOG_WARNING(Frontend, "EDEN_GUEST_FAULT_RETRY {} after {:.1f} s: {}", guest_fault_retries,
                                    session_seconds, completion->guest_fault);
                        Eden::Report("guest fault", ("Restarting the game: " + completion->guest_fault).c_str());
                    } else
#endif
                    {
                        throw std::runtime_error("The game stopped: " + completion->guest_fault +
                            ". Reopen it from the launcher.");
                    }
                }
#ifdef EDEN_PS5_OPENGL
                if (graphics_error) std::rethrow_exception(graphics_error);
#ifndef PS5_NATIVE
                if (game) {
                    bool captured = false;
                    try {
                        captured = game_capture.wait_for(std::chrono::seconds(2)) ==
                            std::future_status::ready && game_capture.get();
                    } catch (const std::future_error& error) {
                        if (error.code() != std::make_error_code(std::future_errc::broken_promise)) throw;
                    }
                    if (!captured) throw std::runtime_error("No completed game frame capture");
                }
#endif
#endif
                if (game) LOG_INFO(Frontend, "EDEN_GAME_SESSION_END {}", cycle + 1);
            }
        }
        passed("core_destroyed");
        if (devices && pad) {
            pad.reset();
            LOG_INFO(Frontend, "EDEN_DEVICE_FRONTEND_PASS");
        }
#ifdef PS5_NATIVE
        if (return_to_menu) continue;
#endif
        passed("HEADLESS_COMPLETE");
#ifdef EDEN_DEV_PROFILE
        // Keep the sandbox mounted long enough to collect final driver counters.
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(20));
#endif
        return 0;
#ifdef PS5_NATIVE
        } catch (const std::exception& error) {
            launch_error = error.what();
            Eden::Report("session failed", error.what());
            std::fflush(stderr);
            std::fflush(stdout);
        }
        }
#endif
    } catch (const std::exception& error) {
        std::fflush(stdout);
        Eden::Report("fatal startup failure", error.what());
        return 1;
    }
}
