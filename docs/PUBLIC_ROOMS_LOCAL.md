# Public room browser

Prepared locally and validated on .40; publication to the existing multiplayer PR was
authorized after the G2 result below. Earlier local-only and deferred-test notes are historical.

The Multiplayer menu offers **Public rooms** and **Private room**. Private room retains the
address/port/nickname/password form. Public rooms fetches the Eden directory anonymously over
HTTPS and presents a controller-friendly list with preferred games, player counts, password
requirements, and protocol compatibility. Cross selects, Triangle refreshes, Circle goes back.
The nickname row opens the existing keyboard. Joining uses the existing room controller.

The browser clears the saved private password before selecting a public endpoint and prompts
for a new password only when needed. Browsing or cancelling does not save connection settings.
Joining a public room does not change the saved private room: only the nickname is shared
between the two. The private room form is saved as each field is entered, so it survives
closing the app before a join. Directory entries with a non-public IPv4 address (loopback,
private, carrier-grade NAT, link-local, multicast, broadcast) are rejected; malformed UTF-8 in
the response is replaced rather than failing the whole list; rooms are listed compatible
first, then by player count; Left/Right moves a page at a time and a refresh keeps the cursor
on the same room.
Directory failure is distinct from joining failure; a failed refresh retains the previous list.

No ENet changes. Search/ownership filters, account login, public hosting,
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
credentials belong in this document.

## Current result

Local application candidate: `28e668d` (on `local/public-room-browser`; not pushed).
Parser/HTTP ASan/UBSan checks, existing remote/download/save-sync checks, launcher preview,
native build and required audits passed. Both normal and development-capture packages passed
freeze/inventory/hash verification.

- Normal eboot SHA-256: `b026426fb796ee8ae3c4b871f2525d6ccd01cee36222393ca43af6d11f027748`.
- Diagnostic eboot SHA-256: `72f93729fcb4dde069dfd0f5c3393ba6cfb6a9e92d40123ae9d7408517c0638b`.

Console testing is deferred at the user's explicit request. Preflight initially found the app
running; after the exact title was closed, it reopened during backup, so the second idle check
stopped deployment before package replacement. No diagnostic launch or public room join occurred.
The environment lock was released. Do not resume console work without new user authorization.

- 2026-10-10 | G1 | 28e668d | .30 | no-run: title reopened during deployment preflight; testing deferred by user | results/public-room-browser/ | retain local packages

## Connection error presentation (local follow-up)

Multiplayer now keeps a highlighted explanation visible on the choice, private and public
screens after a failed attempt. Each reported cause includes a next step: check the address
and UDP port, correct a password or nickname, choose another room, or contact the host.
Hostname lookup timeout and lookup failure have distinct messages. A room-acceptance timeout
explains that the server connection opened but the room did not finish accepting the player.
These messages describe the observed stage, not a diagnosis of a firewall or server fault.
ENet, connection deadlines and retry behavior are unchanged.

Validation: host preview passed and timeout/password layouts were visually inspected;
native build and repository audits passed. The ASan/UBSan multiplayer suite passed with
logging enabled and disabled, including a new localhost server case that keeps ENet alive
but never replies to the room join request. Existing rejection, retry and log-privacy checks
also passed. Translation catalog checking reports untranslated multiplayer text; English
fallback remains in use. Preview images are in `build/public-room-preview/C3*.png`.

No console access or GitHub push was performed for this follow-up. The package hashes above
describe the earlier candidate; they do not contain this UI follow-up. Console validation
remains deferred, and the separate reported connectivity issue is not fixed by these messages.

## Hardware case G2: combined browser and non-blocking socket fix on .40

User authorized .40 only, conditional on idle state. Application source is `2699b69`, including
upstream `f331835`, public/private selection and persistent connection-error explanations.
Normal and diagnostic packages are frozen separately from the same application source.
Target PPSA99008 at 192.168.4.40, using FTP 2121, klog 3232 and title controller 9021.
Require idle state immediately before deployment and launch; never close a pre-existing title.

Acceptance: render Multiplayer selection, fetch and refresh the public directory, return to
the private form, join the owner's server, leave, and stop cleanly with runtime layers released.
Use a temporary test nickname and preserve the original profile/configuration and development
markers. No game launch or unrelated public room join. Stop on another active title, ambiguous
screen, crash, service loss, or the 210-second observation limit. Restore the normal candidate
after clean teardown and verify its remote hash. Firmware and artifact hashes are recorded
with evidence in `results/public-room-browser-40/`. .30 remains out of scope.

G2 result: **partial-pass**. The console rendered both menu choices and the private form,
loaded 130 public rooms (HTTP 200, 654 ms), and refreshed to 131 rooms (HTTP 200, 621 ms).
The owner's private room accepted the test player and displayed the member list. Logs confirm
the non-blocking socket, Joined state, bidirectional packets and RTT samples of 12, 18 and 21 ms.
This used a numeric IPv4 address; it does not qualify console hostname lookup. No game was launched.
The observation limit ended the run before a separate Leave-button or failure-message scenario.
Error presentation remains host-preview tested; public-list scrolling was not exercised here.

The launcher exited normally and ShadowMount reported PPSA99008 PID 566 stopped. This target
does not provide the separate runtime-layer-release receipt used by the .30 runner. Cleanup
initially rejected ftpsrv's successful `226 File deleted` reply; a bounded cleanup accepted
that response, removed remaining temporary capture state and verified all 66 normal-package
files. Original configuration was verified byte-for-byte restored, services remained healthy,
and the environment lock was released. Existing development settings were restored by the runner.

Application source: `2699b69`; diagnostic build after documentation-only `8cf64d8`.
Console reported firmware SDK version `0x12700001`. Frozen eboot SHA-256:

- Normal, installed after testing: `f28165e6a2de9a55160de1a105c480ff8e7f9fb2f6aea1df21e7701dda28b1bb`.
- Diagnostic, tested: `e4e560886338de54cdc6f5e9c0ced50eb021e909807f5a5aea0f56d63df8964d`.

Receipts: `results/public-room-browser-40/cleanup.json`, both deployment manifests, `klog.log`,
`stderr.log`, `lifecycle.log`, `14-multiplayer.png`, `16-refresh.png`, and `24-join.png`.
The pre-restoration `config-check.json` comparison differs as expected because the test used
temporary connection and diagnostic settings; `cleanup.json` records the verified restoration.

- 2026-10-10 | G2 | 2699b69 | .40 | partial-pass: directory, refresh, private join, packets and normal exit; normal package restored | results/public-room-browser-40/ | gameplay/error cases untested
