# Filesystem access with upstream Lapy

ProsperoEden uses [PS5-Lapy-JB-Daemon](https://github.com/ArkSama/PS5-Lapy-JB-Daemon),
created by ArkSama, and pins the cooperative owned-root implementation from
[ProsperoEden's Lapy fork](https://github.com/blackbearreloaded/PS5-Lapy-JB-Daemon), based on
[mpereiraesaa's cooperative helper](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon). The build invokes
upstream's own `owned-helper` target for `PPSA99008`; it does not copy or modify Lapy's privileged
source. Before packaging, it verifies the generated manifest's title, mode, ELF and protocol
hashes, and `root_layout_probe_retry` feature.

At single-threaded startup, the client first publishes the cooperative resident-service request
through `/download0/elevate_proc`. It waits 1.5 seconds for verified `/data` read/write access. If
the resident service claimed the marker, the one-shot path is not started. If the app cancels an
unclaimed marker, it observes one further grace interval and then sends `/app0/lapy.elf` to the
local ELF loader on TCP port 9021. The same connection carries upstream's fixed-width
request/prepare/prepared/response exchange. Only a successful response followed by an actual
`/data` write/read/delete probe enables the normal data paths.

The package includes `lapy.elf`, `lapy-manifest.json`, and Lapy's MIT license. It contains no
locally implemented kernel mutation code. A jailbreak environment with an ELF loader on port
9021 is required when a resident Lapy service is not already available.

The previous donor-release helper passed repeated lifecycle tests on firmware 6.02 and 12.70.
The pinned target-lifetime helper completed its exit-race stress and a full one-shot elevation on
firmware 12.70. Other SDK-supported firmware, including ProsperoEden's firmware 6.02 test console,
remains experimental for this exact pin until it completes an attended launch and clean-exit test.

Credits: Lapy was created by [ArkSama](https://github.com/ArkSama), and the cooperative helper
used here comes from
[mpereiraesaa and contributors](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon/graphs/contributors).
