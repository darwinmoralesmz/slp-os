#pragma once
#include <stdint.h>

/* Always output samples shifted to 28 bits + sign*/
#define ALAC_OUTPUT_DEPTH 29
#define SCALE16 (ALAC_OUTPUT_DEPTH - 16)
#define SCALE24 (ALAC_OUTPUT_DEPTH - 24)
#define ALAC_MAX_CHANNELS 2
#define ALAC_BLOCKSIZE 4096  /* Number of samples per channel per block */

typedef struct
{
    unsigned char *input_buffer;
    int input_buffer_bitaccumulator; /* used so we can do arbitary
                                        bit reads */

    /* stuff from setinfo */
    uint32_t setinfo_max_samples_per_frame; /* 0x1000 = 4096 */    /* max samples per frame? */
    uint8_t setinfo_sample_size; /* 0x10 */
    uint8_t setinfo_rice_historymult; /* 0x28 */
    uint8_t setinfo_rice_initialhistory; /* 0x0a */
    uint8_t setinfo_rice_kmodifier; /* 0x0e */
    /* end setinfo stuff */
} alac_file;

// Decodifica una trama; devuelve las muestras por canal o -1. inbuffer necesita 8 bytes de relleno tras los datos.
int alac_decode_frame(alac_file *alac, unsigned char *inbuffer, int32_t outputbuffer[ALAC_MAX_CHANNELS][ALAC_BLOCKSIZE]);
// Cookie: 24 bytes de relleno + caja "alac" tal como está en el MP4.
void alac_set_info(alac_file *alac, char *inputbuffer);
