#!/bin/sh
# Simulador: la búsqueda tarda 2 s y descubre un dispositivo nuevo.
D=/tmp/r1sim
sleep 2
grep -q 'EE:02' $D/bt_devices 2>/dev/null || echo 'AA:BB:CC:DD:EE:02|0|Auriculares nuevos (sim)' >> $D/bt_devices
