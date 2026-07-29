# f-control v0.1.0-dev

**A hardware bring-up build. There is no encryption in it.**

This is the first build that runs on a real ESP32. Two boards should find each other over the air, appear in each other's rosters with a signal-derived distance, and pass short messages. That is the whole claim.

## Read this before you flash anything

> **Everything this release sends is in the clear.** There is no encryption, no signatures, and no verification. Anyone with a receiver in range can read every message. Anyone can broadcast any name and be believed. The fingerprint words shown in the interface are derived from a random number, not from a key, and prove nothing at all.
>
> The board says this on boot. The dashboard says it on every screen. It is repeated here because a release page is where people stop reading warnings.
>
> Do not use this to say anything you would not shout across a car park.

## What works

| | |
|---|---|
| Boots on ESP32 | yes, verified on an ESP32-D0WD-V3 |
| Own access point and dashboard | yes, served from flash, 42 KB gzipped |
| Beacons every 5 seconds | yes |
| Roster with signal-derived distance | yes |
| Mesh relay with flood suppression | yes, tested to 100 simulated nodes |
| Short messages between boards | yes, unencrypted |
| Desktop flasher writes firmware | yes, verified against a real board |
| Delivery confirmation | **no**, there are no acknowledgements yet |
| Encryption, signatures, verification | **no** |
| Joining your home wifi | **no**, access point mode only |

## Flashing

### With the desktop app, Windows

Download **`f-control-v0.1.0-dev-windows.zip`**, extract it anywhere, and run **`f-control-flasher.exe`**. No installer, nothing to configure. Plug in an ESP32, the app finds it, click **Write firmware**. `START-HERE.txt` in the zip covers the whole thing including what to do when a board does not show up.

The zip was extracted to a clean folder and run from there before release, so it does not depend on anything left behind by a build.

The firmware is compiled into the app itself, so there is no download step and nothing on disk between the build and your board.

It also runs headless, which is useful for flashing several boards in a row:

```
f-control-flasher-windows-x64.exe --list
f-control-flasher-windows-x64.exe --flash COM3
```

macOS and Linux builds are not ready. Use esptool below.

### With esptool, any platform

You need [esptool](https://github.com/espressif/esptool) (`pip install esptool`) and a USB cable that carries data, not just power.

```
esptool.py --chip esp32 -p COM3 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 40m \
  0x1000  bootloader.bin \
  0x8000  partition-table.bin \
  0x10000 f-control.bin
```

Replace `COM3` with your port. On macOS and Linux it looks like `/dev/tty.usbserial-0001` or `/dev/ttyUSB0`.

Verify the downloads first:

```
sha256sum -c SHA256SUMS
```

If the board will not connect, hold the **BOOT** button while plugging it in, release it, then try again.

## Using it

1. Flash both boards. Each generates its own random identity on first boot and keeps it, including across reflashing.
2. Open the serial monitor at 115200 baud to see the fingerprint and access point name, or skip this and read the name off the wifi list.
3. On a phone, join the open network **`f-control-XXXX`** and open **http://192.168.4.1**.
4. The other board should appear in the roster within about ten seconds.

**To send messages you need two client devices,** one for each board. Each board runs its own access point, and a phone can only be joined to one network at a time, so a single phone can watch one board but not both ends of a conversation.

## Known rough edges

- **Sent messages sit at "sending" forever.** There are no acknowledgements in this build, so the board genuinely does not know whether anything arrived. The receiving board will show the message. The sending board will not find out.
- **Every peer shows as unverified with a redacted fingerprint.** That is correct: there is nothing to verify against.
- **The access point is open, with no password.** Adding one would be theatre while the radio itself is in the clear.
- **Timestamps are uptime, not wall clock.** The board has no clock and does not pretend to.
- Two boards have been built and flashed from this exact tree, but a two-board conversation has not yet been observed by the author. You may be the first person to see it work, or not work.

## Checksums

See `SHA256SUMS` in this directory.
