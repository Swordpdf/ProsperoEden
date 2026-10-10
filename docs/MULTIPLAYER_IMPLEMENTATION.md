# Local wireless rooms: implementation status

Work is based on upstream `main` at `dfda802` (2026-10-10), on branch
`feature/ldn-rooms`. The source inspection in [the implementation plan](MULTIPLAYER_PLAN.md)
predates this checkout; the new work does not include the old development branch's changes.

## Completed offline

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
  earlier service destruction and is empty for the next game. Full game teardown remains
  a console acceptance gate.
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
Settings > Multiplayer offers address, UDP port, nickname and password entry, join/cancel,
room status and a scrollable member list with each reported game/version. The connected
view shows eight members at a time and keeps the scroll position valid when players leave.
Cross leaves from that view; Square returns to offline mode, including after a failed join.
Address, port and nickname use the existing per-profile settings store; passwords are omitted.
Changing profiles requires leaving the room first. The system keyboard runs outside the draw
thread and is cancelled before launcher destruction.

Game startup accepts only idle or fully joined connections. While a game exists, the controller
refuses join/leave operations and preserves its socket mode through room loss: new BSD sockets
remain proxies until game teardown. The input worker requests the existing normal return-to-menu
shutdown path on room loss and displays a notice on return. This lifecycle wiring still needs
full application and game qualification.

Offline evidence: the real controller test covers game-start gating, blocked connect/leave during
a game, and the retained proxy-mode flag after loss. The existing settings regression passes with
password omission and unrelated preference preservation. The complete launcher preview suite
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
optional live RomM/Docker check was not requested and remains skipped. An isolated native
package also passes inventory, fixture, import and alignment checks; it has not been deployed.

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

- Qualify socket cleanup and poll behavior in actual game transitions, including the
  existing upstream proxy TCP limitations; offline concurrency tests do not prove game compatibility.
- Qualify cancellation and bounded queues in the native application under room loss.
- Qualify the integrated launcher, system keyboard and game transitions in a full application.
- Repeat affected build/package checks if hardware findings require further changes.
- Validate guest LDN and proxy sockets in a game, then internet room play.
- Validate holdmysocks' outbound UDP forwarding on hardware. Source review confirms that
  the pinned atreus04-GG payload lacks this facility; adding it is optional separate work.
- Test console lifecycle, regressions and performance, only after confirming a console is idle.

Read-only console preflight found one active session and one console with released title
resources; the shared lock was released without deployment or launch. Recheck idle state
under the lock immediately before any hardware case. The idle console has a newer existing
development installation; it was left untouched. The saved Remote Play host entries point
to the active console, so input/capture on the idle console is not currently verified.
The local Windows Tailscale daemon
is running, but WSL uses a NAT interface; its room server still needs a verified inbound
route before console joins can be tested. A bounded Windows-to-WSL UDP relay probe received
no datagrams, and the PC has explicit inbound block rules for Python. Those rules were
left intact; the test relay exited and no console was contacted during this probe.
The room-host choice is pending. Hardware qualification needs an approved reachable room
endpoint and an idle console with a working observation/input path; no existing registration
or PS5 settings were changed to obtain one.

No console application has been launched for this work. Neither PS5 multiplayer nor either Tailscale
integration is qualified yet. Public room browsing and PS5 room hosting remain deferred.
Issue #89 is explicitly reserved for a separate future follow-up; this PR addresses #98.

## Tailscale source qualification and user guide

The [experimental setup guide](MULTIPLAYER.md) documents room connection, recovery and the
holdmysocks forwarding configuration without advertising untested hardware support.
Pinned source review confirms `newForwarder(d.dialTailnet, ...)` connects local UDP rules
through `tsnet.Server.Dial`; `startUDP` supplies the native listener and per-client relay.
The atreus04-GG revision instead configures fixed inbound Remote Play listeners and
explicitly skips outbound dialing. Its required local forward is absent at that pin.

Neither payload was executed. No Go toolchain was found in the current Windows/WSL
environment, so the payload's Go component tests have not run here. This is source-level
evidence only; it does not qualify tsnet transport, PS5 sandbox access or gameplay.
