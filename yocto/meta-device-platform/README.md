# meta-device-platform

Custom Yocto layer (Plan.md V6) that turns the stock `raspberrypi5`
machine into the full custom-acq acquisition stack: kernel driver,
Device Tree overlay, C++ service, and WiFi/SSH so the result is actually
reachable. None of the source these recipes build lives in this
layer - each recipe points back at the real source elsewhere in this
repo (`driver/custom-acq/`, `device-tree/`, `userspace/device-service/`)
via `FILESEXTRAPATHS`, so there's exactly one copy of everything to keep
in sync.

## Layers this depends on

- `poky/meta`, `poky/meta-poky` (core)
- `meta-openembedded/meta-oe` (nlohmann-json, spdlog)
- `meta-raspberrypi` (MACHINE=raspberrypi5 support)

All three live outside this repo (see `docs/session-log.md`'s V6 entries
for the exact clone/setup steps) - only this layer's own source is
tracked here.

## Recipes

| Recipe | What it builds |
|---|---|
| `recipes-kernel/custom-acq-driver` | The SPI kernel driver (`driver/custom-acq/`) |
| `recipes-bsp/custom-acq-overlay` | Compiles `device-tree/custom-acq-overlay.dts` into `custom-acq.dtbo` |
| `recipes-apps/device-service` | The C++ acquisition service (`userspace/device-service/`) |
| `recipes-support/configuration/device-platform-network` | wpa_supplicant + systemd-networkd for WiFi |
| `recipes-core/images/device-platform-image` | Assembles all of the above on top of `core-image-minimal` |

## WiFi credentials

Never committed here. `device-platform-network`'s `DEVICE_PLATFORM_WPA_CONF`
variable points at a real `wpa_supplicant.conf` kept entirely outside
this repo (e.g. `/opt/yocto/local-config/wpa_supplicant.conf`, set via
`local.conf`); `files/wpa_supplicant.conf.example` here is just a
placeholder template so a fresh checkout still builds.

## Build

```bash
bitbake device-platform-image
```

Verified end to end on real Pi 5 + MCU hardware - see
`docs/session-log.md`'s 2026-09-03 V6 entries for what was tested and
the environment-specific gotchas that came up (RPI_EXTRA_CONFIG scope,
core-image-minimal not pulling in WiFi firmware/kernel-modules by
default).
