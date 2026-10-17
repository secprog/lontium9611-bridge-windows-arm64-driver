# Lontium LT9611 Windows driver

Windows ARM64 driver for the LT9611 HDMI bridge on Rubik Pi 3. It handles
HDMI video, audio and CEC, with output settings in Device Manager. The CEC
device emulates a Pulse-Eight USB adapter for use with libCEC.

## Building

Use the Windows 11 EWDK/WDK 10.0.26100 with the ARM64 C++ tools. Open its
build environment and run:

```bat
build.cmd
```

This builds `OEMBridge.sys`, `P8CecUde.sys` and `OEMBridgeProp.dll`. The package
ends up in `%TEMP%\lt9611-build\package`. To put the output somewhere else,
pass a directory outside this source folder as the first argument to `build.cmd`.

The source default is `1.0.0.0`, with `01/01/1999` as the INF placeholder date.
Builds stamp the current date and assign a version from the source contents.
Unchanged source keeps its version; new source gets the next revision.
The assignments are saved in `%LOCALAPPDATA%\LT9611\SourceVersions`, outside
the checkout. Keep that folder to retain the version history.

For a different cache location or an explicit build version, use PowerShell:

```powershell
.\build-source.ps1 -OutputDirectory $env:TEMP\lt9611-build -VersionCacheDirectory D:\BuildCache\LT9611
.\build-source.ps1 -OutputDirectory $env:TEMP\lt9611-custom -Version 1.2.3.4
```

An explicit version does not change the automatic assignments. Generated
metadata stays in the output directory. Direct project builds use `1.0.0.0`.

For a GitHub build, open **Actions** and choose **Build ARM64 drivers**. It also
runs on pushes and pull requests. The download includes the package, matching
debug symbols, source and license notices.
GitHub builds preserve the same assignments in an external cache and a
`source-versions` backup artifact. The manual workflow accepts a saved
`source-versions.json` if the cache and backup need to be restored.

These builds are unsigned. Generate a catalog and sign the package before
installing it.

## Device settings

Open the HDMI bridge's properties in Device Manager and select **Output**.
The page has controls for resolution, HDMI audio, CEC and logging. Audio is
enabled by default; CEC and logging start off.

Resolution changes need a reboot because the firmware sets up the DSI link
at boot. The driver needs matching board firmware and working display and
audio drivers.

## HDMI audio

Video gets to the LT9611 over MIPI DSI. Audio takes a separate I2S link from
Qualcomm LPASS, carrying stereo PCM data with a bit clock and a left/right
clock. The LT9611 combines that audio with the video on its HDMI output.

The Qualcomm audio driver and ADSP handle the stream and clocking. OEMBridge
configures the LPASS `qua_mi2s` pins once the audio subsystem is ready, sets up
the LT9611's I2S input, and tells the audio driver when an HDMI display connects
or disconnects. Those notifications let the audio driver create and update the
HDMI playback device in Windows.

This relies on the board's Qualcomm audio drivers and matching DSP firmware.
Turning off HDMI audio on the Output page disconnects the playback device and
stops HDMI audio packets. Video stays on.

## License

Original project contributions are under [GPL-2.0-only](LICENSE). See
[NOTICE.md](NOTICE.md) for credits and the terms covering retained third-party
material.
