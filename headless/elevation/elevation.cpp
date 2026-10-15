/*
 * ProsperoEden - PS5-Lapy-JB-Daemon elevation client.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "elevation.hpp"
#include "protocol.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
constexpr char request_path[] = "/download0/elevate_proc";
constexpr char result_path[] = "/download0/lapy_owned_result";
constexpr unsigned resident_polls = 30;
constexpr unsigned cancellation_grace_polls = 20;
constexpr useconds_t poll_interval_us = 50000;
const char *selected_path = "none";

struct NetSockaddrIn
{
    std::uint8_t length;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t address;
    std::uint16_t virtual_port;
    std::uint8_t zero[6];
};

extern "C"
{
    int sceNetConnect(int socket, const void *address, std::uint32_t address_length);
    int sceNetSend(int socket, const void *data, std::size_t length, int flags);
    int sceNetRecv(int socket, void *data, std::size_t length, int flags);
    int sceNetSetsockopt(int socket, int level, int option, const void *value, std::uint32_t size);
    int sceNetSocket(const char *name, int domain, int type, int protocol);
    int sceNetSocketClose(int socket);
}

class File
{
  public:
    explicit File(int descriptor = -1) noexcept : descriptor_{descriptor}
    {
    }
    ~File()
    {
        if (descriptor_ >= 0)
            (void)close(descriptor_);
    }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    [[nodiscard]] int get() const noexcept
    {
        return descriptor_;
    }
    int release() noexcept
    {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor;
    }

  private:
    int descriptor_;
};

bool write_all(int descriptor, const void *data, std::size_t length) noexcept
{
    auto *bytes = static_cast<const char *>(data);
    while (length != 0)
    {
        const auto count = write(descriptor, bytes, length);
        if (count <= 0 || static_cast<std::size_t>(count) > length)
            return false;
        bytes += count;
        length -= static_cast<std::size_t>(count);
    }
    return true;
}

bool publish_request(pid_t pid) noexcept
{
    std::array<char, 80> temporary{};
    std::array<char, 64> body{};
    const int body_length =
        std::snprintf(body.data(), body.size(), "{\"PID\":%ld}\n", static_cast<long>(pid));
    const int path_length = std::snprintf(temporary.data(), temporary.size(),
                                          "/download0/.elevate_proc.%ld", static_cast<long>(pid));
    if (body_length <= 0 || static_cast<std::size_t>(body_length) >= body.size() ||
        path_length <= 0 || static_cast<std::size_t>(path_length) >= temporary.size())
        return false;

    (void)unlink(temporary.data());
    File output{open(temporary.data(), O_WRONLY | O_CREAT | O_EXCL, 0644)};
    if (output.get() < 0 ||
        !write_all(output.get(), body.data(), static_cast<std::size_t>(body_length)))
    {
        (void)unlink(temporary.data());
        return false;
    }
    const int descriptor = output.release();
    if (close(descriptor) != 0 || rename(temporary.data(), request_path) != 0)
    {
        (void)unlink(temporary.data());
        return false;
    }
    return true;
}

bool verify_data(pid_t pid, int &open_error) noexcept
{
    std::array<char, 80> path{};
    constexpr std::array<char, 8> token{'L', 'A', 'P', 'Y', 'O', 'W', 'N', '\n'};
    std::array<char, token.size()> actual{};
    const int length =
        std::snprintf(path.data(), path.size(), "/data/.lapy_probe_%ld", static_cast<long>(pid));
    if (length <= 0 || static_cast<std::size_t>(length) >= path.size())
    {
        open_error = EOVERFLOW;
        return false;
    }
    (void)unlink(path.data());
    File file{open(path.data(), O_RDWR | O_CREAT | O_EXCL, 0600)};
    open_error = file.get() < 0 ? errno : 0;
    const bool passed =
        file.get() >= 0 && write_all(file.get(), token.data(), token.size()) &&
        lseek(file.get(), 0, SEEK_SET) == 0 &&
        read(file.get(), actual.data(), actual.size()) == static_cast<ssize_t>(actual.size()) &&
        actual == token;
    (void)unlink(path.data());
    return passed;
}

bool wait_for_data(pid_t pid, unsigned polls, int &open_error, bool &proof_failed) noexcept
{
    for (unsigned poll = 0; poll < polls; ++poll)
    {
        if (verify_data(pid, open_error))
            return true;
        if (open_error == 0)
        {
            proof_failed = true;
            return false;
        }
        (void)usleep(poll_interval_us);
    }
    return false;
}

void report_result(int descriptor, bool data_ok, int open_error) noexcept
{
    std::array<char, 96> result{};
    const int length = std::snprintf(result.data(), result.size(), "DATA_OK=%d OPEN_ERRNO=%d\n",
                                     data_ok, open_error);
    if (length > 0 && static_cast<std::size_t>(length) < result.size())
        (void)write_all(descriptor, result.data(), static_cast<std::size_t>(length));
}

struct ResidentResult
{
    elevation::Status status;
    bool allow_helper;
};

// OnionHEN's app jailbreak (for title IDs in its [app_jailbreak] exact_title_ids list) reads
// this file instead of Lapy's: it gives the process root credentials and the console's root.
// Written beside Lapy's request; whichever service is loaded answers, and /data proves it.
bool publish_onionhen_request(pid_t pid) noexcept
{
    constexpr char path[] = "/download0/onionhen_jailbreak";
    std::array<char, 80> temporary{};
    std::array<char, 64> body{};
    const int body_length =
        std::snprintf(body.data(), body.size(), "{\"PID\":\"%ld\"}\n", static_cast<long>(pid));
    const int path_length = std::snprintf(temporary.data(), temporary.size(),
                                          "/download0/.onionhen_jailbreak.%ld", static_cast<long>(pid));
    if (body_length <= 0 || static_cast<std::size_t>(body_length) >= body.size() ||
        path_length <= 0 || static_cast<std::size_t>(path_length) >= temporary.size())
        return false;
    (void)unlink(temporary.data());
    File output{open(temporary.data(), O_WRONLY | O_CREAT | O_EXCL, 0644)};
    if (output.get() < 0 ||
        !write_all(output.get(), body.data(), static_cast<std::size_t>(body_length)))
    {
        (void)unlink(temporary.data());
        return false;
    }
    const int descriptor = output.release();
    if (close(descriptor) != 0 || rename(temporary.data(), path) != 0)
    {
        (void)unlink(temporary.data());
        return false;
    }
    return true;
}

ResidentResult try_resident(pid_t pid) noexcept
{
    using elevation::Status;
    File result{open(result_path, O_WRONLY | O_CREAT | O_TRUNC, 0644)};
    if (result.get() < 0)
        return {Status::unavailable, true};
    (void)fchmod(result.get(), 0644);
    if (seteuid(geteuid()) != 0)
        return {Status::prepare_failed, false};
    const bool onionhen_asked = publish_onionhen_request(pid);
    if (!publish_request(pid) && !onionhen_asked)
        return {Status::transport_error, true};

    int open_error = 0;
    bool proof_failed = false;
    if (wait_for_data(pid, resident_polls, open_error, proof_failed))
    {
        report_result(result.get(), true, 0);
        return {Status::ok, false};
    }
    if (proof_failed)
    {
        report_result(result.get(), false, open_error);
        return {Status::apply_failed, false};
    }

    errno = 0;
    if (unlink(request_path) != 0)
    {
        // A missing marker means a resident service claimed it. Never start a
        // second helper for that launch; give the claimed request more time.
        if (errno != ENOENT)
            return {Status::transport_error, false};
        if (wait_for_data(pid, cancellation_grace_polls, open_error, proof_failed))
        {
            report_result(result.get(), true, 0);
            return {Status::ok, false};
        }
        report_result(result.get(), false, open_error);
        return {proof_failed ? Status::apply_failed : Status::timeout, false};
    }

    // The marker was cancelled before fallback. Wait once more in case a
    // resident read it immediately before cancellation, then choose one path.
    if (wait_for_data(pid, cancellation_grace_polls, open_error, proof_failed))
    {
        report_result(result.get(), true, 0);
        return {Status::ok, false};
    }
    (void)unlink(result_path);
    return {proof_failed ? Status::apply_failed : Status::timeout, !proof_failed};
}

bool send_all(int socket, const void *data, std::size_t size) noexcept
{
    return elevation::wire::transfer(static_cast<const std::uint8_t *>(data), size,
                                     [socket](const auto *bytes, std::size_t remaining)
                                     { return sceNetSend(socket, bytes, remaining, 0); });
}

bool receive(int socket, elevation::wire::Message &message) noexcept
{
    return elevation::wire::transfer(reinterpret_cast<std::uint8_t *>(&message), sizeof(message),
                                     [socket](auto *bytes, std::size_t remaining)
                                     { return sceNetRecv(socket, bytes, remaining, 0); });
}

elevation::Status exchange(int socket, const elevation::wire::Message &request) noexcept
{
    using namespace elevation;
    if (!send_all(socket, &request, sizeof(request)))
        return Status::transport_error;
    wire::Message reply{};
    if (!receive(socket, reply))
        return Status::transport_error;
    if (wire::matches(reply, request, wire::Kind::response) && reply.status != Status::ok)
        return reply.status;
    if (!wire::matches(reply, request, wire::Kind::prepare) || reply.status != Status::ok)
        return Status::protocol_error;

    wire::Message prepared = request;
    prepared.kind = wire::Kind::prepared;
    if (seteuid(geteuid()) != 0)
        prepared.status = Status::prepare_failed;
    if (!send_all(socket, &prepared, sizeof(prepared)) || !receive(socket, reply))
        return Status::transport_error;
    if (!wire::matches(reply, request, wire::Kind::response))
        return Status::protocol_error;
    if (prepared.status != Status::ok)
        return Status::prepare_failed;
    return reply.status;
}

elevation::Status run_helper(const char *path, const elevation::wire::Message &request) noexcept
{
    using elevation::Status;
    if (path == nullptr)
        return Status::invalid_request;
    File helper{open(path, O_RDONLY)};
    if (helper.get() < 0)
        return Status::unavailable;
    const int socket = sceNetSocket("lapy_owned_helper", 2, 1, 6);
    if (socket < 0)
        return Status::transport_error;

    Status status = Status::transport_error;
    constexpr int socket_level = 0xffff;
    constexpr int timeout_us = elevation::wire::io_timeout_us;
    bool configured = true;
    for (const int option : {0x1105, 0x1106, 0x1109})
        configured &= sceNetSetsockopt(socket, socket_level, option, &timeout_us,
                                      sizeof(timeout_us)) >= 0;
    constexpr std::uint16_t port = 9021;
    const NetSockaddrIn endpoint{sizeof(NetSockaddrIn),
                                2,
                                static_cast<std::uint16_t>((port << 8) | (port >> 8)),
                                0x0100007f,
                                0,
                                {0}};
    if (configured && sceNetConnect(socket, &endpoint, sizeof(endpoint)) >= 0)
    {
        std::array<std::uint8_t, 4096> buffer{};
        bool streamed = true;
        for (;;)
        {
            const auto count = read(helper.get(), buffer.data(), buffer.size());
            if (count == 0)
                break;
            if (count < 0 || !send_all(socket, buffer.data(), static_cast<std::size_t>(count)))
            {
                streamed = false;
                break;
            }
        }
        if (streamed)
            status = exchange(socket, request);
    }
    (void)sceNetSocketClose(socket);
    return status;
}
} // namespace

elevation::Status elevation::request(Capability capability, const char *helper_path) noexcept
{
    selected_path = "none";
    if (capability != Capability::filesystem)
        return Status::unsupported_capability;
    const pid_t pid = getpid();
    wire::Message message{};
    message.pid = static_cast<std::uint32_t>(pid);
    message.capability = capability;
    if (wire::validate(message) != Status::ok)
        return Status::invalid_request;

    int open_error = 0;
    if (verify_data(pid, open_error))
    {
        selected_path = "existing";
        return Status::ok;
    }
    if (open_error == 0)
        return Status::apply_failed;

    const ResidentResult resident = try_resident(pid);
    if (!resident.allow_helper)
    {
        selected_path = "resident";
        return resident.status;
    }
    selected_path = "helper";
    const Status helper = run_helper(helper_path, message);
    if (helper != Status::ok)
        return helper;

    bool proof_failed = false;
    return wait_for_data(pid, cancellation_grace_polls, open_error, proof_failed)
               ? Status::ok
               : proof_failed ? Status::apply_failed : Status::timeout;
}

const char *elevation::path() noexcept
{
    return selected_path;
}
