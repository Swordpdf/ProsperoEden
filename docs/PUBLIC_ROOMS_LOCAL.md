# Public room browser — local development

The Multiplayer menu offers **Public rooms** and **Private room**. Private room retains the
address/port/nickname/password form. Public rooms fetches the Eden directory anonymously over
HTTPS and presents a controller-friendly list with preferred games, player counts, password
requirements, and protocol compatibility. Cross selects, Triangle refreshes, Circle goes back.
The nickname row opens the existing keyboard. Joining uses the existing room controller.

The browser clears the saved private password before selecting a public endpoint and prompts
for a new password only when needed. Browsing or cancelling does not save connection settings.
The selected connection is saved only when Join is requested, matching the existing private flow.
Directory failure is distinct from joining failure; a failed refresh retains the previous list.

No ENet changes. No GitHub publication. Search/ownership filters, account login, public hosting,
and chat are deferred. Directory discovery does not fix the separately reported join timeout.

## Checks

- `python3 tools/check-multiplayer-directory.py`: ASan/UBSan parser bounds, malformed rows/documents,
  numeric ranges, nesting, protocol compatibility, cancellation, HTTPS policy and API-version header.
- `python3 tools/check-remote.py`: existing HTTP/download/save-sync regression checks.
- `tools/launcher/preview.sh`: public/private screens, incompatible join blocked, cancelled password,
  private password isolation, existing connection/member list and leave behavior.
- Normal native build and required audits before packaging.

## Hardware case G1: directory browsing on .30

Goal: open Public rooms, load the live directory, scroll and refresh, return to the menu, and
open the saved Private room form. Expected: responsive input, nonempty room list, unchanged
private configuration and clean shutdown. Reject on blank/stalled UI, request error, accidental
join, unexpected settings mutation, crash, or teardown failure.

Target: 192.168.4.30 only, FW 6.02, PPSA99008; FTP 2121, klog 3232, controller loader 9021.
Use the environment lock, commit the exact candidate, freeze/hash packages, and remotely verify.
Development input/capture may be enabled for a bounded <=210s test with `launcher=first`; do
not launch a game or join unrelated public rooms. Back up and restore development markers.
After teardown restore the normal package from the same source revision and verify it.

Source revision, package hashes, screenshots, request logs, configuration hash comparison,
and lifecycle receipts belong in `results/public-room-browser/`. No proprietary assets or
credentials belong in this document. Hardware result pending.
