#!/bin/bash
# Halo: Combat Evolved for Batocera on x86-64 PCs: the decompilation's
# 32-bit x86 Linux build, with this port's additions (Bot Match, the spawn
# menu). Refer to port/knulli/README.md, "Batocera PCs".
#
# This script goes in /userdata/roms/ports as "Halo CE (PC build).sh", the
# game in the halo-pc folder beside it. Put an Xbox disc image of the game
# (.iso) in that folder: the first start copies its maps folder out (a few
# minutes, with the screen black), then the image can be deleted. The log of
# each start is halo-pc/log.txt; the settings are halo-pc/config.toml.
#
# Hold the hotkey and push START to quit (the .keys file beside this one).

GAMEDIR="$(cd "$(dirname "$0")/halo-pc" && pwd)" || exit 1
LAUNCHER="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$GAMEDIR" || exit 1
# one start at a time, before the log starts again: another meanwhile would
# start a game while this one's update replaces its files. The game inherits
# the lock, which is held as long as it runs (and the launcher started again
# after an update holds it already, on the same descriptor).
[ -n "${HALO_UPDATED:-}" ] || exec 9> /var/run/halo-pc-lock
if ! flock -n 9 2> /dev/null; then
	echo "$(date): Halo is running already; this start was refused" >> log.txt
	exit 1
fi
# the log afresh, written in append mode so that a refused start's line is
# kept (after an update, the new launcher's lines follow the updater's)
[ -n "${HALO_UPDATED:-}" ] || : > log.txt
exec >> log.txt 2>&1
echo "Halo for Batocera PCs, $(date)"

# the maps: copied out of a disc image in this folder
if [ ! -s maps/ui.map ]; then
	shopt -s nullglob
	images=(./*.{iso,ISO,xiso,XISO})
	shopt -u nullglob
	if [ "${#images[@]}" -eq 0 ]; then
		echo "no maps folder and no disc image (.iso) in $GAMEDIR"
		exit 1
	fi
	echo "copying the maps folder out of ${images[0]}"
	python3 halo_extract.py "${images[0]}" "$GAMEDIR" || exit 1
fi

# a newer release installed before the game starts (halo_update.py: unasked,
# as nothing can be shown here before the game's window; the maps, saves and
# settings stay; update.auto = false in config.toml keeps this version).
# Once one is installed, the new launcher starts instead, and after an
# install that stopped part way (the files a mix) no game starts.
if [ -z "${HALO_UPDATED:-}" ]; then
	python3 halo_update.py "$GAMEDIR" "$LAUNCHER" batocera-pc
	case $? in
	10)
		export HALO_UPDATED=1
		exec bash "$LAUNCHER"
		;;
	11)
		exit 1
		;;
	esac
fi
if [ -e update.journal ]; then
	echo "the last update stopped part way: start Halo again with an internet connection to finish it"
	exit 1
fi

# the sound through PipeWire: the 32-bit client finds its 32-bit plugins and
# modules (it looks in the 64-bit folders otherwise; 32-bit PulseAudio is
# broken on Batocera 43.1: 64-bit libraries in /lib32)
export SPA_PLUGIN_DIR=/lib32/spa-0.2 PIPEWIRE_MODULE_DIR=/lib32/pipewire-0.3
exec ./halo
