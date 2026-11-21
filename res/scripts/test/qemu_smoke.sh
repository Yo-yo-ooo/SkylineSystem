#!/bin/sh
# Headless QEMU boot smoke gate (Linux/macOS/WSL, POSIX sh, no PowerShell).
#
# Boots SkylineSystem-x86_64.iso with serial output to a file, waits for the
# expected boot markers, then fails on panic/stall patterns. Exits as soon as
# the final marker appears instead of always burning the full timeout.
#
# Usage:
#   sh res/scripts/test/qemu_smoke.sh [--iso PATH] [--qemu PATH]
#        [--wait SECONDS] [--mem 2G] [--smp 4] [--no-desktop] [--disk PATH]
#
# Exit codes: 0 = PASS, 1 = marker missing / fatal pattern, 2 = usage/env error.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
ISO="$ROOT/SkylineSystem-x86_64.iso"
QEMU="${QEMU:-qemu-system-x86_64}"
WAIT=90
MEM=2G
SMP=4
DESKTOP=1
DISK=""
LOG=$(mktemp "${TMPDIR:-/tmp}/skyline-qemu-XXXXXX.log")

while [ $# -gt 0 ]; do
    case "$1" in
        --iso) ISO="$2"; shift 2 ;;
        --qemu) QEMU="$2"; shift 2 ;;
        --wait) WAIT="$2"; shift 2 ;;
        --mem) MEM="$2"; shift 2 ;;
        --smp) SMP="$2"; shift 2 ;;
        --disk) DISK="$2"; shift 2 ;;
        --no-desktop) DESKTOP=0; shift ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ ! -f "$ISO" ]; then
    echo "qemu-smoke: ISO not found: $ISO" >&2
    exit 2
fi
if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "qemu-smoke: qemu not found: $QEMU" >&2
    exit 2
fi

DRIVE_ARGS=""
if [ -n "$DISK" ] && [ -f "$DISK" ]; then
    case "$DISK" in
        *" "*) echo "qemu-smoke: disk path must not contain spaces" >&2; exit 2 ;;
    esac
    DRIVE_ARGS="-drive file=$DISK,if=none,id=d0,format=raw -device ide-hd,drive=d0,bus=ide.0"
fi

echo "qemu-smoke: booting $ISO (wait=${WAIT}s mem=$MEM smp=$SMP desktop=$DESKTOP)"
# shellcheck disable=SC2086
"$QEMU" -machine q35 -cpu max -m "$MEM" -smp "$SMP" \
    -cdrom "$ISO" $DRIVE_ARGS -snapshot -display none \
    -serial "file:$LOG" -net nic -net user -no-reboot >/dev/null 2>&1 &
QPID=$!

FINAL="desktop ready"
[ "$DESKTOP" -eq 0 ] && FINAL="boot load: 43 requested, 43 ready"

now() { date +%s; }
deadline=$(( $(now) + WAIT ))
while [ "$(now)" -lt "$deadline" ]; do
    if grep -aq "$FINAL" "$LOG" 2>/dev/null; then break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

fail=0
need() {
    if grep -aq "$1" "$LOG"; then
        echo "qemu-smoke: OK   $2"
    else
        echo "qemu-smoke: FAIL missing: $2" >&2
        fail=1
    fi
}
forbid() {
    if grep -aq "$1" "$LOG"; then
        echo "qemu-smoke: FAIL fatal pattern: $2" >&2
        fail=1
    else
        echo "qemu-smoke: OK   no $2"
    fi
}

need "VMM: huge-page split self-test: PASS" "huge-page split self-test"
need "VMM: 1GB split self-test: PASS"       "1GB split self-test"
need "KMOD_SELFTEST PASS 14/14"             "kmod selftest 14/14"
need "boot load: 43 requested, 43 ready"    "43 boot modules ready"
need "kernel exports: 260 symbol"           "260 kernel exports"
[ "$DESKTOP" -eq 1 ] && need "desktop ready" "desktop ready"

forbid "STALL WATCHDOG"      "stall watchdog"
forbid "PANIC"               "panic"
forbid "Unhandled Exception" "unhandled exception"
forbid "SYSTEM HALTED"       "system halted"
forbid "KMOD_SELFTEST FAIL"  "kmod selftest failure"

if [ "$fail" -ne 0 ]; then
    echo "qemu-smoke: serial log kept at $LOG" >&2
    exit 1
fi
rm -f "$LOG"
echo "qemu-smoke: PASS"
exit 0
