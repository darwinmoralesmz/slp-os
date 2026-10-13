# Entorno común de los scripts Bluetooth del slot B.
export PATH=/data/mnt/r1os/bt/bin:$PATH LD_LIBRARY_PATH=/data/mnt/r1os/bt/lib:/data/mnt/r1os/lib
T=/data/mnt/r1os/bt/to.sh   # límite de tiempo: bluetoothctl y bluealsa-cli pueden quedarse colgados
# Opciones de bluealsa según Ajustes > Bluetooth > Calidad LDAC (ldacq= en el estado): adaptativa, alta, estándar o conexión.
case "$(sed -n 's/^ldacq=//p' /usr/data/r1_state.txt 2>/dev/null)" in
	1) BA_OPTS="--ldac-quality=high" ;;
	2) BA_OPTS="--ldac-quality=standard" ;;
	3) BA_OPTS="--ldac-quality=mobile" ;;
	*) BA_OPTS="--ldac-abr --ldac-quality=standard" ;;
esac
