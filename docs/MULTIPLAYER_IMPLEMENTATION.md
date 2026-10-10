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
- Run the actual room server and derived client over loopback: wrong password, retry,
  two members with distinct virtual IPs, LDN broadcast, directed proxy traffic and server loss.
- Run the standalone controller against that server: hostname resolution, input validation,
  wrong-password retry, copied membership, leave/rejoin, room loss, join cancellation and
  destruction during a pending join. A raw ENet server verifies out-of-order join-success
  rejection. Cancellation and pending-join destruction complete within the two-second test limit.

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
room status and member names. Square returns to offline mode, including after a failed join.
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

The packet/LDN and room/proxy test sources run under AddressSanitizer and UndefinedBehaviorSanitizer.
The loopback check compiles the modified client against the pinned server source and
links existing, uninstrumented host common/ENet/fmt dependencies. It is a transport test,
not a game test.

```bash
python3 tools/check-multiplayer.py --source /path/to/pinned/eden
python3 tools/check-multiplayer.py --source /path/to/pinned/eden \
    --host-cache /path/to/existing/host-cache
```

## Remaining before feature qualification

- Finish callback ownership and guest socket synchronization: upstream BSD callbacks iterate
  a static descriptor table while guest calls can change it. Duplicate descriptors and multiple
  BSD service instances also need exactly-once packet delivery. Route to live socket objects
  without holding a descriptor lock across blocking guest receive calls.
- Qualify proxy poll readiness: the pinned proxy has no native descriptor, while upstream
  `Network::Poll` delegates to native poll. The existing direct receive tests do not cover this.
- Qualify cancellation and bounded queues in the native application under room loss.
- Qualify the integrated launcher, system keyboard and game transitions in a full application.
- Build the full host/native candidates and run relevant existing checks.
- Validate guest LDN and proxy sockets in a game, then internet room play.
- Validate holdmysocks' outbound UDP forwarding and investigate the equivalent capability
  in atreus04-GG's payload, following the plan's separate acceptance gates.
- Test console lifecycle, regressions and performance, only after confirming a console is idle.

No console has been used for this work. Neither PS5 multiplayer nor either Tailscale
integration is qualified yet. Public room browsing and PS5 room hosting remain deferred.
Issue #89 is explicitly reserved for a separate future follow-up; this PR addresses #98.
