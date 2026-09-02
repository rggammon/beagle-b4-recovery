#!/bin/sh
# Capture OMAP HSMMC state during a bounded direct write. Run as root.
set -u

DEVMEM=${DEVMEM:-/tmp/devmem2}
TEST_FILE=${TEST_FILE:-/root/.mmc-live-diag}

[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
[ -x "$DEVMEM" ] || { echo "missing executable $DEVMEM" >&2; exit 1; }

mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
echo on > /sys/bus/platform/devices/4809c000.mmc/power/control 2>/dev/null || true

registers() {
    for entry in \
        CON:0x4809c02c BLK:0x4809c104 ARG:0x4809c108 \
        CMD:0x4809c10c PSTATE:0x4809c124 HCTL:0x4809c128 \
        SYSCTL:0x4809c12c STAT:0x4809c130 IE:0x4809c134 \
        ISE:0x4809c138 CAPA:0x4809c140
    do
        name=${entry%%:*}
        address=${entry#*:}
        printf '%s=' "$name"
        "$DEVMEM" "$address" 2>/dev/null | sed 's/^.*: //'
    done
}

show_process() {
    process_id=$1
    ps -o pid,stat,wchan:32,etime,cmd -p "$process_id" 2>/dev/null || true
    if [ -r "/proc/$process_id/stack" ]; then
        cat "/proc/$process_id/stack"
    fi
}

echo '== identity =='
uname -a
findmnt -no SOURCE,FSTYPE,OPTIONS /
cat /sys/block/mmcblk0/device/name 2>/dev/null || true
cat /sys/block/mmcblk0/device/cid 2>/dev/null || true

echo '== negotiated link =='
cat /sys/kernel/debug/mmc0/ios 2>/dev/null || echo 'mmc0 ios unavailable'

echo '== queue =='
for field in logical_block_size physical_block_size max_sectors_kb max_segment_size max_segments nr_requests scheduler; do
    printf '%s=' "$field"
    cat "/sys/block/mmcblk0/queue/$field" 2>/dev/null || echo unavailable
done

echo '== existing blocked tasks =='
for process_id in $(pgrep dd 2>/dev/null || true); do
    show_process "$process_id"
done

echo '== idle controller =='
registers
echo "block_stat=$(cat /sys/block/mmcblk0/stat)"
grep -i mmc /proc/interrupts 2>/dev/null || true

echo '== 128 KiB direct write + live samples =='
rm -f "$TEST_FILE"
dd if=/dev/zero of="$TEST_FILE" bs=128K count=1 oflag=direct conv=fsync status=progress >/tmp/mmc-live-diag-dd.log 2>&1 &
write_pid=$!

sample=1
while [ "$sample" -le 16 ]; do
    printf '\n-- sample %02d --\n' "$sample"
    registers
    echo "block_stat=$(cat /sys/block/mmcblk0/stat)"
    grep -i mmc /proc/interrupts 2>/dev/null || true
    kill -0 "$write_pid" 2>/dev/null || break
    sleep 0.25
    sample=$((sample + 1))
done

cat /tmp/mmc-live-diag-dd.log
if kill -0 "$write_pid" 2>/dev/null; then
    echo 'WRITE STILL BLOCKED AFTER SAMPLING'
    show_process "$write_pid"
    kill "$write_pid" 2>/dev/null || true
    echo "left $TEST_FILE for post-mortem; remove it after I/O returns"
else
    wait "$write_pid"
    result=$?
    echo "write_rc=$result"
    rm -f "$TEST_FILE"
fi

echo '== final kernel messages =='
dmesg | grep -iE 'mmc|timeout|crc|error' | tail -40
