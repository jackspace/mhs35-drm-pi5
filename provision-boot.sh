#!/usr/bin/env bash
# Headless first-boot provisioning for a cloud-init (NoCloud) Raspberry Pi image, e.g. the
# Kali Raspberry Pi image. Run on the imaging host AFTER flashing, with the card's BOOT
# partition mounted. It sets up Wi-Fi, an SSH key and a login user so the Pi comes up on the
# network with no monitor or keyboard, ready for setup-mhs35-drm.sh over SSH.
#
# IMPORTANT, learned the hard way on Kali:
#
#   Do NOT use cloud-init's `network-config` for Wi-Fi here. cloud-init renders it into
#   /etc/netplan/50-cloud-init.yaml, and on a NetworkManager system that file takes ownership
#   of the interfaces and hands them to a renderer that never brings them up. The symptom is
#   brutal: `nmcli dev status` shows wlan0 "unavailable" and eth0 "unmanaged", the radio is
#   not rfkill-blocked, and no amount of `nmcli dev wifi connect` helps because the device
#   never becomes available. It is worse than doing nothing.
#
#   Instead this writes a NetworkManager connection profile straight to the root filesystem
#   via cloud-init's write_files, which is the native mechanism on Kali and Raspberry Pi OS,
#   and disables cloud-init's network rendering entirely.
#
# On Raspberry Pi OS, prefer the Raspberry Pi Imager "Edit settings" dialog, which does the
# same job through that image's own first-boot mechanism.
#
# Nothing is stored or transmitted; you are prompted for your own values, which are written
# only to the card in front of you. The Wi-Fi passphrase necessarily lands in cleartext on the
# card's boot and root partitions, exactly as it would with any other Wi-Fi configuration.
set -euo pipefail

BOOT="${1:-}"
if [ -z "$BOOT" ] || [ ! -d "$BOOT" ]; then
  echo "usage: $0 <path to the mounted BOOT partition>" >&2
  echo "  Linux: /media/\$USER/BOOT   macOS: /Volumes/BOOT   Windows: the drive letter, e.g. E:/" >&2
  exit 1
fi
[ -f "$BOOT/cmdline.txt" ] || { echo "no cmdline.txt in $BOOT - is that the BOOT partition?" >&2; exit 1; }
grep -q 'ds=nocloud' "$BOOT/cmdline.txt" || {
  echo "warning: cmdline.txt has no 'ds=nocloud'; this image may not use cloud-init," >&2
  echo "         in which case these files will be ignored. Continuing anyway." >&2
}

read -r -p "Wi-Fi SSID: " SSID
read -r -s -p "Wi-Fi password: " PSK; echo
read -r -p "Login user to create [doc]: " LUSER; LUSER="${LUSER:-doc}"
# Keep provisioned accounts out of the distro's own range. useradd hands out 1000, 1001 ... so
# an account created here at 1000 displaces the image's desktop user and breaks anything that
# assumes "uid 1000 is the person at the keyboard" - including this project's installer.
# 1000-60000 is the regular-user range; a fixed number high in it never collides.
read -r -p "uid/gid for that user [20010]: " LUID; LUID="${LUID:-20010}"
read -r -p "Hostname [pi-panel]: " HOST; HOST="${HOST:-pi-panel}"
DEFKEY="$HOME/.ssh/id_ed25519.pub"
read -r -p "Path to your SSH public key [$DEFKEY]: " KEY; KEY="${KEY:-$DEFKEY}"
[ -f "$KEY" ] || { echo "no public key at $KEY" >&2; exit 1; }
PUB="$(cat "$KEY")"

# A leftover network-config is actively harmful (see the note above).
rm -f "$BOOT/network-config"

cat > "$BOOT/user-data" <<EOF
#cloud-config
hostname: $HOST
manage_etc_hosts: true
ssh_pwauth: false

users:
  - default
  - name: $LUSER
    uid: $LUID
    primary_group: $LUSER
    groups: [sudo]
    shell: /bin/bash
    sudo: "ALL=(ALL) NOPASSWD:ALL"
    ssh_authorized_keys:
      - $PUB

write_files:
  # Stop cloud-init rendering netplan over the top of NetworkManager.
  - path: /etc/cloud/cloud.cfg.d/99-disable-network-config.cfg
    permissions: '0644'
    content: |
      network: {config: disabled}
  # The Wi-Fi profile NetworkManager actually reads. Must be 0600 and root-owned or it is ignored.
  - path: /etc/NetworkManager/system-connections/$SSID.nmconnection
    permissions: '0600'
    owner: root:root
    content: |
      [connection]
      id=$SSID
      type=wifi
      interface-name=wlan0
      autoconnect=true
      autoconnect-priority=10

      [wifi]
      mode=infrastructure
      ssid=$SSID

      [wifi-security]
      key-mgmt=wpa-psk
      psk=$PSK

      [ipv4]
      method=auto

      [ipv6]
      method=ignore

runcmd:
  - rfkill unblock all
  - systemctl enable --now ssh
  - nmcli radio wifi on
  - nmcli connection reload
  - nmcli device set wlan0 managed yes
  - nmcli connection up "$SSID"
EOF

# cloud-init skips its first-boot modules when the instance id matches what it already ran.
# Changing it is what makes a re-provisioned card actually re-run this.
if [ -f "$BOOT/meta-data" ]; then
  if grep -q '^instance_id:' "$BOOT/meta-data"; then
    sed -i "s/^instance_id:.*/instance_id: $HOST-$(date +%s)/" "$BOOT/meta-data"
  else
    echo "instance_id: $HOST-$(date +%s)" >> "$BOOT/meta-data"
  fi
else
  printf 'dsmode: local\ninstance_id: %s-%s\n' "$HOST" "$(date +%s)" > "$BOOT/meta-data"
fi

sync
echo
echo "Wrote user-data and meta-data to $BOOT (network-config removed on purpose)."
echo "Eject, boot the Pi, wait a minute for Wi-Fi, then: ssh $LUSER@<pi-ip>"
