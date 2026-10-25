#!/bin/sh
# Start iotd on the AP, taking the radio away from the vendor IoT gateway.
#
#   /writable/data/bin/start.sh [-v]        extra arguments go to iotd
#
# What it does, in order:
#   1. SIGSTOP the vendor supervisor rksiot_gw_base if it is running. It must be
#      stopped, not killed: init respawns it, and when it runs it reflashes the
#      vendor Bluetooth image onto the NCP within about 1.5 minutes.
#   2. Kill the vendor gateway processes that hold the IoT radio's UART.
#   3. Kill any cpcd or iotd left from a previous run (iotd starts its own cpcd).
#   4. Run iotd in the background, log in /tmp/iotd.log (tmpfs; /writable is 100 MB).
#
# Stop with: killall iotd        (iotd closes the endpoints and stops its cpcd)
# Give the radio back to the vendor gateway: killall iotd; kill -CONT <rksiot_gw_base pid>
# Busybox: no pgrep/pidof/nohup/timeout here.

BIN=/writable/data/bin
LOG=/tmp/iotd.log

for pid in $(ps | grep '[r]ksiot_gw_base' | awk '{print $1}'); do
    echo "stopping vendor supervisor rksiot_gw_base pid $pid (SIGSTOP)"
    kill -STOP "$pid"
done
killall -9 rksiot_gw rksiot_ble rksiot_zb 2>/dev/null
killall iotd 2>/dev/null
sleep 1
killall -9 iotd cpcd 2>/dev/null
sleep 1

echo "starting iotd, log in $LOG"
"$BIN/iotd" "$@" >> "$LOG" 2>&1 &
sleep 2
ps | grep '[i]otd\|[c]pcd'
tail -n 5 "$LOG"
