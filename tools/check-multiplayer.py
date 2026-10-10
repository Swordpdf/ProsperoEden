#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and exercise the actual derived Eden packet implementation, offline."""
import argparse
import os
from pathlib import Path
import subprocess
import socket
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="Pinned Eden source tree")
    parser.add_argument("--host-cache", type=Path,
                        help="Optional existing host cache with common/ENet/fmt archives")
    parser.add_argument("--thread-sanitizer", action="store_true",
                        help="Check data races instead of address/undefined behavior")
    args = parser.parse_args()
    sanitizer = "-fsanitize=thread" if args.thread_sanitizer else "-fsanitize=address,undefined"
    root = Path(__file__).resolve().parents[1]
    source = args.source.resolve()
    with tempfile.TemporaryDirectory(prefix="eden-multiplayer-") as directory:
        output = Path(directory)
        subprocess.run([
            "cmake", f"-DMULTIPLAYER_SOURCE={source}", f"-DMULTIPLAYER_OUTPUT={output}",
            "-P", str(root / "headless/multiplayer_packets.cmake"),
        ], check=True)
        executable = output / "packets-check"
        includes = [root / "headless", source / ".cache/cpm/fmt/12.1.0/include",
                    source.parent / "fmt-12.1.0/include"]
        subprocess.run([
            os.environ.get("CXX", "clang++"), "-std=c++20", "-g", "-O1",
            sanitizer, "-fno-omit-frame-pointer",
            "-I", str(output), "-I", str(source / "src"),
            *[arg for path in includes for arg in ("-I", str(path))],
            str(root / "headless/multiplayer_packets_check.cpp"),
            str(output / "packet.cpp"), "-lzstd", "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
        if args.host_cache:
            bsd = (output / "bsd.cpp").read_text()
            destructor = bsd.split("BSD_USA::~BSD_USA() {", 1)[1].split(
                "std::unique_lock<std::mutex> BSD_USA::LockService()", 1)[0]
            (output / "bsd_lifetime.cpp").write_text("BSD_USA::~BSD_USA() {" + destructor)
            (output / "bsd_helpers.cpp").write_text(bsd.split("namespace {", 1)[1].split(
                "} // Anonymous namespace", 1)[0])
            cache = args.host_cache.resolve()
            fmt = cache / "source/.cache/cpm/fmt/12.1.0/include"
            enet = cache / "source/.cache/cpm/enet/v1.3.18/include"
            executable = output / "room-check"
            subprocess.run([
                os.environ.get("CXX", "clang++"), "-std=c++20", "-g", "-O1", "-pthread",
                sanitizer, "-fno-omit-frame-pointer",
                "-I", str(output / "socket-headers"), "-I", str(output), "-I", str(root / "headless"),
                "-I", str(source / "src"), "-I", str(fmt), "-I", str(enet),
                str(root / "headless/multiplayer_room_check.cpp"),
                str(root / "headless/multiplayer.cpp"),
                str(output / "room_member.cpp"), str(output / "packet.cpp"),
                str(output / "socket_proxy.cpp"),
                str(source / "src/core/hle/service/sockets/sockets_translate.cpp"),
                str(output / "socket_network.cpp"),
                str(source / "src/core/internal_network/network_interface.cpp"),
                *[str(source / "src/network" / name) for name in
                  ("network.cpp", "room.cpp", "verify_user.cpp")],
                str(cache / "build/src/common/libcommon.a"),
                str(cache / "build/_deps/enet-build/libenet.a"),
                str(cache / "build/_deps/fmt-build/libfmt.a"),
                "-lzstd", "-lssl", "-lcrypto", "-lz", "-lbrotlienc", "-lbrotlidec",
                "-lbrotlicommon", "-o", str(executable),
            ], check=True)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            environment = dict(os.environ, XDG_DATA_HOME=str(output), XDG_CONFIG_HOME=str(output))
            subprocess.run([str(executable), str(port)], check=True, timeout=40, env=environment)
            print("Real room: password rejection, retry, two members, LDN broadcast, proxy routing, loss: PASS")
            print("Proxy socket: decompression, queue limits, truncation, partial reads, recovery: PASS")
            print("Proxy ownership: real-room delivery, shared references, concurrent destruction, close wakeup: PASS")
            print("Proxy poll: timeout, readiness, mixed native sockets, shutdown interrupt, close: PASS")
            print("Generated BSD teardown: shared table survives until final service, closes and clears for next game: PASS")
            print("BSD descriptors: concurrent ownership, duplicate/close, fd reuse, poll buffers and close-during-poll: PASS")
            print("Room controller: hostname, validation, retry, members, leave, loss, cancellation: PASS")
            print("Room send budgets: packet/byte/count limits and stalled ENet peer: PASS")
    print("Multiplayer packet round-trip, truncation, allocation bounds and overflow: PASS")


if __name__ == "__main__":
    main()
