#include "g711.h"

#define BIAS 0x84
#define CLIP 32635
#define SIGN_BIT   0x80
#define QUANT_MASK 0x0F
#define SEG_MASK   0x70
#define SEG_SHIFT  4

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

/* --- decode (backchannel): G.711 byte stream -> PCM16. Canonical Sun/CCITT
 * expand tables, pure C. out[] must hold n samples. */
void g711_ulaw_decode(const uint8_t *in, size_t n, int16_t *out)
{
    for (size_t i=0;i<n;i++){
        uint8_t u = (uint8_t)~in[i];
        int t = ((u & QUANT_MASK) << 3) + BIAS;
        t <<= ((unsigned)u & SEG_MASK) >> SEG_SHIFT;
        out[i] = (int16_t)((u & SIGN_BIT) ? (BIAS - t) : (t - BIAS));
    }
}

void g711_alaw_decode(const uint8_t *in, size_t n, int16_t *out)
{
    for (size_t i=0;i<n;i++){
        uint8_t a = in[i] ^ 0x55;
        int t = (a & QUANT_MASK) << 4;
        int seg = ((unsigned)a & SEG_MASK) >> SEG_SHIFT;
        switch (seg){
            case 0:  t += 8;    break;
            case 1:  t += 0x108; break;
            default: t += 0x108; t <<= seg - 1;
        }
        out[i] = (int16_t)((a & SIGN_BIT) ? t : -t);
    }
}
