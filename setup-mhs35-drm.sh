#!/bin/bash
# setup-mhs35-drm.sh: MHS-3.5" (ILI9486 + ADS7846) SPI panel as a KMS display next to HDMI on a Raspberry Pi 5.
# Run on the Pi as root from the directory holding the gist files. Idempotent: re-run after a kernel upgrade
# with --rebuild to rebuild and reinstall only the module + initramfs.
set -euo pipefail
# Never let apt stop on a config prompt during an unattended run.
export DEBIAN_FRONTEND=noninteractive
HERE=$(cd "$(dirname "$0")" && pwd)
BOOT=/boot/firmware
CFG=$BOOT/config.txt
KVER=$(uname -r)
XDIR=/etc/X11/xorg.conf.d
STAMP=$(date +%Y%m%d-%H%M)

[ "$(id -u)" = 0 ] || { echo "run as root (sudo $0)"; exit 1; }
[ -f "$CFG" ] || { echo "$CFG not found; this script expects Raspberry Pi OS / Kali layout"; exit 1; }
for f in ili9486.c Makefile mhs35-drm-overlay.dts 98-ili9486-panel.conf 99-vc4.conf 99-calibration.conf mhs35-panel-setup.sh mhs35-panel-setup.desktop; do
  [ -f "$HERE/$f" ] || { echo "missing $HERE/$f"; exit 1; }
done

ask() { # ask VAR "prompt" default
  local var=$1 prompt=$2 def=$3 ans
  read -r -p "$prompt [$def]: " ans </dev/tty || true
  printf -v "$var" '%s' "${ans:-$def}"
}

build_module() {
  echo "== building the ili9486 override for $KVER"
  if [ ! -d /lib/modules/$KVER/build ]; then
    apt-get install -y "linux-headers-$KVER" build-essential
  fi
  local src=/usr/local/src/mhs35-ili9486
  mkdir -p $src && cp "$HERE/ili9486.c" "$HERE/Makefile" $src/
  make -C /lib/modules/$KVER/build M=$src modules
  install -D -m 644 $src/ili9486.ko /lib/modules/$KVER/updates/ili9486.ko
  depmod -a
  echo "== regenerating the initramfs (the stock module in it would otherwise win on boot)"
  update-initramfs -u -k "$KVER"
  # A Pi 5 ships two initramfs images (initramfs_2712 and initramfs8). lsinitramfs takes ONE
  # file, so globbing them together makes the check fail even when the module is present.
  local found=""
  for irfs in $BOOT/initramfs_2712 $BOOT/initramfs8 /boot/initrd.img-$KVER; do
    [ -f "$irfs" ] || continue
    # NOTE: do not use `grep -q` here. Under `set -o pipefail` it exits on the first match,
    # SIGPIPEs lsinitramfs, and the pipeline then reports failure exactly when the match
    # succeeded - a test that can only ever say "no". Count instead, which drains the stream.
    n=$(lsinitramfs "$irfs" 2>/dev/null | grep -c "updates/ili9486.ko" || true)
    if [ "${n:-0}" -gt 0 ]; then found="$irfs"; break; fi
  done
  if [ -n "$found" ]; then
    echo "   $found carries updates/ili9486.ko: OK"
  else
    echo "   WARNING: no initramfs lists updates/ili9486.ko; if the panel shows no frames after boot, check auto_initramfs / initramfs path"
  fi
  echo "   module srcversion: $(modinfo -F srcversion /lib/modules/$KVER/updates/ili9486.ko)"
}

if [ "${1:-}" = "--rebuild" ]; then build_module; echo "done; reboot to load it cleanly"; exit 0; fi

echo "MHS-3.5 SPI panel as a KMS display (Pi 5). Answer the prompts; defaults in brackets."
# The desktop user is NOT reliably uid 1000: if an admin/provisioning account was created
# first it takes 1000 and the distro's own desktop user lands on 1001. Prefer what lightdm is
# actually configured to log in, and only then fall back to the lowest regular uid.
default_desktop_user() {
  local u
  u=$(grep -rhs '^autologin-user=' /etc/lightdm 2>/dev/null | head -1 | cut -d= -f2)
  if [ -n "$u" ]; then printf '%s' "$u"; return; fi
  getent passwd | awk -F: '$3>=1000 && $3<65000 && $7 !~ /(nologin|false)$/ {print $3":"$1}'     | sort -n | head -1 | cut -d: -f2
}
ask DESKTOP_USER "Desktop user (the one lightdm logs in, owns the Xfce session)" "$(default_desktop_user)"
id "$DESKTOP_USER" >/dev/null 2>&1 || { echo "no such user $DESKTOP_USER"; exit 1; }
ask ROTATION "Panel rotation 0/90/180/270 (180 = ribbon at the top on the vendor MHS board, matching fbtft rotate=90)" "180"
ask SPI_HZ "Panel SPI clock in Hz (100000000 is the RP1 ceiling; use 62500000 or 50000000 if you see noise)" "100000000"
# Default OFF. Autologin means anyone who powers the device on lands on a desktop. The panel
# still works at the login screen, because the layout runs from lightdm display-setup-script.
ask AUTOLOGIN "Enable lightdm autologin for $DESKTOP_USER? (y/n, n = ask for a login)" "n"
ask XFCE_BAR "Install the Xfce bar-follows-HDMI helper? (y/n; needs xfconf-query)" "y"
ask DO_CAL "Install the evdev touch calibration from this gist (measured on one unit; re-calibrate later with xinput_calibrator)? (y/n)" "y"

echo "== packages"
apt-get update
apt-get install -y build-essential device-tree-compiler "linux-headers-$KVER" xserver-xorg-input-evdev xinput xdotool

echo "== overlay"
dtc -@ -I dts -O dtb -o $BOOT/overlays/mhs35-drm.dtbo "$HERE/mhs35-drm-overlay.dts" 2>&1 | grep -v "Warning (unit_address" || true
[ -f $BOOT/overlays/mhs35-drm.dtbo ] || { echo "overlay compile failed"; exit 1; }

echo "== $CFG (backup $CFG.bak-$STAMP)"
cp -n "$CFG" "$CFG.bak-$STAMP"
# disable any fbtft panel overlays (vendor LCD-show installs these)
sed -i -E 's/^(dtoverlay=(mhs35|tft35a|waveshare35|piscreen|rpi-display)[^#]*)$/#\1  # disabled by setup-mhs35-drm/' "$CFG"
sed -i -E '/^dtoverlay=mhs35-drm/d' "$CFG"
grep -q '^dtparam=spi=on' "$CFG" || echo 'dtparam=spi=on' >> "$CFG"
grep -q '^dtparam=i2c_arm=on' "$CFG" || echo 'dtparam=i2c_arm=on' >> "$CFG"
grep -q '^dtoverlay=vc4-kms-v3d' "$CFG" || echo 'dtoverlay=vc4-kms-v3d' >> "$CFG"
echo "dtoverlay=mhs35-drm,speed=$SPI_HZ,rotation=$ROTATION" >> "$CFG"
# the vendor LCD-show script also forces HDMI settings; leave them, they do not conflict with KMS

build_module

echo "== X configuration"
mkdir -p $XDIR
install -m 644 "$HERE/98-ili9486-panel.conf" $XDIR/98-ili9486-panel.conf
install -m 644 "$HERE/99-vc4.conf" $XDIR/99-vc4.conf
if [ "$DO_CAL" = y ]; then install -m 644 "$HERE/99-calibration.conf" $XDIR/99-calibration.conf; fi
# any fbdev conf from LCD-show must go: the DRM driver also exposes an emulated fb and X fbdev on it kills the server
for f in $XDIR/99-fbturbo.conf $XDIR/99-panel-fbdev.conf; do [ -f "$f" ] && mv "$f" "$f.disabled-$STAMP" && echo "   moved $f aside"; done

echo "== panel layout + touch mapping (greeter and session)"
install -m 755 "$HERE/mhs35-panel-setup.sh" /usr/local/bin/mhs35-panel-setup.sh
install -m 644 "$HERE/mhs35-panel-setup.desktop" /etc/xdg/autostart/mhs35-panel-setup.desktop
# X binds the panel but never gives it a mode, so without this the panel is dark at the login
# screen. display-setup-script runs as root when X starts, before the greeter is drawn.
mkdir -p /etc/lightdm/lightdm.conf.d
printf '[Seat:*]
display-setup-script=/usr/local/bin/mhs35-panel-setup.sh
' > /etc/lightdm/lightdm.conf.d/51-mhs35-panel.conf
# drop the older session-only name if upgrading from an earlier copy of this gist
rm -f /usr/local/bin/mhs35-touch-map.sh /etc/xdg/autostart/mhs35-touch-map.desktop

if [ "$XFCE_BAR" = y ] && [ -f "$HERE/mhs35-bar-follow.sh" ]; then
  echo "== Xfce bar follows HDMI"
  sed "s/<DESKTOP_USER>/$DESKTOP_USER/g" "$HERE/mhs35-bar-follow.sh" > /usr/local/bin/mhs35-bar-follow.sh
  chmod 755 /usr/local/bin/mhs35-bar-follow.sh
  install -m 644 "$HERE/99-mhs35-bar-follow.rules" /etc/udev/rules.d/99-mhs35-bar-follow.rules
  udevadm control --reload
fi

if [ -d /etc/lightdm ]; then
  mkdir -p /etc/lightdm/lightdm.conf.d
  if [ "$AUTOLOGIN" = y ]; then
    printf '[Seat:*]
autologin-user=%s
autologin-user-timeout=0
' "$DESKTOP_USER" > /etc/lightdm/lightdm.conf.d/50-mhs35-autologin.conf
    echo "== lightdm autologin ENABLED for $DESKTOP_USER"
  else
    rm -f /etc/lightdm/lightdm.conf.d/50-mhs35-autologin.conf
    echo "== lightdm will ask for a login (no autologin)"
  fi
fi

cat <<EOF

Done. Summary:
  config.txt : dtoverlay=mhs35-drm,speed=$SPI_HZ,rotation=$ROTATION (backup $CFG.bak-$STAMP)
  module     : /lib/modules/$KVER/updates/ili9486.ko (+ initramfs)
  X          : $XDIR/98-ili9486-panel.conf, 99-vc4.conf$( [ "$DO_CAL" = y ] && echo ", 99-calibration.conf")
  layout     : /usr/local/bin/mhs35-panel-setup.sh (lightdm display-setup-script + session autostart)
After reboot check: xrandr (Unknown19-1 primary 480x320+0+0, HDMI-1 +480+0), and
  cat /sys/bus/spi/devices/spi0.0/mhs35_stats   (sent must increase while the panel changes)
After a kernel upgrade: sudo $0 --rebuild
EOF
ask REBOOT "Reboot now? (y/n)" "y"
[ "$REBOOT" = y ] && reboot
exit 0
