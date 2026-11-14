# 17. Hardware support: GPUs, firmware, sound, Bluetooth, printing, displays, power

This chapter says which hardware FreeLFS supports, which drivers and firmware are built in, and what is tested and what is not. It is the admin's view of `docs/HARDWARE.md`, extended with the round 17 features.

Status: round 17 (2026-11-14). **Nothing has been run on an Intel or a newer AMD machine yet.** The one laptop that ran the desktop (AMD Ryzen 5 7520U, Radeon 610M, Realtek rtw89 Wi-Fi) was tested in earlier rounds. The round 15 kernel and firmware changes and all round 17 features are built and run in QEMU only. Please report results with `flreport` (see 17.12).

## Contents

1. [Support at a glance](#171-support-at-a-glance)
2. [Graphics: drivers and userspace](#172-graphics-drivers-and-userspace)
3. [Kernel drivers by vendor](#173-kernel-drivers-by-vendor)
4. [Firmware and microcode](#174-firmware-and-microcode)
5. [Video decoding](#175-video-decoding)
6. [Sound](#176-sound)
7. [Bluetooth](#177-bluetooth)
8. [Printing](#178-printing)
9. [Displays: outputs, scaling, multi-monitor, night light](#179-displays-outputs-scaling-multi-monitor-night-light)
10. [Power: battery, power saving, performance mode](#1710-power-battery-power-saving-performance-mode)
11. [Backlight and keys](#1711-backlight-and-keys)
12. [Diagnostics and reports](#1712-diagnostics-and-reports)
13. [Known gaps and testers wanted](#1713-known-gaps-and-testers-wanted)

## 17.1 Support at a glance

FreeLFS targets laptops and desktops from about 2015 on, x86_64, UEFI, 1 GB of RAM and more.

| Area | AMD | Intel | NVIDIA |
|---|---|---|---|
| Kernel modesetting | amdgpu (GCN and newer), radeon (TeraScale) | i915 (Gen 4 to Alder Lake, DG1), xe (Xe2, Lunar Lake, Battlemage; Tiger Lake and newer with `xe.force_probe`) | not enabled (nouveau is not built) |
| 3D (OpenGL) | radeonsi | llvmpipe (software) until the 16 GB build machine | none; the firmware framebuffer (simpledrm) is used |
| Vulkan | radv (AMD), lavapipe (software) | lavapipe only; anv and hasvk are deferred | none |
| Video decode (VA-API) | Mesa radeonsi video driver | intel-media-driver (Broadwell and newer) | none |
| Wi-Fi | rtw88, rtw89, mt7921 family | iwlwifi family | none |
| Status | Ryzen 5 7520U laptop tested in earlier rounds; the rest untested | prepared, **untested on real hardware** | not supported yet |

Other areas (all vendors): the kernel has the common storage, Ethernet, Bluetooth and sensor drivers; see 17.3.

## 17.2 Graphics: drivers and userspace

Mesa 26.1.7 release 3 (`recipes/mesa`) is built with:

| Driver | Built | Notes |
|---|---|---|
| `radeonsi` | yes | AMD GCN and RDNA GPUs, OpenGL |
| `llvmpipe`, `softpipe` | yes | software OpenGL: used for Intel GPUs and for machines with no 3D driver |
| `virgl`, `zink` | yes | QEMU's 3D (virgl) and OpenGL on Vulkan (zink) |
| `radv`, `lavapipe`, `amd` Vulkan | yes | AMD Vulkan (radv) and software Vulkan (lavapipe, from `vulkan-drivers=amd,swrast`) |
| `iris`, `crocus` (Intel OpenGL) | **no, deferred** | need clang; see below |
| `anv`, `hasvk` (Intel Vulkan) | **no, deferred** | same |
| `r600` (TeraScale 3D) | **no** | the kernel's `radeon` driver does modesetting only; these GPUs draw through llvmpipe |

Why Intel 3D is deferred: Mesa 26.1 compiles the Intel drivers' built-in OpenCL C kernels with `mesa_clc` at build time. That needs clang, spirv-llvm-translator and libclc, and linking clang needs about 8 GB of RAM, while the build host has 7 GB. The recipes `llvm` (release 2, with the clang steps written in its header), `spirv-llvm-translator` and `libclc` (group `deferred`) are ready and wait for the 16 GB build machine, about January 2027 (`docs/HARDWARE.md`). Until then an Intel GPU draws with llvmpipe, which works but costs CPU.

## 17.3 Kernel drivers by vendor

The kernel is `linux` 7.3.4 (`config/kernel/freelfs.config`). Built in: the root device's drivers (btrfs, NVMe, SATA/AHCI, Intel VMD), so an installed system needs no initramfs. Modules: the rest, loaded by udev.

| Area | Intel | AMD |
|---|---|---|
| CPU frequency | intel_pstate (built in); acpi-cpufreq for old CPUs | amd-pstate (mode "active", the energy preference that flpower writes); acpi-cpufreq |
| Idle and suspend | intel_idle, pmc_core (S0ix) | amd_pmc (s2idle) |
| Graphics | i915, xe, backlight `intel_backlight` | amdgpu with the display core (also GCN 1 and 2), radeon (TeraScale, modesetting only), `amdgpu_bl*` |
| Audio | HDA codecs (Realtek, Conexant, Cirrus, Sigmatel, Analog, VIA, CA0132), HDMI audio, SOF for Apollo Lake to Lunar Lake, SoundWire | HDA, HDMI audio, ACP6x (Ryzen 6000 and 7020), ACP 6.3 (7040, 8040, 7640HS), SOF Renoir and ACP63 |
| Wi-Fi | iwlwifi with iwlmvm (7260 and newer), iwldvm (5000 and 6000), iwlmld (Wi-Fi 7) | Realtek rtw88 (8821, 8822, 8723) and rtw89 (8851B, 8852A/B/C, 8922A), MediaTek mt7921, mt7922, mt7925 |
| Bluetooth | btusb with btintel, btrtl, btmtk, btbcm; Intel PCIe Bluetooth | btusb (Realtek and MediaTek chips) |
| Ethernet | e1000e, igb, igc | r8169, r8152 (USB), CDC USB tethering |
| Storage | NVMe, SATA/AHCI, Intel VMD (built in), SD readers (Realtek, SDHCI) | NVMe, SATA/AHCI |
| Touchpad and sensors | Intel LPSS and pin controllers, I2C-HID, SMBus touchpads (Synaptics RMI4, Elan), Intel ISH | I2C-HID, SMBus touchpads, AMD sensor hub (`amd_sfh`) |
| Temperatures | coretemp, package thermal, powerclamp, RAPL, int340x (DPTF) | k10temp |
| Laptop extras | ThinkPad, IdeaPad, Dell, HP, ASUS, Acer extras (hotkeys, platform profile), Intel HID/VBTN | the same drivers by maker |
| USB-C and Thunderbolt | Thunderbolt and USB4, UCSI | UCSI |

Not enabled: AMD PMF (needs the AMD TEE driver), AMD PSP and ccp, Intel MEI, Atheros ath10k, ath11k and ath12k, Broadcom Wi-Fi, webcams (there is no camera app), NVIDIA nouveau.

## 17.4 Firmware and microcode

| Package | Version | Content |
|---|---|---|
| `linux-firmware` | 20261019 release 2 | amdgpu, radeon, i915 (GuC, HuC, DMC), xe, Intel Bluetooth (`intel/ibt`), `intel/ish`, iwlwifi, rtw88, rtw89, rtl_nic, rtl_bt, MediaTek (MT79xx Wi-Fi and Bluetooth), Cirrus amplifiers, AMD microcode |
| `sof-firmware` | 2026.09.2 release 1 | Sound Open Firmware for Intel SOF audio and SoundWire codecs (about 57 MB unpacked) |
| `microcode` | 20261110 release 1 | Intel (client CPUs from 2013) and AMD microcode in `/boot/ucode.img` |

linux-firmware was pruned to about 420 MB unpacked: iwlwifi keeps the newest three files of each family, and the Intel camera, NPU, server and audio-DSP firmware, and the MediaTek router firmware, are left out.

**Early microcode loading:** the installer copies `/boot/ucode.img` to the ESP and writes `initrd=\ucode.img` before the root line in `flboot.conf`. The kernel's EFI stub loads it first. The live ISO's boot image is too small for it (El Torito images are limited), so the live system runs without early microcode. An **existing** install keeps its `flboot.conf` without the line until someone adds `initrd=\ucode.img` and copies the file (see [11](11-boot-process.md)). Check with `dmesg | grep -i microcode`.

## 17.5 Video decoding

- VA-API: libva 2.24 (DRM and Wayland backends).
- AMD: Mesa's `radeonsi_drv_video.so` with all the codecs Mesa has (H.264, HEVC, AV1, VP9 decode).
- Intel: `intel-media-driver` (iHD) with gmmlib, for Broadwell and newer. Older Intel GPUs have no VA-API driver.
- flvideo and mpv use `hwdec=auto-copy-safe`: the hardware path when a driver works, software otherwise.
- FreeBWS (WebKit with GStreamer) decodes in software: GStreamer's `va` plugin is not built.
- VA-API was compiled in but not run on a real GPU (`docs/INTEGRATION-TODO.md`).

## 17.6 Sound

The stack: **PipeWire** 1.6.8 (`recipes/pipewire`, release 2) as the sound server, **WirePlumber** 0.5.15 as the session policy, **pipewire-pulse** for PulseAudio programs. They are started by the session script (see [14](14-services-init.md)). The ALSA drivers are in the kernel (17.3).

- PipeWire is built with ALSA, D-Bus, udev and readline, and the Bluetooth plugin (see 17.7).
- **flsound** (`src/flsound`) plays the five system sounds (login, notification, error, volume tick, screenshot). They are synthesised in the program, not stored as files. `flsound --list` lists them. Settings > General > System sounds switches them off (`sounds=0` in `desktop.conf`).
- Volume and microphone keys are read by flcomp: XF86AudioRaiseVolume, LowerVolume, Mute, MicMute; each shows the on-screen indicator. Media keys (Play, Stop, Next, Prev) run `flmusic --ctl`.
- Status: the sounds were played once through the host's PipeWire in the build. Audible output on a real machine is **not yet verified**, and so are the levels.

## 17.7 Bluetooth

- **BlueZ** 5.88 release 1 (`recipes/bluez`): `bluetoothd` with the SBC codec (`libsbc`), for A2DP headphones and speakers.
- PipeWire's bluez5 plugin takes the audio (SBC for A2DP; native HSP and HFP for headset microphones). AAC, LDAC, aptX, LC3 and Opus are off (`recipes/pipewire`).
- The rc service `bluetooth` starts `bluetoothd` **only while an adapter exists** (`/sys/class/bluetooth/hci*`). A udev rule (`90-flbluetooth.rules`) starts it when the adapter appears and calls `maybe-stop` when it goes away. Without an adapter nothing runs and nothing is allocated (see [14](14-services-init.md)).
- A default pairing agent (NoInputNoOutput, "just works": headphones, speakers, mice) runs with `bluetoothd`, so that a pairing from Settings does not fail with "No agent is registered" (`config/etc/rc.d/init.d/bluetooth`).
- D-Bus policy `bluetooth-seat.conf`: members of group `seat` may use `org.bluez`.
- **Settings > Bluetooth** (`src/flsettings/bluetooth.c`) runs `bluetoothctl` only: show and power, devices, info, a 10-second scan, pair, trust, connect, disconnect, remove. Paired devices show a battery level when the device reports one. Pair runs pair, trust and connect in one go. "No Bluetooth adapter found" when there is none.
- Status: built and checked headless (`build/m17/bluetooth.cmds`). Not yet tested with real hardware.

## 17.8 Printing

- **CUPS** 2.4.20 release 3 (`recipes/cups`) with avahi (printer discovery), libusb (USB printers), openssl (`ipps://`) and mupdf.
- It is **driverless**: printers that support IPP Everywhere need no driver. `lpadmin -E -m everywhere` writes a PPD from the printer's own attributes.
- Two small scripts (`config/cups/pdftopdf`, `config/cups/pdftopwg`) stand in for cups-filters, which needs qpdf (C++). They convert PDF to what the printer takes.
- **On demand**: the rc service `cups` starts only `flsocketd` (about 0.2 MB). It listens on `/run/cups/cups.sock`, and starts `cupsd` (and avahi) when someone connects: `lp`, `lpstat`, `lpadmin`, or Settings > Printers. `cupsd` and avahi stop after 60 seconds without a job or a connection (`config/cups/flcupsd`).
- **Settings > Printers** (`src/flsettings/printers.c`) uses only the CUPS tools: `lpstat -p -d`, `lpinfo --include-schemes dnssd,ipp,ipps,usb -v` (the search), `lpadmin -E -m everywhere` (add), `lpadmin -x` (remove, asked first), `lpoptions -d` (default), `lp` (test page), `cancel -a` (cancel jobs). Without `lpstat` the page says "Printing is not installed".
- Status: built and checked headless (`build/m17/printing.cmds`). **Not tested with a real printer.**

## 17.9 Displays: outputs, scaling, multi-monitor, night light

**flcomp** (fl_shell version 9) handles every connector as an **output** (names such as `eDP-1` or `HDMI-A-1`).

- Enabled outputs form one desktop. The **primary** output carries the panel, the start menu, notifications, and maximize and snap. The other outputs are extra space for windows and the pointer.
- Each output has its own mode and scale. The scale is 100 to 200 percent. A client draws at scale 1 and flcomp enlarges the picture for each output (bilinear, a little soft at non-integer scales). `fldisplay` offers 1, 1.25, 1.5, 1.75 and 2.
- Hot-plugging: flcomp re-reads the outputs when the kernel reports a change (a udev `drm` event, with a 300 ms settle).
- Limits (`src/flcomp/README`): the panel, the overview and the desktop icons assume the primary is at the top left. The pointer can enter empty corners between outputs of different sizes.
- Settings are saved in `~/.config/freelfs/desktop.conf` as `output.NAME=mode=1920x1080@60 scale=1.25 pos=0,0 enabled=1 primary=1`.
- **Settings > Display** and the command line:

```text
fldisplay list                                   one line per output
fldisplay set NAME [mode=WxH@HZ] [scale=S] [pos=X,Y] [enabled=0|1] [primary=1]
fldisplay revert                                 go back to the settings before the last change
fldisplay nightlight on|off|auto [temp=2700..6500] [from=HH:MM] [to=HH:MM]
```

- `fldisplay set` applies at once and saves. There is **no automatic revert**: the caller asks "keep these settings?" and calls `fldisplay revert` if the answer is no.
- **Night light**: a colour fade towards warmer light, over 2 seconds. Default temperature 4000 K, default times 20:00 to 07:00 for `auto`, checked every 30 seconds. The gamma table of the CRTC is used. Outputs without one (virtio-gpu) get a per-pixel table in the copy to the screen. The keys are `nightlight=`, `nightlight_temp=`, `nightlight_from=`, `nightlight_to=` (and the older `night_light=` family).
- Status: `src/fldisplay/tests/check.sh` runs list, set, revert, refusals and night light against a headless flcomp with two fake outputs (`FLCOMP_HEADLESS_OUTPUTS=2`). **Real multi-monitor hardware is not yet verified.**

## 17.10 Power: battery, power saving, performance mode

| Feature | How | Where |
|---|---|---|
| Power menu (off, restart, log out) | `flpower off`, `flpower restart`, `flpower logout` | flpanel's power menu; setuid, group `seat` (see [15](15-users-security.md)) |
| Battery | percentage and time left in the panel flyout (when `/sys` has a battery). Built; not tested in QEMU, which has no battery (`docs/ROADMAP-NEXT.md`, A6) | flpanel |
| Power saving | `flpower saver on\|off`: every CPU gets the `powersave` governor and the power-saving energy preference (`/run/flpower-saver` remembers the old values). flcomp also draws at most 30 frames per second and dims the backlight to 70 percent | flpower, flcomp (`desktop.conf` `power_saver=1`) |
| Performance mode | `performance=auto\|on\|off`: auto turns it on while the window on top is fullscreen; while on, there is no frame cap and only the fullscreen window is drawn | flcomp (`perf.c`); the battery flyout has the switch |
| Lid | closing the lid locks the screen, unless `lock_on_lid=0` | flcomp |
| Frequency and idle drivers | see 17.3 | kernel |
| Thermal daemons | none: no thermald (it needs upower and dbus-glib), no power-profiles-daemon; the kernel's drivers and the firmware throttle | `docs/HARDWARE.md` |

There is **no suspend path yet**: flpower knows only off and restart. Whatever later suspends the machine should run `fllock` first (`src/flcomp/README`).

## 17.11 Backlight and keys

- `/sys/class/backlight/*` is used by flsettings and flcomp. When a laptop has several (for example `acpi_video0` next to `intel_backlight` or `amdgpu_bl1`), the order of preference is "firmware", then "platform", then "raw".
- `90-flsettings-backlight.rules` gives group `seat` write access to every backlight's `brightness` file.
- XF86MonBrightnessUp and Down change the backlight in 5 percent steps (at least 5 percent). The `brightness=` key in `desktop.conf` sets it at start.
- XF86AudioMicMute toggles the microphone.
- Every key press shows the on-screen indicator.

## 17.12 Diagnostics and reports

| Tool or file | Shows |
|---|---|
| `dmesg` (filter for `firmware` or `microcode`) | driver and firmware messages |
| `/var/log/dmesg.log` | the kernel messages of this boot |
| `lsmod` | loaded modules |
| `lspci -nnk` | PCI devices with their drivers (if `pciutils` is present; not verified in the image) |
| `/sys/class/backlight` | backlight devices |
| `vainfo` | the VA-API drivers (if `libva-utils` is present; not verified) |
| `flreport` | a tarball with logs, kernel messages, `/sys` device lists, settings, mounts, the network, and `memory.txt`. Run it from a terminal or the desktop; it writes `~/freelfs-report-DATE.tar.gz` ([19](19-admin-reference.md)) |
| Settings > About > Save a report | the same report from the desktop |

`vulkaninfo --summary` is named in `docs/HARDWARE.md` as wanted from testers, but it is not packaged yet.

## 17.13 Known gaps and testers wanted

| Gap | Status |
|---|---|
| Intel 3D (iris, crocus, anv, hasvk) | deferred to the 16 GB machine (about January 2027) |
| r600 (TeraScale 3D) | not built; modesetting only |
| NVIDIA | not supported: the firmware framebuffer or the integrated GPU |
| Anything on an Intel or a new AMD machine | untested |
| Round 15 kernel and firmware on real hardware | untested |
| Sound on real hardware, levels and taste of the system sounds | untested |
| Bluetooth with a real adapter and device | untested |
| Printing with a real printer | untested |
| Multi-monitor and night light on real hardware | untested (QEMU and headless only) |
| Battery panel item and the power-saving switch on a real battery | untested (no battery in QEMU) |
| Suspend | not implemented |
| Secure Boot | not implemented ([15](15-users-security.md)) |
| Webcam and camera | not enabled in the kernel |

**Testers wanted** (from `docs/HARDWARE.md`): an Intel laptop with VMD or RST NVMe, one with SOF audio, one with Wi-Fi 7 (BE200), an AMD laptop with MT7922 or MT7925, and a Ryzen 7040 or 8040 (Phoenix) machine. Send the `flreport` tarball and the output of `dmesg`, `lsmod`, `lspci -nnk`, `/sys/class/backlight`, `vainfo` and `vulkaninfo --summary`.
