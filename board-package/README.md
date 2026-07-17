# AutoTLM boards package

Makes **Tools → Board → AutoTLM One** appear in the Arduino IDE. It is a thin
platform that *references* the Espressif esp32 Arduino core — the toolchain
we build on (`build.core=esp32:esp32`) — so Espressif's compiler does the
building while the IDE shows our board, our pin map
(`variants/one/pins_arduino.h`) and our defaults. FQBN: **`autotlm:esp32:one`**.

> Why not `autotlm:autotlm:one`? The Arduino platform spec only lets a
> platform reference a core from a package **of the same architecture** — a
> literal `autotlm` architecture would mean forking the entire esp32 core.
> Vendor prefix + esp32 architecture is the standard thin-wrapper pattern.

## What users do (put this in the main README / site)

1. **File → Preferences → Additional boards manager URLs**, add:
   `https://raw.githubusercontent.com/AcidAlchamy/autotlm-core/master/board-package/package_autotlm_index.json`
2. **Boards Manager**: install **esp32** (Espressif, 3.x) and **AutoTLM Boards**.
3. **Tools → Board → AutoTLM Boards → AutoTLM One.** Done.

arduino-cli equivalent:

```
arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/AcidAlchamy/autotlm-core/master/board-package/package_autotlm_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32 autotlm:esp32
arduino-cli compile --fqbn autotlm:esp32:one <sketch>
```

## Publishing a release

The index points at a GitHub release asset. After changing the platform:

1. Bump `version=` in `autotlm/esp32/platform.txt`.
2. Run `make-release.ps1` — it zips the platform and prints the checksum/size.
3. Create a GitHub release tagged `boards-<version>` on `autotlm-core` and
   upload the zip as an asset.
4. Paste the printed version/checksum/size into `package_autotlm_index.json`
   (keep old platform versions in the array if you want them installable) and
   commit — the raw-URL index updates instantly.

## Local development install (no release needed)

```
powershell -File board-package\install-local.ps1
```

installs the platform into `Documents\Arduino\hardware\autotlm\esp32` and
copies in the helper tools (below). The esp32 core must already be installed
via Boards Manager. The FQBN is the same either way: `autotlm:esp32:one`.

## Why `tools/` is copied in (not in git)

The esp32 core's build recipes resolve a few helpers relative to the
**board's** platform (`{runtime.platform.path}/tools/...`): the partition
tables + `boot_app0.bin`, `gen_esp32part`, and `espota`. A referencing
platform therefore has to carry copies. They're pulled from the locally
installed esp32 core by `install-local.ps1` (dev) and `make-release.ps1`
(release zip) rather than committing ~12 MB of Espressif binaries to git —
which also means a release is built against, and stays in sync with, the
esp32 core version on the release machine.

**Exception:** `autotlm/esp32/tools/partitions/autotlm_ota.csv` IS in git —
it's ours, not Espressif's. It is the **locked AutoTLM One partition table**
(owner ruling 2026-07-14): 4 MB flash as two 1.875 MB OTA app slots + NVS +
coredump, decided once so every unit flashed from boards 0.4.0 onward can
receive BLE and OTA features later without a destructive field re-partition.
It is the default `PartitionScheme`; the stock esp32 schemes remain in the
menu for bench experiments only. Both copy scripts merge the esp32 core's
partition files *around* it.

### 🔒 Hard constraint: `otadata` @ 0xE000 and `app0` @ 0x10000

Never move those two. The esp32 core hardcodes them as **literals** in its
`upload`, `merge-bin` and `program` recipes (platform.txt: `… 0xe000
boot_app0.bin 0x10000 firmware.bin`), so a table that relocates them makes
every arduino-cli/IDE upload write the sketch into whatever partition sits at
0x10000 — the bootloader then finds no bootable slot and the unit boot-loops
(`ota data partition invalid… No bootable app partitions`).

**Boards 0.3.0 shipped exactly that bug** (NVS widened to 0x14000 pushed
`app0` to 0x20000) and bricked the bench One on its first flash; it was
recalled and pulled from the index in favour of 0.4.0. **Grow the app slots,
never the head of the table.**

Compiling proves nothing about this — 0.3.0 compiled perfectly. So CI runs
`verify-partitions.py`, which stands in for the flash: it asserts the two
immovable offsets, that both OTA slots exist and match, and that nothing
overlaps or runs past 4 MB. Run it yourself after any table change:

```
python3 board-package/verify-partitions.py
```

For a deeper check, the core's own parser also round-trips the table:

```
python <esp32-core>/tools/gen_esp32part.py --verify autotlm/esp32/tools/partitions/autotlm_ota.csv out.bin
```
