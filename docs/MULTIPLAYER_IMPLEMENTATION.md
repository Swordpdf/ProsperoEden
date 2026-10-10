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

## Controller in development

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

The controller is included in the application target but is not yet constructed by the launcher.
Integration must initialize logging first, create exactly one controller before game services,
prevent connect/leave/profile changes while game services exist, and stop a running game through
its normal shutdown path if its room is lost. No gameplay readiness claim follows from the
standalone controller tests.

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

- Qualify cancellation, callback ownership and outgoing queue bounds.
- Integrate the room controller with application lifetime and safe game transitions.
- Add launcher direct-connect settings and room/member UI with text entry.
- Build the full host/native candidates and run relevant existing checks.
- Validate guest LDN and proxy sockets in a game, then internet room play.
- Validate holdmysocks' outbound UDP forwarding and investigate the equivalent capability
  in atreus04-GG's payload, following the plan's separate acceptance gates.
- Test console lifecycle, regressions and performance, only after confirming a console is idle.

No console has been used for this work. Neither PS5 multiplayer nor either Tailscale
integration is qualified yet. Public room browsing and PS5 room hosting remain deferred.
