// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of the self-update helper's move_tree (self_update_helper/files.cpp): the version an
// update replaces is moved to where it is kept, on the app's drive or on another one.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include "self_update_helper/files.hpp"

namespace
{
namespace fs = std::filesystem;
int failures = 0;

void expect(bool ok, const char *what)
{
    std::printf("%s self-update files: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

std::string read(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

// An app folder as an update leaves it aside: files, a folder in a folder, a mode that matters.
void make_app(const fs::path &root)
{
    fs::create_directories(root / "sce_sys");
    fs::create_directories(root / "ui/lang");
    std::ofstream(root / "eboot.bin") << std::string(300000, 'e');
    std::ofstream(root / "sce_sys/param.json") << "{\"contentVersion\": \"01.000.095\"}";
    std::ofstream(root / "ui/lang/de-DE.po") << "msgid";
    chmod((root / "eboot.bin").c_str(), 0777);
    chmod((root / "ui").c_str(), 0777);
    (void)!symlink("/etc/passwd", (root / "link").c_str());
}

bool whole(const fs::path &root)
{
    struct stat eboot
    {
    };
    struct stat ui
    {
    };
    return read(root / "eboot.bin") == std::string(300000, 'e') &&
           read(root / "sce_sys/param.json") == "{\"contentVersion\": \"01.000.095\"}" &&
           read(root / "ui/lang/de-DE.po") == "msgid" && stat((root / "eboot.bin").c_str(), &eboot) == 0 &&
           (eboot.st_mode & 0777) == 0777 && stat((root / "ui").c_str(), &ui) == 0 && (ui.st_mode & 0777) == 0777;
}
} // namespace

int main()
{
    const fs::path base = fs::temp_directory_path() / ("eden-update-files-" + std::to_string(getpid()));
    fs::remove_all(base);
    fs::create_directories(base);

    make_app(base / "backup");
    expect(self_update::move_tree((base / "backup").string(), (base / "kept").string()) &&
               whole(base / "kept") && !fs::exists(base / "backup"),
           "on one drive the folder is moved whole");
    expect(!self_update::move_tree((base / "missing").string(), (base / "other").string()) &&
               !fs::exists(base / "other"),
           "a folder that is not there is not moved");
    make_app(base / "backup");
    expect(!self_update::move_tree((base / "backup").string(), (base / "kept").string()) &&
               whole(base / "backup") && whole(base / "kept"),
           "nothing is moved onto a folder that exists");

    // Another drive: /dev/shm is one on Linux, where the checks run.
    const fs::path other = fs::path("/dev/shm") / ("eden-update-files-" + std::to_string(getpid()));
    struct stat here
    {
    };
    struct stat there
    {
    };
    std::error_code error;
    fs::create_directories(other, error);
    if (!error && stat(base.c_str(), &here) == 0 && stat(other.c_str(), &there) == 0 && here.st_dev != there.st_dev)
    {
        expect(self_update::move_tree((base / "backup").string(), (other / "kept").string()) &&
                   whole(other / "kept") && !fs::exists(base / "backup"),
               "onto another drive it is copied with its modes, and the original removed");
        expect(!fs::exists(other / "kept/link") && !fs::is_symlink(other / "kept/link"),
               "a link in it is not followed and not kept");
    }
    else
    {
        std::printf("SKIP self-update files: no second drive to copy onto here\n");
    }
    fs::remove_all(other, error);
    fs::remove_all(base);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
