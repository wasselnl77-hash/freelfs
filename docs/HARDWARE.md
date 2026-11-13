# Hardware support

FreeLFS targets typical laptops and desktops from about 2015 to now, x86_64, UEFI, 1 GB RAM and up.

**Status in short**

| What | Status |
|---|---|
| AMD Ryzen 5 7520U laptop (Radeon 610M, rtw89 Wi-Fi) | tested, partly (earlier rounds; the milestone M10 run on the laptop failed and was fixed, not retested after the kernel change below) |
| AMD, everything else | prepared, untested on real hardware |
| Intel | **prepared, untested on real hardware: needs testers** |
| NVIDIA | nouveau and NVK planned, proprietary driver later; today the machine runs on the firmware framebuffer (simpledrm) or the integrated GPU |
| QEMU/KVM, virtio-gpu | tested every round |

The 2026-11-12 hardware round was **built on 2026-11-13** (round 15: linux 7.3.4 release 2, linux-lts 6.18.63 release 2,
linux-firmware release 2, sof-firmware, microcode, gmmlib, intel-media-driver, Mesa release 3 without the Intel drivers,
backlight preference in flcomp/flsettings) and run in QEMU only. Nothing of it has run on any Intel or AMD machine yet.

**Deferred to the 16 GB build machine (about January 2027): Mesa's Intel 3D drivers.** iris, crocus, anv and hasvk all
need clang, spirv-llvm-translator and libclc at build time (Mesa 26.1 compiles their built-in OpenCL C kernels with
mesa_clc), and linking clang needs about 8 GB of RAM; the build host has 7 GB. The recipes llvm (release 2 with clang
described in its header), spirv-llvm-translator and libclc (group "deferred") are ready but not built. Until then an Intel
GPU gets kernel modesetting (i915/xe), software OpenGL (llvmpipe) and lavapipe, and hardware video decoding through
intel-media-driver.

## Kernel (config/kernel/freelfs.config, linux 7.x and linux-lts 6.18)

No initramfs on an installed system, so what the root device needs is built in, the rest is modules found by udev.

| Area | Intel | AMD |
|---|---|---|
| CPU frequency | intel_pstate (built in), acpi-cpufreq for old CPUs | amd-pstate, default mode "active" (EPP, which flpower writes), acpi-cpufreq |
| Idle / suspend | intel_idle, pmc_core (S0ix) | amd_pmc (s2idle) |
| Microcode | early load, see below | early load, see below |
| Graphics | i915 (Gen4 to Alder Lake/DG1), xe (Xe2, Lunar Lake, Battlemage; Tiger Lake and newer with `xe.force_probe`), backlight `intel_backlight` | amdgpu with display core (GCN 1/2 too), radeon (TeraScale, modesetting only), `amdgpu_bl*` |
| Audio | HDA + codecs (Realtek, Conexant, Cirrus, Sigmatel, Analog, VIA, CA0132), HDMI audio, SOF for Apollo/Gemini/Jasper/Elkhart/Tiger/Alder/Meteor/Lunar Lake and SoundWire | HDA, HDMI audio, ACP6x (Ryzen 6000/7020), ACP 6.3 "PS" (7040/8040, 7640HS), SOF Renoir/ACP63 |
| Wi-Fi | iwlwifi + iwlmvm (7260 and newer), iwldvm (5000/6000), iwlmld (Wi-Fi 7) | Realtek rtw88 (8821/8822/8723, PCIe and USB) and rtw89 (8851B, 8852A/B/C, 8922A), MediaTek mt7921/mt7922/mt7925 (PCIe, mt7921 USB); these chips are common on both vendors |
| Bluetooth | btusb with btintel/btrtl/btmtk/btbcm, Intel PCIe Bluetooth | same (Realtek and MediaTek chips) |
| Ethernet | e1000e, igb, igc | r8169, r8152 (USB), CDC USB tethering |
| Storage | NVMe, SATA/AHCI built in, **Intel VMD built in** (many Intel laptops hide the NVMe behind it), SD readers (Realtek, SDHCI) | NVMe, SATA/AHCI |
| Touchpad, sensors | Intel LPSS + pin controllers (Baytrail to Meteor Lake), I2C-HID, SMBus touchpads (Synaptics RMI4, Elan), Intel ISH | I2C-HID, SMBus touchpads, AMD SFH sensor hub (`amd_sfh`) |
| Thermal, temperatures | coretemp, package thermal, powerclamp, RAPL, int340x (DPTF) | k10temp |
| Platform | ThinkPad, IdeaPad, Dell, HP, ASUS, Acer extras (hotkeys, platform profile), Intel HID/VBTN | same drivers by maker |
| Ports | Thunderbolt/USB4, USB-C (UCSI) | same |

Not enabled: AMD PMF (needs the AMD TEE driver), AMD PSP/ccp (not needed to boot), Intel MEI (not needed),
Atheros ath10k/11k/12k and Broadcom Wi-Fi, webcams (no camera app), NVIDIA nouveau, Bluetooth userspace (bluez is not packaged).

## Firmware (recipe linux-firmware 20261019 release 2, sof-firmware, microcode)

* linux-firmware ships amdgpu, radeon, i915 (GuC/HuC/DMC), xe, intel/ibt (Bluetooth), intel/ish, iwlwifi, rtw88, rtw89, rtl_nic,
  rtl_bt, mediatek (MT79xx Wi-Fi and Bluetooth), cirrus (laptop amplifiers) and amd-ucode.
* Pruned: iwlwifi keeps the newest three firmware files of each chip family (the driver asks for the highest API first; the newest
  file in the tarball equals the highest API of both kernels), camera/NPU/server/audio-DSP firmware of Intel, mediatek router and
  Chromebook SoC firmware. Result about 420 MB unpacked (was about 510 MB, with fewer families).
* sof-firmware (new): Sound Open Firmware 2026.09.2, `intel/sof*` (about 57 MB unpacked), needed for SOF microphones and
  SoundWire codecs.
* microcode (new): `/boot/ucode.img`, an uncompressed cpio with `kernel/x86/microcode/GenuineIntel.bin` (client CPUs from 2013, Intel
  release 20261110) and `AuthenticAMD.bin` (all AMD families). **Early loading:** the installer copies it to the ESP next to the
  kernel and writes `initrd=\ucode.img` first in the flboot.conf cmdline; the kernel's EFI stub loads it as the first initrd. It is
  not in the live ISO's boot image (El Torito images are limited to 31 MB), so the live system runs without it; the installed
  one has it. An existing install keeps its flboot.conf (no ucode) until `initrd=\ucode.img` and the file on the ESP are added by hand.
  Check: `dmesg | grep -i microcode`.

## Graphics userspace (recipe mesa release 3, built 2026-11-13)

* OpenGL: radeonsi (AMD), llvmpipe and softpipe (software), virgl (QEMU), zink. **Intel iris (Gen8 and newer) and crocus (Gen4
  to Haswell): deferred**, see above; an Intel GPU draws with llvmpipe until then.
* Vulkan: radv (AMD), lavapipe (software). **Intel anv (Gen9 and newer) and hasvk (Ivy Bridge to Broadwell): deferred.**
* The Intel drivers need clang (llvm recipe release 2), spirv-llvm-translator and libclc for the shader compiler: they wait
  for the 16 GB build machine.
* Video decode: libva 2.24 (DRM and Wayland), AMD through Mesa's `radeonsi_drv_video.so` (video-codecs=all), Intel through
  `intel-media-driver` (iHD, Broadwell and newer, needs gmmlib; older Intel GPUs have no VA-API driver). flvideo and mpv use
  `hwdec=auto-copy-safe`, which takes VA-API when a driver works and software decoding otherwise. FreeBWS (WPE/GStreamer) decodes in software:
  GStreamer's `va` plugin is not built (needs libgudev).
* Brightness: flsettings and flcomp use `/sys/class/backlight` and now prefer the "firmware", then "platform", then "raw"
  device when a laptop has several (acpi_video0 next to intel_backlight or amdgpu_bl1). The udev rule
  `90-flsettings-backlight.rules` matches every backlight device, so group seat can write all of them.
* Power: no thermald (needs upower and dbus-glib; intel_pstate, powerclamp and the firmware do the throttling) and no
  power-profiles-daemon; flpower writes the governor and the energy preference directly.

## What was tested where

| Item | Where | Result |
|---|---|---|
| Boot, desktop, Wi-Fi (rtw89_8852BE), amdgpu on Radeon 610M | Ryzen 5 7520U laptop | partly, earlier rounds |
| Kernel config applies (olddefconfig) | host, 7.3.4 and 6.18.63 trees | yes |
| Kernels, firmware, microcode, gmmlib, intel-media-driver, Mesa 3 build | build host, 2026-11-13 | built |
| Boot with the new kernel and `initrd=\ucode.img`, desktop, installer | QEMU/KVM | see docs/ROADMAP-NEXT.md, round 15 |
| Everything else in this document | nowhere | untested |

## Wanted testers

Please report with `dmesg`, `lsmod`, `lspci -nnk`, `/sys/class/backlight`, `vainfo`, `vulkaninfo --summary` (not packaged yet) of:
an Intel laptop with VMD/RST NVMe, one with SOF audio, one with Wi-Fi 7 (BE200), an AMD laptop with MT7922/MT7925, a Ryzen 7040/8040
(Phoenix) machine such as the planned NucBox M6 Ultra.
