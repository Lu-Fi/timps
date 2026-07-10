/* g711.h - PCM16 -> G.711 mu-law / A-law (pure C, no library) */
#ifndef MS_G711_H
#define MS_G711_H
#include <stdint.h>
#include <stddef.h>

void g711_ulaw_encode(const int16_t *pcm, size_t n, uint8_t *out);
void g711_alaw_encode(const int16_t *pcm, size_t n, uint8_t *out);

#endif
