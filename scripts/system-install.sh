#!/bin/sh
#
# system-install.sh - install lavapool and trickle as systemd system services
#
# Run as root from the top of a built source tree (after `make all`), either
# directly or through `make install`:
#
#	scripts/system-install.sh [camtype [videodev]]
#	make install [CAMTYPE=camtype] [VIDEODEV=videodev]
#
# camtype is required the first time, when there is no config yet; it is
# ignored afterwards (the existing config is kept).  videodev defaults to
# /dev/video0.  The list of camera types is printed by:
#
#	LD_LIBRARY_PATH=lib/shared tool/camset list all
#
# Installs:
#
#	$PREFIX/sbin/lavapool				the daemon
#	$PREFIX/lib/lavarnd/*.so			LavaRnd shared libraries
#	$PREFIX/lib/lavarnd/python/lavarnd/		Python client and trickle
#	$CFGDIR/cfg.lavapool				daemon config (kept if present)
#	$UNITDIR/lavapool.service			lavapool, unprivileged
#	$UNITDIR/lavarnd-trickle.service		trickle, crediting entropy
#
# PREFIX defaults to /usr/local, CFGDIR to /etc/LavaRnd and UNITDIR to
# /etc/systemd/system.  Nothing is compiled here and no service is started.
#
# Both services run as a throwaway user (systemd DynamicUser).  lavapool gets
# the video group so it can open the camera; trickle gets only CAP_SYS_ADMIN,
# which the RNDADDENTROPY ioctl needs.  scripts/system-uninstall.sh (or
# `make uninstall`) removes all of it.

set -eu

camtype=${1:-}
videodev=${2:-/dev/video0}
PREFIX=${PREFIX:-/usr/local}
CFGDIR=${CFGDIR:-/etc/LavaRnd}
UNITDIR=${UNITDIR:-/etc/systemd/system}

sbindir=$PREFIX/sbin
libdir=$PREFIX/lib/lavarnd
pydir=$libdir/python
port=23209

die() {
    echo "$0: $*" 1>&2
    exit 1
}

[ "$(id -u)" -eq 0 ] || die "must be run as root"
if [ ! -x daemon/lavapool ] || ! ls lib/shared/*.so >/dev/null 2>&1; then
    die "run from the top of a built source tree (make all first)"
fi
python=$(command -v python3) || die "python3 not found (trickle needs it)"

# the camera type is needed only to write a new config
if [ -e "$CFGDIR/cfg.lavapool" ]; then
    [ -z "$camtype" ] ||
	echo "keeping existing $CFGDIR/cfg.lavapool (ignoring camtype $camtype)"
else
    if [ -z "$camtype" ]; then
	echo "$0: no $CFGDIR/cfg.lavapool yet, so a camera type is needed:" 1>&2
	echo "    make install CAMTYPE=<type> [VIDEODEV=/dev/videoN]" 1>&2
	LD_LIBRARY_PATH=lib/shared tool/camset list all 1>&2
	exit 2
    fi
    if ! LD_LIBRARY_PATH=lib/shared tool/camset list all 2>&1 |
	    awk '{print $1}' | grep -qx "$camtype"; then
	echo "$0: unknown camera type: $camtype" 1>&2
	LD_LIBRARY_PATH=lib/shared tool/camset list all 1>&2
	exit 2
    fi
fi

install -d -m 0755 "$sbindir" "$libdir" "$pydir" "$CFGDIR" "$UNITDIR"

# libraries and daemon
for lib in lib/shared/*.so; do
    install -m 0755 "$lib" "$libdir/"
done
install -m 0755 daemon/lavapool "$sbindir/lavapool"
echo "installed $sbindir/lavapool and libraries in $libdir"

# Python package (replaced wholesale, so removed modules don't linger)
rm -rf "$pydir/lavarnd"
install -d -m 0755 "$pydir/lavarnd"
for py in python/src/lavarnd/*.py; do
    install -m 0644 "$py" "$pydir/lavarnd/"
done
echo "installed the lavarnd Python package in $pydir"

# daemon config: never overwrite one the admin may have edited
if [ ! -e "$CFGDIR/cfg.lavapool" ]; then
    sed -e "s|INSERT_CAMTYPE_HERE|$camtype|" \
	-e "s|INSERT_VIDEODEV_HERE|$videodev|" \
	daemon/cfg.lavapool > "$CFGDIR/cfg.lavapool"
    chmod 0644 "$CFGDIR/cfg.lavapool"
    echo "wrote $CFGDIR/cfg.lavapool ($camtype on $videodev)"
fi

# systemd system units
for unit in lavapool lavarnd-trickle; do
    sed -e "s|@SBINDIR@|$sbindir|g" \
	-e "s|@LIBDIR@|$libdir|g" \
	-e "s|@PYDIR@|$pydir|g" \
	-e "s|@CFGDIR@|$CFGDIR|g" \
	-e "s|@PYTHON@|$python|g" \
	"systemd/$unit.system.service.in" > "$UNITDIR/$unit.service"
    chmod 0644 "$UNITDIR/$unit.service"
    echo "wrote $UNITDIR/$unit.service"
done
systemctl daemon-reload

# only one lavapool can hold the port (and the camera)
if ! systemctl -q is-active lavapool.service &&
	ss -ltnH "sport = :$port" 2>/dev/null | grep -q .; then
    echo
    echo "WARNING: something already listens on port $port, probably a" 1>&2
    echo "user-level lavapool (scripts/user-install.sh).  Stop it before" 1>&2
    echo "starting the system service, e.g. scripts/user-uninstall.sh." 1>&2
fi

echo
echo "start it with:  systemctl enable --now lavapool.service"
echo "and optionally: systemctl enable --now lavarnd-trickle.service"
