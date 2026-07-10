#include "aac.h"

static const int srates[] = {
    96000,88200,64000,48000,44100,32000,24000,22050,
    16000,12000,11025,8000,7350,0,0,0
};

int aac_srate_index(int sr)
{
    for (int i=0;i<13;i++) if (srates[i]==sr) return i;
    return -1;
}

int aac_asc(int samplerate, int channels, uint8_t out[2])
{
    int idx = aac_srate_index(samplerate);
    if (idx < 0) idx = 8; /* 16k fallback */
    int obj = 2; /* AAC-LC */
    out[0] = (uint8_t)((obj<<3) | (idx>>1));
    out[1] = (uint8_t)(((idx&1)<<7) | (channels<<3));
    return 0;
}

int aac_adts_strip(const uint8_t *buf, size_t len, size_t *payload_len)
{
    if (len>=7 && buf[0]==0xFF && (buf[1]&0xF0)==0xF0){
        int hdr = (buf[1]&1) ? 7 : 9;  /* protection absent -> 7, else 9 */
        if ((size_t)hdr <= len){
            *payload_len = len - hdr;
            return hdr;
        }
    }
    *payload_len = len;
    return 0;
}
