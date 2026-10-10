#!/bin/bash
# port/darkosen/Halo.sh -> /roms2/ports/Halo.sh
# Halo: Combat Evolved for dArkOSen (RK3326, Mali-G31).
# The game folder is ./halo next to this script (/roms2/ports/halo).
# First-build launcher: safe defaults, override any HALO_* variable from the environment.

GAMEDIR="$(cd "$(dirname "$0")/halo" && pwd)" || exit 1
cd "$GAMEDIR" || exit 1

LOCK=/dev/shm/halo-lock
SAVED=/dev/shm/halo-clocks
CPU=/sys/devices/system/cpu/cpufreq/policy0/scaling_governor
GPU=""
for g in /sys/class/devfreq/*.gpu; do
	[ -d "$g" ] && GPU="$g" && break
done

# one start at a time; fd 9 is inherited by the game, so the lock lives as long as it runs
exec 9> "$LOCK" || { echo "$(date): cannot open $LOCK" >> "$GAMEDIR/log.txt"; exit 1; }
if ! flock -n 9; then
	echo "$(date): Halo is running already; this start was refused" >> "$GAMEDIR/log.txt"
	exit 1
fi
exec > "$GAMEDIR/log.txt" 2>&1

kv() { printf '%-18s %s\n' "$1" "$2"; }

# root writes for sysfs: sudo without a password, or nothing
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
	if sudo -n true 2> /dev/null; then SUDO="sudo -n"; else SUDO="none"; fi
fi
put() {
	[ "$SUDO" = none ] && return 1
	echo "$1" | $SUDO tee "$2" > /dev/null 2>&1
}

# the GPU is held at GPU_MAX_HZ or the highest step under it: 600 MHz hangs
# the GPU under load on these boards (kernel hard-stop and reset, ~8.7 s)
GPU_MAX_HZ="${HALO_GPU_MAX_HZ:-520000000}"

restore() {
	if [ -f "$SAVED" ]; then
		read -r saved_governor saved_gpu_min saved_gpu_max < "$SAVED"
		[ -n "$saved_governor" ] && [ "$saved_governor" != none ] && put "$saved_governor" "$CPU"
		if [ -n "$GPU" ]; then
			low=$(tr ' ' '\n' < "$GPU/available_frequencies" 2> /dev/null | grep . | sort -n | head -n 1)
			[ -n "$low" ] && put "$low" "$GPU/min_freq"
			[ -n "$saved_gpu_max" ] && [ "$saved_gpu_max" != none ] && put "$saved_gpu_max" "$GPU/max_freq"
			[ -n "$saved_gpu_min" ] && [ "$saved_gpu_min" != none ] && put "$saved_gpu_min" "$GPU/min_freq"
		fi
		rm -f "$SAVED"
	fi
}
restore
echo "$(cat "$CPU" 2> /dev/null || echo none) $(cat "$GPU/min_freq" 2> /dev/null || echo none) $(cat "$GPU/max_freq" 2> /dev/null || echo none)" > "$SAVED"
trap restore EXIT

put performance "$CPU"
if [ -n "$GPU" ]; then
	steps=$(tr ' ' '\n' < "$GPU/available_frequencies" 2> /dev/null | grep . | sort -n)
	target=$(echo "$steps" | awk -v max="$GPU_MAX_HZ" '$1 <= max { t = $1 } END { print t }')
	[ -z "$target" ] && target=$(echo "$steps" | head -n 1)
	if [ -n "$target" ]; then
		put "$target" "$GPU/min_freq"
		put "$target" "$GPU/max_freq"
		put "$target" "$GPU/min_freq"
	fi
fi

export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-kmsdrm}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
# safe first run: turn these on one at a time (HALO_GL_THREAD=1 ./Halo.sh)
export HALO_GL_THREAD="${HALO_GL_THREAD:-1}"
export HALO_ASYNC_PROGRAMS="${HALO_ASYNC_PROGRAMS:-1}"

echo "Halo for dArkOSen, $(date)"
kv "device"           "$(cat /home/ark/.config/.DEVICE 2> /dev/null)"
kv "kernel"           "$(uname -sr)"
kv "sdl2"             "$(readlink -f /usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0 2> /dev/null)"
kv "mem available"    "$(grep MemAvailable /proc/meminfo | tr -s ' ' | cut -d' ' -f2-)"
kv "fb0 modes"        "$(cat /sys/class/graphics/fb0/modes 2> /dev/null)"
kv "root helper"      "${SUDO:-root}"
kv "cpu governor"     "$(cat "$CPU" 2> /dev/null)"
kv "gpu devfreq"      "${GPU:-none}"
kv "gpu min/max/cur"  "$(cat "$GPU/min_freq" 2> /dev/null) / $(cat "$GPU/max_freq" 2> /dev/null) / $(cat "$GPU/cur_freq" 2> /dev/null)"
kv "SDL_VIDEODRIVER"  "$SDL_VIDEODRIVER"
kv "SDL_AUDIODRIVER"  "$SDL_AUDIODRIVER"
kv "HALO_GL_THREAD"   "$HALO_GL_THREAD"
kv "HALO_ASYNC_PROGS" "$HALO_ASYNC_PROGRAMS"

if [ ! -s maps/ui.map ]; then
	echo "no maps/ui.map in $GAMEDIR"
	exit 1
fi

# first start only: settings for this handheld (the game keeps and extends config.toml)
if [ ! -f config.toml ]; then
	if ! { cp config.default.toml config.toml.new &&
		sed -i 's/^render_scale[[:space:]]*=.*/render_scale = 0.5/' config.toml.new &&
		mv -f config.toml.new config.toml; }; then
		rm -f config.toml.new
		echo "cannot write config.toml"
		exit 1
	fi
fi

# the handheld's own controls, from EmulationStation's config
if [ -f sdl_mapping.py ] && command -v python3 > /dev/null; then
	SDL_GAMECONTROLLERCONFIG="$(python3 sdl_mapping.py)"
	export SDL_GAMECONTROLLERCONFIG
fi

./halo &
child=$!
trap 'kill -TERM "$child" 2> /dev/null' TERM INT HUP
while :; do
	wait "$child"
	status=$?
	kill -0 "$child" 2> /dev/null || break
done
trap - TERM INT HUP
echo "exit status $status"
[ "$status" -eq 137 ] && echo "killed, most likely out of memory: set high_res_text = false and high_res_hud = false in config.toml"
exit "$status"