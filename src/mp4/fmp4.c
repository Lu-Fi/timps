#include "fmp4.h"
#include "../config.h"
#include "../codec/nal.h"
#include "../codec/aac.h"
#include <string.h>

#define TRK_VIDEO 1
#define TRK_AUDIO 2

/* ---------- box helpers ---------- */
static size_t box_open(ms_buf *b, const char *type)
{
    size_t pos = b->len;
    ms_buf_be32(b, 0);
    ms_buf_put(b, type, 4);
    return pos;
}
static void box_close(ms_buf *b, size_t pos)
{
    /* b->err covers both "never allocated" and "a later append silently
     * failed to grow" - in the latter case b->data is still a valid (old,
     * smaller) block, but `pos` no longer reliably points inside it, so
     * patching would be an out-of-bounds write. Once err is set the whole
     * buffer is corrupt/truncated anyway; the caller (fragment() /
     * fmp4_init_segment()) checks b->err and reports failure instead of
     * handing a malformed box tree to a client. */
    if (!b->data || b->err) return;
    wr_be32(b->data + pos, (uint32_t)(b->len - pos));
}
static void put_fullbox(ms_buf *b, uint8_t ver, uint32_t flags)
{
    ms_buf_u8(b, ver);
    uint8_t f[3]; wr_be24(f, flags);
    ms_buf_put(b, f, 3);
}

void fmp4_init(fmp4_mux *m)
{
    memset(m, 0, sizeof(*m));
    m->v_timescale = 90000;
    m->base_pts_us   = -1;   /* set by the first sample of either track */
    m->v_last_pts_us = -1;
    m->a_last_pts_us = -1;
}

/* Produce a CONTINUOUS decode timeline per track (no gaps -> smooth MSE
 * playback) while keeping the two tracks locked to the same wall clock.
 *
 * The timeline (tfdt) is the running sum of emitted durations, so consecutive
 * fragments never leave a hole (setting tfdt from absolute PTS per fragment
 * left sub-frame gaps under capture jitter, which stalled the browser after
 * the first frame). Sample durations follow the real PTS deltas, so the
 * timeline still advances at the true capture rate. The FIRST sample of a
 * track is placed at its real offset from the shared zero point, which keeps
 * audio aligned to video (A/V sync) without breaking video continuity.
 *   *dts_io:      accumulator = tfdt to use for this sample (advanced here)
 *   *last_pts_io: previous sample PTS (<0 = first sample of this track)
 *   *dur_io:      nominal duration in, actual duration out
 *   returns:      the tfdt (baseMediaDecodeTime) for this fragment */
static uint64_t pts_track_time(fmp4_mux *m, int64_t pts_us, int64_t *last_pts_io,
                               uint64_t *dts_io, uint32_t timescale, uint32_t *dur_io)
{
    uint32_t nominal = *dur_io;
    if (nominal == 0) nominal = 1;     /* a 0-duration sample stalls MSE */
    if (pts_us > 0 && m->base_pts_us < 0) m->base_pts_us = pts_us; /* shared zero */

    if (*last_pts_io < 0) {
        /* first sample of this track with a usable PTS: anchor to its real
         * offset from the shared base so this track lines up with the other;
         * duration stays nominal (no previous sample to measure against yet).
         * Never rewind below the accumulator: samples may already have been
         * emitted with unknown PTS, and tfdt must stay strictly monotonic. */
        uint64_t start = *dts_io;
        if (pts_us > 0 && m->base_pts_us >= 0 && pts_us >= m->base_pts_us) {
            uint64_t off = (uint64_t)(pts_us - m->base_pts_us) * timescale / 1000000u;
            if (off > start) start = off;
        }
        if (pts_us > 0) *last_pts_io = pts_us;
        *dur_io = nominal;
        *dts_io = start + nominal;     /* accumulator for the next fragment */
        return start;
    }

    /* subsequent samples: continuous tfdt = current accumulator */
    uint64_t dts = *dts_io;
    uint32_t dur = nominal;
    if (pts_us > 0 && pts_us > *last_pts_io) {
        uint64_t d = (uint64_t)(pts_us - *last_pts_io) * timescale / 1000000u;
        if (d > 0 && d < (uint64_t)timescale * 10) dur = (uint32_t)d; /* clamp jitter/gaps */
        *last_pts_io = pts_us;
    }
    *dur_io = dur;
    *dts_io = dts + dur;
    return dts;
}

/* Annex-B AU -> length-prefixed (AVCC) sample, skipping parameter sets. */
static void annexb_to_sample(const fmp4_mux *m, const uint8_t *au, size_t len, ms_buf *out)
{
    nal_iter it; nal_unit u;
    nal_iter_init(&it, au, len);
    while (nal_iter_next(&it, &u)) {
        int t = (m->vcodec==MS_VC_H264) ? h264_nal_type(u.data) : h265_nal_type(u.data);
        if (m->vcodec==MS_VC_H264) {
            if (t==7||t==8||t==9) continue;             /* SPS/PPS/AUD */
        } else {
            if (t==32||t==33||t==34||t==35) continue;   /* VPS/SPS/PPS/AUD */
        }
        ms_buf_be32(out, (uint32_t)u.len);
        ms_buf_put(out, u.data, u.len);
    }
}

/* ---------- moov sub-boxes ---------- */
static void write_mvhd(ms_buf *b)
{
    size_t p = box_open(b, "mvhd");
    put_fullbox(b, 0, 0);
    ms_buf_be32(b, 0); ms_buf_be32(b, 0);        /* creation, modification */
    ms_buf_be32(b, 1000);                        /* timescale */
    ms_buf_be32(b, 0);                           /* duration (0 = live) */
    ms_buf_be32(b, 0x00010000);                  /* rate 1.0 */
    ms_buf_be16(b, 0x0100); ms_buf_be16(b, 0);   /* volume + reserved */
    ms_buf_be32(b, 0); ms_buf_be32(b, 0);
    /* unity matrix */
    uint32_t mtx[9] = {0x10000,0,0, 0,0x10000,0, 0,0,0x40000000};
    for (int i=0;i<9;i++) ms_buf_be32(b, mtx[i]);
    for (int i=0;i<6;i++) ms_buf_be32(b, 0);     /* predefined */
    ms_buf_be32(b, 0xFFFFFFFF);                  /* next track id */
    box_close(b, p);
}

static void write_tkhd(ms_buf *b, int track_id, int w, int h)
{
    size_t p = box_open(b, "tkhd");
    put_fullbox(b, 0, 0x000007);                 /* enabled|inmovie|inpreview */
    ms_buf_be32(b, 0); ms_buf_be32(b, 0);
    ms_buf_be32(b, track_id);
    ms_buf_be32(b, 0);
    ms_buf_be32(b, 0);                           /* duration */
    ms_buf_be32(b, 0); ms_buf_be32(b, 0);
    ms_buf_be16(b, 0);                           /* layer */
    ms_buf_be16(b, 0);                           /* alt group */
    ms_buf_be16(b, h ? 0 : 0x0100);              /* volume (audio=1.0) */
    ms_buf_be16(b, 0);
    uint32_t mtx[9] = {0x10000,0,0, 0,0x10000,0, 0,0,0x40000000};
    for (int i=0;i<9;i++) ms_buf_be32(b, mtx[i]);
    ms_buf_be32(b, (uint32_t)w << 16);
    ms_buf_be32(b, (uint32_t)h << 16);
    box_close(b, p);
}

static void write_mdhd(ms_buf *b, uint32_t timescale)
{
    size_t p = box_open(b, "mdhd");
    put_fullbox(b, 0, 0);
    ms_buf_be32(b, 0); ms_buf_be32(b, 0);
    ms_buf_be32(b, timescale);
    ms_buf_be32(b, 0);                           /* duration */
    ms_buf_be16(b, 0x55C4);                      /* language 'und' */
    ms_buf_be16(b, 0);
    box_close(b, p);
}

static void write_hdlr(ms_buf *b, const char *hdlr, const char *name)
{
    size_t p = box_open(b, "hdlr");
    put_fullbox(b, 0, 0);
    ms_buf_be32(b, 0);
    ms_buf_put(b, hdlr, 4);
    ms_buf_be32(b, 0); ms_buf_be32(b, 0); ms_buf_be32(b, 0);
    ms_buf_put(b, name, strlen(name)+1);
    box_close(b, p);
}

static void write_dinf(ms_buf *b)
{
    size_t p = box_open(b, "dinf");
    size_t q = box_open(b, "dref");
    put_fullbox(b, 0, 0);
    ms_buf_be32(b, 1);
    size_t u = box_open(b, "url ");
    put_fullbox(b, 0, 1);                        /* self-contained */
    box_close(b, u);
    box_close(b, q);
    box_close(b, p);
}

static void write_avc_hev_sample_entry(ms_buf *b, fmp4_mux *m)
{
    const char *type = (m->vcodec==MS_VC_H264) ? "avc1" : "hvc1";
    size_t p = box_open(b, type);
    for (int i=0;i<6;i++) ms_buf_u8(b, 0);       /* reserved */
    ms_buf_be16(b, 1);                           /* data ref index */
    ms_buf_be16(b, 0); ms_buf_be16(b, 0);        /* predefined/reserved */
    for (int i=0;i<3;i++) ms_buf_be32(b, 0);     /* predefined */
    ms_buf_be16(b, (uint16_t)m->width);
    ms_buf_be16(b, (uint16_t)m->height);
    ms_buf_be32(b, 0x00480000);                  /* h res 72dpi */
    ms_buf_be32(b, 0x00480000);                  /* v res */
    ms_buf_be32(b, 0);
    ms_buf_be16(b, 1);                           /* frame count */
    for (int i=0;i<32;i++) ms_buf_u8(b, 0);      /* compressor name */
    ms_buf_be16(b, 0x0018);                      /* depth */
    ms_buf_be16(b, 0xFFFF);                      /* predefined */
    /* codec config box */
    const char *cfgtype = (m->vcodec==MS_VC_H264) ? "avcC" : "hvcC";
    size_t c = box_open(b, cfgtype);
    vparam_mp4_config(&m->vp, b);
    box_close(b, c);
    box_close(b, p);
}

/* ESDS for AAC-LC */
static void write_esds(ms_buf *b, const uint8_t asc[2])
{
    size_t p = box_open(b, "esds");
    put_fullbox(b, 0, 0);
    /* ES_Descriptor */
    ms_buf_u8(b, 0x03);
    ms_buf_u8(b, 0x19);                          /* length */
    ms_buf_be16(b, 0);                           /* ES_ID */
    ms_buf_u8(b, 0);                             /* flags */
    /* DecoderConfigDescriptor */
    ms_buf_u8(b, 0x04);
    ms_buf_u8(b, 0x11);
    ms_buf_u8(b, 0x40);                          /* objectType AAC */
    ms_buf_u8(b, 0x15);                          /* streamType audio */
    ms_buf_put(b, (const uint8_t[]){0,0,0}, 3);  /* bufferSizeDB */
    ms_buf_be32(b, 0);                           /* maxBitrate */
    ms_buf_be32(b, 0);                           /* avgBitrate */
    /* DecoderSpecificInfo */
    ms_buf_u8(b, 0x05);
    ms_buf_u8(b, 0x02);
    ms_buf_put(b, asc, 2);
    /* SLConfigDescriptor */
    ms_buf_u8(b, 0x06);
    ms_buf_u8(b, 0x01);
    ms_buf_u8(b, 0x02);
    box_close(b, p);
}

static void write_audio_sample_entry(ms_buf *b, fmp4_mux *m)
{
    size_t p = box_open(b, "mp4a");
    for (int i=0;i<6;i++) ms_buf_u8(b, 0);
    ms_buf_be16(b, 1);                           /* data ref index */
    ms_buf_be32(b, 0); ms_buf_be32(b, 0);        /* reserved */
    ms_buf_be16(b, (uint16_t)m->a_channels);
    ms_buf_be16(b, 16);                          /* sample size */
    ms_buf_be16(b, 0); ms_buf_be16(b, 0);        /* predefined/reserved */
    ms_buf_be32(b, m->a_timescale << 16);        /* sample rate 16.16 */
    write_esds(b, m->asc);
    box_close(b, p);
}

static void write_stbl(ms_buf *b, fmp4_mux *m, int video)
{
    size_t p = box_open(b, "stbl");
    size_t s = box_open(b, "stsd");
    put_fullbox(b, 0, 0);
    ms_buf_be32(b, 1);
    if (video) write_avc_hev_sample_entry(b, m);
    else       write_audio_sample_entry(b, m);
    box_close(b, s);
    /* empty tables */
    const char *empties[] = {"stts","stsc","stsz","stco"};
    for (int i=0;i<4;i++){
        size_t e = box_open(b, empties[i]);
        put_fullbox(b, 0, 0);
        if (!strcmp(empties[i],"stsz")) ms_buf_be32(b, 0); /* sample_size */
        ms_buf_be32(b, 0);                                 /* entry_count */
        box_close(b, e);
    }
    box_close(b, p);
}

static void write_video_trak(ms_buf *b, fmp4_mux *m)
{
    size_t p = box_open(b, "trak");
    write_tkhd(b, TRK_VIDEO, m->width, m->height);
    size_t md = box_open(b, "mdia");
    write_mdhd(b, m->v_timescale);
    write_hdlr(b, "vide", "VideoHandler");
    size_t mf = box_open(b, "minf");
    size_t vm = box_open(b, "vmhd");
    put_fullbox(b, 0, 1);
    ms_buf_be16(b,0); ms_buf_be16(b,0); ms_buf_be16(b,0); ms_buf_be16(b,0);
    box_close(b, vm);
    write_dinf(b);
    write_stbl(b, m, 1);
    box_close(b, mf);
    box_close(b, md);
    box_close(b, p);
}

static void write_audio_trak(ms_buf *b, fmp4_mux *m)
{
    size_t p = box_open(b, "trak");
    write_tkhd(b, TRK_AUDIO, 0, 0);
    size_t md = box_open(b, "mdia");
    write_mdhd(b, m->a_timescale);
    write_hdlr(b, "soun", "SoundHandler");
    size_t mf = box_open(b, "minf");
    size_t sm = box_open(b, "smhd");
    put_fullbox(b, 0, 0);
    ms_buf_be16(b, 0); ms_buf_be16(b, 0);
    box_close(b, sm);
    write_dinf(b);
    write_stbl(b, m, 0);
    box_close(b, mf);
    box_close(b, md);
    box_close(b, p);
}

static void write_trex(ms_buf *b, int track_id, uint32_t def_flags)
{
    size_t p = box_open(b, "trex");
    put_fullbox(b, 0, 0);
    ms_buf_be32(b, track_id);
    ms_buf_be32(b, 1);                           /* default sample desc index */
    ms_buf_be32(b, 0);                           /* default duration */
    ms_buf_be32(b, 0);                           /* default size */
    ms_buf_be32(b, def_flags);                   /* default flags */
    box_close(b, p);
}

int fmp4_init_segment(fmp4_mux *m, ms_buf *out)
{
    if (m->has_video && !m->vp_ready) return -1;
    /* ftyp */
    size_t f = box_open(out, "ftyp");
    ms_buf_put(out, "isom", 4);
    ms_buf_be32(out, 0x00000200);
    ms_buf_put(out, "isom", 4);
    ms_buf_put(out, "iso5", 4);
    ms_buf_put(out, "dash", 4);
    ms_buf_put(out, "mp41", 4);
    box_close(out, f);
    /* moov */
    size_t mv = box_open(out, "moov");
    write_mvhd(out);
    if (m->has_video) write_video_trak(out, m);
    if (m->has_audio) write_audio_trak(out, m);
    size_t mx = box_open(out, "mvex");
    if (m->has_video) write_trex(out, TRK_VIDEO, 0x01010000); /* non-sync default */
    if (m->has_audio) write_trex(out, TRK_AUDIO, 0);
    box_close(out, mx);
    box_close(out, mv);
    return out->err ? -1 : 0;
}

static int fragment(fmp4_mux *m, int track_id, const uint8_t *sample, size_t slen,
                    uint32_t duration, uint64_t dts, uint32_t first_flags,
                    int use_first_flags, ms_buf *out)
{
    size_t moof = box_open(out, "moof");
    size_t mfhd = box_open(out, "mfhd");
    put_fullbox(out, 0, 0);
    ms_buf_be32(out, ++m->seq);
    box_close(out, mfhd);

    size_t traf = box_open(out, "traf");
    /* tfhd: default-base-is-moof (0x020000) */
    size_t tfhd = box_open(out, "tfhd");
    put_fullbox(out, 0, 0x020000);
    ms_buf_be32(out, track_id);
    box_close(out, tfhd);
    /* tfdt v1 */
    size_t tfdt = box_open(out, "tfdt");
    put_fullbox(out, 1, 0);
    { uint8_t t[8]; wr_be64(t, dts); ms_buf_put(out, t, 8); }
    box_close(out, tfdt);
    /* trun */
    uint32_t tr_flags = 0x000001 | 0x000100 | 0x000200; /* data-off, dur, size */
    if (use_first_flags) tr_flags |= 0x000004;
    size_t trun = box_open(out, "trun");
    put_fullbox(out, 0, tr_flags);
    ms_buf_be32(out, 1);                          /* sample count */
    size_t data_off_pos = out->len;
    ms_buf_be32(out, 0);                          /* data offset (patched) */
    if (use_first_flags) ms_buf_be32(out, first_flags);
    ms_buf_be32(out, duration);
    ms_buf_be32(out, (uint32_t)slen);
    box_close(out, trun);
    box_close(out, traf);
    box_close(out, moof);

    /* data offset from start of moof = moof size + 8 (mdat header) */
    uint32_t data_off = (uint32_t)(out->len - moof) + 8;
    if (out->data && !out->err) wr_be32(out->data + data_off_pos, data_off);

    size_t mdat = box_open(out, "mdat");
    ms_buf_put(out, sample, slen);
    box_close(out, mdat);
    /* any append above (box_open/ms_buf_put/box_close) may have silently
     * failed to grow the buffer under memory pressure; err is sticky, so
     * checking it once here catches every one of those sites. Report
     * failure instead of handing the caller a truncated/corrupt fragment. */
    return out->err ? -1 : 0;
}

int fmp4_video_fragment(fmp4_mux *m, const uint8_t *au, size_t len,
                        int keyframe, int64_t pts_us, ms_buf *out)
{
    ms_buf s; ms_buf_init(&s, len+32);
    annexb_to_sample(m, au, len, &s);
    /* parameter-set-only AU (no VCL data): emit nothing and do not advance
     * the timeline - a 0-byte sample would make MSE choke. s.err covers the
     * OOM case (annexb_to_sample's ms_buf_put failed partway through, or
     * the initial ms_buf_init itself) - treat it the same as "nothing to
     * emit" rather than muxing a truncated sample. */
    if (s.err || s.len == 0) { ms_buf_free(&s); return s.err ? -1 : 0; }
    uint32_t dur = m->fps>0 ? m->v_timescale/(uint32_t)m->fps : 3000; /* nominal */
    uint64_t dts = pts_track_time(m, pts_us, &m->v_last_pts_us,
                                  &m->v_dts, m->v_timescale, &dur);
    uint32_t flags = keyframe ? 0x02000000 : 0x01010000;
    int r = fragment(m, TRK_VIDEO, s.data, s.len, dur, dts, flags, 1, out);
    ms_buf_free(&s);
    return r;
}

int fmp4_audio_fragment(fmp4_mux *m, const uint8_t *frame, size_t len,
                        int64_t pts_us, ms_buf *out)
{
    size_t plen; int off = aac_adts_strip(frame, len, &plen);
    if (plen == 0) return 0;                      /* nothing to emit */
    uint32_t dur = 1024;                          /* nominal: AAC frame size */
    uint64_t dts = pts_track_time(m, pts_us, &m->a_last_pts_us,
                                  &m->a_dts, m->a_timescale, &dur);
    return fragment(m, TRK_AUDIO, frame+off, plen, dur, dts, 0, 0, out);
}
