# VocalBridge

**Real-time VST3 vocal processing for any Windows app.** Plug in your mic or audio interface, build a chain of VST3 plugins (gate, EQ, compressor, de-esser, reverb, pitch correction...), hear yourself with low-latency monitoring, and send the processed voice to a **virtual microphone** that games, Discord, OBS, Zoom etc. can select as their input.

```
 Mic / Interface ──► VocalBridge.exe ──► VST3 chain ──┬──► Headphones (monitoring, same device = lowest latency)
   (ASIO / WASAPI)                                    │
                                                      └──► shared ring buffer ──► VocalBridge.sys
                                                                                   "Microphone (VocalBridge Virtual Mic)"
                                                                                        │
                                                            Games / Discord / OBS ◄─────┘
```

## Components

| Path | What it is |
|---|---|
| `app/` | **VocalBridge.exe** (C++ / JUCE 8): device selection, VST3 hosting, monitoring, virtual-mic feed |
| `driver/` | **VocalBridge.sys**: WaveRT kernel driver exposing one capture endpoint, "Microphone (VocalBridge Virtual Mic)". Based on Microsoft's SimpleAudioSample (MIT) |
| `shared/VocalBridgeShared.h` | The app ↔ driver contract (IOCTLs + ring buffer layout) |
| `tools/vbsetup/` | **vbsetup.exe**: creates the `ROOT\VocalBridge` device and installs the driver (like `devcon install`) |
| `scripts/` | `install-driver.ps1` / `uninstall-driver.ps1`; `attestation/` for getting the driver Microsoft-signed |
| `.github/workflows/build.yml` | Windows CI: builds the app, the driver package, and a ready-to-run release zip |

## Low-latency design

Latency was the main design constraint, so every hop is kept as short as possible:

- **Monitoring path.** Input → plugins → output all happen inside one audio callback on the same device. Monitoring latency is just your interface's input + output buffers plus any plugin lookahead. The app shows the round-trip figure live.
  - **ASIO** is used when the app is built with the ASIO SDK (see below).
  - Otherwise **WASAPI Low Latency** is the default.
  - **WASAPI Exclusive** can be selected in Audio Settings.
  - A 64-sample buffer is chosen by default if the device supports it.
- **Virtual mic path: no second audio stream.** The app does not play into a virtual "speaker" endpoint, which would add a WASAPI render buffer and a loopback hop. Instead it writes the processed audio straight into a ring buffer that the driver reads from:
  1. The app allocates the ring and hands it to the driver once (`IOCTL_VOCALBRIDGE_ATTACH_RING`, `METHOD_OUT_DIRECT`).
  2. The pages stay locked while that IOCTL is pending.
  3. The driver's capture stream copies from the ring into its WaveRT buffer on a 1 ms high-resolution timer. This is a `memcpy` in the native format (48 kHz / 32-bit / stereo), with no conversion in the kernel.
- **Clock drift.** Your interface and the virtual mic run on different clocks.
  - A tiny 4-point Hermite resampler (about 2 samples of delay) converts to 48 kHz.
  - It also trims its ratio by at most ±0.3 %, so the queued audio stays at a few milliseconds instead of slowly drifting into underruns or growing latency.
  - The driver re-syncs cleanly if the app stalls or restarts.
  - Measured in the offline tests (`app/Tests`): **~4 ms** added into the virtual mic at a 128-sample buffer, with zero underruns across ±200 ppm drift at 44.1, 48 and 96 kHz.
- **Tuning.** The **Safety buffer** slider (default 3 ms) trades latency against robustness. If the underrun counter climbs on a busy system, raise it a little.
- **Windows.** The app opts out of EcoQoS / power throttling, so it isn't scheduled like a background app.

> Note: the app or game recording the virtual mic adds its own capture buffering. Windows' shared-mode audio engine typically runs a 10 ms period. That part is outside VocalBridge's control.

## Building

### App (any Windows PC with Visual Studio 2022 + CMake 3.22+)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release      # resampler + drift-compensation tests
# -> build\app\VocalBridge_artefacts\Release\VocalBridge.exe
# -> build\tools\vbsetup\Release\vbsetup.exe
```

JUCE 8.0.9 is downloaded automatically (or pass `-DVOCALBRIDGE_JUCE_DIR=path\to\JUCE`).

**ASIO (recommended for audio interfaces):**
1. Download the ASIO SDK from Steinberg.
2. Add `-DVOCALBRIDGE_ASIO_SDK_DIR=C:\path\to\asiosdk` to the configure command.

In CI, set the repository variable `ENABLE_ASIO=true`.

The app and its tests also build on Linux (ALSA), which is handy for development. The driver link is Windows-only.

### Driver (Visual Studio 2022 + WDK via NuGet)

```powershell
cd driver
nuget restore packages.config -PackagesDirectory packages
msbuild VocalBridgeDriver.sln /p:Configuration=Release /p:Platform=x64
```

The build is **test-signed** with an auto-generated certificate.

Or just take the `VocalBridge-win64` artifact from the GitHub Actions run, which contains everything below.

## Installing (keep Secure Boot on)

Windows only loads kernel drivers with Secure Boot on if **Microsoft** has signed them; test mode is the only way around that, and Secure Boot locks test mode. So VocalBridge offers two Secure Boot-friendly routes, and the app's **Auto** virtual-mic mode (the default) uses whichever is available:

| Route | What you install | Secure Boot | Anti-cheat | Extra latency |
|---|---|---|---|---|
| **A. Signed virtual cable** (works today) | [VB-CABLE](https://vb-audio.com/Cable/) (free/donationware, Microsoft-signed) | ✅ stays on | ✅ | ~ app buffer + safety + the cable's internal buffer |
| **B. VocalBridge driver, attestation-signed** | `install-driver.ps1` with a Microsoft-signed package | ✅ stays on | ✅ | lowest (~ app buffer + safety, shared ring) |
| C. VocalBridge driver, test-signed (CI default) | `install-driver.ps1` + test mode | ❌ must be off | ❌ many refuse | lowest |

### Route A: signed virtual cable, no driver of ours

1. Install VB-CABLE from vb-audio.com and reboot if it asks.
2. Start VocalBridge. **Auto** detects `CABLE Input (VB-Audio Virtual Cable)` and opens it in **WASAPI exclusive mode** with a small buffer, falling back to low-latency or shared mode if that fails. Drift compensation keeps the hand-off at a few ms.
3. In your game, Discord or OBS, select **CABLE Output (VB-Audio Virtual Cable)** as the microphone. The app shows the exact name to pick.
4. For the lowest latency, lower the cable's internal buffer in VB-Audio's CABLE control panel (*Max Latency*) to the smallest value that stays clean.

VB-Audio's A+B, C+D and Hi-Fi cables are recognised too. Any other output device can be chosen manually from the dropdown.

### Route B: get the VocalBridge driver Microsoft-signed (attestation signing)

This is the proper fix for distributing the driver. It's a one-time setup.

1. **Buy an EV code-signing certificate** (DigiCert, Sectigo, GlobalSign, SSL.com, ...). EV certificates are issued to a **registered business**, not individuals, and are delivered on a hardware token or a cloud HSM.
2. **Register a Partner Center hardware account** (free) using that EV certificate.
3. **Sign and submit**, either by hand or from CI.

**By hand:**

```powershell
msbuild driver\VocalBridgeDriver.sln /p:Configuration=Release /p:Platform=x64 /p:SignMode=Off
scripts\attestation\make-attestation-cab.ps1 -DriverDir driver\Source\Main\x64\Release -CertThumbprint <your EV cert thumbprint>
```

Then upload `VocalBridge.cab` in Partner Center → *Hardware* → *Submit new hardware*: pick the Windows 10/11 x64 signatures and download the signed package. Alternatively, automate the upload with `scripts\attestation\submit-attestation.ps1`, which uses Microsoft's [SDCM](https://github.com/microsoft/SDCM).

**From CI:**
1. Set the repository variable `ATTESTATION_ENABLED=true`.
2. Add these secrets:
   - an EV certificate in Azure Key Vault (Premium/HSM): `EV_KEYVAULT_URL`, `EV_KEYVAULT_CERT_NAME`, `EV_KEYVAULT_TENANT_ID`, `EV_KEYVAULT_CLIENT_ID`, `EV_KEYVAULT_CLIENT_SECRET`
   - an Entra ID app [associated with your Partner Center account](https://learn.microsoft.com/windows-hardware/drivers/dashboard/dashboard-api): `PARTNER_CENTER_TENANT_ID`, `PARTNER_CENTER_CLIENT_ID`, `PARTNER_CENTER_CLIENT_SECRET`

The `driver-attested` job then builds the driver unsigned, packs and EV-signs the CAB, submits it, waits for Microsoft, and puts the signed driver into the `VocalBridge-win64` release zip.

The attestation script requests the Windows 11 x64 signature (`WINDOWS_v100_X64_CO_FULL`) by default. Confirm the current codes in Partner Center. The INF currently targets Windows 11 (build 22000+).

### Installing the driver (routes B and C)

1. Unzip the `VocalBridge-win64` artifact.
2. Right-click `install-driver.ps1` and choose **Run with PowerShell**. It elevates itself, then:
   - **Microsoft-signed package:** installs directly. Secure Boot stays on.
   - **Test-signed package with Secure Boot on:** stops and points you to route A or B. It doesn't try to change boot settings.
   - **Test-signed package with Secure Boot off:** offers to enable test mode (`bcdedit /set testsigning on`, then reboot) and trusts the test certificate.
3. Check that **Settings → Sound → Input** now lists **Microphone (VocalBridge Virtual Mic)**. `vbsetup.exe status` shows the device and app-link state.

To remove it, run `uninstall-driver.ps1`.

## Using the app

1. **Audio Settings…**
   - Choose the driver type (ASIO / Windows Audio Low Latency / Exclusive).
   - Choose your **input device** (mic or interface) and the input channel(s) your mic is on.
   - Choose your **output device** for monitoring (usually your interface or headphones).
   - Pick the smallest buffer that doesn't crackle.
2. **Input mode.** Use *Mono – input 1* for a single mic on input 1 of an interface. Mono signals are duplicated to L/R for the plugins.
3. **Scan for Plugins…** → *Options* → *Scan for new or updated VST3 plug-ins* (default folder `C:\Program Files\Common Files\VST3`), or use **Add VST3 File…**.
4. **+ Add Plugin.** Adding a plugin opens its editor.
   - Each row has an enable/bypass toggle, **Edit**, reorder (▲▼) and remove (✕).
   - Each plugin's latency is shown in its row.
5. **Monitoring.** Toggle it and set the level. The latency line shows the real round-trip.
6. **Virtual Mic Output.** Leave it on **Auto**. It uses the VocalBridge driver if installed, otherwise a signed virtual cable (see *Installing*), and re-checks every few seconds.
   - The status dot turns **green** while streaming, **yellow** when connected but idle, and **red** when nothing is available. The line below always names the exact input to select in your game.
   - Use **Mute** to silence only what the game hears; you can still monitor yourself.
7. Select the input the app names as the microphone in your game or app: **Microphone (VocalBridge Virtual Mic)** or **CABLE Output (VB-Audio Virtual Cable)**.
8. **Save Preset… / Load Preset…** stores the whole chain with every plugin's settings. The current chain, devices and settings are also restored automatically on the next launch.

## Project layout

```
app/
  Source/
    Main.cpp                 app entry, settings, power-throttling opt-out
    AudioEngine.*            device callback: input routing → chain → monitor + virtual mic
    PluginChain.*            VST3 chain (thread-safe edits, mono/stereo bus negotiation, state save/restore)
    VirtualMicOutput.*       resampler + drift control, DriverBackend (ring/IOCTL), DeviceBackend (fallback)
    StreamingResampler.h     4-point Hermite variable-ratio resampler
    MainComponent.*          UI
  Tests/                     offline simulation tests for the virtual-mic path
driver/
  Source/Main/vbcontrol.cpp  control device (\\.\VocalBridge) + shared ring consumer   ← VocalBridge-specific
  Source/Main/minwavertstream.cpp  capture stream reads the ring every 1 ms tick         ← modified
  Source/Main/adapter.cpp    mic-only endpoint, control-device lifetime, IRP routing   ← modified
  Source/Main/VocalBridge.inx                                                         ← new INF
shared/VocalBridgeShared.h   app ↔ driver contract
tools/vbsetup/               device installer
scripts/                     install / uninstall scripts
scripts/attestation/         CAB packing + Partner Center submission for Microsoft signing
```

## Licensing notes

- The driver is derived from Microsoft's [Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples) SimpleAudioSample (MIT; see `driver/LICENSE.microsoft-samples`).
- The app uses [JUCE 8](https://juce.com), which is dual-licensed: AGPLv3 or a commercial JUCE license. Distributing VocalBridge closed-source requires a JUCE license.
- VST is a trademark of Steinberg Media Technologies GmbH. The ASIO SDK is not included and is subject to Steinberg's license.
