#!/bin/sh
# Simulador: el PC ya tiene red. "cli" imita wpa_cli con las redes guardadas en /tmp/r1sim/wnets (id|ssid|actual).
N=/tmp/r1sim/wnets
[ -f $N ] || printf '0|Casa (sim)|1\n' > $N
[ "$1" = cli ] || exit 0
case "$2" in
status) cur=$(grep '|1$' $N | cut -d'|' -f2); [ -n "$cur" ] && printf 'wpa_state=COMPLETED\nssid=%s\nip_address=127.0.0.1\n' "$cur" || echo wpa_state=SCANNING;;
signal_poll) echo RSSI=-58;;
scan_results) printf 'bssid / frequency / signal level / flags / ssid\n'
	printf 'aa:00:00:00:00:01\t2437\t-58\t[WPA2-PSK-CCMP][ESS]\tCasa (sim)\n'
	printf 'aa:00:00:00:00:02\t2412\t-71\t[WPA2-PSK-CCMP][ESS]\tVecino\n'
	printf 'aa:00:00:00:00:03\t2462\t-80\t[ESS]\tCafé abierto\n';;
list_networks) echo 'network id / ssid / bssid / flags'
	while IFS='|' read -r id s c; do [ "$c" = 1 ] && f='[CURRENT]' || f=''; printf '%s\t%s\tany\t%s\n' "$id" "$s" "$f"; done < $N;;
add_network) id=$(( $(cut -d'|' -f1 $N | sort -n | tail -1) + 1 )); echo "$id||0" >> $N; echo $id;;
set_network) [ "$4" = ssid ] && { s=$(echo "$5" | sed 's/../\\x&/g'); s=$(printf "$s"); sed -i "s/^$3||/$3|$s|/" $N; }; echo OK;;
select_network) sed -i 's/|1$/|0/; s/^\('"$3"'|.*\)|0$/\1|1/' $N; echo OK;;
remove_network) sed -i "/^$3|/d" $N; echo OK;;
*) echo OK;;
esac
