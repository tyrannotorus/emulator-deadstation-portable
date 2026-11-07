#!/bin/sh
# Deadstation Portable for NextUI on the TrimUI Brick (tg5040). NextUI passes the game picked in Roms: an .iso, .cso,
# .chd or .pbp.
PAK_DIR="$(cd "$(dirname "$0")" && pwd)"
PAK_NAME="$(basename "$PAK_DIR")"
PAK_NAME="${PAK_NAME%.*}"

rm -f "$LOGS_PATH/$PAK_NAME.txt"
exec >>"$LOGS_PATH/$PAK_NAME.txt" 2>&1
echo "$0" "$@"
cd "$PAK_DIR" || exit 1

ROM="$1"
if [ ! -f "$ROM" ]; then
	echo "Deadstation Portable: no game at $ROM"
	exit 1
fi

# Settings, saves and save states are kept in DEADSTATION_PORTABLE_HOME.
export DEADSTATION_PORTABLE_HOME="$USERDATA_PATH/$PAK_NAME"
export HOME="$DEADSTATION_PORTABLE_HOME"
mkdir -p "$DEADSTATION_PORTABLE_HOME"

# For the menu: the game's name
export DEADSTATION_PORTABLE_ROM="$ROM"

cleanup() {
	rm -f /tmp/stay_awake
	[ -n "$CONSOLE_LEVEL" ] && echo "$CONSOLE_LEVEL" >/proc/sys/kernel/printk
	[ -n "$GPU_IRQ" ] && echo f >"/proc/irq/$GPU_IRQ/smp_affinity"
	[ -n "$DISPLAY_IRQ" ] && echo f >"/proc/irq/$DISPLAY_IRQ/smp_affinity"
	sync
}
trap cleanup EXIT INT TERM HUP QUIT

# no auto sleep while playing
echo 1 >/tmp/stay_awake
# The kernel's messages off the serial console while playing (dmesg still has them): they hold up the display's flip,
# and a weak Wi-Fi link's reconnects stalled frames up to 200 ms
CONSOLE_LEVEL="$(cut -f1 /proc/sys/kernel/printk)"
echo 1 >/proc/sys/kernel/printk || echo "Deadstation Portable: the kernel's console is still on: frames can stall on its messages"
# The GPU's interrupts onto the last core: up to 1500 a second, all on the first one otherwise, whichever thread's running there
GPU_IRQ="$(grep pvrsrvkm /proc/interrupts | cut -d: -f1 | tr -d ' ')"
{ [ -n "$GPU_IRQ" ] && echo 8 >"/proc/irq/$GPU_IRQ/smp_affinity"; } || echo "Deadstation Portable: the GPU's interrupts are still on the first core"
# and the display's: Wi-Fi's are on the first core, and a weak link losing its access point held every flip up 40 ms for most of a minute
DISPLAY_IRQ="$(grep dispaly /proc/interrupts | cut -d: -f1 | tr -d ' ')"
{ [ -n "$DISPLAY_IRQ" ] && echo 8 >"/proc/irq/$DISPLAY_IRQ/smp_affinity"; } || echo "Deadstation Portable: the display's interrupts are still on the first core"
# NextUI starts paks at its performance clock; run at its Auto, as its own emulators do
sh "$SYSTEM_PATH/bin/governor.sh" auto
# the emulator raises the floor of that range while a game runs behind (core/deadstation/clock.cpp): back where it was, should it have
# been killed with it raised
CPU_MIN="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq)"

# exit 86: the game's other core chosen in Options (Emulator or its recomp module): started again,
# the emulator hosting the module when that is the core (ui/src/host.h); a few times at most
for run in 1 2 3 4 5; do
	"$PAK_DIR/deadstation-portable" "$ROM"
	code=$?
	echo "Deadstation Portable: it exited with $code"
	[ "$code" = 86 ] || break
done
[ -n "$CPU_MIN" ] && echo "$CPU_MIN" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
# NextUI's display mode again, should it have been killed before restoring it
command -v fbset > /dev/null && fbset -g 1024 768 1024 16384 32
# and the panel's own rate (core/deadstation/panel.cpp)
"$PAK_DIR/deadstation-portable" --restore-panel
