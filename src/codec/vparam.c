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
    /* an oversized parameter set used to be silently clipped, producing a
     * corrupt (truncated-NAL) avcC/hvcC/sprop-parameter-sets that no client
     * can parse. Not reachable with the Ingenic encoders this targets
     * (SPS/PPS are always small), but skip-on-overflow - keep whatever
     * valid set was cached before, if any - is a safer failure mode than
     * emitting truncated data. */
    if ((int)n > cap) return;
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

/* minimal MSB-first Exp-Golomb bit reader over a de-emulated RBSP buffer;
 * reads past the end return 0 bits (deterministic, not a crash) rather than
 * assuming callers only ever hand it well-formed, long-enough data. */
typedef struct { const uint8_t *d; int nbits; int pos; } eg_reader;
static void eg_init(eg_reader *r, const uint8_t *d, int n){ r->d=d; r->nbits=n*8; r->pos=0; }
static int eg_bit(eg_reader *r)
{
    if (r->pos >= r->nbits) return 0;
    int byte=r->pos>>3, bit=7-(r->pos&7);
    r->pos++;
    return (r->d[byte]>>bit)&1;
}
static uint32_t eg_ue(eg_reader *r)
{
    int zeros=0;
    while (r->pos<r->nbits && eg_bit(r)==0 && zeros<32) zeros++;
    uint32_t val=0;
    for (int i=0;i<zeros;i++) val=(val<<1)|(uint32_t)eg_bit(r);
    return ((uint32_t)1<<zeros)-1+val;
}

/* H.264 profile_idc values whose SPS carries the chroma_format_idc/
 * bit_depth_*_minus8 fields (spec 7.3.2.1.1) - the "High profile family".
 * timps' default encoder profile is High (100), but cover the full set. */
static int is_h264_high_family(int profile_idc)
{
    switch (profile_idc){
        case 100: case 110: case 122: case 244: case 44: case 83:
        case 86: case 118: case 128: case 138: case 139: case 134: case 135:
            return 1;
        default: return 0;
    }
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

    /* ISO/IEC 14496-15 5.3.3.1.2: for the High-profile family the record
     * must carry chroma_format/bit_depth fields after the SPS/PPS arrays.
     * Browsers/ffmpeg tolerate the omission, but strict parsers (Bento4,
     * some hardware demuxers/smart TVs) reject a High-profile avcC without
     * them. Parse them out of the SPS itself (Exp-Golomb, right after
     * seq_parameter_set_id) instead of hardcoding 4:2:0/8-bit, so this
     * stays correct if the encoder config ever changes. */
    if (is_h264_high_family(v->sps[1])){
        uint8_t rbsp[192];
        /* skip the 1-byte NAL header (H.264 has a 1-byte header, unlike
         * H.265's 2 bytes); rbsp[0..2] are profile_idc/constraints/level_idc
         * (already known from sps[1..3]), so start the bit reader at bit 24 */
        int rn = deemulate(v->sps+1, v->sps_len-1, rbsp, sizeof rbsp);
        if (rn >= 4){
            eg_reader r; eg_init(&r, rbsp, rn); r.pos = 24;
            eg_ue(&r);                          /* seq_parameter_set_id */
            uint32_t chroma = eg_ue(&r);        /* chroma_format_idc */
            if (chroma == 3) eg_bit(&r);        /* separate_colour_plane_flag */
            uint32_t bd_luma   = eg_ue(&r);     /* bit_depth_luma_minus8 */
            uint32_t bd_chroma = eg_ue(&r);     /* bit_depth_chroma_minus8 */
            ms_buf_u8(o, (uint8_t)(0xFC | (chroma   & 0x3)));
            ms_buf_u8(o, (uint8_t)(0xF8 | (bd_luma  & 0x7)));
            ms_buf_u8(o, (uint8_t)(0xF8 | (bd_chroma& 0x7)));
            ms_buf_u8(o, 0);                   /* numOfSequenceParameterSetExt=0 */
        } else {
            /* SPS too short to reach these fields (shouldn't happen with a
             * real encoder) - fall back to the spec's implied 4:2:0/8-bit
             * defaults rather than omitting the extension entirely. */
            ms_buf_u8(o, 0xFC | 1); ms_buf_u8(o, 0xF8); ms_buf_u8(o, 0xF8); ms_buf_u8(o, 0);
        }
    }
    return 0;
}

static int hvcc(const vparam *v, ms_buf *o)
{
    /* extract 12-byte general profile_tier_level from SPS RBSP */
    uint8_t rbsp[192];
    int rn = deemulate(v->sps+2, v->sps_len-2, rbsp, sizeof rbsp); /* skip 2B nal hdr */
    /* an SPS too short to reach the profile_tier_level would otherwise
     * silently produce an all-zero PTL (profile_space=0/tier=0/profile=0/
     * level=0) - a bogus-but-well-formed-looking hvcC that lies about the
     * stream's actual profile/level instead of failing loudly, matching
     * vparam_hevc_codecs()'s same guard. */
    if (rn < 1+12) return -1;
    uint8_t ptl[12];
    /* after 1 byte (sps_vps_id u4, max_sub_layers_minus1 u3, nesting u1) */
    memcpy(ptl, rbsp+1, 12);

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
    ms_buf_u8(o,0x0B);              /* cfr=0|numTempLayers=1|nested=0|lengthSizeMinusOne=3 */
    ms_buf_u8(o,3);                 /* numOfArrays: VPS,SPS,PPS */
    /* array_completeness=1 (top bit set: 0xA_ not 0x20/0x21/0x22) - ISO/IEC
     * 14496-15 8.3.3.1 constrains this to 1 for an hvc1 sample entry (all
     * parameter sets are in the array, none arrive in-band in samples,
     * which matches what annexb_to_sample() actually does: it strips
     * VPS/SPS/PPS out of every sample). completeness=0 is only valid for
     * hev1; browsers ignore the flag either way, but strict validators
     * (Bento4, Apple's HLS tools) flag the mismatch. */
    /* VPS */
    ms_buf_u8(o,0xA0); ms_buf_be16(o,1);
    ms_buf_be16(o,(uint16_t)v->vps_len); ms_buf_put(o,v->vps,v->vps_len);
    /* SPS */
    ms_buf_u8(o,0xA1); ms_buf_be16(o,1);
    ms_buf_be16(o,(uint16_t)v->sps_len); ms_buf_put(o,v->sps,v->sps_len);
    /* PPS */
    ms_buf_u8(o,0xA2); ms_buf_be16(o,1);
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
