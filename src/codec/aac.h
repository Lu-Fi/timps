/* aac.h - ADTS handling + AudioSpecificConfig */
#ifndef MS_AAC_H
#define MS_AAC_H
#include <stdint.h>
#include <stddef.h>

int  aac_srate_index(int samplerate);            /* -1 if unknown */
/* Build 2-byte AudioSpecificConfig for AAC-LC (object type 2). */
int  aac_asc(int samplerate, int channels, uint8_t out[2]);
/* If buf starts with an ADTS header, return payload offset and set *payload_len;
 * returns 0 (offset) and leaves *payload_len=len if no ADTS. */
int  aac_adts_strip(const uint8_t *buf, size_t len, size_t *payload_len);

#endif
