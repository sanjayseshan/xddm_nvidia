#!/usr/bin/env bash
set -euo pipefail
trap '' HUP

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

NVX_RUN_LOG="${NVX_SERIAL_LOG:-$ROOT_DIR/test_logs/xp_v2_$(date +%Y%m%d-%H%M%S).log}"
ln -sfn "$NVX_RUN_LOG" "$ROOT_DIR/vm_serial.log"
touch "$NVX_RUN_LOG"

MAX_RETRIES="${MAX_RETRIES:-10}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-35}"

echo "=========================================================="
echo " Starting Windows XP x64 VFIO Runner with Auto-Retry"
echo " Serial log: $NVX_RUN_LOG"
echo " Max retries: $MAX_RETRIES | Boot detection timeout: ${BOOT_TIMEOUT}s"
echo "=========================================================="

attempt=1
while [ "$attempt" -le "$MAX_RETRIES" ]; do
    echo "==> [Attempt $attempt/$MAX_RETRIES] Launching Windows XP VM..."
    > "$NVX_RUN_LOG"

    # Start VM in background
    NVX_SERIAL_LOG="$NVX_RUN_LOG" "$SCRIPT_DIR/run_vfio_vm.sh" < /dev/null &
    VM_PID=$!

    # Watch for serial progress
    booted=0
    for i in $(seq 1 "$BOOT_TIMEOUT"); do
        sleep 1
        if ! kill -0 "$VM_PID" 2>/dev/null; then
            echo "--> QEMU process exited on attempt $attempt."
            break
        fi

        if grep -q "NVX" "$NVX_RUN_LOG" 2>/dev/null || \
           grep -q "Miniport" "$NVX_RUN_LOG" 2>/dev/null; then
            echo "==> Boot confirmed! Windows XP kernel/driver is actively booting."
            booted=1
            break
        fi
    done

    if [ "$booted" -eq 1 ]; then
        echo "==> Driver activity confirmed (PID $VM_PID). Monitoring session..."

        # Monitor loop for test completion or timeout, then soft powerdown
        for s in $(seq 1 120); do
            sleep 1
            if ! kill -0 "$VM_PID" 2>/dev/null; then
                break
            fi

            # Send Enter key at key startup points to ensure automated logon
            if [ "$s" -ge 8 ] && [ "$s" -le 24 ] && [ $((s % 4)) -eq 0 ]; then
                echo "sendkey ret" | socat - UNIX-CONNECT:/tmp/qemu-monitor.sock 2>/dev/null || true
            fi

            if grep -Eq "ALL_BENCHMARKS_FINISHED|\\[TEST_COMPLETE\\]|\\[NVX_3D_RAPID_RESULT\\]" "$NVX_RUN_LOG" 2>/dev/null; then
                if grep -Fq "[NVX_3D_RAPID_RESULT]" "$NVX_RUN_LOG" 2>/dev/null; then
                    echo "==> Focused 3D test completed. Sending soft ACPI powerdown..."
                else
                    echo "==> All benchmarks completed! Sending soft ACPI powerdown..."
                fi
                echo "system_powerdown" | socat - UNIX-CONNECT:/tmp/qemu-monitor.sock 2>/dev/null || true
                sleep 5
                break
            fi

            # If desktop has been active for 75s, perform soft powerdown
            if [ "$s" -ge 75 ] && grep -q "GPU solid fill" "$NVX_RUN_LOG" 2>/dev/null; then
                echo "==> Desktop active session completed ($s s). Sending soft ACPI powerdown..."
                echo "system_powerdown" | socat - UNIX-CONNECT:/tmp/qemu-monitor.sock 2>/dev/null || true
                sleep 6
                break
            fi
        done

        # Graceful wait for VM exit, fallback to quit
        if kill -0 "$VM_PID" 2>/dev/null; then
            echo "system_powerdown" | socat - UNIX-CONNECT:/tmp/qemu-monitor.sock 2>/dev/null || true
            sleep 4
        fi
        if kill -0 "$VM_PID" 2>/dev/null; then
            echo "quit" | socat - UNIX-CONNECT:/tmp/qemu-monitor.sock 2>/dev/null || true
            sleep 2
        fi

        wait "$VM_PID" || true
        echo "==> Windows XP session completed."
        exit 0
    else
        echo "==> No serial activity within ${BOOT_TIMEOUT}s; GPU did not boot cleanly. Retrying..."
        pkill -9 -P "$VM_PID" 2>/dev/null || true
        kill -9 "$VM_PID" 2>/dev/null || true
        wait "$VM_PID" 2>/dev/null || true
        sleep 3
    fi

    attempt=$((attempt + 1))
done

echo "Error: Exceeded max retries without successful boot." >&2
exit 1
