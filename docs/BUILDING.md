# Building ProsperoEden

ProsperoEden builds on Linux (Ubuntu 26.04; WSL works). One command builds the release:

```bash
make
```

The first run fetches every dependency at its pinned revision, builds the RADV driver and
Eden for the PS5, and writes the release files to `dist/`:

- `ProsperoEden-vX.Y.Z.zip`: the `PPSA99008` folder to copy to `/data/homebrew/PPSA99008`;
- `SHA256SUMS` (the ZIP's checksum) and `release-notes.md`.

The app package includes an exact-title one-shot helper built from the pinned upstream
[PS5-Lapy-JB-Daemon](https://github.com/blackbearreloaded/PS5-Lapy-JB-Daemon) source. Its generated
manifest, ELF hash, protocol hash, title and required retry feature are checked before packaging.
At runtime a resident Lapy service gets the first bounded opportunity; otherwise ProsperoEden
sends the packaged helper to the local ELF loader on TCP port 9021. Without that loader or a
resident service, ProsperoEden has no access to its data and says so; it keeps no second copy of it in its sandbox.

The pinned commit includes the donor-release and firmware 13.60 corrections merged in
[PS5-Lapy-JB-Daemon PR #48](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon/pull/48) and
[PR #49](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon/pull/49), plus the guarded
target-lifetime and fail-closed accounting changes from
[PR #50](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon/pull/50).
The previous `54a095c` pin passed five automated launch/elevate/close cycles on both firmware 6.02
and 12.70 with root `/data` access and balanced donors. The pinned `57fcc19` helper completed the
target-exit stress and a full one-shot elevation on firmware 12.70; it still requires an attended
ProsperoEden qualification run on firmware 6.02.

The first build takes a while (RADV and Eden are large). Later builds reuse everything that
already exists: the dependencies, this checkout's build cache in
`~/.cache/ps5-eden-headless.<hash>`, and ccache.

## Make targets

| Target | What it does |
|---|---|
| `make` / `make release` | Release files in `dist/` |
| `make package` | Only the app folder, `build/release/PPSA99008` |
| `make install PS5_HOST=<address>` | Copy `build/release/PPSA99008` to a console over FTP (close ProsperoEden first) |
| `make dev DEV_TITLE=<title ID>` | Development build, `build/dev/PPSA99008` (or `EDEN_DEV_PACKAGE_DIR`): profiling counters, `dev-settings.txt` switches, boots the given title |
| `make test` | Host (Linux) build of the emulator and its test suites |
| `make deps` | Fetch missing dependencies at their pinned revisions |
| `make deps-status` | List the dependencies, where they live and whether they match their pins |
| `make prepare` | Everything besides Eden itself: build cache, FFmpeg, packaging tool, driver stub, RADV |
| `make toolchain` | Check the host tools |
| `make clean` | Remove `build/` and `dist/` |
| `make distclean` | Also remove the fetched `.deps` and this checkout's build cache |

`JOBS=<n>` sets the number of parallel compile jobs (default: all cores).

The download sources and the save sync can be checked against real RomM servers and
ftpsrv, the console's FTP server, in Docker (`ROMM_CHECK=1 tools/check-romm.py`, or `ROMM_CHECK=1 make
test`; it is not part of a plain `make test`, since it pulls several server images and, while it
runs, an FTP server without a sign-in listens on the computer's network): for each version (default: the oldest the save sync takes, `kMinimumVersion` in
`headless/remote/romm/romm_saves.h`, the newest it was checked with, and one older than the oldest,
which has to be refused) it starts a RomM with fake games (`tools/romm-test/`), scans them,
downloads them as a download source (a game of 400 MB stopped part way and gone on with, compared
byte for byte; updates and DLC, a cancel, two sources, the queue kept), plays a second console and
a phone against its save sync, and takes it down again. It needs Docker with Compose (without it,
it is skipped); the first run of a version downloads its image (about 1.2 GB on disk, MariaDB's
0.5 GB once; ftpsrv's is built once). ftpsrv runs on the host's network while the check runs (its
passive mode needs it). `tools/check-remote.py` has what needs no server, and against stand-ins
(`tools/romm-mock-server.py`, `tools/ftp-mock-server.py`) only what a real RomM and ftpsrv do not
show on demand: a server that caps its pages or cannot resume, games told apart by metadata ids
and title IDs, another file name on a second source, and a full drive. Raise
`kMinimumVersion` only to a version this check passes with.

## Dependencies

Every input is pinned in `tools/deps.json` and fetched by `make deps` (`tools/deps.py`) only when
it is missing: archives are checked against their SHA-256/SHA-512 before use and git
repositories are fetched at their pinned commit. Nothing that already exists is modified, so a
checkout you work in stays at whatever revision it has (`make deps-status` shows it).
Downloads are cached in `~/.cache/prosperoeden-deps` (`PROSPEROEDEN_DEPS_CACHE`).

Inside this repository, in `.deps/`:

- **Eden** at commit `67bada77f8a43a90da2e94e89b8e7da73c256989` (GitHub mirror archive), with
  Eden's own hash-pinned packages, which its configure step downloads. ProsperoEden does not
  modify Eden's files: the PS5 frontend in `headless/` replaces and derives sources at
  configure time (`headless/inject.cmake`).
- **FFmpeg** at the commit Eden pins, built with only the decoders games use.
- **PS5 OpenGL 4.6 SDK 1.0.0** (release archive), for the launcher and the OpenGL renderer.
- **OpenSSL, zlib, libcurl and libpsl** from pacbrew v0.40.2.
- **LLVM 18.1.8 compiler-rt** emulated-TLS sources and **fmt 12.1.0** headers.
- **PS5-Lapy-JB-Daemon** at its pinned compatibility-fork commit, its pinned **ps5log** build input,
  and the official **PS5 Payload SDK v0.42** used only to build the exact-title one-shot helper.
  Eden itself continues to use the boilerplate's Payload SDK v0.42.

Next to this repository (`../`), as git checkouts:

- **ps5-native-app-boilerplate**: the PS5 Payload SDK v0.42, the runtime `libc.prx` and the
  native packaging tool;
- **Mihawk's PS5_Vulkan, PS5_Mesa and PS5_PayloadSDK** (`../mihawk-*-review`): RADV and its
  build recipe. `make prepare` builds RADV once and isolates it beside the OpenGL Mesa
  (`tools/isolate-radv.py`). RADV's display code carries this repository's adaptation
  (`tools/patch-radv-wsi.py`: the output's lifetime, and the 120 Hz output a game session can ask
  for); when the adaptation changes, `make prepare` builds RADV again, which compiles only that
  file. The three checkouts have to be at the commits `tools/deps.json` pins; when the Mesa pin
  changes, `make prepare` builds RADV again from the new revision.

The `libSceAgcDriver` import facade both drivers link against is built from
`tools/stubs/libSceAgcDriver.c`. Small contracts from our research repositories are in
`third_party/`.

## Launcher

The launcher (`headless/prosperoeden`) draws with OpenGL through the PS5 OpenGL SDK and plays its
sounds through the console's audio output; it needs no other library. Its font atlas, artwork
and sounds are committed in `headless/prosperoeden/ui`, so a build does not regenerate them.
The tools that made them are in `tools/launcher`:

- `assets.sh` bakes the font (`third_party/fonts/Montserrat-Medium.ttf`) and renders the art from
  the source pictures in `sce_sys/`; it needs a host C++ compiler and Python with Pillow.
- `process-sfx.py` trims and levels the raw sound effects (needs `ffmpeg` and `numpy`).
- `bake-loading-text.py` writes the lettering of the loading screen
  (`headless/loading_text.glsl`).
- `preview.sh` draws every launcher screen on a PC (Mesa's software renderer, sample games) to
  PNG files or a video, with the same code, shaders and font as on the console.
- `strings.py` keeps the translations: `extract` writes the template (`launcher.pot`) from the
  text in the code, `new <tag>` starts a catalog in `headless/prosperoeden/ui/lang`, and `check`
  fails on missing or stale text, changed placeholders and characters the font does not have.
- `text-check.sh` compares the launcher's right-to-left text code with ICU on generated lines
  and on every translation (needs `libicu-dev`).

What the PS5's home screen shows for the app is in `sce_sys/`, as it goes into the package:
`param.json` (title, ID, version, and the 120 Hz output declaration), `icon0.png`, `pic0.dds`,
`pic1.dds` and `snd0.at9`. The pictures' sources are beside them.

## Host tools

`make toolchain` checks them: `clang-18`, `lld-18` and the LLVM 18 tools, `cmake`, `ninja`,
`ccache`, `make`, `nasm`, `meson`, `rsync`, `git`, `glslangValidator`, `spirv-val`, `bison`,
`flex`, `curl`, `wget`, `unzip`, and Python 3.11 or later with `venv`, `mako` and `yaml`.

**Nix shell.** With the [Nix](https://nixos.org/download/) package manager, `shell.nix` provides all of these tools (LLVM 18 and 21, Meson, CMake, Ninja and the rest): `nix-shell shell.nix --run "make"`.

RADV's host tools (`mesa_clc`, `vtn_bindgen2`) are built against the host's LLVM 21: its
development files, Clang 21 libraries, libclc and the SPIR-V LLVM translator. The Payload SDK's
`prospero-*` wrappers (the RADV build and the final link) use `$LLVM_CONFIG`, else the newest of
LLVM 21 to 15 that is installed: the releases are linked with LLD 21. On Ubuntu 26.04 every
package the release build needs is listed in `tools/ci/ubuntu-packages.txt`:

```bash
sudo apt install --no-install-recommends $(grep -v '^#' tools/ci/ubuntu-packages.txt)
```

## Crash reports

When the app crashes it writes `crash-YYYYMMDD-HHMMSS.txt` to its logs folder
(`/data/prosperoeden/logs`), starts again and says on the home screen where the report is
(`headless/crash_report.h`). At the next start that run's logs are moved beside the report
(`-stderr.log`, `-heap.log`, `-eden_log.txt`); the five newest reports are kept.

The report lists addresses as `eboot+0x...`. To get function names, files and lines:

```bash
python3 tools/symbolize-crash.py crash-20261001-121314.txt build/symbols/ProsperoEden-v1.000.040.elf
```

The second argument is the unstripped executable of the build that wrote the report:
`build/headless-native/llvm-pie.elf` right after a build. `make release` copies it to
`build/symbols/ProsperoEden-vX.Y.Z.elf`; keep that file with the release, it is not published.

`make test` checks the report on the host (`tools/check-crash-report.py`). A development build
crashes on request, to try it on a console: write `segv`, `thread`, `abort` or `throw` to
`crash-app.txt` in the app folder.

## Release workflow

`.github/workflows/release.yml` runs `tools/ci/build-release.sh` (`make release`) on a
GitHub-hosted runner (`ubuntu-24.04`), without a container. Ubuntu 24.04 lacks part of what the
build needs, so `tools/ci/setup-ubuntu-24.04.sh` installs the packages in
`tools/ci/ubuntu-24.04-packages.txt` with LLVM 21 from apt.llvm.org (the same LLVM versions as a
build on your own machine), Meson from PyPI, and builds the SPIR-V LLVM translator for LLVM 21
from source (kept in a cache); CMake is the runner's own. The workflow first deletes SDKs the
runner image carries (.NET, Android, GHC, parts of the tool cache) for
disk space, and builds with as many jobs as the runner has cores (`JOBS`, and `RADV_BUILD_JOBS`
for RADV, whose build otherwise runs 24).

Between runs it caches the downloads (`PROSPEROEDEN_DEPS_CACHE`, keyed on `tools/deps.json`),
ccache, and the built RADV driver with the SDK it is linked with (keyed on its pins and build
scripts), so a later run compiles mostly what changed and takes about ten minutes. A run without
these caches builds everything on four cores and takes about an hour. Should a build ever reach
the build step's limit (315 minutes), ccache is still saved, and running the workflow again
continues from there. A pull request starts from `main`'s caches and, when it builds, saves no
ccache of its own.

GitHub drops a cache that nobody used for a week, and `main` is only built by hand, so ccache has
a second source: `ccache-u24.tar`, a file of the `build-cache` release. A run that finds no ccache
downloads it. To replace it, start the workflow on `main` by hand with "snapshot" ticked;
`.github/workflows/build-cache.yml` then publishes that run's ccache.

`tools/ci/build-release.sh` still accepts `EDEN_DEV_CHECKOUT` (a development checkout whose
dependencies are reused instead of fetched) for a build on your own machine.

- **Pull request** (by itself, at every push to it) and **manual run** (Actions > Release build >
  Run workflow): builds the release files, checks the ZIP (intact, `eboot.bin` present, every
  entry stored as 0777) and keeps `dist/` (the ZIP, `SHA256SUMS` and `release-notes.md`) as a
  7-day artifact. Nothing is published. A push to
  `main` builds nothing: a build of `main` is a manual run. The artifact is `ProsperoEden`; for
  a pull request it is `ProsperoEden-PR<number>-<commit>`, with the first seven characters of
  the pull request's own head commit. A newer push to a pull request, or a newer manual run on
  the same branch, cancels the run in progress.
- **Tag `vX.Y.Z`** (by itself, when the tag is pushed): builds and checks them the same way, checks that the tag matches the package
  version, and publishes a pre-release with the ZIP and `SHA256SUMS` (a second job). The release notes come from the README's "Changes in vX.Y.Z" section.
- Every run also keeps `build/symbols/` as an artifact with `-symbols` after the name: 90 days
  for a tag, 7 days otherwise.
- A tag's or a manual run's ZIP is attested (signed build provenance): a release ZIP built by
  the workflow can be checked with
  `gh attestation verify ProsperoEden-vX.Y.Z.zip -R blackbearreloaded/ProsperoEden` (GitHub
  CLI). This covers releases built by GitHub Actions from now on (after v1.000.090), not
  earlier ones.

A release is made by pushing the tag: the workflow builds, attests and publishes the ZIP and
`SHA256SUMS`. Do not attach files to a release by hand. With no release for the tag, the workflow
creates it. A release that already exists without a ZIP (notes written in advance, or a draft)
gets the workflow's ZIP and `SHA256SUMS`, and keeps its title and notes. A release that already
has a ZIP keeps its files (the catalog at homebrew.page records each release ZIP's checksum, so
a published ZIP is never replaced): the run ends successfully with a warning that those files
were not published by it and may have no attestation.

To cut a release:

1. Bump `contentVersion` in `sce_sys/param.json` (the launcher is built with it, and the package
   carries the file).
2. Add the "Changes in" section to the README.
3. Test the build on a console.
4. Push a `vX.Y.Z` tag.
5. Keep `build/symbols/ProsperoEden-vX.Y.Z.elf` from the build that was published (crash reports
   are read with it): download the tag run's `ProsperoEden-symbols` artifact before it expires.
