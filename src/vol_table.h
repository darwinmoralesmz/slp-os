// Volumen por hardware del CS43131: porcentaje 0..100 -> registro de atenuación (255 = silencio, 0,5 dB por unidad).
// 1 % = VOL_REG_MIN (apenas audible) y 100 % = VOL_REG_MAX (el máximo del equipo original), lineal en dB.
#pragma once
#define VOL_REG_MIN 134
#define VOL_REG_MAX 12
static inline unsigned char vol_reg(int v) {
	if (v <= 0) return 255;
	if (v > 100) v = 100;
	return VOL_REG_MIN - ((v - 1) * (VOL_REG_MIN - VOL_REG_MAX) + 49) / 99;
}
