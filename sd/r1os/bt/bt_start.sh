#!/bin/sh
# Levanta Wi-Fi/BT (driver cywdhd), bluetoothd, bluealsa y reconecta el último dispositivo.
# Idempotente: si ya hay una pila sana no hace nada; "force" (o una pila rota) la reinicia desde cero.
. /data/mnt/r1os/bt/bt_env.sh
healthy() { pidof bluetoothd >/dev/null && pidof bluealsa >/dev/null && hciconfig hci0 2>/dev/null | grep -q "UP RUNNING"; }
if [ "$1" != "force" ] && healthy; then exit 0; fi
mkdir /tmp/bt_start.lock 2>/dev/null || exit 0   # un único arranque a la vez
trap 'rmdir /tmp/bt_start.lock' EXIT

killall bluealsa bt-agent bluetoothd brcm_patchram_plus dbus-daemon 2>/dev/null
echo 0 > /sys/class/rfkill/rfkill0/state 2>/dev/null
sleep 1

[ -d /sys/class/net/wlan0 ] || insmod /data/mnt/r1os/cywdhd.ko wlan_mmc_num=0 bt_wifi_power_valid_level=0 gpio_bt_wifi_power=-1 bt_wifi_regulator_name=-1 gpio_wlan_reg_on=PB03 gpio_wlan_wake_host=PA08 gpio_bt_rst_n=-1 gpio_bt_reg_on=PB04 gpio_host_wake_bt=PB05 gpio_bt_wake_host=-1 gpio_bt_uart_rts=-1
sleep 2
/data/mnt/r1os/bt/bt_up.sh
# Bluetooth apagado desde la aplicación (bton=0 en el estado): la radio queda apagada y no se reconecta.
if grep -q '^bton=0' /usr/data/r1_state.txt 2>/dev/null; then /data/mnt/r1os/bt/bt_power.sh off; exit 0; fi
/data/mnt/r1os/bt/bt_power.sh on
# Calidad LDAC elegida en Ajustes (BA_OPTS, ver bt_env.sh): a 990 kbps fijos la radio del R1 no siempre da abasto.
bluealsa -p a2dp-source $BA_OPTS > /tmp/bluealsa.log 2>&1 &
sleep 2
[ -f /usr/data/bt_last ] && $T 15 bluetoothctl connect "$(cat /usr/data/bt_last)" >/dev/null 2>&1
exit 0
