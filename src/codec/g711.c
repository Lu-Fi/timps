#include "g711.h"

#define BIAS 0x84
#define CLIP 32635

void g711_ulaw_encode(const int16_t *pcm, size_t n, uint8_t *out)
{
    for (size_t i=0;i<n;i++){
        int s = pcm[i];
        int sign = (s>>8)&0x80;
        if (sign) s = -s;
        if (s > CLIP) s = CLIP;
        s += BIAS;
        int exp = 7;
        for (int m=0x4000; (s&m)==0 && exp>0; m>>=1) exp--;
        int mant = (s >> (exp+3)) & 0x0F;
        out[i] = (uint8_t)~(sign | (exp<<4) | mant);
    }
}

void g711_alaw_encode(const int16_t *pcm, size_t n, uint8_t *out)
{
    for (size_t i=0;i<n;i++){
        int s = pcm[i];
        int sign = ((~s)>>8)&0x80;
        if (!sign) s = -s;
        if (s > CLIP) s = CLIP;
        int exp, mant;
        if (s >= 256){
            exp = 7;
            for (int m=0x4000; (s&m)==0 && exp>0; m>>=1) exp--;
            mant = (s >> (exp+3)) & 0x0F;
            out[i] = (uint8_t)((sign | (exp<<4) | mant) ^ 0x55);
        } else {
            out[i] = (uint8_t)((sign | (s>>4)) ^ 0x55);
        }
    }
}
