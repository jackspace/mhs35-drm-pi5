# A 3.5 inch SPI touchscreen as a real second display on a Raspberry Pi 5

This is the write-up I wished existed when I started. The cheap 3.5 inch resistive panels that plug onto the Pi's header (sold as MHS-3.5, Waveshare 3.5 A/C, and a dozen other names, all with an ILI9486 controller) have always been a compromise: either the panel is your only screen, driven by the old fbtft framebuffer hack, or you give up on it and use HDMI. I wanted both at once. A small always-on status panel on the box itself, a big monitor when one is plugged in, touch that stays on the small panel, and no stutter on the big one.

That turned out to need a small override of the in-tree DRM driver, a device tree overlay, four X configuration files, and one script that places the panel at the login screen. All of it is here, with an installer that asks you the few questions that matter and does the rest. It was validated end to end on a freshly flashed Kali 2026.2 arm64 card: image, provision headless, run the installer, reboot, panel live at the login prompt with touch working.

## Who this is for

I built it for a small off-grid box in a lakeside town in Mexico, where the internet is good until it is not. The panel shows what the box is doing; the HDMI port is for when I sit down with it. But nothing here is specific to that, and I would be glad if it ended up in places like these:

- **A community library or classroom on a Pi.** Kiwix serving Wikipedia and Gutenberg to a room with no internet, with the panel showing the library's address and how many people are connected, and a monitor for whoever is at the desk.
- **A field recorder or a lab bench instrument.** Something that lives in a case, where the panel is the instrument's face and HDMI is the engineering port.
- **A mesh radio node or a ham shack box.** Status on the panel, waterfall on the monitor, touch for the two buttons you need in the field.
- **A kiosk in a clinic, a co-op, a repair cafe, a school.** Boot to a login, not to a desktop, so the machine is not an open door, with the small screen telling people it is alive.
- **Anyone learning how DRM, PRIME and X fit together.** The "why" sections below are the part I would have paid for: each fix is tied to a measurement, not a guess.

This project is for people who share things: knowledge, tools, bandwidth, time. It is not a bunker accessory. If your plan for the end of the world involves keeping other people out, this is not the repo for you, and I would rather you did not use it.

## What you get

- One X server, two outputs. The 480x320 panel is a DRM/KMS connector (`card*-SPI-1`, X names it `Unknown19-1`), primary at +0+0. HDMI-1 at +480+0, fully accelerated through vc4 at 24-bit.
- Resistive touch (ADS7846) mapped to the panel only.
- Unplug HDMI and the desktop collapses onto the panel; plug it back and it restores. Optional: the Xfce bar follows HDMI.
- Every panel update costs X about 1 ms. The SPI frame itself (25 ms at 100 MHz) runs on a kernel worker, so the HDMI cursor stays smooth.

Tested on a Raspberry Pi 5, Debian/Kali based arm64, kernel 6.12.34+rpt-rpi-2712, X.Org 21.1 with the modesetting driver, Xfce.

## Files

| File | Goes to | Purpose |
|---|---|---|
| `provision-boot.sh` | run once on the imaging host after flashing | optional headless Wi-Fi + SSH provisioning before first boot |
| `setup-mhs35-drm.sh` | run once on the Pi as root | interactive installer for everything below |
| `mhs35-drm-overlay.dts` | `/boot/firmware/overlays/mhs35-drm.dtbo` | device tree overlay: panel on spi0.0 with the in-tree DRM `ili9486` driver, touch on spi0.1 |
| `ili9486.c`, `Makefile` | `/lib/modules/$(uname -r)/updates/ili9486.ko` | out-of-tree override of the in-tree driver (see "Why an override") |
| `98-ili9486-panel.conf` | `/etc/X11/xorg.conf.d/` | X OutputClass for the panel: no glamor, no page flip, software cursor |
| `99-vc4.conf` | `/etc/X11/xorg.conf.d/` | X OutputClass for the HDMI side: modesetting, PrimaryGPU |
| `99-calibration.conf` | `/etc/X11/xorg.conf.d/` | evdev InputClass for the resistive touch with raw calibration |
| `mhs35-panel-setup.sh` + `.desktop` | `/usr/local/bin/`, `/etc/xdg/autostart/`, and lightdm `display-setup-script` | places the panel as an X output and confines touch to it, at the greeter AND in the session |
| `mhs35-bar-follow.sh` + `.rules` | `/usr/local/bin/`, `/etc/udev/rules.d/` | Xfce only: bar to HDMI when connected, else to the panel |

## Quick start

1. Flash the OS with Raspberry Pi Imager. In the OS customisation dialog set a hostname, a user, and enable SSH. Boot once, log in, run `sudo apt update`. (Kali users who want to skip the monitor entirely: see "Headless first boot" below.)
2. Copy these files to the Pi, for example `scp -r mhs35-drm-pi5 <user>@<pi-host>:`.
3. `cd mhs35-drm-pi5 && sudo ./setup-mhs35-drm.sh`. It asks for the desktop user, the panel rotation, the SPI clock, whether to enable autologin, and whether to install the Xfce bar helper. It installs the headers, compiles the overlay and the module, edits `config.txt`, installs the X files, regenerates the initramfs, and offers to reboot.
4. After the reboot, `xrandr` should list `Unknown19-1 connected primary 480x320+0+0` and `HDMI-1 connected 1920x1080+480+0`. Touch the panel; the cursor should follow on the panel only. `/sys/bus/spi/devices/spi0.0/mhs35_stats` shows `sent` increasing while you move the mouse over the panel.

If you edit files by hand instead of running the script, these placeholders must change: `<DESKTOP_USER>` (the autologin/Xfce user, in `mhs35-bar-follow.sh` and the lightdm autologin snippet), `<SPI_HZ>` (the `speed=` value in `config.txt`), `<ROTATION>` (0/90/180/270 in `config.txt`).

## Headless first boot: Wi-Fi and SSH with no monitor

If you want to flash the card and do everything over SSH, provision Wi-Fi and your SSH key onto the card before first boot. The Kali Raspberry Pi image enables the cloud-init NoCloud data source: its `cmdline.txt` ends with `ds=nocloud`, and the BOOT partition carries `user-data` and `meta-data`. After flashing, run `provision-boot.sh` on the imaging host with the BOOT partition mounted:

```
./provision-boot.sh /media/you/BOOT      # macOS: /Volumes/BOOT ; Windows: write the files to the BOOT drive by hand
```

It prompts for your Wi-Fi SSID and password, a username, a uid, and your SSH public key.

**Do not configure Wi-Fi through cloud-init's `network-config` on Kali.** This is the single biggest trap in this whole project and it cost me an evening. cloud-init renders that file into `/etc/netplan/50-cloud-init.yaml`, and on a NetworkManager system that file seizes the interfaces and hands them to a renderer that never brings them up. The symptom is unmistakable once you have seen it: `nmcli dev status` shows `wlan0 unavailable` and `eth0 unmanaged`, `rfkill` reports nothing blocked, and `nmcli dev wifi connect` fails with "No network with SSID found" because the device never becomes available to scan. It is strictly worse than leaving Wi-Fi unconfigured. `provision-boot.sh` therefore writes a NetworkManager connection profile directly and disables cloud-init's network rendering.

**Give provisioned accounts a high uid.** `useradd` hands out 1000, then 1001, so an account created at first boot takes 1000 and pushes the image's own desktop user to 1001. Anything that assumes "uid 1000 is the person at the keyboard" then picks the wrong account. The regular-user range is 1000 to 60000, so a fixed number high in it, 20010 by default here, never collides. Stay below 60000; above that some tooling stops treating the account as a normal user, and 65536 and up overlaps the ranges used for container user namespaces.

On **Raspberry Pi OS**, do not use this script; use the Raspberry Pi Imager "Edit settings" dialog, which sets Wi-Fi, hostname and SSH through that image's own first-boot mechanism.

## Logging in, and how the panel gets arranged

The installer defaults to **no autologin**, so the machine asks for a login. A device that boots straight to a desktop is a hole, especially a portable one. Answer `y` at the autologin prompt only if you are building an appliance that nobody can walk off with.

That raises a problem the installer solves for you. X binds this panel as a reverse-PRIME output sink but leaves it with **no mode**: it reads `connected` in `xrandr` and stays black forever until something places it explicitly. Nothing in X does that for you. So `mhs35-panel-setup.sh` is wired into lightdm's `display-setup-script`, which runs as root when X starts and before the greeter is drawn, and into the session autostart as well. The result is that the panel is live at the login screen, not just after login.

You can instead arrange the displays in Xfce's **Displays** dialog, and for a logged-in desktop that works. Two caveats. Its saved layout is per-user session configuration, so it cannot do anything for the login screen. And this panel publishes no EDID, reporting as `0mm x 0mm` with a generated output name, so there is little stable identity for a saved profile to match against. If you do save a layout in Displays, expect it to compete with the script at session start; pick one.

## Headless use with a remote desktop

If you run the box without a monitor and reach it with RustDesk, VNC or xrdp, you will find that the X screen shrinks to the 480x320 panel the moment HDMI is unplugged, so the remote view is tiny. Force the HDMI connector on with a fixed mode by appending this to the single line in `/boot/firmware/cmdline.txt`:

```
video=HDMI-A-1:1920x1080@60D
```

The trailing `D` keeps the digital output enabled with nothing attached. X then always has a full-size head, the panel-setup script still places it at +480+0, and a real monitor plugged back in negotiates the same mode. An HDMI dummy plug does the same job for a few dollars if you would rather not touch the boot line.

## Wiring assumed (vendor "MHS-3.5inch RPi Display" layout, also Waveshare 3.5 A/C style boards)

| Signal | GPIO |
|---|---|
| panel SPI | spi0.0 (CE0) |
| panel DC | GPIO24 |
| panel RESET | GPIO25 |
| touch SPI | spi0.1 (CE1) |
| touch PENIRQ | GPIO17 |

Different board, different pins: edit `mhs35-drm-overlay.dts` (`dc-gpios`, `reset-gpios`, `interrupts`/`pendown-gpio`) before running the script.

## Why an override driver

The in-tree `drivers/gpu/drm/tiny/ili9486.c` works as a lone display but fails as a second screen under X's reverse-PRIME path (panel = GPU screen, vc4 = master). Each failure below was confirmed with the SPI controller's `bytes_tx` counter (`/sys/bus/spi/devices/spi0.0/statistics/bytes_tx`) and ftrace/kprobes, and each fix is a small block in `ili9486.c`:

1. **Framebuffer size.** The in-tree driver only accepts framebuffers of exactly 480x320. X's sink pixmap for a slave output is larger. The override raises `max_width/max_height` and clips the enable-time flush to the visible source rectangle.
2. **"randr: failed to create shared pixmap".** X mmaps the imported pixmap through the sink device with `MODE_MAP_DUMB`; `drm_gem_dumb_map_offset()` refuses imported objects. The override hands out the offset for imported objects and defers their mmap to the exporter.
3. **Silent `-EINVAL` on PRIME import.** vc4 buffers on the Pi 5 are scattered (shmem behind the HVS IOMMU, 38 segments for a 622 KB buffer); the DMA-GEM import wants contiguous memory. Also, the SPI slave device has no DMA mask, so the attachment fails even earlier. The override sets a 64-bit DMA mask on the SPI device and imports by vmapping the scattered pages (the imported buffer is only ever CPU-read).
4. **Synchronous SPI inside the atomic commit.** The core `mipi_dbi_pipe_update` converts and transmits the frame while X waits in the DIRTYFB ioctl: 77 ms at 32 MHz, 25.8 ms at 100 MHz, on every panel update, and the cursor stutters on both screens. The override converts the damage into one of two panel-sized RGB565 buffers and queues an ordered high-priority worker that does the SPI transfer; commits that arrive during a transfer are coalesced (union rect, re-read from the framebuffer, which always holds the full current screen). Measured: DIRTYFB 1.0 to 1.3 ms, SPI frame 24.8 ms on the worker, about 30% of commits coalesced.

## Why those X options

- `AccelMethod none` on the panel: with glamor on the sink, modesetting uses a vblank-driven offload copy. The SPI panel never raises a vblank, so the panel froze on its first frame.
- `PageFlip false`: randr otherwise uses shared-pixmap flipping, also vblank-driven. Off, it falls back to a single shared pixmap with dirty tracking and DIRTYFB copies, which is what the driver needs.
- `SWCursor true`: the SPI pipe has no cursor plane; without this the panel shows no cursor.
- `PrimaryGPU true` on vc4: makes HDMI the master so it keeps acceleration.

## Traps

- **The initramfs carries the stock module.** On Pi OS/Kali with `auto_initramfs=1`, the initramfs contains the stock `ili9486.ko.xz`, which probes before your `updates/` copy exists. Symptoms: panel "connected" with no mode, "failed to create shared pixmap", touch not mapped. Run `update-initramfs -u -k $(uname -r)` after every build of the override, and check with `lsinitramfs /boot/firmware/initramfs_2712 | grep ili9486` that the `updates/` path is listed. The script does this.
- **Kernel upgrades** drop the override (new module directory). Re-run the script's build step (`setup-mhs35-drm.sh --rebuild`) after installing the matching `linux-headers`.
- **DRM card numbers move between boots.** Never hard-code `card1`; match by driver name in X (`MatchDriver`) and glob `card*-SPI-1` in scripts.
- **Any fbdev X configuration must go.** The DRM driver also exposes an emulated `/dev/fb1`; pointing the X fbdev driver at it fails `FBIOPUT_VSCREENINFO` and lightdm hits its start limit.
- **libinput hovers on resistive touch** (jitter cancels GTK clicks). Use `xserver-xorg-input-evdev` with raw calibration. Measure your own range with `evtest` or `xinput_calibrator`; the values in `99-calibration.conf` are from one unit.
- **Touch spans both screens** unless mapped: `xinput --map-to-output "ADS7846 Touchscreen" Unknown19-1` at every login (the autostart file does it).
- **Testing with `xrandr --output Unknown19-1 --off` then `--auto --pos 0x0`** leaves HDMI at +0+0, overlapping. Restore with `xrandr --output Unknown19-1 --primary --pos 0x0 --output HDMI-1 --pos 480x0`.
- **SPI clock.** RP1's SPI divides 200 MHz, so 100 MHz is the real ceiling (vendor fbtft "125 MHz"/"115 MHz" settings run at 100 MHz too). 100 MHz worked on this unit with the vendor ribbon; drop to 62.5 or 50 MHz if you see noise.

## Measuring instead of guessing

```
B=/sys/bus/spi/devices/spi0.0/statistics/bytes_tx
b0=$(cat $B); xrefresh; sleep 2; echo $(( $(cat $B) - b0 ))     # should be about 307200 per full frame
cat /sys/bus/spi/devices/spi0.0/mhs35_stats                     # queued / sent / coalesced / copy and tx times
# DIRTYFB cost seen by X:
cd /sys/kernel/tracing; echo drm_mode_dirtyfb_ioctl > set_ftrace_filter; echo function_graph > current_tracer
xdotool mousemove 100 100 mousemove 300 200; sleep 1; grep dirtyfb trace | head; echo nop > current_tracer
```

## Rollback

Delete `/lib/modules/$(uname -r)/updates/ili9486.ko`, run `depmod -a` and `update-initramfs -u -k $(uname -r)`, and remove or comment the `dtoverlay=mhs35-drm` line in `config.txt` (the script keeps `config.txt.bak-<date>`).

## Credits and licence

In-tree driver by Kamlesh Gurudasani (GPL-2.0+); this override keeps that licence. Device tree pins from the vendor `mhs35` fbtft overlay. The reverse-PRIME debugging leaned on the SPI statistics counters and ftrace; nothing here needed a kernel rebuild. Use it, change it, share it onward.
