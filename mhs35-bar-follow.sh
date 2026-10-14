#!/bin/bash
# Put the Xfce bar on HDMI-1 when an HDMI monitor is connected, else on the primary (the SPI panel).
# Called by udev on DRM hotplug (detached via systemd-run so udev never waits) and at session start by mhs35-touch-map.sh.
if [ "$1" != "--now" ]; then systemd-run --quiet --on-active=4 /usr/local/bin/mhs35-bar-follow.sh --now; exit 0; fi
hdmi=$(cat /sys/class/drm/card*-HDMI-A-*/status 2>/dev/null | grep -c '^connected' || true)
out=Primary; [ "${hdmi:-0}" -gt 0 ] && out=HDMI-1
uid=$(id -u <DESKTOP_USER>)
cur=$(sudo -u <DESKTOP_USER> env DISPLAY=:0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus xfconf-query -c xfce4-panel -p /panels/panel-1/output-name 2>/dev/null || echo none)
[ "$cur" = "$out" ] && exit 0
sudo -u <DESKTOP_USER> env DISPLAY=:0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus xfconf-query -c xfce4-panel -p /panels/panel-1/output-name -n -t string -s "$out"
logger -t mhs35-bar-follow "hdmi=$hdmi -> bar on $out (was $cur)"
