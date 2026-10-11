#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and exercise the actual derived Eden packet implementation, offline."""
import argparse
import json
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
        logging_source = output / "logging-check.cpp"
        logging_source.write_text('''#include "multiplayer_logging.h"
int main() {
    using namespace Eden::Multiplayer;
    Debug("hidden initially");
    Eden::Report("multiplayer", "ordinary error");
    SetDetailedLogging(true);
    Debug("enabled detail");
    SetDetailedLogging(false);
    Debug("hidden after toggle");
}
''')
        logging_check = output / "logging-check"
        subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++20", "-I", str(root / "headless"),
                        str(logging_source), "-o", str(logging_check)], check=True)
        logged = subprocess.run([str(logging_check)], capture_output=True, text=True, check=True)
        assert logged.stderr == ("[ProsperoEden] multiplayer: ordinary error\n"
                                 "[ProsperoEden] multiplayer debug: enabled detail\n")
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
            boost_version = json.loads((source / "cpmfile.json").read_text())["boost"]["version"]
            boost = sorted((cache / f"source/.cache/cpm/boost/{boost_version}/libs").glob("*/include"))
            assert boost, "Build the host cache with this Eden revision before running the check"
            executable = output / "room-check"
            subprocess.run([
                os.environ.get("CXX", "clang++"), "-std=c++20", "-g", "-O1", "-pthread",
                sanitizer, "-fno-omit-frame-pointer",
                "-I", str(output / "socket-headers"), "-I", str(output), "-I", str(root / "headless"),
                "-I", str(source / "src"), "-I", str(fmt), "-I", str(enet),
                *[arg for path in boost for arg in ("-I", str(path))],
                str(root / "headless/multiplayer_room_check.cpp"),
                str(root / "headless/multiplayer.cpp"),
                str(output / "room_member.cpp"), str(output / "packet.cpp"),
                str(output / "socket_proxy.cpp"),
                str(source / "src/core/hle/service/sockets/sockets_translate.cpp"),
                str(output / "socket_network.cpp"),
                str(output / "network_interface.cpp"), str(output / "lan_discovery.cpp"),
                *[str(source / "src/network" / name) for name in
                  ("network.cpp", "room.cpp", "verify_user.cpp")],
                str(cache / "build/src/common/libcommon.a"),
                str(cache / "build/_deps/enet-build/libenet.a"),
                str(cache / "build/_deps/fmt-build/libfmt.a"),
                "-Wl,--wrap=enet_address_set_host",
                "-lzstd", "-lssl", "-lcrypto", "-lz", "-lbrotlienc", "-lbrotlidec",
                "-lbrotlicommon", "-o", str(executable),
            ], check=True)
            environment = dict(os.environ, XDG_DATA_HOME=str(output), XDG_CONFIG_HOME=str(output))
            for detailed in (False, True):
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                    probe.bind(("127.0.0.1", 0))
                    port = probe.getsockname()[1]
                run = subprocess.run([str(executable), str(port), *(["debug"] if detailed else [])],
                                     capture_output=True, text=True, timeout=40, env=environment)
                if run.returncode:
                    print(run.stdout, run.stderr)
                    run.check_returncode()
                assert ("multiplayer debug:" in run.stderr) == detailed
                assert ("Transport: rtt_ms=" in run.stderr) == detailed
                assert ("Hostname lookup result=0 usable_ipv4=1" in run.stderr) == detailed
                assert "Incorrect password. Enter the room password again; it is case-sensitive." in run.stderr
                assert not any(value in run.stderr for value in ("secret", "PlayerOne", "localhost", "127.0.0.1"))
            print("Multiplayer logs: default off, live toggle, normal errors, transport details and privacy: PASS")
            print("Real room: password rejection, retry, two members, LDN broadcast, proxy routing, loss: PASS")
            print("Proxy socket: decompression, queue limits, truncation, partial reads, recovery: PASS")
            print("Proxy ownership: real-room delivery, shared references, concurrent destruction, close wakeup: PASS")
            print("Proxy poll: timeout, readiness, mixed native sockets, shutdown interrupt, close: PASS")
            print("Generated BSD teardown: shared table survives until final service, closes and clears for next game: PASS")
            print("BSD descriptors: concurrent ownership, duplicate/close, fd reuse, poll buffers and close-during-poll: PASS")
            print("Room controller: hostname, validation, retry, members, leave, loss, cancellation: PASS")
            print("LDN: virtual IP/subnet without a host adapter, create, peer scan/connect, sync and destroy: PASS")
            print("Room rejection: duplicate nickname, full room, wire protocol mismatch and successful retry: PASS")
            print("Room send budgets: packet/byte/count limits and stalled ENet peer: PASS")
    print("Multiplayer packet round-trip, truncation, allocation bounds and overflow: PASS")


if __name__ == "__main__":
    main()
