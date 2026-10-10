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
- Add room initialization, controller lifecycle, join/leave/retry and safe game transitions.
- Add launcher direct-connect settings and room/member UI with text entry.
- Build the full host/native candidates and run relevant existing checks.
- Validate guest LDN and proxy sockets in a game, then internet room play.
- Validate holdmysocks' outbound UDP forwarding and investigate the equivalent capability
  in atreus04-GG's payload, following the plan's separate acceptance gates.
- Test console lifecycle, regressions and performance, only after confirming a console is idle.

No console has been used for this work. Neither PS5 multiplayer nor either Tailscale
integration is qualified yet. Public room browsing and PS5 room hosting remain deferred.
