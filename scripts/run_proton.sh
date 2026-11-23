#!/usr/bin/env bash
# Run conker.exe (or the launcher) through Proton or plain Wine on Linux, then
# copy this run's logs next to the script's output folder and print the lines
# the Linux checks look at (docs/linux.md phase T, r-linux-proton.md).
#
#   scripts/run_proton.sh [options] [-- conker.exe arguments]
#
#   -d DIR     game folder holding conker.exe (default: $CONKER_DIR, else .)
#   -p DIR     Proton folder (default: $PROTON, else the newest "Proton 11*",
#              then "Proton*", in any Steam library: the internal drive's and
#              the ones in steamapps/libraryfolders.vdf, e.g. an SD card)
#   -w         plain Wine ($WINE or wine) instead of Proton
#   -x PREFIX  Wine prefix (default: ~/conker/pfx; Proton: its compatdata folder
#              ~/conker/compatdata, holding pfx/)
#   -e A=B     a switch for the game (repeatable), e.g. -e RECOMP_SOAK=1
#   -s DIR     save folder (--save-dir; default: the game's own userdata\)
#   -l         start conker-launcher.exe instead of conker.exe (and wait for a
#              game started with Play: its log is copied too)
#   -t SEC     stop the game after SEC seconds (default: none)
#   -o DIR     where logs are copied (default: ./linux_logs/<date_time>)
#   -c LOG     only print the check lines of a log, then exit
#   -k         list the switches most Linux tests use, then exit
#
# Linux paths in -s are turned into Wine paths (Z:\...). Our switches that
# hold a path (RECOMP_PAD_LIVE, RECOMP_WATCHDOG_FILE, ...) take Wine paths:
# write them as Z:/home/<you>/... . The game writes its own log under Wine
# (userdata/logs/conker_*.log, P2); PROTON_LOG=1 adds ~/steam-<id>.log.
set -u

GAME_DIR=${CONKER_DIR:-.}
PROTON_DIR=${PROTON:-}
USE_WINE=0
PREFIX=""
SAVE_DIR=""
LAUNCHER=0
TIMEOUT=0
OUT=""
ENVS=()

check_log() {
    # The lines r-linux-proton.md's test plan reads, in one place.
    local log=$1
    echo "== checks: $log"
    grep -E '^\[HOST\]|xbox_MemoryLayoutInit: (mapped|base|RECOMP_MEM_BASE)|RAM mirror|Extended VMA|WARNING: .*failed|no write breakpoint|\[PGRAPH\]|\[VSYNC\] no refresh watcher|\[GPU\] d3d11: (device|adapter|feature)|\[GPU\] d3d11: shader .* failed|draw shader failed|\[CRASH\]|\[XA2\]|\[APU\] (Using|XAudio2|waveOut)' "$log" | head -60
    echo "== fps: $(grep -o '\[GPU\] [0-9.]* fps' "$log" | tail -3 | tr '\n' ' ')"
    echo "== counts: $(grep -E 'failed|FAILED' "$log" | grep -vc '(0 failed\|, 0 failed') lines with 'failed' (not '0 failed'), $(grep -c 'WARNING' "$log") with 'WARNING'"
}

list_switches() {
    cat <<'EOF'
Switches most Linux tests use (-e NAME=VALUE):
  RECOMP_FAST_LOADS=1          quicker loads (test runs use it)
  RECOMP_MASTER_VOLUME=0       muted
  RECOMP_RENDER_RES=640x480    1x picture, for frame comparisons / benches
  RECOMP_FRAME_RATE=60|240     frame rate cap (= F2)
  RECOMP_VSYNC=on|off          (= F2)
  RECOMP_SOAK=1                every map, nobody at the pad (~6 min) - the shader test
  RECOMP_VSH_SCAN=1            vertex program scan lines ([VSHSCAN])
  RECOMP_HITCH_MS=70           [HITCH] per long frame
  RECOMP_VSYNC_LOG=1           refreshes each frame stayed up
  RECOMP_APU_PROFILE=1         audio starvation / clicks
  RECOMP_SAMPLE_PROFILE=20     [PROF] hot functions per thread
  RECOMP_GPU_TIMES=1           [GPUTIME] GPU ms a frame
  RECOMP_MEM_BASE=<hex>        force the guest memory base (R1)
  RECOMP_PGRAPH_NO_DR0=0       the old DR0 write breakpoint instead of the handler override (R2)
  RECOMP_VSYNC_WATCHER=1       the WaitForVBlank watcher even under Wine (R3)
  RECOMP_LOG_STDOUT=1          log to stdout instead of userdata/logs
Proton's own: PROTON_LOG=1, DXVK_HUD=fps,frametimes,compiler, WINEDEBUG=+seh,
  PULSE_LATENCY_MSEC=60 (crackle), PROTON_USE_WINED3D=1 (no DXVK).
EOF
}

while getopts "d:p:wx:e:s:lt:o:c:kh" opt; do
    case $opt in
        d) GAME_DIR=$OPTARG ;;
        p) PROTON_DIR=$OPTARG ;;
        w) USE_WINE=1 ;;
        x) PREFIX=$OPTARG ;;
        e) ENVS+=("$OPTARG") ;;
        s) SAVE_DIR=$OPTARG ;;
        l) LAUNCHER=1 ;;
        t) TIMEOUT=$OPTARG ;;
        o) OUT=$OPTARG ;;
        c) check_log "$OPTARG"; exit 0 ;;
        k) list_switches; exit 0 ;;
        *) sed -n '2,30p' "$0"; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

GAME_DIR=$(realpath "$GAME_DIR")
EXE=conker.exe
[ $LAUNCHER = 1 ] && EXE=conker-launcher.exe
if [ ! -f "$GAME_DIR/$EXE" ]; then
    echo "No $EXE in $GAME_DIR (use -d)"; exit 1
fi

winpath() { echo "Z:$(realpath -m "$1" | sed 's|/|\\|g')"; }

ARGS=()
if [ -n "$SAVE_DIR" ]; then
    mkdir -p "$SAVE_DIR"
    ARGS+=(--save-dir "$(winpath "$SAVE_DIR")")
    LOG_DIR=$(realpath "$SAVE_DIR")/logs
else
    LOG_DIR=$GAME_DIR/userdata/logs
fi
ARGS+=("$@")

# Started over SSH (no display): borrow the logged-in desktop's display and sound
# from the running Steam client (Desktop and Game Mode alike), else from KDE's
# plasmashell. Steam gives SteamDeck=1 only to games it starts: pass -e SteamDeck=1.
if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    SESSION_PID=$(pgrep -u "$(id -u)" -f 'steamrt64/steam( |$)|ubuntu12_32/steam( |$)' | head -1)
    [ -z "$SESSION_PID" ] && SESSION_PID=$(pgrep -u "$(id -u)" -x plasmashell | head -1)
    if [ -n "$SESSION_PID" ]; then
        while IFS= read -r kv; do
            case $kv in
                DISPLAY=*|WAYLAND_DISPLAY=*|XAUTHORITY=*|XDG_RUNTIME_DIR=*|DBUS_SESSION_BUS_ADDRESS=*|PULSE_SERVER=*)
                    export "$kv" ;;
            esac
        done < <(tr '\0' '\n' < "/proc/$SESSION_PID/environ")
        echo "No display here: using the desktop session's (process $SESSION_PID): DISPLAY=${DISPLAY:-} ${WAYLAND_DISPLAY:-}"
    fi
fi

for kv in "${ENVS[@]}"; do export "$kv"; done

if [ $USE_WINE = 1 ]; then
    export WINEPREFIX=${PREFIX:-$HOME/conker/pfx}
    RUN=("${WINE:-wine}" "$GAME_DIR/$EXE")
    echo "Wine: $("${WINE:-wine}" --version), prefix $WINEPREFIX"
else
    if [ -z "$PROTON_DIR" ]; then
        # Every Steam library, not only the internal drive's: Steam puts Proton
        # wherever the default library is (a Deck's SD card, 2026-11-20).
        proton_find() {
            { echo "$HOME/.steam/steam"
              sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' \
                  "$HOME/.steam/steam/steamapps/libraryfolders.vdf" 2>/dev/null
            } | while IFS= read -r lib; do
                for d in "$lib/steamapps/common/$1"*; do
                    [ -x "$d/proton" ] && printf '%s\t%s\n' "${d##*/}" "$d"
                done
            done | sort -V | tail -1 | cut -f2
        }
        PROTON_DIR=$(proton_find "Proton 11")
        [ -z "$PROTON_DIR" ] && PROTON_DIR=$(proton_find "Proton")
    fi
    if [ ! -x "$PROTON_DIR/proton" ]; then
        echo "No Proton found (install Proton 11 in Steam, or use -p / -w)"; exit 1
    fi
    export STEAM_COMPAT_DATA_PATH=${PREFIX:-$HOME/conker/compatdata}
    export STEAM_COMPAT_CLIENT_INSTALL_PATH=${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$HOME/.steam/steam}
    mkdir -p "$STEAM_COMPAT_DATA_PATH"
    RUN=("$PROTON_DIR/proton" run "$GAME_DIR/$EXE")
    echo "Proton: $PROTON_DIR, prefix $STEAM_COMPAT_DATA_PATH/pfx"
fi

STAMP=$(date +%Y%m%d_%H%M%S)
OUT=${OUT:-$PWD/linux_logs/$STAMP}
mkdir -p "$OUT"
MARK=$OUT/.start
touch "$MARK"
echo "Switches: ${ENVS[*]:-none}"
echo "Running: ${RUN[*]} ${ARGS[*]}"
(cd "$GAME_DIR" && if [ "$TIMEOUT" -gt 0 ]; then
    timeout -k 10 "$TIMEOUT" "${RUN[@]}" "${ARGS[@]}"
else
    "${RUN[@]}" "${ARGS[@]}"
fi) > "$OUT/stdout.txt" 2>&1
echo "Exit: $?"
# Play in the launcher starts conker.exe, then the launcher closes: wait for the game too, so
# its log is copied below and an SSH session stays open (SteamOS ends every process of a
# closed session, logind KillUserProcesses: a game started with Play died with it, 2026-11-20).
if [ $LAUNCHER = 1 ]; then
    if [ $USE_WINE = 1 ]; then
        WS=${WINESERVER:-wineserver}
    else
        export WINEPREFIX=$STEAM_COMPAT_DATA_PATH/pfx
        WS=$PROTON_DIR/files/bin/wineserver
        [ -x "$WS" ] || WS=$PROTON_DIR/dist/bin/wineserver
    fi
    echo "Launcher closed $(date +%T): waiting for a game started from it"
    "$WS" -w
    echo "Done waiting $(date +%T)"
fi
[ $USE_WINE = 1 ] && wineserver -k 2>/dev/null

# This run's game log(s), Proton's log, and the system facts the checks ask for.
find "$LOG_DIR" -maxdepth 1 -name 'conker_*.log' -newer "$MARK" -exec cp {} "$OUT/" \; 2>/dev/null
find "$HOME" -maxdepth 1 -name 'steam-*.log' -newer "$MARK" -exec cp {} "$OUT/" \; 2>/dev/null
{
    echo "uname: $(uname -a)"
    echo "mmap_min_addr: $(cat /proc/sys/vm/mmap_min_addr 2>/dev/null)"
    echo "ptrace_scope: $(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null)"
    echo "SteamDeck: ${SteamDeck:-unset}"
    command -v vulkaninfo >/dev/null && vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverName' | head -4
} > "$OUT/system.txt"
echo "Logs in $OUT"
for f in "$OUT"/conker_*.log; do
    [ -f "$f" ] && case $f in *_kernel.log) ;; *) check_log "$f" ;; esac
done
exit 0   # no game log (a launcher closed without Play) is not a failure
