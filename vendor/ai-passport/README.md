<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# DSH Passport

Turn the [FoloToy AI Passport](https://ai-passport.folotoy.cn/) into a
**pocket task terminal** for [DeepSeek Harness](https://github.com/deepseek-ai) (DSH).

Manage the AI tasks running on your computer right from the device:

| What you can do | Description |
| --- | --- |
| Task board | Browse, switch, and create DSH tasks; a running task shows its current step and the operation being executed |
| Completion alerts | Banner + sound when a task finishes, with distinct sounds for success, failure, and interruption; unread tasks carry a red dot |
| Approve tool calls | When the AI on your computer touches files or runs commands, the device pops up an authorization page; pick allow/deny with the up/down keys; an expired countdown defaults to deny |
| Speak to instruct | Hold to record and release when done; Chinese speech is transcribed and filled into the computer's input box |
| Check balance | Hold the down key to view the DeepSeek balance summary |

The link is a **direct Bluetooth connection to the computer**: no Wi-Fi, no network
provisioning, no passwords. On first pairing the device shows a 6-digit code to enter
on the computer once; the code changes on every power-up.

## Required companion computer-side plugin

This firmware only covers the device half; the full experience needs the computer-side plugin:

**Computer-side plugin: [`slinxiaosun-blip/dsh-ai-passport-plugin`](https://github.com/slinxiaosun-blip/dsh-ai-passport-plugin)**

It handles the Bluetooth link, task bridging, speech recognition, and the desktop widget.
To install, clone that repository and run `npm run install-plugin` (the same command on
macOS and Windows; the installer adapts to both platforms). See that repository's README
"Quick Start" for details.

The protocol version must match between firmware and plugin (currently protocol v1).
The firmware is **host-platform agnostic**: the same image works with DSH clients on
macOS and Windows, so one flash covers both. The device is a pure BLE peripheral,
chunking follows the negotiated MTU, and pairing uses an application-level code —
none of it depends on the host operating system.

## Download the firmware (no build required)

The latest firmware is on the
[Releases](https://github.com/slinxiaosun-blip/ai-passport-dsh/releases/latest) page;
download `FoloToy-AI-Passport-full.bin` and flash it from `0x0` in one go:

```bash
# macOS / Linux (serial ports look like /dev/cu.usbserial-XXXX or /dev/cu.wchusbserial-XXXX)
esptool.py -p /dev/cu.usbserial-XXXX write_flash 0x0 FoloToy-AI-Passport-full.bin

# Windows (serial ports look like COM5; check Device Manager)
esptool.py -p COM5 write_flash 0x0 FoloToy-AI-Passport-full.bin
```

Flashing overwrites the firmware already on the device and does not guarantee that
existing settings survive. **Note the current firmware version before flashing** so
you can roll back. To restore the stock firmware, use the
[AI Passport Web Flasher](https://ai-passport.folotoy.cn/tools/web-flasher/).

## Build it yourself

ESP-IDF **v5.5.3** is required (installed outside the repository, path without spaces):

```bash
# Repository checks + logic tests (seconds, no hardware needed)
./tools/validate.sh --static

# Full build producing a merged image flashable from 0x0 (minutes)
./tools/validate.sh --firmware
```

Artifact: `build/FoloToy-AI-Passport-full.bin`

Flash (this overwrites the firmware on the device):

```bash
# macOS / Linux
esptool.py -p /dev/cu.usbserial-XXXX write_flash 0x0 build/FoloToy-AI-Passport-full.bin

# Windows
esptool.py -p COM5 write_flash 0x0 build/FoloToy-AI-Passport-full.bin
```

To restore the stock firmware, use the
[AI Passport Web Flasher](https://ai-passport.folotoy.cn/tools/web-flasher/).
For build-environment setup, common deviations, plugin installation, and enabling the
speech model, see the plugin repository's
[docs/03 build and flash guide](https://github.com/slinxiaosun-blip/dsh-ai-passport-plugin/blob/main/docs/03-%E6%9E%84%E5%BB%BA%E4%B8%8E%E7%83%A7%E5%BD%95.md).

## Version

The firmware version comes from `version.txt` (currently `1.1.0`); the build appends a
short git hash automatically, e.g. `1.1.0+g1a2b3c4-dirty`. This string is reported to the
computer during the handshake, and the widget's info cell shows the real version running
on the device.

- `1.1.0` — released together with plugin 1.1.0 (the plugin side now adapts to both the
  macOS and Windows DSH clients). Firmware behavior is identical to `1.0.0`; this release
  only aligns version numbers, and one flash covers both host platforms.
- `1.0.0` — first complete version (task board / approvals / balance / voice / pairing).

## Origin and License

This repository is a secondary development of the upstream
[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) `main @ 0b9e4c8` (MIT) by
[slinxiaosun-blip](https://github.com/slinxiaosun-blip). All changes live on the
`feature/dsh-passport` branch:

- **Added**: `main/app_*.c/.h` device application (Bluetooth link, task board, approvals,
  Q&A, voice, pairing, settings)
- **Invasive changes**: four spots in `components/bsp`, `main/CMakeLists.txt`,
  `main/main.c`, `sdkconfig.defaults`, `tests/bsp_stubs`, `tools/`
- The upstream `main` branch stays in sync with `FoloToy/ai-passport` and carries none of
  this project's changes

Released under the upstream MIT license; the original copyright belongs to FoloToy.
