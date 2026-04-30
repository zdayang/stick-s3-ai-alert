# StickS3 AI Alert

Firmware and helper scripts for turning an M5Stack StickS3 into a small AI coding alert device.

The device can show alerts from Codex and Claude Code over BLE. It is designed to sit idle with the screen off, flash and beep briefly when attention is needed, then turn the screen off again. Press `A` to review the last alert.

[中文说明](README.zh-CN.md)

## Current Firmware Scope

This repository currently builds the full StickS3 mini launcher firmware, not an AI Alert-only image.

The compiled firmware includes:

- `AI Alert`
- `Dodge`
- `Stone`
- `MineZ`
- `Settings`

`AI Alert` is the main feature, while the games and settings screens are kept as part of the current playground-style firmware.

## Features

- Vertical StickS3 launcher with multiple apps.
- `AI Alert` app for Codex and Claude Code notifications.
- BLE GATT receiver using the Nordic UART-style UUIDs.
- Alert payload format: `TOOL|SESSION|STATE|HH:MM`.
- Distinguishes:
  - `CX`: Codex
  - `CC`: Claude Code
  - `ASK`: approval / intervention needed
  - `DONE`: current turn or task finished
- Shows the source tool, session/project label, alert state, and local time.
- Flashes 3 times, beeps 3 times, then automatically turns the screen off.
- Press `A` on a dark screen to show the last alert.
- Includes small games and system settings used during development.

## Hardware

- M5Stack StickS3
- USB-C cable
- macOS computer for Codex / Claude Code BLE hooks

The firmware is built with PlatformIO and Arduino.

## Firmware Setup

Install PlatformIO:

```sh
python3 -m pip install --user -U platformio
```

Build:

```sh
pio run
```

Upload:

```sh
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

Find the serial port with:

```sh
ls /dev/cu.*
```

If upload cannot connect, put StickS3 into download mode: connect USB, hold the side reset/power key until the internal green LED blinks, then retry the upload command.

## macOS BLE Hook Setup

Create a Python virtual environment and install BLE support:

```sh
python3 -m venv ~/.stick-s3-ai-alert-venv
~/.stick-s3-ai-alert-venv/bin/python -m pip install -U pip bleak
```

Copy the helper script somewhere stable:

```sh
mkdir -p ~/.stick-s3-ai-alert
cp scripts/stick_alert.py ~/.stick-s3-ai-alert/stick_alert.py
chmod +x ~/.stick-s3-ai-alert/stick_alert.py
```

Manual test:

```sh
printf '{"hook_event_name":"ManualTest"}' | \
~/.stick-s3-ai-alert-venv/bin/python ~/.stick-s3-ai-alert/stick_alert.py codex done
```

The StickS3 must be in the `AI Alert` app. The app starts BLE advertising as `StickS3-AI`.

On first use, macOS may ask for Bluetooth permission for Terminal, Python, or your terminal app. Allow it.

## Codex Configuration

Add hooks to `~/.codex/config.toml`:

```toml
[features]
codex_hooks = true

[[hooks.PermissionRequest]]
matcher = ".*"

[[hooks.PermissionRequest.hooks]]
type = "command"
command = "~/.stick-s3-ai-alert-venv/bin/python ~/.stick-s3-ai-alert/stick_alert.py codex ask"
timeout = 10
statusMessage = "Sending StickS3 alert"

[[hooks.Stop]]

[[hooks.Stop.hooks]]
type = "command"
command = "~/.stick-s3-ai-alert-venv/bin/python ~/.stick-s3-ai-alert/stick_alert.py codex done"
timeout = 10
statusMessage = "Sending StickS3 alert"
```

Restart Codex after changing the config.

## Claude Code Configuration

Add hooks to `~/.claude/settings.json`:

```json
{
  "hooks": {
    "Notification": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "~/.stick-s3-ai-alert-venv/bin/python ~/.stick-s3-ai-alert/stick_alert.py claude ask"
          }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "~/.stick-s3-ai-alert-venv/bin/python ~/.stick-s3-ai-alert/stick_alert.py claude done"
          }
        ]
      }
    ]
  }
}
```

Restart Claude Code after changing the config.

## Device Controls

Launcher:

- `B` click: next app
- `B` hold: previous app
- `A` click: enter app
- `A` hold: settings
- `A+B` hold: home

AI Alert:

- Idle: screen turns off after the ready preview.
- New alert: flashes and beeps 3 times, then auto-off.
- `A` while alert is visible: turn screen off.
- `A` while screen is off: show the last alert.
- `B`: mute / unmute beeps.
- `A` hold: return home.

## Privacy Notes

The repository intentionally excludes:

- PlatformIO build output
- Arduino CLI caches
- downloaded binaries
- local hook installations
- local logs
- personal Codex / Claude configuration files

The helper script only sends a short BLE message to the StickS3. It does not call any cloud API.

## License

MIT
