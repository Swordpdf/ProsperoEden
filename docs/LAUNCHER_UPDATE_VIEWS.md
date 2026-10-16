# Launcher-drawn update pages

Optional capability: `updates.launcher-packs.view.v1` (launcher API 1.1).
The default updater remains native. A launcher opens its own page with
`host.action("launcher_updates.open", {view="launcher"})` and exports all three
callbacks in its main module:

- `updater_status(event)`: status, preferences, controller mask, busy/can_update
  flags and a first-entry flag. The host sends changes at most ten times a second.
- `updater_input(event)`: physical controller key names, as in the main input API.
  Apply the saved menu-button mapping before interpreting them.
- `updater_draw(event)`: time plus the existing drawing API. The launcher owns
  the page's layout, text, artwork, focus, progress display and input behavior.

Only these callbacks run while the update page is open; the main UI is paused.

Inside an updater input callback:

- `launcher_updates.check` checks the calling pack.
- `launcher_updates.download` takes `{confirmed=true, version=offered_version}`.
  The host requires a live input callback, a matching offer, an idle manager and
  an available update. The launcher must present the offer and obtain confirmation.
- `launcher_updates.close` cancels a check/download, then closes when safe.
- `launcher_updates.cancel` cancels without requesting closure.

Other application actions are blocked in these callbacks. Sound playback remains
available. Queries are limited to runtime metadata, measurement and the calling
pack's update status. Game-cover loading is unavailable. Existing Lua instruction,
VM memory, drawing and resource limits still apply. Scripts cannot perform file,
network or process operations.

Before replacement, the host shuts down the main launcher and detaches its update
view. Its existing Lua VM, font, textures and recordings remain owned in memory;
only updater callbacks may run. No pack resource is re-read while its files change.
The replacement launcher initializes separately. The old view is retired after
closure, retaining audio ownership until the mixer fence completes. It cannot save
old UI state over the new launcher's state.

Missing or failed callbacks fall back to the native updater without cancelling the
operation. If graphics resources are recreated after the view has been detached,
the host uses the native recovery page for the remainder of that operation. This
avoids reloading assets from a directory currently being replaced.

Native update selection, verification, compatibility checks, replacement and
rollback are unchanged. Calling `launcher_updates.open` without the view option,
or opening updates from the built-in launcher, retains the native page. Existing
packs remain compatible; hosts without this capability ignore the optional view
argument and display the native page.
