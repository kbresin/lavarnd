#!/bin/sh
#
# user-install.sh - install lavapool as a systemd user service, no root needed
#
# Run from the top of a built source tree (after `make all CC_WARN=-Wall`):
#
#	scripts/user-install.sh [camtype] [videodev]
#
# camtype defaults to pwc740 and videodev to /dev/video0.  The list of
# camera types is printed by: LD_LIBRARY_PATH=lib/shared tool/camset list all
#
# Installs:
#
#	$PREFIX/bin/lavapool			the daemon
#	$PREFIX/lib/lavarnd/*.so		LavaRnd shared libraries
#	$XDG_CONFIG_HOME/LavaRnd/cfg.lavapool	daemon config (kept if present)
#	$XDG_CONFIG_HOME/systemd/user/lavapool.service
#
# PREFIX defaults to $HOME/.local and XDG_CONFIG_HOME to $HOME/.config.
#
# The user must be able to open the camera (usually: be in the video group).
# lavapool listens on 127.0.0.1:23209, which is also where LavaRnd clients
# look when no /etc/LavaRnd/cfg.random exists.
#
# Afterwards:
#
#	systemctl --user enable --now lavapool.service
#	journalctl _SYSTEMD_USER_UNIT=lavapool.service	(logs)
#
# To keep the service running while logged out: loginctl enable-linger

set -eu

camtype=${1:-pwc740}
videodev=${2:-/dev/video0}
PREFIX=${PREFIX:-$HOME/.local}
XDG_CONFIG_HOME=${XDG_CONFIG_HOME:-$HOME/.config}

bindir=$PREFIX/bin
libdir=$PREFIX/lib/lavarnd
cfgdir=$XDG_CONFIG_HOME/LavaRnd
unitdir=$XDG_CONFIG_HOME/systemd/user

if [ ! -x daemon/lavapool ] || [ ! -d lib/shared ]; then
    echo "$0: run from the top of a built source tree (make all first)" 1>&2
    exit 1
fi

# check that the camera type is one the library knows
if ! LD_LIBRARY_PATH=lib/shared tool/camset list all 2>&1 |
	awk '{print $1}' | grep -qx "$camtype"; then
    echo "$0: unknown camera type: $camtype" 1>&2
    LD_LIBRARY_PATH=lib/shared tool/camset list all 1>&2
    exit 2
fi

mkdir -p "$bindir" "$libdir" "$cfgdir" "$unitdir"

# libraries and daemon
for lib in lib/shared/*.so; do
    install -m 0755 "$lib" "$libdir/"
done
install -m 0755 daemon/lavapool "$bindir/lavapool"
echo "installed lavapool in $bindir and libraries in $libdir"

# daemon config: never overwrite one the user may have edited
if [ -e "$cfgdir/cfg.lavapool" ]; then
    echo "keeping existing $cfgdir/cfg.lavapool"
else
    sed -e "s|INSERT_CAMTYPE_HERE|$camtype|" \
	-e "s|INSERT_VIDEODEV_HERE|$videodev|" \
	daemon/cfg.lavapool > "$cfgdir/cfg.lavapool"
    chmod 0644 "$cfgdir/cfg.lavapool"
    echo "wrote $cfgdir/cfg.lavapool ($camtype on $videodev)"
fi

# systemd user unit
sed -e "s|@BINDIR@|$bindir|g" \
    -e "s|@LIBDIR@|$libdir|g" \
    -e "s|@CFGDIR@|$cfgdir|g" \
    systemd/lavapool.service.in > "$unitdir/lavapool.service"
chmod 0644 "$unitdir/lavapool.service"
echo "wrote $unitdir/lavapool.service"

systemctl --user daemon-reload

# The user systemd manager keeps the groups it had when it started.  If the
# user joined the video group later (common with loginctl enable-linger, where
# the manager runs for a long time), services cannot open the camera even
# though a login shell can.
if ! systemd-run --user --wait --pipe --quiet test -r "$videodev" 2>/dev/null; then
    echo
    echo "WARNING: your systemd user manager cannot read $videodev." 1>&2
    echo "It was probably started before you joined the device's group." 1>&2
    echo "Restart it once (this restarts all your user services):" 1>&2
    echo "    sudo systemctl restart user@$(id -u).service" 1>&2
fi

echo
echo "start it with:  systemctl --user enable --now lavapool.service"
