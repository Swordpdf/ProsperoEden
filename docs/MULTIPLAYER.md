# Experimental local-wireless multiplayer

This guide describes the implementation proposed for [issue #98](https://github.com/blackbearreloaded/ProsperoEden/issues/98).
It is not a claim that the current release supports multiplayer. Host tests and native
builds pass; PS5 gameplay, internet play and Tailscale forwarding remain unqualified.
See [validation status](MULTIPLAYER_IMPLEMENTATION.md) for the evidence and remaining gates.

## What connects to what

Each player runs their own copy of the same game and uses its **Local Wireless** mode.
First, every emulator joins the same Eden room. That room carries discovery and game
traffic; it needs to remain running throughout the session. Players keep independent saves.

This does not provide Nintendo online services, physical Switch compatibility, or support
for every game's separate LAN mode. A successful room join proves room connectivity only.
Game compatibility must be tested with the actual emulator builds, game update and host role.

## Join an ordinary room

Use a private room server on a PC/Linux host for initial tests. The PS5 build is a room
client; it does not host a room or browse public rooms. Obtain the host address, UDP port
and optional room password from the room operator. The usual UDP port is **24872**.

1. In ProsperoEden, open **Settings > Multiplayer** before launching a game.
2. Select **Room address** and enter a numeric IPv4 address or hostname. Enter the port
   separately. IPv6 literals are not supported by this connection form.
3. Choose a distinct **Nickname**: 4–20 characters using English letters, digits, spaces,
   dots, hyphens or underscores. Enter the room password if required.
4. Select **Join room**. Wait for **Connected** and check the room/member list.
   Reported game names and versions appear beside members; up/down scrolls long lists.
5. Return to the library, launch the same game/update as the other players, then select
   its **Local Wireless** mode. One player creates the in-game session and others join it.
   The in-game host can be different from the computer running the room server.

Address, port and nickname are remembered per profile. Passwords are not saved and are
cleared from the form after a connection attempt; enter them again for a retry. Leave the
room before changing profiles or connection details.

While connected, Cross chooses **Leave room**. Square chooses **Go offline**, including
after a failed join. Circle returns to the previous launcher screen without leaving a
connected room. During a game, use the existing Touchpad + L1 shortcut to return to the
launcher before changing rooms. If the room connection fails during play, the implementation
requests the normal return-to-menu path; that native recovery remains a hardware test gate.

## Private server example

Use an Eden `eden-room` executable whose revision is recorded for the test. Initial
reference tests use Eden commit `5f142c7926d0c7fcbbd0ce30794d72f638a43b2a`.
Substitute the server's reachable LAN or tailnet IPv4 address for `ROOM_HOST_IP`:

```sh
eden-room --room-name "Friends" --bind-address ROOM_HOST_IP \
  --port 24872 --max-members 8 --password "choose-a-room-password" \
  --preferred-game "Mario Kart 8 Deluxe" \
  --preferred-game-id 0x0100152000022000 \
  --ban-list-file ./room-bans.txt --log-file ./room.log
```

The preferred game is room metadata; it does not select a game on another device. Omit
announcement credentials to keep this pinned server private. Use its long option names:
at this revision the advertised `-b` shorthand is missing from the short-option parser.
The room operator must provide an actual inbound UDP route to the server. A WSL NAT
address is not automatically reachable from the PS5 or other tailnet devices.

## Tailscale through a PS5 payload

Being on the same tailnet is not sufficient by itself. The inspected PS5 payloads run
Tailscale in their own process; they do not add a system-wide Tailscale network interface
that ProsperoEden can dial directly. The client needs a local UDP forward to the room.

| Payload revision inspected | Source-level capability | ProsperoEden validation |
|---|---|---|
| [holdmysocks `e4986b4`](https://github.com/holdmysocks/ps5-tailscale/tree/e4986b4d52af6a63f9be14d9de616d267c566de1) | Configurable local UDP forward to a tailnet host | Candidate; not yet tested on PS5 |
| [atreus04-GG `d99261c`](https://github.com/atreus04-GG/ps5tailscale/tree/d99261c21b678d9637a92c88168e1f4a72563040) | Fixed inbound Remote Play proxy; no configured local outbound UDP path | Unsupported for this room topology at this revision |

For the holdmysocks candidate, run the room server on a tailnet device reachable by the
PS5 node's access policy. In the payload's dashboard, add a **UDP local forward**. This is
the equivalent fragment to merge into its existing configuration, preserving other entries:

```json
{
  "forwards": [
    {
      "proto": "udp",
      "listen": "127.0.0.1:24872",
      "target": "eden-room-pc:24872"
    }
  ]
}
```

Replace `eden-room-pc` with the actual room host's tailnet name or address. Follow the
selected payload release's instructions for applying configuration. In ProsperoEden,
connect to **127.0.0.1**, port **24872**. If that local port is occupied, use another free
local port in both places while keeping the target port equal to the room server's port.

Each PS5 needs its own local forward to the same room. A desktop with ordinary Tailscale
routing connects directly to the room host's tailnet address. Use the room password in
ProsperoEden; Tailscale registration and keys belong in the payload's own setup.

Do not put this outbound room port in the payload's inbound `udpPorts`, or use its HTTP
proxy for ENet traffic. Those are different paths. Room-encapsulated local-wireless discovery
does not require LAN broadcast to cross Tailscale.

The holdmysocks relay has a two-minute idle threshold and a 256-datagram queue per flow.
These are not a total daemon memory bound or a guarantee of game performance. Qualification
must cover cross-process loopback access, two clients, lobby idle/reconnection, packet sizes,
payload restart, and both direct and DERP-relayed paths. The complete matrix is in the
[implementation plan](MULTIPLAYER_PLAN.md#8-validation-matrix-and-evidence).

## Troubleshooting during qualification

| Symptom | Check |
|---|---|
| Room did not respond | Correct host/UDP port, server running, host firewall/router route, and tailnet policy when used |
| Hostname fails but IPv4 works | Name resolution on the device making the dial; with a PS5 forward, the payload resolves the target |
| Wrong password or nickname collision | Re-enter the password or choose a different nickname, then retry |
| Connected, but no in-game session | Same game/update, Local Wireless selected, supported game path, both in-game host roles |
| PS5 cannot connect to a `100.x` address | For these payloads, connect to the configured local UDP listener instead |
| Session stalls after inactivity | Separate ENet keepalive traffic from a genuinely expired relay flow; capture client/server/payload logs |
| Payload or server was restarted | Return to the launcher, go offline and reconnect; do not expect live-game automatic rejoin |

Record app/server/payload revisions, game update, topology, host role, error and stage
reached when reporting a result. Remove passwords and Tailscale credentials from reports.
Issue #89 and public-room browsing remain separate follow-ups.
