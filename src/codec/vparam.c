#include "vparam.h"
#include "nal.h"
#include "../config.h"
#include <string.h>
#include <stdio.h>

void vparam_init(vparam *v, int codec)
{
    memset(v, 0, sizeof(*v));
    v->codec = codec;
}

static void store(uint8_t *dst, int *dlen, const uint8_t *s, size_t n, int cap)
{
    if ((int)n > cap) n = cap;
    memcpy(dst, s, n);
    *dlen = (int)n;
}

int vparam_update(vparam *v, const uint8_t *au, size_t len)
{
    nal_iter it; nal_unit u;
    nal_iter_init(&it, au, len);
    while (nal_iter_next(&it, &u)) {
        if (u.len < 2) continue;
        if (v->codec == MS_VC_H264) {
            int t = h264_nal_type(u.data);
            if (t==7) store(v->sps,&v->sps_len,u.data,u.len,sizeof v->sps);
            else if (t==8) store(v->pps,&v->pps_len,u.data,u.len,sizeof v->pps);
        } else {
            int t = h265_nal_type(u.data);
            if (t==32) store(v->vps,&v->vps_len,u.data,u.len,sizeof v->vps);
            else if (t==33) store(v->sps,&v->sps_len,u.data,u.len,sizeof v->sps);
            else if (t==34) store(v->pps,&v->pps_len,u.data,u.len,sizeof v->pps);
        }
    }
    return vparam_ready(v);
}

int vparam_ready(const vparam *v)
{
    if (v->codec==MS_VC_H264) return v->sps_len>0 && v->pps_len>0;
    return v->vps_len>0 && v->sps_len>0 && v->pps_len>0;
}

/* de-emulate RBSP (remove 0x03 after 0x00 0x00) into out, return length */
static int deemulate(const uint8_t *in, int n, uint8_t *out, int outcap)
{
    int o=0, zeros=0;
    for (int i=0;i<n && o<outcap;i++){
        uint8_t b=in[i];
        if (zeros>=2 && b==0x03 && i+1<n && in[i+1]<=0x03){ zeros=0; continue; }
        out[o++]=b;
        if (b==0) zeros++; else zeros=0;
    }
    return o;
}

static int avcc(const vparam *v, ms_buf *o)
{
    if (v->sps_len<4) return -1;
    ms_buf_u8(o,1);              /* configurationVersion */
    ms_buf_u8(o,v->sps[1]);      /* profile */
    ms_buf_u8(o,v->sps[2]);      /* compat  */
    ms_buf_u8(o,v->sps[3]);      /* level   */
    ms_buf_u8(o,0xFF);           /* lengthSizeMinusOne=3 */
    ms_buf_u8(o,0xE1);           /* numSPS=1 */
    ms_buf_be16(o,(uint16_t)v->sps_len); ms_buf_put(o,v->sps,v->sps_len);
    ms_buf_u8(o,1);              /* numPPS=1 */
    ms_buf_be16(o,(uint16_t)v->pps_len); ms_buf_put(o,v->pps,v->pps_len);
    return 0;
}

static int hvcc(const vparam *v, ms_buf *o)
{
    /* extract 12-byte general profile_tier_level from SPS RBSP */
    uint8_t rbsp[192];
    int rn = deemulate(v->sps+2, v->sps_len-2, rbsp, sizeof rbsp); /* skip 2B nal hdr */
    uint8_t ptl[12]; memset(ptl,0,sizeof ptl);
    /* after 1 byte (sps_vps_id u4, max_sub_layers_minus1 u3, nesting u1) */
    if (rn >= 1+12) memcpy(ptl, rbsp+1, 12);

    ms_buf_u8(o,1);                 /* configurationVersion */
    ms_buf_u8(o,ptl[0]);            /* profile_space|tier|profile_idc */
    ms_buf_put(o,ptl+1,4);          /* compatibility flags */
    ms_buf_put(o,ptl+5,6);          /* constraint indicator flags */
    ms_buf_u8(o,ptl[11]);           /* general_level_idc */
    ms_buf_be16(o,0xF000);          /* reserved|min_spatial_segmentation_idc=0 */
    ms_buf_u8(o,0xFC);              /* reserved|parallelismType=0 */
    ms_buf_u8(o,0xFD);              /* reserved|chromaFormat=1 (4:2:0) */
    ms_buf_u8(o,0xF8);              /* reserved|bitDepthLumaMinus8=0 */
    ms_buf_u8(o,0xF8);              /* reserved|bitDepthChromaMinus8=0 */
    ms_buf_be16(o,0);               /* avgFrameRate */
    ms_buf_u8(o,0x03);              /* cfr=0|numTempLayers=0..|nested|lengthSizeMinusOne=3 */
    ms_buf_u8(o,3);                 /* numOfArrays: VPS,SPS,PPS */
    /* VPS */
    ms_buf_u8(o,0x20); ms_buf_be16(o,1);
    ms_buf_be16(o,(uint16_t)v->vps_len); ms_buf_put(o,v->vps,v->vps_len);
    /* SPS */
    ms_buf_u8(o,0x21); ms_buf_be16(o,1);
    ms_buf_be16(o,(uint16_t)v->sps_len); ms_buf_put(o,v->sps,v->sps_len);
    /* PPS */
    ms_buf_u8(o,0x22); ms_buf_be16(o,1);
    ms_buf_be16(o,(uint16_t)v->pps_len); ms_buf_put(o,v->pps,v->pps_len);
    return 0;
}

int vparam_mp4_config(const vparam *v, ms_buf *out)
{
    return v->codec==MS_VC_H264 ? avcc(v,out) : hvcc(v,out);
}

/* general_profile_tier_level, as read by hvcc() above: byte0 = profile_space
 * (u2) | tier_flag (u1) | profile_idc (u5); bytes1-4 = 32-bit
 * profile_compatibility_flags; bytes5-10 = 48-bit constraint indicator flags;
 * byte11 = general_level_idc. */
int vparam_hevc_codecs(const vparam *v, char *dst, int dstsz)
{
    if (v->codec != MS_VC_H265 || v->sps_len < 2+13) return -1;
    uint8_t rbsp[192];
    int rn = deemulate(v->sps+2, v->sps_len-2, rbsp, sizeof rbsp); /* skip 2B nal hdr */
    if (rn < 1+12) return -1;
    const uint8_t *ptl = rbsp+1;

    int space   = (ptl[0] >> 6) & 0x3;
    int tier    = (ptl[0] >> 5) & 0x1;
    int profile = ptl[0] & 0x1F;

    /* general_profile_compatibility_flags: the codecs string wants it with
     * bit order reversed (flag[0] as the LSB of the printed hex number) */
    uint32_t compat = ((uint32_t)ptl[1]<<24)|((uint32_t)ptl[2]<<16)|
                       ((uint32_t)ptl[3]<<8)|ptl[4];
    uint32_t rev = 0;
    for (int i = 0; i < 32; i++) if (compat & (1u<<i)) rev |= 1u << (31-i);

    /* 6-byte constraint indicator flags, trailing zero bytes omitted */
    int nconstraint = 0;
    for (int i = 5; i >= 0; i--) if (ptl[5+i]) { nconstraint = i+1; break; }
    char constraints[24] = ""; int co = 0;
    for (int i = 0; i < nconstraint && co < (int)sizeof(constraints)-4; i++)
        co += snprintf(constraints+co, sizeof(constraints)-co, ".%X", ptl[5+i]);

    return snprintf(dst, dstsz, "hvc1.%s%d.%X.%c%d%s",
                    space==1?"A":space==2?"B":space==3?"C":"",
                    profile, rev, tier?'H':'L', ptl[11], constraints);
}

int vparam_sdp_fmtp(const vparam *v, int pt, char *dst, int dstsz)
{
    char b64a[512], b64b[512], b64c[512];
    if (v->codec==MS_VC_H264) {
        ms_base64(b64a, v->sps, v->sps_len);
        ms_base64(b64b, v->pps, v->pps_len);
        return snprintf(dst, dstsz,
            "a=fmtp:%d packetization-mode=1;profile-level-id=%02X%02X%02X;"
            "sprop-parameter-sets=%s,%s\r\n",
            pt, v->sps[1], v->sps[2], v->sps[3], b64a, b64b);
    } else {
        ms_base64(b64a, v->vps, v->vps_len);
        ms_base64(b64b, v->sps, v->sps_len);
        ms_base64(b64c, v->pps, v->pps_len);
        return snprintf(dst, dstsz,
            "a=fmtp:%d sprop-vps=%s;sprop-sps=%s;sprop-pps=%s\r\n",
            pt, b64a, b64b, b64c);
    }
}
