# Running THR Remote on Linux (Wine)

Instructions and a compatibility layer for running Yamaha's THR II editor, "THR Remote" (the Windows version), under Wine and connecting it to the amp.

**This is an unofficial tool.** It is not affiliated with Yamaha; use it at your own risk. This repository contains no Yamaha files (no installer, no DLLs). You use the application as obtained by yourself.

## Tested environment

| Item | Details |
|---|---|
| OS | Linux Mint 22.3 (Ubuntu 24.04 based) |
| Wine | 9.0 (Ubuntu package) |
| THR Remote | 1.7.0 (Windows version) |
| Amp | THR10II Wireless , firmware 1.50.0b |
| Audio | PipeWire |

What was verified: detecting the amp, the handshake, and displaying the amp's current preset values. The following is **not verified**:

- Saving and loading presets, and applying knob changes to the amp (any of it)
- Firmware updates (the amp re-enumerates as a different USB device during an update, which this layer has not been tested with. **Updating the firmware from Windows is recommended.**)
- Amps other than the THR10II Wireless

## How it works

No file of the application is modified. A small DLL (`thr_compat.c`) that stands in for `version.dll` is placed in the application's folder and loaded through a Wine DLL override. When the application's engine is loaded, the DLL redirects five of the Windows functions the engine imports to its own replacements, **in memory only**. The real `version.dll` functions are forwarded unchanged to Wine's own.

Why the application does not work on plain Wine, and what is done about each problem:

| Symptom | Cause | Fix |
|---|---|---|
| The window appears but its content is blank | Wine cannot compile WPF's rendering shaders | Disable WPF hardware rendering (registry) |
| The amp is not found | The application enumerates USB devices through the USB device interface GUID `{a5dcbf10-…}`, which Wine (including 11.20) does not publish | Replace `SetupDiGetClassDevsW` / `SetupDiEnumDeviceInfo` and use Wine's USB enumeration instead (composite child interfaces `&MI_xx` are excluded) |
| The MIDI port cannot be matched to the USB device | Wine's ALSA driver does not implement `DRV_QUERYDEVICEINTERFACE` | `midiInMessage` / `midiOutMessage` report the USB interface path of the MIDI port |
| A second MIDI open fails | Wine's winmm frees the handle even when `midiInClose` fails with `MIDIERR_STILLPLAYING`, leaving the ALSA port open | `midiInClose` calls `midiInReset` first |
| The amp does not answer SysEx | The amp answers MIDI **only while a USB audio capture stream is open** (the Windows driver keeps one open) | The launch script keeps a capture stream open in the background |

Three of these are bugs or gaps in Wine itself (the USB GUID, `midiInClose`, `DRV_QUERYDEVICEINTERFACE`). If they are fixed upstream, the corresponding parts of the shim become unnecessary.

## Contents

| File | Purpose |
|---|---|
| `install.sh` | Builds the shim and places it next to the application as `version.dll` |
| `uninstall.sh` | Removes the shim |
| `run-thr-remote.sh` | Opens a capture stream and starts THR Remote |
| `thr_compat.c` / `thr_compat.def` | Source of the shim |

## Installation

### 1. Packages

```bash
sudo apt install wine winetricks gcc-mingw-w64-i686 pipewire-bin alsa-utils
```

`pipewire-bin` provides `pw-record`; `alsa-utils` provides `amidi` / `arecord`, used for troubleshooting.

### 2. Set up Wine and install THR Remote

.NET Framework 4.7.2 or later is required (the application is WPF-based).

```bash
winetricks dotnet472
wine "THR Remote 1.7.0 Installer.exe"   # the installer from Yamaha's Windows download
```

By default the application is installed in `~/.wine/drive_c/Program Files (x86)/Yamaha/THR Remote`. If you installed it elsewhere, point the environment variable `THR_APP_DIR` at that folder.

### 3. USB permissions (udev rule)

Give Yamaha devices (vendor ID `0499`) the permissions Wine needs to open them.

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0499", MODE="0666"' | sudo tee /etc/udev/rules.d/70-yamaha-thr.rules
sudo udevadm control --reload && sudo udevadm trigger
```

`MODE="0666"` lets every user read and write the device. To restrict it to yourself, replace `MODE="0666"` with `TAG+="uaccess"`. Unplug and replug the amp afterwards.

### 4. Install the shim

```bash
./install.sh
```

This:

- builds `thr_compat.c` as `version.dll` and places it in the application's folder;
- enables WPF software rendering (`DisableHWAcceleration=1` under `HKCU\Software\Microsoft\Avalon.Graphics`).

The application's existing files are not touched. The script can be run repeatedly.

## Running

```bash
./run-thr-remote.sh
```

1. Switch the amp on and connect it over USB.
2. Run the script. It opens a THR capture stream through PipeWire (48 kHz, 2 channels) and starts THR Remote with the DLL override (`WINEDLLOVERRIDES=version=n,b`).
3. On first start the license agreement (EULA) appears. Read it and press Accept (its text is not rendered under Wine).
4. After "Creating THR Engine" and "Syncing to device…", the editor window appears.

**Quit by closing the window with "×".** Avoid Ctrl+C or killing the process from the terminal. When the application exits, the script releases the capture stream.

## Troubleshooting

### The amp is not detected

```bash
lsusb | grep 0499          # is the amp visible?
amidi -l                   # is there an ALSA MIDI port? (note hw:N,0,0)
ls -l /dev/bus/usb/*/*     # is the device rw-rw-rw-? (is the udev rule effective?)
```

### Checking that the shim is active

Start the application with `THR_COMPAT_LOG` set to a file path (Windows form) to get a trace.

```bash
cd "$HOME/.wine/drive_c/Program Files (x86)/Yamaha/THR Remote"
WINEDLLOVERRIDES="version=n,b" THR_COMPAT_LOG='Z:\tmp\thr_compat.log' wine "THR Remote.exe"
cat /tmp/thr_compat.log
```

These lines mean the shim is installed correctly:

```
thr_compat loaded
hooked SETUPAPI.dll!SetupDiGetClassDevsW
hooked SETUPAPI.dll!SetupDiEnumDeviceInfo
hooked WINMM.dll!midiInClose
hooked WINMM.dll!midiOutMessage
hooked WINMM.dll!midiInMessage
```

If `thr_compat loaded` does not appear, `WINEDLLOVERRIDES` is missing or `version.dll` is not in the application's folder.

### Connection stalls (application log)

The application writes its internal log through `OutputDebugString`, which Wine shows with `+debugstr`.

```bash
cd "$HOME/.wine/drive_c/Program Files (x86)/Yamaha/THR Remote"
WINEDLLOVERRIDES="version=n,b" WINEDEBUG=err,+debugstr wine "THR Remote.exe" 2>&1 | grep -E "L6|Handshake|rejecting"
```

What to look for:

- `enumerateUsbDevices(devIndex 0) vid 0x499, pid 0x7182`: the USB device was found.
- `unable to match midi in interface string`: the MIDI port could not be matched. The shim is not active.
- `getDeviceQueryInfo() unexpected size 0`: the amp did not answer. Check that a capture stream is open (is `pw-record` running?).
- `midi_in_open device already open`: a MIDI port from an earlier session is still open. Stop Wine with `wineserver -k` and run again.
- `Handshake - version 0x1500062` and `Handshake complete`: success.

### Audio sounds wrong after a session

- During development, opening the amp directly with `arecord -f cd` (44.1 kHz) was followed by distorted desktop audio, which went away after restarting the amp. The cause was not confirmed, but the current script captures through PipeWire at 48 kHz to avoid it.
- Always close the application with the window's "×".
- If the audio is still distorted, restart the amp.

### Removing

```bash
./uninstall.sh
sudo rm /etc/udev/rules.d/70-yamaha-thr.rules   # if you also want to drop the udev rule
```

The application's files were never changed, so there is nothing to restore.

## Notes

- Only five functions are replaced: `SetupDiGetClassDevsW`, `SetupDiEnumDeviceInfo`, `midiInMessage`, `midiOutMessage` and `midiInClose`. Everything else runs on Wine's own implementations.
- The MIDI port's USB information is a fixed value matching the amp (`vid_0499&pid_7182&mi_01`), and only ports whose name contains `THR` are affected. For other models, review `THR_PATH` in `thr_compat.c`.
- The amp talks over MIDI (Line 6 SysEx, manufacturer ID `00 01 0C`). The application first sends a standard Universal Device Inquiry (`F0 7E 7F 06 01 F7`) and receives the firmware information in reply.

## License

[BSD Zero Clause License (0BSD)](LICENSE). It applies to the code and documentation in this repository, not to THR Remote itself (Yamaha's software).
