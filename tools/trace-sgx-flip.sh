#!/bin/sh
# Split an SGX-backed OMAP page flip into dma-fence and scanout phases.
# Run as root and pass a short, frame-limited kmscube command as arguments.
set -eu

if [ "$#" -eq 0 ]; then
	echo "usage: $0 kmscube [kmscube arguments...]" >&2
	exit 2
fi

tracefs=/sys/kernel/tracing
mounted_tracefs=false
if [ ! -e "$tracefs/kprobe_events" ]; then
	tracefs=/sys/kernel/debug/tracing
fi
if [ ! -e "$tracefs/kprobe_events" ]; then
	tracefs=/sys/kernel/tracing
	mkdir -p "$tracefs"
	if mount -t tracefs tracefs "$tracefs"; then
		mounted_tracefs=true
	else
		echo "tracefs is unavailable" >&2
		exit 1
	fi
fi

clear_probes()
{
	printf '0\n' > "$tracefs/tracing_on" 2>/dev/null || true
	for event in dma_fence_wait_start dma_fence_wait_end dma_fence_signaled; do
		if [ -e "$tracefs/events/dma_fence/$event/enable" ]; then
			printf '0\n' > "$tracefs/events/dma_fence/$event/enable"
		fi
	done
	for path in irq/irq_handler_entry irq/irq_handler_exit workqueue/workqueue_queue_work; do
		if [ -e "$tracefs/events/$path/enable" ]; then
			printf '0\n' > "$tracefs/events/$path/enable"
		fi
	done
	for event in fence_wait fence_signal pvr_check pvr_process omap_flush omap_vblank; do
		if [ -e "$tracefs/events/sgxflip/$event/enable" ]; then
			printf '0\n' > "$tracefs/events/sgxflip/$event/enable"
		fi
	done
	printf '%s\n' '-:sgxflip/fence_wait' '-:sgxflip/fence_signal' \
		'-:sgxflip/pvr_check' '-:sgxflip/pvr_process' \
		'-:sgxflip/omap_flush' '-:sgxflip/omap_vblank' \
		>> "$tracefs/kprobe_events" 2>/dev/null || true
}

cleanup()
{
	clear_probes
	if [ "$mounted_tracefs" = true ]; then
		umount "$tracefs" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

clear_probes
printf 'nop\n' > "$tracefs/current_tracer"
printf 'mono\n' > "$tracefs/trace_clock" 2>/dev/null || true
printf '16384\n' > "$tracefs/buffer_size_kb"
printf '\n' > "$tracefs/trace"

add_probe()
{
	event=$1
	symbol=$2
	spec=$3

	if ! grep -q " [tT] $symbol\$" /proc/kallsyms; then
		echo "warning: kernel symbol unavailable: $symbol" >&2
		return
	fi

	if printf '%s\n' "p:sgxflip/$event $symbol $spec" >> "$tracefs/kprobe_events"; then
		printf '1\n' > "$tracefs/events/sgxflip/$event/enable"
	else
		echo "warning: could not probe: $symbol" >&2
	fi
}

have_fence_trace=false
for event in dma_fence_wait_start dma_fence_wait_end dma_fence_signaled; do
	if [ -e "$tracefs/events/dma_fence/$event/enable" ]; then
		printf '1\n' > "$tracefs/events/dma_fence/$event/enable"
		have_fence_trace=true
	fi
done

if [ -e "$tracefs/events/irq/irq_handler_entry/enable" ]; then
	printf 'irq == 37\n' > "$tracefs/events/irq/irq_handler_entry/filter"
	printf 'irq == 37\n' > "$tracefs/events/irq/irq_handler_exit/filter"
	printf '1\n' > "$tracefs/events/irq/irq_handler_entry/enable"
	printf '1\n' > "$tracefs/events/irq/irq_handler_exit/enable"
fi
if [ -e "$tracefs/events/workqueue/workqueue_queue_work/enable" ]; then
	printf 'workqueue == "PVR Linux Fence"\n' \
		> "$tracefs/events/workqueue/workqueue_queue_work/filter"
	printf '1\n' > "$tracefs/events/workqueue/workqueue_queue_work/enable"
fi

add_probe pvr_check PVRLinuxFenceCheckAll ''
add_probe pvr_process PVRLinuxFenceProcess ''
add_probe omap_flush omap_crtc_atomic_flush ''
add_probe omap_vblank omap_crtc_vblank_irq ''

if [ "$have_fence_trace" = false ] && ! grep -q '^p:sgxflip/' "$tracefs/kprobe_events"; then
	echo "no diagnostic probes could be registered" >&2
	exit 1
fi

printf '1\n' > "$tracefs/tracing_on"
status=0
"$@" || status=$?
printf '0\n' > "$tracefs/tracing_on"

cat "$tracefs/trace"
exit "$status"