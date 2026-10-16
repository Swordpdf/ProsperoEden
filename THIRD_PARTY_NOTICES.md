# Third-party notices

ProsperoEden is licensed under GPL-3.0-or-later (see [LICENSE](LICENSE)). It
builds on the following projects, each under its own license.

## Emulator

- **[Eden](https://github.com/eden-emulator/mirror)**, GPL-3.0-or-later. The emulator core, built
  from the commit pinned in `UPSTREAM.json`, together with the dependencies Eden
  fetches and pins itself: Dynarmic, Boost, fmt, xbyak, zstd, lz4, Opus, SDL3,
  simpleini, sirit, SPIRV-Headers, Vulkan-Headers, Vulkan-Utility-Libraries,
  Vulkan Memory Allocator, ENet, frozen, cpp-httplib, nlohmann/json, and the time
  zone data. Each keeps its upstream license.
- **[FFmpeg](https://ffmpeg.org)**, LGPL-2.1-or-later. Built from Eden's pinned
  source with only the H.264, VP8 and VP9 decoders.
- **[OpenSSL](https://www.openssl.org)**, Apache-2.0, and
  **[zlib](https://zlib.net)**, zlib license. Taken from the
  [pacbrew](https://github.com/ps5-payload-dev/pacbrew-repo) PS5 packages.
- **[curl](https://curl.se)** (libcurl), curl license, and
  **[libpsl](https://github.com/rockdaboot/libpsl)**, MIT. From the same pacbrew
  packages; the update check uses them.
- **[PS5 Native App Boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)**,
  GPL-3.0-or-later. Its update check and self-update kit (`headless/update_check`),
  its self-update helper (`headless/self_update_helper`, built into `self-updater.elf`)
  and `tools/validate-loader-elf.py`.
- **[miniz](https://github.com/richgel999/miniz)** 3.0.2, MIT, unmodified in
  `third_party/miniz` with its `LICENSE`. The self-update helper reads release
  ZIPs with it; the app packs and unpacks save data for the save sync with it.
- **[QR Code generator library](https://github.com/nayuki/QR-Code-generator)** (C), MIT,
  unmodified in `third_party/qrcodegen` with its `LICENSE`. The launcher draws the QR code
  of a save sync's pairing with it.
- **LLVM compiler-rt** `emutls.c`, Apache-2.0 WITH LLVM-exception.

## Graphics

- **[PS5 Mesa](https://github.com/mihawk-99/PS5_Mesa)** and
  **[PS5 Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** by Mihawk. The
  RADV Vulkan driver (Mesa, mainly MIT) and its PS5 link recipe and platform
  layer (GPL-3.0-or-later).
- **[ps5-vulkan](https://github.com/mpereiraesaa/ps5-vulkan)** by mpereiraesaa,
  an experimental Vulkan graphics and compute API for native PS5 homebrew.
- **[ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl)**,
  GPL-3.0-or-later, which includes Mesa components under their own licenses.
  `third_party/app_heap.c` comes from it.
- **[bcdec](https://github.com/iOrange/bcdec)** by Sergii "iOrange" Kudlai, MIT or public
  domain, unmodified in `third_party/bcdec` with its `LICENSE`. Decodes the BC7 blocks of a
  DDS wallpaper into pixels (docs/WALLPAPERS.md).

## Platform and interface

- **[PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk)** by John
  Törnblom (ps5-payload-dev).
- **[PS5 Native App Boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)**,
  GPL-3.0-or-later. The native app runtime and packaging tool. Its packaging
  tool translates parts of SvenGDK's
  [SharpProspero](https://github.com/SvenGDK/SharpProspero).
- **[PS5-Lapy-JB-Daemon](https://github.com/ArkSama/PS5-Lapy-JB-Daemon)** by
  ArkSama, with the cooperative owned-root design from
  **[mpereiraesaa's fork](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon)** and the pinned
  **[ProsperoEden compatibility fork](https://github.com/blackbearreloaded/PS5-Lapy-JB-Daemon)**.
  Lapy is MIT licensed; its shared protocol header is LGPL-2.1-or-later.
  ProsperoEden pins the fork, invokes its unmodified exact-title helper build,
  verifies its generated manifest and bundles the helper ELF with its license.
- **[ps5log](https://github.com/mpereiraesaa/ps5-agc-gears/tree/1ae1f9182abd2770c131b97419034fb85173c2dc/native/ps5log)**,
  GPL-3.0-or-later. The pinned single-header logging client used as an upstream
  Lapy helper build input.
- **[ProsperoPuzzles](https://github.com/blackbearreloaded/ProsperoPuzzles)**,
  GPL-3.0-or-later. The launcher's drawing, text, animation and sound code
  (`headless/prosperoeden/pe`) started there.
- **[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui)**,
  GPL-3.0-or-later. The launcher's themes start from its theme recipes: its
  theme table (`src/ui/theme.cpp`) is compiled into the launcher. Fetched at
  the commit pinned in `tools/deps.json`.
- **Montserrat** by Julieta Ulanovsky and contributors, SIL Open Font License
  1.1 (`third_party/fonts`). The launcher's font is baked from Montserrat Medium.
- **[HarfBuzz](https://github.com/harfbuzz/harfbuzz)**, "Old MIT" license (its
  `COPYING` file). Shapes the launcher's text in the scripts that need it, such
  as Thai and Arabic. Fetched at the release pinned in `tools/deps.json`.
- **[stb](https://github.com/nothings/stb)** by Sean Barrett, MIT or public
  domain (`tools/launcher/stb` and `third_party/stb`). `stb_truetype` bakes the launcher's font and,
  in the app, reads the console's system fonts for the scripts Montserrat does
  not have. `stb_image` (`third_party/stb`) reads the wallpaper images in the app.
  `stb_image_write` saves the PC previews and is not part of the app.
- The launcher's sound effects were generated with
  [ElevenLabs](https://elevenlabs.io) and edited for this project.
- `third_party/ps5_pad.hpp` and `third_party/native_audio.hpp` are this
  project's own platform declarations, GPL-3.0-or-later.

## Thanks

ProsperoEden exists thanks to the Eden maintainers and contributors, Mihawk,
mpereiraesaa, John Törnblom, SvenGDK, and the whole PS5 homebrew community.

## Lua 5.4.9

Embedded launcher scripting runtime. Copyright Lua.org, PUC-Rio. MIT license; see `third_party/lua/LICENSE.txt`. Source and checksum: `third_party/lua/SOURCE.json`.
