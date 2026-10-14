#!/bin/bash
# Place the SPI panel as an X output, and confine the resistive touch to it.
#
# Run from TWO places on purpose:
#   * lightdm's display-setup-script, as root, when X starts and BEFORE the greeter. Without
#     this the panel is dark at the login screen, because nothing has given it a mode yet.
#   * the user's session autostart, so the layout and touch mapping are re-applied for the
#     session (and after a hotplug, if you wire it to a udev rule).
#
# Why it is needed at all: X binds this panel as a reverse-PRIME output sink but leaves it
# with NO mode. It reads "connected" in xrandr and stays black forever until something places
# it explicitly. Nothing in X does that for you.
#
# Nothing here hard-codes a card number or an output name: DRM minor numbers move between
# boots, and X names the panel UnknownNN-1 where NN is not stable.

PANEL_W=480   # must match the panel's width in its current rotation

# The SPI device can probe after X has already started, so wait for the output to appear.
PANEL=""
for _ in $(seq 1 15); do
  PANEL=$(xrandr 2>/dev/null | awk '/^Unknown[0-9]+-[0-9]+ connected/ {print $1; exit}')
  [ -n "$PANEL" ] && break
  sleep 2
done
[ -n "$PANEL" ] || exit 0

HDMI=$(xrandr 2>/dev/null | awk '/^HDMI-[0-9]+ connected/ {print $1; exit}')

if [ -n "$HDMI" ]; then
  xrandr --output "$PANEL" --auto --primary --pos 0x0 --output "$HDMI" --auto --pos "${PANEL_W}x0"
else
  xrandr --output "$PANEL" --auto --primary --pos 0x0
fi

# Touch must be mapped to the panel or it spans the whole desktop including the HDMI monitor.
xinput --map-to-output "ADS7846 Touchscreen" "$PANEL" 2>/dev/null

# The panel only receives pixels when something damages the screen; force one full repaint so
# it is not black until the first window opens.
command -v xrefresh >/dev/null 2>&1 && xrefresh

exit 0
