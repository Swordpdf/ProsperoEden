# Launcher pack updates

ProsperoEden can check, download and replace an installed runtime launcher independently of the application. This is an optional host feature. Existing packs without update metadata continue to load normally, and do not cause network requests.

## Using the updater

Open **Settings → Appearance → Launcher**, highlight an external launcher and press the displayed **Updates** button (Triangle with the default menu bindings). This opens a native screen that works independently of the pack's layout.

- **Triangle:** check the pack's feed.
- **Cross:** confirm the offered update. Downloading, validation and installation follow this confirmation.
- **Square, then Cross:** confirm restoring the previous installed version.
- **Circle:** cancel a check or download, or return to the launcher picker. A file replacement already in progress finishes before leaving.
- **Options:** toggle automatic checks. This setting applies to the selected launcher's background checks and persists independently of application preferences.

The host checks the selected external launcher after five seconds in the frontend, with a persisted six-hour cooldown. Other installed launchers can be checked manually. Checks do not install updates automatically. Failed checks also observe the cooldown; a manual check bypasses it. Background network requests never run in drawing callbacks, and leaving for a game cancels an outstanding background check without waiting for the network.

The update screen shows the installed version, offered version, a short release-note preview, download progress and failure messages. After updating an active pack, the host reloads it. If its initialization fails, the previous version is restored and reloaded once. A second failure leaves the built-in interface available rather than entering a retry loop. A pack that fails later during an ordinary callback follows the existing built-in fallback behavior; manual rollback remains available.

## Pack manifest

Add a numeric version and an HTTPS feed URL to `manifest.json`:

```json
{
  "version": "1.0.0",
  "updates": {
    "feed": "https://github.com/OWNER/REPOSITORY/releases/latest/download/launcher-updates.json"
  }
}
```

These fields augment the existing manifest; keep its ID, runtime contract, API, entry, resources and state schema. Versions have exactly three numeric components. For example, `1.002.001` and `1.2.1` compare equally; prerelease suffixes and arbitrary version labels are not supported. A separate feed URL can represent a beta channel.

The neutral example includes a version but deliberately does not point to a live update feed. No third-party launcher or copyrighted platform resources are bundled with this feature.

## Feed format

```json
{
  "schema": 1,
  "id": "grid-example",
  "releases": [
    {
      "version": "1.1.0",
      "url": "https://github.com/OWNER/REPOSITORY/releases/download/v1.1.0/grid-example-1.1.0.zip",
      "sha256": "<64 lowercase hexadecimal characters>",
      "bytes": 123456,
      "host_contract": "launcher-v1.0",
      "api": { "major": 1, "min_minor": 0 },
      "required_capabilities": ["draw.atlas.v1", "actions.launch.v1"],
      "state_schema": 1,
      "min_app": "1.003.000",
      "notes": "Updated settings layout and fixed navigation."
    }
  ]
}
```

`min_app` and `notes` are optional. `required_capabilities` defaults to an empty array, and `state_schema` defaults to 1. The feed can retain up to 64 releases; its total size is limited to 256 KiB and notes to 8192 UTF-8 bytes per release. The host selects the highest version newer than the installed pack that it can run, rather than blindly downloading the first or newest listed release.

A release must have a supported runtime contract, matching API major, a supported minimum API minor, available capabilities, a compatible minimum application version, and the same state schema as the installed pack. A state-schema change requires an explicit manual migration; the updater does not guess how to migrate user data. The downloaded manifest must match the feed's identity, version, API and capabilities, and retain the installed update-feed URL.

## Publishing with GitHub Releases

Keep the feed URL stable and the archive URL version-specific. GitHub's [release asset links](https://docs.github.com/en/repositories/releasing-projects-on-github/linking-to-releases) support `/releases/latest/download/<asset>` for a manually uploaded release asset. Use a versioned `/releases/download/<tag>/<asset>` URL for each ZIP.

1. Update your pack's version and files.
2. Generate the ZIP and feed locally:

   ```sh
   python3 tools/launcher/package-update.py /path/to/grid-example \
     --output /path/to/published \
     --url https://github.com/OWNER/REPOSITORY/releases/download/v1.1.0/grid-example-1.1.0.zip \
     --previous /path/to/previous/launcher-updates.json \
     --notes /path/to/release-notes.txt \
     --min-app 1.003.000
   ```

   Omit `--previous` on the first release. On later releases, retain older compatible entries so users on an older host can still receive an appropriate pack version. The tool creates no repository, tag or release and uploads nothing.

3. Test the generated files with the target application.
4. Attach both the versioned ZIP and `launcher-updates.json` to your GitHub Release. Do not overwrite an existing versioned ZIP; publish a new version instead.

The feed can also be a file on a dedicated branch, for example `https://raw.githubusercontent.com/OWNER/REPOSITORY/update-feed/launcher-updates.json`. A release workflow can regenerate that file when each versioned release is published. Its address stays fixed; each entry points to an immutable, versioned ZIP. Keep the workflow and publisher credentials in the publisher repository, not in the launcher pack or the application.

Any public HTTPS host can serve this format. There is no GitHub account authorization or provider-specific release API in the application. Private repositories requiring tokens are not supported by this initial implementation.

Package only the launcher ID directory:

```text
grid-example/
  manifest.json
  main.lua
  assets/...
```

Keep generated ZIPs and feeds outside the pack folder. Do not put application executables, configuration, saves, credentials or launcher state in the pack. The authoring tool rejects links and special files; the native installer independently validates the archive.

## Storage, limits and recovery

```text
/data/prosperoeden/launchers/<id>/                     installed pack
/data/prosperoeden/config/launcher-state/<id>.json     existing pack state, untouched
/data/prosperoeden/config/launcher-updates.json        automatic-check preference and timestamps
/data/prosperoeden/launcher-updates/work/<id>/         temporary staging
/data/prosperoeden/launcher-updates/backups/<id>/      previous pack version
/data/prosperoeden/launcher-updates/transactions/      replacement journals
```

Downloads are bounded to 128 MiB, expanded contents to 256 MiB, individual files to 64 MiB and archives to 4096 entries. Extraction streams to staging files, checks CRCs and SHA-256, and rejects traversal, absolute paths, duplicate entries, symbolic links, special files, encryption and unsupported ZIP methods. Resource paths and Lua source syntax are checked before replacement; fonts and decoded artwork are also validated when the runtime activates the pack.

Updates are staged on the same data filesystem as the installed pack. File contents are flushed before replacement. A journal and directory renames allow startup recovery after interruption: an uncommitted replacement restores the old live folder; a committed replacement finishes backup rotation. Only one previous version is retained. Rollback uses the same staging and transaction mechanism, so the version it replaces becomes the next backup. It cannot migrate state schemas.

Keep enough storage for the download, expanded candidate and retained versions. A full disk or write failure reports an error and retains or restores the installed folder where filesystem access permits recovery. Failed staging is cleaned up; a cancelled candidate that had already finished validation may remain in staging until the next update attempt. Do not manually change the installed pack while an update is in progress. Concurrent modification by another process is outside the updater's transaction model.

HTTPS authenticates the download connection; SHA-256 checks that the archive matches the feed. The digest is **not** a publisher signature. Installing a pack opts into its author's update source. The application does not run native code or grant Lua additional permissions, but a malicious publisher can still change its launcher's behavior through the existing allowed APIs.

## Optional Lua integration

Runtime API minor 1 adds the capability `updates.launcher-packs.v1`; the contract remains `launcher-v1.0`. Existing API 1.0 packs remain compatible. A pack using the new operations should declare that capability and `api.min_minor: 1`, or feature-detect before using them:

```lua
local runtime = host.query("runtime")
-- runtime.api_major, runtime.api_minor, runtime.host_contract, runtime.capabilities
local update = host.query("launcher_updates")
-- phase, id, automatic_checks; installed_version/release/error/progress when available
host.action("launcher_updates.check")
host.action("launcher_updates.open")
```

`launcher_updates.check` starts a host check for the calling pack only. `launcher_updates.open` opens native management for that pack. `launcher_updates.install` opens the same confirmation screen; it never bypasses confirmation. `launcher_updates.rollback` opens the rollback confirmation, and `launcher_updates.cancel` cancels a check or download. Scripts cannot select arbitrary target IDs, write files, provide an arbitrary URL to the host, or change the global automatic-check preference.

For a pack-owned layout, see [Launcher-drawn update pages](LAUNCHER_UPDATE_VIEWS.md). That optional capability changes presentation only; verification, staging and recovery remain native.

Status phases are `idle`, `unconfigured`, `checking`, `current`, `available`, `downloading`, `validating`, `ready`, `installing`, `installed`, `failed` and `cancelled`. `ready` is an internal staging result; the host unloads active consumers before committing. Physical confirmation and menu bindings are handled by the native screen.

The application updater now checks API major/minor and required capabilities in addition to the existing contract name. This prevents installing a base-app release that advertises the same contract but lacks a selected pack's required operations. The pack updater and application updater remain separate. Pack updates distribute changes made by the launcher author; they do not automatically invent UI for new backend settings.

## Development tests

```sh
tools/launcher/test-launcher-updates.sh
tools/launcher/runtime-preview.sh /tmp/launcher-preview
```

The updater test script builds the actual C++ implementation with fixture downloads and tests compatibility selection, package integrity, confinement, rollback, publisher output and transaction recovery. Requires C++20, Python 3, nlohmann/json headers (3.11 or newer), libcurl and OpenSSL development files; miniz and Lua come from the repository.

For full renderer integration tests, build `runtime-preview.sh`, then link its objects with `test-launcher-update-transport.cpp` **in place of** the `launcher_update_http.o` object. Run `test-launcher-update-ui.py <fixture-preview-binary>` and `test-launcher-update-views.py <fixture-preview-binary>`. This transport is test-only and is never linked by production scripts or the PS5 target. The production preview always uses the normal verified HTTPS transport.

The updater test script also checks the runtime library snapshot and handle index against the original serialization, including refreshes, duplicate handles and the 8192-game limit. Its timing output measures host-side snapshot construction only, excluding Lua table conversion and rendering.

Console testing is still needed for real HTTPS downloads, storage failures, interruption recovery on console mounts, game launch/return and update installation. Desktop and compile checks do not establish those console results.
