// Comprueba la curva de volumen: monótona, 1 % = mínimo audible, 100 % = máximo del equipo.
#include <assert.h>
#include <stdio.h>
#include "vol_table.h"
int main(void) {
	assert(vol_reg(0) == 255 && vol_reg(1) == VOL_REG_MIN && vol_reg(100) == VOL_REG_MAX);
	for (int v = 1; v < 100; v++) assert(vol_reg(v + 1) < vol_reg(v) && vol_reg(v) - vol_reg(v + 1) <= 2);
	puts("ok");
	return 0;
}
