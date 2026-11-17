# Hardware support

FreeLFS targets typical laptops and desktops from about 2015 to now, x86_64, UEFI, 1 GB RAM and up.

**Status in short**

| What | Status |
|---|---|
| AMD Ryzen 5 7520U laptop (Radeon 610M, rtw89 Wi-Fi) | tested, partly (earlier rounds; the milestone M10 run on the laptop failed and was fixed, not retested after the kernel change below) |
| AMD, everything else | prepared, untested on real hardware |
| Intel | **prepared, untested on real hardware: needs testers** |
| NVIDIA | **phase 1 built in round 20, untested on real NVIDIA hardware** (nouveau kernel driver with KMS, firmware framebuffer fallback, "Safe graphics" boot entry, CPU-composited desktop); NVK and a proprietary driver are planned, see "NVIDIA" below |
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

Not enabled: AMD PMF (needs the AMD TEE driver), AMD PSP/ccp (not needed to boot), Intel MEI (not needed), webcams (no camera app).


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

## NVIDIA

A TikTok viewer with an NVIDIA machine could not get to the desktop (round 19). FreeLFS's desktop is composited on the CPU
(flcomp copies a shadow buffer into a KMS dumb buffer), so a desktop needs only a working display, not 3D.

**Status: phase 1 is in the round-20 build. QEMU cannot emulate NVIDIA: none of the NVIDIA paths ran on a real card.**
What was run: the firmware-framebuffer path (below) in QEMU with `-vga std` and `nomodeset`.

### Phase 1 (done): a desktop on NVIDIA

| Piece | What | Tested |
|---|---|---|
| Kernel | `CONFIG_DRM_NOUVEAU=m` (linux 7.3.x and linux-lts 6.18.x release 3), with KMS; `CONFIG_FB_EFI=y` next to the existing `DRM_SIMPLEDRM` + `SYSFB_SIMPLEFB` | builds; nouveau never ran on hardware |
| Firmware | linux-firmware release 3 ships `nvidia/` (acr, gr, nvdec, sec2 for Maxwell to Ada; GSP and booter files for Turing, Ampere, Ada, Blackwell). Pruned: Tegra SoC firmware, and the GSP version 535.113.01 where 570.144 exists (the newest only, 25 to 60 MB each). Nouveau on Turing and newer needs GSP: without these files the card stays dark | package contents only |
| Boot | the boot menu (flboot) always offers **"FreeLFS (Safe graphics, kernel X)"**: the newest kernel with `nomodeset nouveau.modeset=0`, no GPU driver, the screen stays on the firmware framebuffer (simpledrm). Use it when nouveau hangs or the screen stays black | flboot menu test; QEMU boot with nomodeset |
| flcomp | prefers a card of a real GPU driver over simpledrm; when a PCI display controller has no driver yet it waits up to 10 s for it (nouveau loads GSP firmware slowly) instead of starting on simpledrm and losing the screen when nouveau takes over; with `nomodeset` it does not wait. On simpledrm it runs on one dumb buffer with no gamma table: the night light uses its software path (log: "night light is done in software") | QEMU `-vga std` + nomodeset: desktop, windows, night light |
| Mesa | `nouveau` added to the gallium drivers (Mesa release 4): OpenGL on NV30 to Ada through the classic nouveau driver, using the libdrm_nouveau that was already built. No new big dependency. Most NVIDIA GPUs from Maxwell on cannot reclock with nouveau (slow), and GL on Turing and newer through the classic driver is limited; llvmpipe stays the fallback | builds; never ran on hardware |
| Diagnosis | flcomp logs which card it took ("display on cardN (GPU driver or firmware framebuffer)"); `flreport` collects it with `dmesg` and `lspci -nnk` | |

If the screen is black after boot on NVIDIA: pick Safe graphics in the boot menu; then run `flreport` and send it.
The first-start wait of up to 10 s is only for a display controller whose driver did not bind.

### Phase 2 (planned): NVK, Vulkan and modern OpenGL on nouveau

NVK (Mesa's Vulkan driver for NVIDIA, with zink for OpenGL) is the open path to 3D on Turing and newer. It needs, at build time:
Rust (rustc 1.82 or newer, cargo, bindgen-cli and cbindgen, because Mesa's NAK shader compiler is written in Rust), `rust-bindgen`,
`libclc` and `spirv-llvm-translator`/`spirv-tools` (already deferred for the Intel drivers: the same 16 GB machine in January 2027),
and meson `-D vulkan-drivers=nouveau`. Runtime: kernel 6.6 or newer with the new uAPI (`nouveau` with `DRM_NOUVEAU_GSP` firmware, 7.3 and
6.18 have it) and the GSP firmware above. Work items: a `rust` recipe (or a rustc binary tarball, licensing MIT/Apache-2.0), the Mesa
recipe flags, flcomp's GPU path for NVK buffers (dmabuf with NVIDIA's block-linear modifiers: flcomp announces only modifiers it
can import, see src/flcomp/dmabuf.c), a test on real Turing, Ampere and Ada cards. Pascal and older keep the classic nouveau OpenGL driver.

### Phase 3 (optional): a proprietary or open-kernel NVIDIA driver through flpkg

Not part of the base system and never installed without being asked. A package `nvidia-driver` in a separate repository, installed by
flpkg after the person confirms the licence:

* **NVIDIA proprietary driver** (user space and kernel module): license "NVIDIA proprietary", redistribution only under NVIDIA's terms;
  FreeLFS must not ship it in the ISO or the default repository, and the package would download the installer from NVIDIA at
  install time (with the licence shown) instead of containing it.
* **NVIDIA open GPU kernel modules** (`nvidia.ko`, MIT/GPL-2.0 dual licensed, Turing and newer) with the proprietary user space (GSP
  firmware and libraries stay under NVIDIA's licence). Built against the installed kernel by a hook, so it needs kernel headers.
* Both need: DRM modeset (`nvidia-drm.modeset=1`) and GBM/EGL external-platform libraries to work with a Wayland compositor. flcomp
  composites on the CPU and would keep working through the dumb-buffer path; GPU buffers for FreeBWS need the GBM backend.
* Conflicts with nouveau (blacklist), with Secure Boot (modules must be signed), and with LTS kernel updates (rebuild on each kernel upgrade).
* Licensing notes belong in the package's own page in the store, and in `docs/CVE-REPORT.md` as "not scanned".

Order of work: phase 1 verified on real hardware (a tester with an NVIDIA card is wanted), then phase 2 on the 16 GB machine, then
phase 3 only if phase 2 does not give a usable desktop.

## Wi-Fi coverage (round 20) and Secure Boot

**Found on real hardware:** an old Acer laptop (Intel Ice Lake, i915 works, the desktop runs with software rendering) had no Wi-Fi
interface in the r19 live ISO: its card is a Qualcomm Atheros **QCA9377** (PCI 168c:0042, driver `ath10k_pci`), and the kernel had no
Atheros driver and the image no `ath10k` firmware. Round 20 fixes that for both kernels (7.x and 6.18 LTS), as modules:

| Family | Modules | Firmware (linux-firmware release 4) |
|---|---|---|
| Atheros | ath9k (PCIe) and ath9k_htc (USB), ath10k (PCIe, SDIO, USB: QCA6174, QCA9377, QCA988x, QCA99x0, QCA9984/88/87, QCA4019), ath11k (PCIe: QCA6390, WCN6855, QCA2066, QCA6698AQ), ath12k (WCN7850) | `ath9k_htc`, `ath10k` (phone chip WCN3990 pruned), `ath11k` (router, server and phone chips pruned), `ath12k` (router/server pruned), plus the Bluetooth firmware that comes with them (`qca`, `ar3k`) |
| Realtek | rtw88, rtw89 (already), **rtl8xxxu** (USB sticks) and the old PCIe rtlwifi family (rtl8192ce/de, rtl8723be, rtl8188ee, rtl8192ee, rtl8821ae) | rtw88, rtw89, `rtlwifi` |
| Intel | iwlwifi (iwldvm, iwlmvm, iwlmld) already | `intel/iwlwifi` (newest three files per family) |
| MediaTek | MT7921/MT7922/MT7925 (already) and now mt76x2e, mt7615e, mt7915e | `mediatek` |
| Broadcom | **brcmfmac** (PCIe, USB, SDIO) | `brcm` |

Cost: about 70 MB of extra firmware unpacked (compressed in the ISO about half); modules cost nothing in RAM until a matching card appears,
so the idle numbers do not change. **Real Wi-Fi is not testable in QEMU:** the round-20 check only proves that the modules (with the right PCI
ids) and the firmware files are in the image (`build/m20-features.sh wifi-modules`); no Wi-Fi card was tried. `flreport` now lists the network
interfaces and, for every PCI network controller (class 0x0280 and 0x0200), its id, the driver bound ("NO DRIVER BOUND" when none) and the
modules the kernel would pick, plus the `dmesg` lines about firmware. `lspci` (pciutils), `lsusb` (usbutils) and `ip` are in the base image
and in the PATH of ordinary users (`/usr/sbin` is now last in everybody's PATH).

**Secure Boot.** The same laptop would not start Ventoy until Ventoy's MOK key was enrolled: Secure Boot is on in most laptops. FreeLFS does not
boot with Secure Boot on either: flboot and the kernels are not signed and there is no signed shim. For now: turn Secure Boot off in the firmware
setup, or enrol a key (Ventoy shows how for itself; FreeLFS's flboot and kernel would still be unsigned and refused). **Needed: a Secure Boot
chain**, i.e. a signed shim (Microsoft-signed, from a distribution or through the shim review board) that loads flboot, a FreeLFS key enrolled
as a MOK for flboot and the kernels (flinstall would sign each new kernel, the live ISO needs signed images), and signed kernel modules or a
module signature policy. Until that exists, "Secure Boot must be off" belongs on the download page and in the installer.

## What was tested where

| Item | Where | Result |
|---|---|---|
| Boot, desktop, Wi-Fi (rtw89_8852BE), amdgpu on Radeon 610M | Ryzen 5 7520U laptop | partly, earlier rounds |
| Kernel config applies (olddefconfig) | host, 7.3.4 and 6.18.63 trees | yes |
| Kernels, firmware, microcode, gmmlib, intel-media-driver, Mesa 3 build | build host, 2026-11-13 | built |
| Boot with the new kernel and `initrd=\ucode.img`, desktop, installer | QEMU/KVM | see docs/ROADMAP-NEXT.md, round 15 |
| Round 20: Wi-Fi modules and firmware (Atheros, Realtek, Broadcom) | QEMU: modules and files present only | no Wi-Fi card tried |
| Round 20: NVIDIA phase 1 (nouveau, firmware, Safe graphics) | QEMU: the nomodeset/simpledrm path only | see NVIDIA; real NVIDIA untested |
| Everything else in this document | nowhere | untested |

## Wanted testers

Please report with `dmesg`, `lsmod`, `lspci -nnk`, `/sys/class/backlight`, `vainfo`, `vulkaninfo --summary` (not packaged yet) of:
an Intel laptop with VMD/RST NVMe, one with SOF audio, one with Wi-Fi 7 (BE200), an AMD laptop with MT7922/MT7925, a Ryzen 7040/8040
(Phoenix) machine such as the planned NucBox M6 Ultra.
