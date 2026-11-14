# 9. Troubleshooting

> Known problems and what to do about them, how to find the logs, how to go back
> to an earlier state, and how to send a useful bug report. Start with the table
> of known problems, then read the section that matches your problem.

## Contents

- [Known problems](#known-problems)
- [Where the logs are](#where-the-logs-are)
- [Sound](#sound)
- [Wi-Fi](#wi-fi)
- [Shutdown and restart](#shutdown-and-restart)
- [The web browser cannot log in to a site](#the-web-browser-cannot-log-in-to-a-site)
- [Videos and music](#videos-and-music)
- [The "Something went wrong" screen](#the-something-went-wrong-screen)
- [The screen and the keyboard](#the-screen-and-the-keyboard)
- [The lock screen](#the-lock-screen)
- [USB sticks](#usb-sticks)
- [The live system](#the-live-system)
- [Going back to an earlier state (snapshots)](#going-back-to-an-earlier-state-snapshots)
- [Updates](#updates)
- [Reporting a bug (flreport)](#reporting-a-bug-flreport)
- [Testing new hardware](#testing-new-hardware)

## Known problems

| Problem | Status | What to do now |
|---|---|---|
| Shutdown did not power off the laptop (round 16 and 17) | Fix in round 18. Not verified on the laptop when this chapter was written | Wait a minute. If it still does not power off, hold the power button. Report it with `flreport` |
| Opening media from Files failed on the laptop (round 17) | Round 18 is working on it | Open the app from the start menu, then open the file from inside it. Report it |
| Video did not play on a Ryzen laptop | Software decoding is the default since round 18 | See [Videos and music](#videos-and-music) |
| FreeBWS was blank on the laptop's graphics card | Fixed in release 9. Not checked on the laptop since | Report it again if it happens |
| Some websites cannot log in in FreeBWS (Google sign-in, some anti-bot checks) | Known, not fixed | Use Firefox from Flathub in the Software Store |
| Wi-Fi on the laptop (Realtek rtw89) | Worked on the tested laptop. Other chips are untested | See [Wi-Fi](#wi-fi) |
| Intel machines (graphics, Wi-Fi, sound) | Untested on real hardware | Report with the information in [Testing new hardware](#testing-new-hardware) |
| The installer on a real disk | Not tested on a real disk | Try it in a virtual machine first. See [03-installing.md](03-installing.md) |
| The public FreeLFS package repository | Not published yet | Use Flathub in the Store |
| Secure Boot | Not supported | Turn Secure Boot off in the firmware |

## Where the logs are

| Log | Where | What it has |
|---|---|---|
| The whole desktop session | `$XDG_RUNTIME_DIR/flcomp.log` (the previous session: `flcomp.log.1`). `XDG_RUNTIME_DIR` is `/run/user/YOUR-ID` | The compositor, the panel, and every program started from it |
| Session services | `$XDG_RUNTIME_DIR/session.log` | The sound and the bus services that the session starts |
| One program | `~/.local/state/freelfs/log/NAME.log` (about 1 MB; one older copy as `NAME.log.1`) | What a program printed, when it was started from the panel or the start menu |
| Boot, live system (verbose) | The "verbose" boot menu entry shows the messages on the screen | Startup of the live system |
| Video player | `~/.cache/flvideo/mpv.log` | The mpv messages from Videos |
| FreeBWS | Started from a terminal: `freebws 2>~/fb.log` | Errors of the browser |

To see the desktop's log in a terminal:

```sh
less $XDG_RUNTIME_DIR/flcomp.log
```

## Sound

Work through these steps in order.

1. Click the **volume icon** in the panel. Check that the volume is not zero and
   that the mute button is off.
2. Press the volume keys. If an indicator appears but nothing plays, the problem is
   with the output, not with the keys.
3. Open a terminal and run `wpctl status`. It lists the outputs (sinks). The
   one with `*` is the one in use. If the list is empty, the sound server did
   not start. Look at `$XDG_RUNTIME_DIR/session.log`.
4. Check for a Bluetooth headset that took the sound. The Bluetooth flyout shows it.
5. Check the **System sounds** switch in Settings > General. It only controls the
   short event sounds (login, notification, screenshot), not music or video.

The sound settings are described in [08-settings-reference.md](08-settings-reference.md).

## Wi-Fi

Open **Settings > Wi-Fi**. Its message says what is wrong.

| Message | Meaning | What to do |
|---|---|---|
| "No Wi-Fi hardware found" | The kernel did not find a Wi-Fi adapter, or its firmware did not load | Report it with `flreport`. The file has the hardware and the firmware messages |
| "Wi-Fi is turned off" | The Wi-Fi switch is off | Turn the switch on |
| "Looking for the Wi-Fi service..." | The Wi-Fi service is starting | Wait a few seconds |
| "The Wi-Fi service (iwd) is not running" | The service is stopped | Ask the administrator (the system guide, chapters 10 to 19, covers services). Report it if it stays stopped |
| "You are not allowed to use Wi-Fi" | Your account has no access to the Wi-Fi service | The account needs the group `seat`, `wheel` or `netdev` (the installer adds the account you make to these groups) |

**Tested:** the Realtek rtw89 chip (RTL8852BE) on the AMD laptop. **Not tested:**
other chips, including Intel and MediaTek.

If a known network does not connect on its own, connect once by hand from the
Wi-Fi page. A network you copied from the live system should connect at the first boot
of the installed system, but this was not verified on real hardware.

## Shutdown and restart

Shutdown from the start menu failed on the laptop in round 17. Round 18 changes the
shutdown path. Until you have that build:

1. Save your work.
2. Choose **Shut down** from the start menu and wait for the 30 second countdown.
3. If the screen goes off but the computer stays on, or nothing happens after a
   minute, hold the power button until it turns off.
4. Afterwards, check the log of the session for errors (see [Where the logs are](#where-the-logs-are)).
5. Report the case with `flreport`.

Restarting goes through the same shutdown path, so the same caveat applies.

## The web browser cannot log in to a site

FreeBWS does not manage every login. The known cases are:

- **Google sign-in.**
- **Anti-bot checks** such as Cloudflare Turnstile.

For these sites, use **Firefox** instead. On an installed system, open the Software
Store, search for Firefox, and install it from Flathub. Firefox is not in the FreeLFS
repository. The live system does not set up Flathub, so install Firefox after you
install FreeLFS.

Other FreeBWS problems:

- A page that stays blank or crashes at once: run `freebws` from a terminal with
  the errors kept (`freebws 2>~/fb.log`), then read `~/fb.log`. You can also try
  `FREEBWS_SANDBOX=off freebws` to see whether the sandbox is the cause. Report it.
- `freebws --sandbox-check` says whether the sandbox works on your computer.

## Videos and music

- **A video does not play.** Videos decodes in software by default. If it still
  stutters, the file may be too big for the processor. Try a smaller file. Open
  `~/.cache/flvideo/mpv.log` and look for errors.
- **Hardware decoding** can be tried with `FLVIDEO_HWDEC=auto-copy-safe flvideo FILE`
  from a terminal. It did not work on the tested Ryzen laptop, so it is off by default.
- **Subtitles do not show.** Subtitles are shown from the file, or from a `.srt`,
  `.ass` or `.sub` file with a similar name, next to the video.
- **Music does not play a format.** The music player uses GStreamer. MP3, FLAC, AAC,
  WAV, Ogg Vorbis and Opus are the expected formats. Report the file's type if
  another one fails.

## The "Something went wrong" screen

This screen appears when the desktop ends badly. It shows a code, the program
that failed, and the status. The codes are:

| Code | Meaning |
|---|---|
| FL-SIGSEGV | The compositor crashed (a segmentation fault) |
| FL-SIGKILL | The compositor was killed, often because the computer ran out of memory |
| FL-START | The desktop did not start within its first five seconds |
| FL-EXITn | The desktop stopped with the status n |

Keys: **Enter** starts the desktop again. **R** restarts the computer. **S** shows the end
of the session log. **Esc** leaves to a text console.

If it comes back every time, note the code and report it with `flreport`.

## The screen and the keyboard

- **The screen is black after the boot menu.** Try the "verbose" boot entry to see
  where it stops. Graphics drivers for Intel are not tested. AMD's are the ones tested.
- **The screen resolution is wrong.** Open Settings > Display, change the mode, and
  press **Apply**. Within 15 seconds, press **Keep these settings?**. If you do not,
  the old mode comes back.
- **The screen is upside down or the wrong size.** Check the scale in Settings > Display.
- **The keyboard layout is wrong.** Open Settings > Keyboard, pick the layout, and
  press Try it to test. The choice is saved in `/etc/keyboard.conf` when that is allowed.
  On a live system the choice is lost at the next boot, because the stick has no
  place to keep it.
- **Ctrl+Alt+F2 shows a text console.** That is the way to a console when the desktop
  is stuck. Log in there and end the session with `pkill flcomp`.

## The lock screen

- **Your password is refused.** Check Caps Lock. Each wrong password adds a second
  before the next check.
- **The screen stays locked and shows a plain colour.** The lock program crashed. The
  desktop starts it again. If that fails, the screen says so: press Enter, or
  switch to a console with Ctrl+Alt+F2, log in, and run `pkill flcomp`.
- **The lock screen says the account cannot be unlocked by a password.** The password
  is locked or has expired. Use Ctrl+Alt+F2 to log in on a console, and ask the
  administrator to fix the account.

## USB sticks

- **The stick does not appear in Files.** Wait a few seconds. Then run
  `flmount list` in a terminal. If it is listed but not mounted, click it in Files.
- **The stick cannot be mounted.** Check its file system. NTFS (Windows) is **not
  supported**. FAT32, exFAT, ext4 and btrfs work. Reformat the stick if it matters, but
  first copy any files you need.
- **The stick is not read after a copy.** Eject it from Files before you take it out.
- **No notification appears when you plug a stick in.** The notification comes from
  the panel. It may be hidden by Do not disturb (see the notification centre).

See [07-files-drives-usb.md](07-files-drives-usb.md).

## The live system

- **"could not find the FreeLFS live system".** The prompt on the screen lists the keys:
  `r` tries again, `l` lists the disks, `p` powers off, `b` reboots. The stick may be slow.
  Retry. Try another USB port.
- **Settings > About says "Could not keep your files".** The persistence file could
  not be made. The message says why when it knows. The stick needs 4 GB free and the
  file system must allow writing. See [02-getting-started.md](02-getting-started.md).
- **Memory runs out.** The live system keeps everything in RAM. Close programs you
  do not need, and do not install large programs on a 1 GB computer. Two gigabytes or
  more are comfortable.
- **Files are gone after a restart.** That is how the live system works, unless
  persistence is on. Copy files to a stick before you shut down.

## Going back to an earlier state (snapshots)

FreeLFS takes a snapshot of the system before each change to its packages. If an
update breaks something, you can go back. Three steps:

1. **Start an earlier state.** At the boot menu, choose "Start an earlier state" and
   pick the snapshot from before the change. The system starts **read-only**: it cannot
   change the system folder `/`. Your home folder, `/tmp` and `/run` still work.
2. **Check it.** Look around. If it works, you know the snapshot is good.
3. **Make it the current state.** A notice on the desktop offers **Make this the
   current state**. You can also use **Settings > Snapshots** > "Go back to this", or in a
   terminal `flsnap rollback`. The state you left is kept as a snapshot too. Restart
   to use it.

Notes:

- The kernel is not part of a snapshot. An old snapshot boots with the newest kernel. If
  the snapshot is older than the kernel's modules, choose an older kernel entry in
  the boot menu. This case is not tested.
- FreeLFS keeps the last 8 snapshots by default (`keep = 8` in `/etc/flsnap.conf`).
- You can take a snapshot yourself in **Settings > Snapshots** before a risky change.

## Updates

- An update notice ("N updates available") appears a few minutes after you log in,
  and once a day after that. It only tells you what is available. It never installs
  anything.
- Click the notice, or open the **Software Store**, and choose the **Updates** tab.
- Flathub apps update from the same tab.
- FreeLFS packages come from the public repository, which is not published yet. Until
  then, only Flathub updates apply to most people.

## Reporting a bug (flreport)

A good report has four parts: what you did, what you expected, what happened, and
the report file. Do this:

1. **Write down the steps.** Number them. Note the time when the problem happened.
2. **Make the report.** Settings > About > **Save a report**. It takes about 10
   seconds and says where the file is. The file is named like
   `~/freelfs-report-YYYYMMDD-HHMMSS.tar.gz`. The command `flreport` does the same in a
   terminal.
3. **Find the file.** On the live system, the home folder is in RAM. Copy the file to a
   USB stick with Files before you shut down.
4. **Look through the file for private data.** It contains network names, device
   identifiers and MAC addresses. It does not contain passwords, Wi-Fi keys, browser
   history or bookmarks. Remove what you do not want to share.
5. **Open an issue.** The project is on GitHub at `github.com/wasselnl77-hash/freelfs`.
   Open an issue, describe the problem with the steps, and attach the report (or a link
   to it). A template for testers is planned.

For a security problem, read `docs/SECURITY.md` first. It describes what is known and
how the project handles it.

**What the report holds:** the release and kernel, the machine (its DMI data), the
processor, memory, the logs of the desktop and the programs, the graphics, sound,
network (names of networks, addresses, a name lookup and a test connection), the
configuration files, the mounts, the processes, the installed packages and the
crash lines. It is produced with your ordinary user rights, so it leaves out what
needs an administrator.

## Testing new hardware

The project wants reports from machines it has not run on. These help the most:

- The model of the computer and its graphics card, Wi-Fi chip and sound chip.
- The output of these commands, run in a terminal, saved to a file:
  `dmesg`, `lsmod`, `lspci -nnk`, `ls /sys/class/backlight`, and `vainfo` (if installed).
- Whether the desktop starts, the Wi-Fi page finds networks, the sound works, and
  shutdown powers the machine off.
- The file from **Save a report**.

Particularly wanted (from `docs/HARDWARE.md`): Intel laptops with an NVMe disk behind
Intel VMD or RST, with SOF audio, or with Wi-Fi 7; AMD laptops with MediaTek MT7922
or MT7925 Wi-Fi; and Ryzen 7040 or 8040 machines.

See also [01-introduction.md](01-introduction.md) for the status of the project, and the
table of contents in [README.md](README.md).
