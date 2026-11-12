#!/bin/sh
#
# system-uninstall.sh - remove what system-install.sh installed
#
# Run as root, directly or through `make uninstall`:
#
#	scripts/system-uninstall.sh [--purge]
#	make uninstall [PURGE=1]
#
# Stops and disables lavapool.service and lavarnd-trickle.service and removes
# the daemon, libraries, Python package and units.  The config directory
# ($CFGDIR, default /etc/LavaRnd) is kept unless --purge is given.  PREFIX,
# CFGDIR and UNITDIR must match the values used to install.

set -eu

purge=0
case "${1:-}" in
    --purge) purge=1 ;;
    "") ;;
    *) echo "usage: $0 [--purge]" 1>&2; exit 2 ;;
esac

PREFIX=${PREFIX:-/usr/local}
CFGDIR=${CFGDIR:-/etc/LavaRnd}
UNITDIR=${UNITDIR:-/etc/systemd/system}

if [ "$(id -u)" -ne 0 ]; then
    echo "$0: must be run as root" 1>&2
    exit 1
fi

for unit in lavarnd-trickle lavapool; do
    if [ -e "$UNITDIR/$unit.service" ]; then
	systemctl disable --now "$unit.service" 2>/dev/null || true
	rm -f "$UNITDIR/$unit.service"
	echo "removed $UNITDIR/$unit.service"
    fi
done
systemctl daemon-reload

rm -f "$PREFIX/sbin/lavapool"
rm -rf "$PREFIX/lib/lavarnd"
echo "removed $PREFIX/sbin/lavapool and $PREFIX/lib/lavarnd"

if [ "$purge" -eq 1 ]; then
    rm -rf "$CFGDIR"
    echo "removed $CFGDIR"
elif [ -d "$CFGDIR" ]; then
    echo "kept $CFGDIR (use --purge to remove it)"
fi
