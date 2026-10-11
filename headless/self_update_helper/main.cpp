// ps5-native-app-boilerplate - The self-update helper, a payload for the console's loader.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app sends this program to the payload loader (loopback port 9021). The
// loader starts it and hands it the same connection as its standard input and
// output, where it speaks self_update_protocol.h. It stays alive after the
// app has closed, which is when it puts the new files in place.
#include "updater.hpp"
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mount.h>
#include <sys/param.h>
#include <unistd.h>

extern "C"
{
    int sceKernelSendNotificationRequest(int device, void *request, std::size_t size, int blocking);
    int klog_puts(const char *text);
}

namespace
{
struct NotificationRequest
{
    std::uint8_t reserved[45];
    char message[3075];
};

void notify(const std::string &message)
{
    static NotificationRequest request;
    std::memset(&request, 0, sizeof(request));
    std::snprintf(request.message, sizeof(request.message), "%s", message.c_str());
    (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

bool free_space(const std::string &path, std::uint64_t &bytes)
{
    struct statfs info
    {
    };
    if (statfs(path.c_str(), &info) != 0 || info.f_bavail < 0)
        return false;
    bytes = static_cast<std::uint64_t>(info.f_bavail) * static_cast<std::uint64_t>(info.f_bsize);
    return true;
}
} // namespace

int main()
{
    std::signal(SIGPIPE, SIG_IGN);
    self_update::Environment environment;
    environment.roots = self_update::default_roots();
    environment.drives = self_update::default_drives();
    environment.space = free_space;
    environment.notify = notify;
    // The version an update replaces stays here until the next update.
    environment.previous = "/data/prosperoeden/previous-version";
    environment.log = [](const std::string &line)
    { (void)klog_puts(("[self-update] " + line).c_str()); };
    const self_update::Io io{[](void *data, std::size_t size)
                             { return static_cast<long>(read(STDIN_FILENO, data, size)); },
                             [](const void *data, std::size_t size)
                             { return static_cast<long>(write(STDOUT_FILENO, data, size)); }};
    return self_update::run(environment, io);
}
