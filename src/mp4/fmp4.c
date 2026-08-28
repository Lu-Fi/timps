#include "fmp4.h"
#include "../config.h"
#include "../codec/nal.h"
#include "../codec/aac.h"
#include <string.h>
#include <stdlib.h>

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
 *   fixed_dur:    1 = never re-derive the duration from PTS deltas (AAC:
 *                 always the nominal 1024 samples/frame - deriving it from
 *                 capture PTS jitter instead made the MSE audio timeline
 *                 drift); 0 = video keeps following the real PTS deltas
 *   returns:      the tfdt (baseMediaDecodeTime) for this fragment */
static uint64_t pts_track_time(fmp4_mux *m, int64_t pts_us, int64_t *last_pts_io,
                               uint64_t *dts_io, uint32_t timescale, uint32_t *dur_io,
                               int fixed_dur)
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
        /* A PTS-following (video) track absorbs the gap into THIS sample's
         * duration, but only while the delta is credible: a delta of 10 s or
         * more is far more likely a garbage/rolled-over capture stamp than a
         * real one, and muxing it as a single 10-second-long sample would
         * wreck the presentation. Such a delta therefore falls through to the
         * re-anchor below instead of silently keeping `nominal` - see the M6
         * note there for why "silently keeping nominal" was a bug. */
        uint64_t d = fixed_dur ? 0
                   : (uint64_t)(pts_us - *last_pts_io) * timescale / 1000000u;
        if (!fixed_dur && d > 0 && d < (uint64_t)timescale * 10) {
            dur = (uint32_t)d;                        /* jitter/short gap */
        } else if (m->base_pts_us >= 0 && pts_us >= m->base_pts_us) {
            /* M2 (audio): a fixed-duration track otherwise advances by
             * exactly `nominal` per sample, so any input gap (audio.mute
             * toggle, AI stall, dropped fragment) leaves the audio timeline
             * permanently behind the PTS-following video track. If the real
             * media time implied by this PTS has run more than 2 nominal
             * durations AHEAD of the accumulator, re-anchor to it (same
             * offset-from-shared-base math as the first-sample branch).
             * Only forward jumps are possible (`off > dts` by the guard),
             * so tfdt stays strictly monotonic; contiguous audio (delta of
             * about one frame) never trips the threshold and keeps the
             * exact fixed-1024 behavior.
             *
             * M6 (video): the SAME recovery, for the one case the duration
             * clamp above rejects. A transport stall longer than the clamp -
             * a weak-WiFi client whose csend() blocks, its fanqueue evicting
             * meanwhile, then httpd.c's adaptive drop draining the backlog to
             * the next keyframe - resumes with a delta of tens of seconds.
             * With no recovery here the video timeline kept `nominal` for that
             * sample and simply LOST the whole stall: every later frame stayed
             * that much behind real time for the rest of the connection, while
             * the audio track re-anchored via M2 and stayed correct. The two
             * tracks then sat at a fixed A/V offset equal to the stall, which
             * never healed because the offset is an accumulator, not a
             * measurement (cam-vorne-garage 2h fMP4 longrun 2026-08-28: ~0.03 s
             * skew for 45 min, one WiFi stall, then a dead-flat 24.0 s for the
             * remaining 65 min; RTSP over the same link was unaffected because
             * RTP timestamps are absolute and simply resume at the right
             * place). Re-anchoring costs one tfdt jump forward to true media
             * time - exactly what the client needs to keep lip-sync - and the
             * threshold keeps every normal frame on the continuous
             * accumulator, so healthy streams are bit-identical. */
            uint64_t off = (uint64_t)(pts_us - m->base_pts_us) * timescale / 1000000u;
            if (off > dts + 2ull * nominal) dts = off;
        }
        *last_pts_io = pts_us;
    }
    *dur_io = dur;
    *dts_io = dts + dur;
    return dts;
}

/* NALs the AVCC sample never carries: parameter sets (they live in the moov's
 * avcC/hvcC) and the access-unit delimiter. ONE definition, because
 * fmp4_video_fragment's NAL index and the annexb_to_sample() fallback below
 * must agree on it exactly - the index is what tells trun the sample length. */
static int nal_skipped(const fmp4_mux *m, const nal_unit *u)
{
    int t = (m->vcodec==MS_VC_H264) ? h264_nal_type(u->data) : h265_nal_type(u->data);
    if (m->vcodec==MS_VC_H264) return t==7||t==8||t==9;          /* SPS/PPS/AUD */
    return t==32||t==33||t==34||t==35;                           /* VPS/SPS/PPS/AUD */
}

/* Annex-B AU -> length-prefixed (AVCC) sample, skipping parameter sets.
 * Only used for the rare AU that overflows fmp4_video_fragment's NAL index. */
static void annexb_to_sample(const fmp4_mux *m, const uint8_t *au, size_t len, ms_buf *out)
{
    nal_iter it; nal_unit u;
    nal_iter_init(&it, au, len);
    while (nal_iter_next(&it, &u)) {
        if (nal_skipped(m, &u)) continue;
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
    /* An SPS shorter than 4 bytes makes avcc() bail, leaving an EMPTY avcC box
     * (8 bytes, no profile/level/SPS/PPS). Without this check b->err stayed 0,
     * fmp4_init_segment() reported success, and the client got 200 OK with a
     * moov no decoder can start from - a silent black stream. Fold the failure
     * into ms_buf's sticky error so the existing bail-out path handles it. */
    if (vparam_mp4_config(&m->vp, b) != 0) b->err = 1;
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
    /* L14: the stsd sample rate is 16.16 fixed point - only 16 integer bits.
     * This pipeline never produces >48000 Hz, but clamp defensively so a
     * hypothetical rate >65535 Hz can't shift into the fraction field. */
    { uint32_t sr = m->a_timescale > 0xFFFFu ? 0xFFFFu : m->a_timescale;
      ms_buf_be32(b, sr << 16); }                /* sample rate 16.16 */
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
    /* A3: the cmfc/cmf2 CMAF brands were removed here. CMAF (ISO/IEC 23000-19)
     * requires ONE track per CMAF file, but this muxer always writes combined
     * video+audio into a single moov (write_video_trak + write_audio_trak
     * below), so advertising CMAF conformance is wrong for the A/V case - the
     * strict validators the brands were meant to satisfy would flag it. Browsers
     * ignore compatible-brands entirely, so this was purely cosmetic. */
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

/* Write moof + the mdat box header, and return the mdat box position so the
 * caller can append exactly `slen` bytes of sample data and box_close() it.
 * Split out of fragment() because the video path writes its sample straight
 * into `out` instead of handing over a contiguous buffer - see
 * fmp4_video_fragment. The moof contents are unchanged either way; trun still
 * records the sample length before any mdat byte is written, which is why the
 * caller has to know `slen` up front. */
static size_t fragment_head(fmp4_mux *m, int track_id, size_t slen,
                            uint32_t duration, uint64_t dts, uint32_t first_flags,
                            int use_first_flags, ms_buf *out)
{
    size_t moof = box_open(out, "moof");
    size_t mfhd = box_open(out, "mfhd");
    put_fullbox(out, 0, 0);
    /* L14: mfhd sequence_number is 32-bit by spec; m->seq (uint32_t) simply
     * wraps after 2^32 fragments (>2 years of continuous fragments at 60/s).
     * Players treat it as an opaque increasing counter, so the natural
     * unsigned wrap-around is the intended behavior - no extra guard. */
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

    return box_open(out, "mdat");
}

static int fragment(fmp4_mux *m, int track_id, const uint8_t *sample, size_t slen,
                    uint32_t duration, uint64_t dts, uint32_t first_flags,
                    int use_first_flags, ms_buf *out)
{
    size_t mdat = fragment_head(m, track_id, slen, duration, dts,
                                first_flags, use_first_flags, out);
    ms_buf_put(out, sample, slen);
    box_close(out, mdat);
    /* any append above (box_open/ms_buf_put/box_close) may have silently
     * failed to grow the buffer under memory pressure; err is sticky, so
     * checking it once here catches every one of those sites. Report
     * failure instead of handing the caller a truncated/corrupt fragment. */
    return out->err ? -1 : 0;
}

/* How many NALs of one access unit fmp4_video_fragment indexes on the stack.
 * A typical AU is 1-5 NALs (SPS/PPS/SEI + one slice); 32 covers heavily
 * multi-sliced encodes too, and an AU beyond that just takes the slower
 * re-scan path below. 256 B of a 128 KB (MS_STACK_STREAM) thread stack. */
#define FMP4_NAL_IDX 32

int fmp4_video_fragment(fmp4_mux *m, const uint8_t *au, size_t len,
                        int keyframe, int64_t pts_us, ms_buf *out)
{
    /* The trun box must record the sample length BEFORE any mdat byte is
     * written, which is why this used to build the whole AVCC sample in a
     * per-thread scratch ms_buf and then copy it a second time into `out`.
     * Index the NALs in the one Annex-B pass instead: the walk already has to
     * happen, and it yields both the total length (for trun) and the pointers
     * the mdat body is then written from - directly into `out`. That drops one
     * full copy of every video access unit per client per frame, and with it a
     * second frame-sized persistent buffer per streaming thread.
     * nal_iter/find_start is a byte-at-a-time start-code scan, so re-deriving
     * the length in a separate pre-pass would have cost more than the copy it
     * saved - the index is what makes one pass enough. */
    struct { const uint8_t *p; size_t n; } nals[FMP4_NAL_IDX];
    int nn = 0, overflow = 0;
    size_t slen = 0;
    {
        nal_iter it; nal_unit u;
        nal_iter_init(&it, au, len);
        while (nal_iter_next(&it, &u)) {
            if (nal_skipped(m, &u)) continue;
            if (nn < FMP4_NAL_IDX) { nals[nn].p = u.data; nals[nn].n = u.len; nn++; }
            else overflow = 1;
            slen += 4 + u.len;
        }
    }
    /* parameter-set-only AU (no VCL data): emit nothing and do not advance
     * the timeline - a 0-byte sample would make MSE choke. */
    if (slen == 0) return 0;
    uint32_t dur = m->fps>0 ? m->v_timescale/(uint32_t)m->fps : 3000; /* nominal */
    uint64_t dts = pts_track_time(m, pts_us, &m->v_last_pts_us,
                                  &m->v_dts, m->v_timescale, &dur, 0);
    uint32_t flags = keyframe ? 0x02000000 : 0x01010000;
    size_t mdat = fragment_head(m, TRK_VIDEO, slen, dur, dts, flags, 1, out);
    /* one grow for the whole sample instead of one per NAL */
    ms_buf_reserve(out, slen);
    if (overflow) annexb_to_sample(m, au, len, out);
    else for (int i = 0; i < nn; i++) {
        ms_buf_be32(out, (uint32_t)nals[i].n);
        ms_buf_put(out, nals[i].p, nals[i].n);
    }
    box_close(out, mdat);
    /* err is sticky across every append above (see fragment()) - a truncated
     * fragment is reported, never handed to the caller as valid. */
    return out->err ? -1 : 0;
}

int fmp4_audio_fragment(fmp4_mux *m, const uint8_t *frame, size_t len,
                        int64_t pts_us, ms_buf *out)
{
    size_t plen; int off = aac_adts_strip(frame, len, &plen);
    if (plen == 0) return 0;                      /* nothing to emit */
    uint32_t dur = 1024;                          /* fixed: AAC frame size (M5 -
                                                     * never re-derived from PTS
                                                     * jitter, see pts_track_time) */
    uint64_t dts = pts_track_time(m, pts_us, &m->a_last_pts_us,
                                  &m->a_dts, m->a_timescale, &dur, 1);
    return fragment(m, TRK_AUDIO, frame+off, plen, dur, dts, 0, 0, out);
}
