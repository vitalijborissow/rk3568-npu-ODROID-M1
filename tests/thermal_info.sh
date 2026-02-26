#!/bin/bash
echo "=== THERMAL ZONES ==="
for tz in /sys/class/thermal/thermal_zone*; do
    name=$(cat $tz/type)
    temp=$(cat $tz/temp)
    policy=$(cat $tz/policy 2>/dev/null)
    echo "$name: ${temp} mdeg, policy=$policy"
    for i in 0 1 2 3 4 5; do
        f=$tz/trip_point_${i}_temp
        test -f "$f" || continue
        t=$(cat "$f")
        tp=$(cat "${f%_temp}_type")
        echo "  trip $i: ${t} mdeg [$tp]"
    done
    for i in 0 1 2 3; do
        f=$tz/cdev${i}_trip_point
        test -f "$f" || continue
        ctype=$(cat "$tz/cdev${i}/type" 2>/dev/null)
        echo "  cdev$i -> $ctype, bound to trip $(cat "$f")"
    done
done
echo ""
echo "=== COOLING DEVICES ==="
for cd in /sys/class/thermal/cooling_device*; do
    echo "$(basename "$cd"): type=$(cat "$cd/type") max=$(cat "$cd/max_state") cur=$(cat "$cd/cur_state")"
done
