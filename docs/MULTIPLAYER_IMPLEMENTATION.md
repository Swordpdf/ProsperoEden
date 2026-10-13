# Local wireless rooms: implementation status

Work is rebased onto upstream `main` at `351057f` (2026-10-10), on branch
`feature/ldn-rooms`. The source inspection in [the implementation plan](MULTIPLAYER_PLAN.md)
predates this checkout; the new work does not include the old development branch's changes.

## Rebase and CI cache correction

The rebase includes Eden `67bada77` and upstream's display and Vulkan fixes. CI run
38097079300 linked successfully but the multiplayer header audit rejected two network
objects that depended on the original `room_member.h`. A focused reproduction confirmed
that ccache direct mode can reuse that dependency after a generated header starts
shadowing it. The network compiler launcher now hashes the generated room-member and
packet headers as extra cache inputs. Warm-cache shadowing and subsequent header-change
checks pass; the post-build header audit remains mandatory.

The rebased native build and mandatory checks passed, including the header audit. Packet
and real-room ASan/UBSan tests passed after rebuilding host libraries for the new Eden
settings layout; the test now reads Boost's version from Eden's dependency manifest.

## Server ping overlay

The OpenGL and Vulkan game overlays append `P42` for a 42 ms room-server round trip.
The Eden room receive thread copies ENet's existing measurement into an atomic field;
the renderer never reads the live ENet peer. Leaving or losing the room removes the
reading. ENet source, retransmission, timeouts and wire behavior are unchanged.

The native build and mandatory checks, formatting, bounded values, disconnect clearing,
real-room sanitizer tests and both shader compilations passed. The glyph capacity is 28 and the Vulkan push constants
remain within the guaranteed 128 bytes. Console display validation is still pending.

- 2026-10-10 | ping overlay | 8123fe3 | .30 FW 6.02 | pass: deployed, all 66 file hashes verified | results/multiplayer-ping-deploy | operator display test

## Multiplayer logging

Multiplayer diagnostic details now follow the saved Detailed logging preference at startup
and immediately after a successful Settings save. Launcher joins use the existing Report
writer because Eden's game logger is not yet running. Normal errors remain visible;
detailed lookup/handshake/state messages and five-second transport summaries are optional.
The summaries contain no credentials, endpoints, player names or packet contents. The
existing Write logs at once setting applies through the same stderr writer. Console
validation of this logging update is pending; the installed ping build predates it.
Native build/checks and real-room ASan/UBSan tests with logging on and off passed, including
live log-gate toggling, normal errors, detailed DNS/transport output and credential checks.

## Latest hardware result

The operator subsequently confirmed creating a lobby in the test game while connected to the
public server. This establishes operator-observed lobby creation; no second game joined
and no race was completed.

The later public-cloud room case passed on `.30` using the server's resolved IPv4
address and supplied UDP port/password. The host controller joined by hostname; a second
host join while the PS5 was connected reported two members. The PS5 launcher showed
Connected and the expected room name. The test game reached its title screen,
with the native log confirming game startup with room networking. Remote Play keyboard
input did not advance the title screen, so this case does not qualify cloud lobby creation
or racing. Both test launches closed and released their runtime layers.

Native hostname resolution failed before the room handshake, whereas the IPv4 control
passed with the same installed binary. This is an open PS5 DNS defect, not a failed public
room route or password. The current profile retains the working IPv4 endpoint and password;
the original configuration is backed up. Private endpoint/configuration evidence is in
`results/multiplayer-public-room` and `results/multiplayer-public-ipv4` and is not committed.

On 2026-10-10, `.30` (FW 6.02) joined the private room with its saved password,
launched the test game, selected Wireless Play, created a room, and
reached the Mii waiting lobby without the reported communication error. The candidate
uses the room-assigned virtual IPv4 address and /16 subnet for guest interface queries,
while preserving the native adapter fallback when offline. The host regression covers
LDN creation, peer discovery, connection, synchronization and network destruction.

This hardware case used source `540f9a9` with the existing development input runner
enabled for reliable timed button presses. The normal candidate uses the same networking
code. Exact package hashes and lifecycle evidence are in `results/multiplayer-development`.
Title-specific shutdown and runtime release passed; temporary test-control files were
restored. A subsequent corrected host probe discovered the PS5 lobby, connected a second
LDN node and received the two-node synchronization. It received 98 UDP proxy
packets (port 12345, 205 bytes) before sending Disconnect and leaving. This verifies the
PS5's guest discovery and outbound game traffic through a real room; the probe is not a
second running game and does not establish racing or inbound guest game-packet handling.

Stopping the owned temporary room server during that lobby caused normal game shutdown
and return to the launcher with a reconnect message. Title close and runtime release passed.
The normal package was restored and all 66 files verified; saved credentials were preserved.
Evidence is in `results/multiplayer-live-ldn-joined` and `results/multiplayer-normal-restored`.
The initial live probe was inconclusive because it used upstream `IsConnected()`, which
includes Joining, before sending Scan. The corrected probe waits for Joined, matching the
application's existing startup requirement. No production change was needed for that finding.

- 2026-10-10 | LDN hardware | 540f9a9 | FW 6.02 | partial-pass: wireless lobby created; clean teardown | results/multiplayer-development | validate second peer
- 2026-10-10 | LDN hardware | 27d6cb3 | FW 6.02 | partial-pass: peer scan/connect/sync, game packets, room-loss recovery | results/multiplayer-live-ldn-joined | qualify two-game racing

## Completed offline

- Parse numeric room addresses before ENet's hostname resolver. On PS5 the resolver failed
  before its numeric fallback; regression joins still work when hostname lookup is forced
  to fail. Preserve hostname resolution in the application controller.
- Preserve Eden's packet format while rejecting invalid lengths before allocation and
  preventing overflowing reads.
- Validate received room messages before existing handlers consume them, including
  membership counts, truncated messages and enum values.
- Bound ENet's incoming packet/reassembly allocation to 1 MiB for the client.
- Bound each outgoing stage (producer queue, drained batch and ENet outstanding packets)
  to 4 MiB / 1,024 packets, with a 1 MiB per-packet limit. Exhaustion ends the connection.
  A stalled ENet server regression verifies failure and subsequent return to offline mode.
  The three bounded stages can coexist; 4 MiB is not a total connection-memory limit.
- Validate LDN structure lengths, node counts and advertisement lengths before copies;
  reject duplicate/excess station connections.
- Reject malformed or oversized compressed proxy frames before payload allocation;
  cap each socket's receive queue at 4 MiB / 1,024 packets, including empty datagrams.
- Exercise actual proxy sockets for decompression, queue exhaustion, recovery, UDP
  truncation and TCP partial-read accounting. This does not implement the upstream TCP stubs.
- Give each proxy socket its own receive subscription, removed before destruction.
  Room callbacks no longer iterate BSD's mutable descriptor table or deliver duplicate
  packets through multiple BSD services/descriptors. Protect endpoint/queue metadata and
  cross-thread flags; closing a socket clears its queue and stops blocked receives.
  Real-room tests cover shared references, destruction during traffic and close wakeup.
- Implement proxy queue readiness through `Network::Poll`, including mixed native/proxy
  descriptors, finite/infinite waits, close notifications and the existing shutdown interrupt.
  Native-only polls retain the original path. Proxy polls bound native waits to 1 ms; their
  scheduling cost still needs console measurement. The native derivation retains main's
  socketpair interrupt implementation rather than replacing it with upstream pipe handling.
- Clear BSD's shared descriptor table when its final service is destroyed, closing sockets
  before releasing references. An offline fixture compiles the generated destructor with
  real proxy sockets and a minimal stand-in for the IPC class; it verifies the table survives
  earlier service destruction and is empty for the next game. The hardware cases above
  additionally verify normal game teardown and runtime release.
- Protect the BSD descriptor table with short locks and retain shared ownership for each
  socket operation and poll. Publish only initialized sockets, keep duplicated descriptors
  usable until their last alias closes, and preserve existing operations across descriptor
  reuse. Blocking calls run outside the table lock. Poll validates both buffer lengths and
  descriptor bounds; invalid entries report `Nval`, and negative entries are ignored.
  Tests compile the production operation overrides with the real socket and translation
  code, replacing only the unrelated IPC framework. They cover capacity, four concurrent
  descriptor workers, reuse, duplicate/close, malformed poll buffers and close during poll.
- Run the actual room server and derived client over loopback: wrong password, retry,
  two members with distinct virtual IPs, LDN broadcast, directed proxy traffic and server loss.
- Run the standalone controller against that server: hostname resolution, input validation,
  wrong-password retry, copied membership, leave/rejoin, room loss, join cancellation and
  destruction during a pending join. A raw ENet server verifies out-of-order join-success
  rejection. Cancellation and pending-join destruction complete within the two-second test limit.
- Verify duplicate-nickname and full-room failures against the real room server, plus a
  protocol-version mismatch supplied over ENet. Each reports its specific error and permits
  a successful later connection after correcting the conflict or using a compatible server.

## Controller and launcher integration

`headless/multiplayer.{h,cpp}` owns the application room client and serializes connect/leave
operations on a worker. The UI receives copied snapshots, including member names and specific
join errors. Nickname, host, port and password lengths are validated before connecting.
Numeric IPv4 endpoints bypass DNS; hostname lookups run separately with a five-second UI
deadline. Because libc DNS is not cancellable, one abandoned lookup may finish in the
background. Further hostname lookups are refused while it is still running; numeric endpoints
remain usable. The lookup owns no controller, ENet or game state.

The derived room client accepts an optional cancellation token for ENet connection attempts,
limits graceful disconnect cleanup to 500 ms, clears unsent messages on leave, and rejects
join-success messages received before member information. Existing callers keep their defaults.

The native application now initializes logging and one controller before its launcher loop.
The Multiplayer section of the menu offers address, UDP port, nickname and password entry, join/cancel,
room status and a scrollable member list with each reported game/version. The connected
view shows eight members at a time and keeps the scroll position valid when players leave.
The home footer adds `Multiplayer: Connected` alongside game count and firmware status
while joined, and removes it when disconnected. The Settings summary shows `Off` whenever
the room is not connected. Host previews cover both footer states and the offline summary;
these UI additions have not yet been deployed to the console.
Cross leaves from that view; Square returns to offline mode, including after a failed join.
Address, port, nickname and password use the existing per-profile settings store. Password persistence was explicitly requested during hardware testing.
Changing profiles requires leaving the room first. The system keyboard runs outside the draw
thread and is cancelled before launcher destruction.

Game startup accepts only idle or fully joined connections. While a game exists, the controller
refuses join/leave operations and preserves its socket mode through room loss: new BSD sockets
remain proxies until game teardown. The input worker requests the existing normal return-to-menu
shutdown path on room loss and displays a notice on return. The lobby case above
qualifies this recovery path; active races and subsequent sessions still need qualification.

Offline evidence: the real controller test covers game-start gating, blocked connect/leave during
a game, and the retained proxy-mode flag after loss. The existing settings regression passes with
password round-trip, clearing, length limits and unrelated preference preservation. The complete launcher preview suite
passes, including rendered offline/connecting/connected/failed room states and the Go offline
action; the connected dialog was visually inspected. Native SDK syntax checks pass for main,
frontend, services, controller and the derived BSD implementation, using cached SDK/dependency
headers. These are not a full native build or gameplay tests.

The isolated full native build has now linked the Vulkan/OpenGL application and passed
the build script's required post-build checks (startup, JIT, shutdown, worker placement,
GPU cancellation and existing lifecycle regressions). It exposed and fixed generated-header
ordering for the room controller. Native clock/topology calibration now runs once before
logging and multiplayer workers are created. This is build evidence, not console execution.

An additional Ninja dependency audit caught four cached object/header mismatches after
socket class overrides were introduced. The socket headers now use a distinct include root
to invalidate those objects. Both build wrappers require every active consumer of the five
modified multiplayer headers to have used the derived declaration.

The full host build and required integration suite now pass, including the updated room-loss
stop-deadline check, profile/settings, saves, devices, keyboard and audio regressions. The
optional live RomM/Docker check was not requested and remains skipped. The isolated native
package passes inventory, fixture, import and alignment checks and was deployed to `.30`.

The packet/LDN and room/proxy test sources run under AddressSanitizer and UndefinedBehaviorSanitizer.
The expanded room/proxy/BSD suite also passed ThreadSanitizer using clang 18.
The loopback check compiles the modified client against the pinned server source and
links existing, uninstrumented host common/ENet/fmt dependencies. It is a transport test,
not a game test.

The controller regression also verifies a member's reported game/version appears in the
snapshot and disappears after an empty game update. Preview coverage includes the final
page of a 16-member room, a shrinking list and leaving from the connected view.

The pinned standalone `eden-room` executable also builds and passes private loopback
creation and orderly shutdown. The local reference uses GCC and shared system OpenSSL:
Clang 18 rejects the upstream TLS flag, and the host lacks an OpenSSL static dependency.
Those are reference-host build adaptations, with no room-source or protocol changes.
Use the CLI's long options; its advertised `-b` ban-list shorthand is absent from the
pinned short-option parser and can leave argument processing stuck.

```bash
python3 tools/check-multiplayer.py --source /path/to/pinned/eden
python3 tools/check-multiplayer.py --source /path/to/pinned/eden \
    --host-cache /path/to/existing/host-cache
CXX=clang++-18 python3 tools/check-multiplayer.py --source /path/to/pinned/eden \
    --host-cache /path/to/existing/host-cache --thread-sanitizer
```

## Remaining before feature qualification

- Fix and qualify native hostname resolution. The public-cloud room joins by IPv4,
  but the PS5 hostname path fails before attempting the handshake.
- Run two actual game instances with matching updates, test both host roles, complete a
  race, and verify leaving/rejoining and another game session. The live protocol probe
  does not replace this test or qualify the upstream proxy TCP limitations.
- Repeat the two-game test across independent internet peers and record latency/topology.
- Repeat affected checks if those hardware findings require further changes.

The native build/package checks, host packet/controller/socket regressions, and the
hardware milestones above have passed. Keep this PR experimental until gameplay and
transport qualification are complete. Only `.30` was used for these hardware cases.
Public room browsing, PS5 room hosting and issue #89 remain separate follow-ups.
Per the updated scope, Tailscale is an optional externally configured transport. Payload
integration and qualification are not release gates for this direct-room client; the
source findings and setup notes below are retained as optional reference only.

### Desktop reference preparation

The Windows host has a GTX 1070 Ti and an existing copy of the test game. An isolated
portable copy of [official Eden 0.2.1](https://git.eden-emu.dev/eden-emu/eden/releases/tag/v0.2.1)
is downloaded for the next compatibility case. The archive SHA-256 is
`6c1b53ce325170a026cc0f87098380027dc6170d94ba95f913aab7596fd097cb`;
the GUI executable SHA-256 is
`df08b0fe90ec07988e9bfbe598b32a6dbda3e4e81e35ed2120793e42dd67f565`.
It has an empty portable profile; no existing profile or save was changed.
The matching update is available on `.30`, but copying it and the existing desktop keys
into this isolated profile is awaiting explicit authorization. No desktop gameplay was run.
WSLg currently reports unaccelerated llvmpipe, so native Windows is the reference target.

## Tailscale source qualification and user guide

The derived client previously passed password rejection, two-member join, LDN broadcast,
directed UDP proxy delivery and controller join/leave against the pinned standalone server
on Bee through its Tailscale address, under ASan/UBSan. Both clients ran on the same PC;
that test does not qualify independent internet peers or either PS5 payload.

The [experimental setup guide](MULTIPLAYER.md) documents room connection, recovery and the
holdmysocks forwarding configuration without advertising untested hardware support.
Pinned source review confirms `newForwarder(d.dialTailnet, ...)` connects local UDP rules
through `tsnet.Server.Dial`; `startUDP` supplies the native listener and per-client relay.
The atreus04-GG revision instead configures fixed inbound Remote Play listeners and
explicitly skips outbound dialing. Its required local forward is absent at that pin.

The pinned holdmysocks `tsd` Go component suite passed on Bee/Linux with
`go test -count=1 ./...`. The unmodified race-enabled suite failed in
`TestLocalForward`: its TCP and UDP callbacks append to the same test-owned `dialed`
slice without synchronization. In the temporary validation copy only, protecting the
recorder's writes and assertion read with a mutex made `go test -race -count=1 ./...`
pass. No production payload code changed. The initial failure and both follow-up runs
are retained in `results/multiplayer-tailscale-forward-tests`.

Neither payload was executed on PS5. These component tests use loopback in place of
the tailnet dialer; they do not qualify real tsnet transport, PS5 sandbox access or gameplay.
