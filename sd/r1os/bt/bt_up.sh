#!/bin/sh
# Levanta Bluetooth en el slot B: dbus, firmware del AP6212 por /dev/ttyS0, bluetoothd y agente (rutas en la SD).
K=/data/mnt/r1os/bt
export PATH=$K/bin:$PATH LD_LIBRARY_PATH=$K/lib:/data/mnt/r1os/lib

rm -f /var/run/messagebus.pid
mkdir -p /tmp/dbus /var/lib/dbus /var/run/dbus
dbus-uuidgen > /var/lib/dbus/machine-id
dbus-daemon --config-file=/usr/share/dbus-1/system.conf

echo 1 > /sys/class/rfkill/rfkill0/state
sleep 1

bt_addr=$(cat /usr/data/bt_macaddr.txt)
CHIPVENDOR=/sys/devices/platform/md_ingenic,mmc.0/mmc_host/mmc0/mmc0:0001/mmc0:0001:2/chipvendor
case "$(cat $CHIPVENDOR 2>/dev/null)" in
	0x81) firmware=BCM4343A1_001.002.009.0122.0538.hcd ;;
	*)    firmware=BCM4343A1_001.002.009.1010.1030.hcd ;;
esac
echo "firmware: $firmware addr: $bt_addr"

brcm_patchram_plus --enable_hci --baudrate 3000000 --no2bytes --patchram /lib/firmware/bt_bcm/$firmware /dev/ttyS0 --tosleep=50000 --use_baudrate_for_download --enable_lpm --bd_addr $bt_addr &
sleep 5
hciconfig hci0 up
sleep 1
# Enlace en sniff tras 2 s sin datos (intervalo 250-500 ms): sin esto la radio sondea a los auriculares sin pausa y se oye un zumbido en el equipo.
D=/sys/kernel/debug/bluetooth/hci0
grep -q " /sys/kernel/debug " /proc/mounts || mount -t debugfs none /sys/kernel/debug
echo 2000 > $D/idle_timeout; echo 800 > $D/sniff_max_interval; echo 400 > $D/sniff_min_interval
bluetoothd -E -C &
hciconfig hci0 reset
sleep 1
bt-agent -c NoInputNoOutput &
sleep 2
hciconfig -a
