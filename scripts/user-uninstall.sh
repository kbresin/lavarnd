#!/bin/sh
#
# user-uninstall.sh - remove what user-install.sh installed
#
#	scripts/user-uninstall.sh [--purge]
#
# Stops and disables the lavapool and trickle user services and removes the
# daemon, libraries, Python package and units.  The config directory
# ($XDG_CONFIG_HOME/LavaRnd) is kept unless --purge is given.  PREFIX and
# XDG_CONFIG_HOME must match the values used to install.

set -eu

purge=0
case "${1:-}" in
    --purge) purge=1 ;;
    "") ;;
    *) echo "usage: $0 [--purge]" 1>&2; exit 2 ;;
esac

PREFIX=${PREFIX:-$HOME/.local}
XDG_CONFIG_HOME=${XDG_CONFIG_HOME:-$HOME/.config}
cfgdir=$XDG_CONFIG_HOME/LavaRnd
unitdir=$XDG_CONFIG_HOME/systemd/user

for unit in trickle lavapool; do
    if [ -e "$unitdir/$unit.service" ]; then
	systemctl --user disable --now "$unit.service" 2>/dev/null || true
	rm -f "$unitdir/$unit.service"
	echo "removed $unitdir/$unit.service"
    fi
done
systemctl --user daemon-reload

rm -f "$PREFIX/bin/lavapool"
rm -rf "$PREFIX/lib/lavarnd"
echo "removed $PREFIX/bin/lavapool and $PREFIX/lib/lavarnd"

if [ "$purge" -eq 1 ]; then
    rm -rf "$cfgdir"
    echo "removed $cfgdir"
elif [ -d "$cfgdir" ]; then
    echo "kept $cfgdir (use --purge to remove it)"
fi
