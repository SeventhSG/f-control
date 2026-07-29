# f-control v0.1.0-dev

**Encrypted with a shared network key. Not private, per-person end to end encryption.**

This is the first build that runs on a real ESP32. Two boards should find each other over the air, appear in each other's rosters with a signal-derived distance, and pass short messages sealed under a key you choose when you flash them. That is the whole claim.

## Read this before you flash anything

> **Every board given the same network key can read every message on that network, and nobody without it can.** That is real protection against a stranger with a receiver, and it is just as real a limit: it does not say who sent a message, it does not stop one person on your network reading another's traffic, and nothing here is signed, so anyone holding the key can claim to be anyone. This is a shared secret among a group, not a private channel between two people.
>
> The board says this on boot. The dashboard says it on every screen. It is repeated here because a release page is where people stop reading warnings.

## What works

| | |
|---|---|
| Boots on ESP32 | yes, verified on an ESP32-D0WD-V3 |
| Own access point and dashboard | yes, served from flash, 44 KB gzipped |
| Beacons every 5 seconds | yes |
| Roster with signal-derived distance | yes |
| Mesh relay with flood suppression | yes, tested to 100 simulated nodes |
| Messages sealed with a shared network key | yes, XChaCha20-Poly1305, see below |
| Desktop flasher writes firmware | yes, verified against a real board |
| Flasher sets your name, network name, wifi, and network key at flash time | yes |
| Rename yourself, join wifi from the dashboard too | yes, in Settings, kept across restarts |
| Delivery confirmation | **no**, there are no acknowledgements yet |
| Per-contact end to end encryption, signed identity | **no** |

## What the encryption actually is

Every board holds one 32 byte key, derived from a passphrase you set in the flasher (or one it generates for you). Every message is sealed under that key with XChaCha20-Poly1305, a standard authenticated cipher. A passive listener without the key, which is almost anyone nearby, reads nothing.

This was chosen over the Noise XX handshake the original design spec describes because Noise needs a real per-contact key exchange and signed identities, neither of which exist yet, and building that quickly is exactly how a privacy tool ends up with a bug that hurts someone. A shared key is honest about being simpler: it protects the network from outsiders, not members of the network from each other. The real per-contact handshake is still the plan; this is what shipped in the meantime rather than nothing.

## Flashing, and setting up the board at the same time

### With the desktop app, Windows

Download **`f-control-v0.1.0-dev-windows.zip`**, extract it anywhere, and run **`f-control-flasher.exe`**. No installer, nothing to configure. Plug in an ESP32, the app finds it, and before you write, you can set:

- **Name**, what other boards see. Optional, defaults to "unnamed".
- **Network name**, the wifi network this board hosts. Optional, defaults to an automatic `f-control-xxxx`.
- **Home wifi**, join a network you already have instead of hosting one. Optional, leave blank to skip.
- **Network key**, generated for you, shown, and copyable. **Boards that should talk to each other need the same one.** It carries over automatically if you flash more than one board in the same run of the app, or paste in a key from elsewhere.

Click **Write firmware**. Everything above is sent to the board right after writing, over the same USB connection, and the board restarts already configured. Nothing needs to be typed into the dashboard by hand unless you want to change it later.

`START-HERE.txt` in the zip covers the whole thing, including what to do when a board does not show up.

The zip was extracted to a clean folder and run from there before release, so it does not depend on anything left behind by a build.

It also runs headless, for flashing several boards without the window:

```
f-control-flasher-windows-x64.exe --list
f-control-flasher-windows-x64.exe --flash COM3
f-control-flasher-windows-x64.exe --monitor COM3
```

The headless `--flash` path writes firmware only; the setup fields above are a GUI feature for now.

macOS and Linux builds are not ready. Use esptool below, then set everything from the dashboard's Settings screen instead.

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

If the board will not connect, hold the **BOOT** button while plugging it in, release it, then try again. Some boards' auto-reset circuit is unreliable enough that this is needed every time.

## Using it

1. Flash both boards, giving them the **same network key** if you want them to be able to read each other.
2. Join the wifi network you set, or the automatic `f-control-xxxx` if you left it blank, from a phone.
3. Open **http://192.168.4.1**.
4. The other board should appear in the roster within about ten seconds.

**To send messages you need two client devices,** one for each board, unless both boards were joined to the same home network. Each board's own access point is a separate wifi network, and a phone can only be joined to one at a time.

## Joining your own wifi from the dashboard

The flasher can do this at flash time now, but the dashboard's Settings screen still works too, for changing it later. Press **Find networks**, pick yours, type the password, join. The board remembers it and rejoins by itself after a restart.

One thing to understand, because it is physics rather than a bug. The board has **one radio**, so when it joins a network on channel 6, the mesh moves to channel 6 along with it. Two boards on the **same** network land on the same channel and talk to each other normally. Two boards on **different** networks cannot hear each other at all. The settings screen shows which channel you are on.

Access point mode, the default, always sits on channel 1 and always works.

## Known rough edges

- **Sent messages sit at "sending" forever.** There are no acknowledgements in this build, so the board genuinely does not know whether anything arrived. Check the receiving board to confirm.
- **Every peer shows as unverified with a redacted fingerprint.** That is correct: the fingerprint is not signed, so there is nothing to cryptographically verify against yet.
- **The access point is open, with no wifi password.** The network key protects the messages themselves regardless of who can join the wifi.
- **Timestamps are uptime, not wall clock.** The board has no clock and does not pretend to.
- **One board in hand needed the physical BOOT button held to flash**, because its auto-reset circuit did not respond to the software reset sequence. This is a known quirk of some cheap ESP32 boards, not a bug in the flasher; if writing fails at the handshake step, hold BOOT while plugging in and try again.

## Checksums

See `SHA256SUMS` in this directory.
