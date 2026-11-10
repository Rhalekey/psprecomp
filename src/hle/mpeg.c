/* psprecomp — sceMpeg, the PSP's movie player.
 *
 * A PSMF movie is an MPEG program stream: H.264 video in the PES packets of stream 0xE0, and
 * ATRAC3plus sound in private stream 1 (0xBD), each of its packets led by four bytes of its own.
 * The game reads the file into a ring of 2048-byte packets through a callback of its own
 * (sceMpegRingbufferPut), asks for each access unit with its time stamp (sceMpegGetAvcAu,
 * sceMpegGetAtracAu), and has it decoded into a picture or into 2048 stereo samples
 * (sceMpegAvcDecode, sceMpegAtracDecode).
 *
 * Here every packet put is split at once: video into access units by FFmpeg's H.264 parser,
 * sound into its frames by their headers. They wait in queues until the game asks for them.
 * FFmpeg's decoders are loaded at run time, from avcodec and avutil beside the executable;
 * without them sceMpegInit fails, as on a console with no movie support, and psp_mpeg_available
 * says so, so that a game can be steered past its movies. Only one movie plays at a time. */

#include "psprecomp/hle.h"
#include "psprecomp/cpu.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/mem.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPEG_ERROR_NO_DATA     0x80618001u
#define MPEG_ERROR_INVALID     0x806101FEu
#define MPEG_ERROR_NOT_READY   0x80618005u
#define PACKET                 2048u
#define RING_EXTRA             104u     /* of the ring's memory for each packet, as the console's */
#define MPEG_MEMSIZE           0x10000u
#define ATRAC_ES_SIZE          2112u
#define ATRAC_OUT_SIZE         8192u
#define ATRAC_SAMPLES          2048
#define VIDEO_W                480
#define VIDEO_H                272
#define QUEUE                  256
#define VIDEO_FRAME_TICKS      3003     /* of the 90 kHz clock: 29.97 pictures a second */

#ifdef PSPRECOMP_FFMPEG
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

/* ---- FFmpeg, as far as this needs it, found by name in its DLLs ---------- */
static struct {
    const AVCodec *(*find_decoder)(enum AVCodecID);
    AVCodecContext *(*alloc_context3)(const AVCodec *);
    int (*open2)(AVCodecContext *, const AVCodec *, AVDictionary **);
    void (*free_context)(AVCodecContext **);
    int (*send_packet)(AVCodecContext *, const AVPacket *);
    int (*receive_frame)(AVCodecContext *, AVFrame *);
    void (*flush_buffers)(AVCodecContext *);
    AVPacket *(*packet_alloc)(void);
    void (*packet_free)(AVPacket **);
    AVCodecParserContext *(*parser_init)(int);
    int (*parser_parse2)(AVCodecParserContext *, AVCodecContext *, uint8_t **, int *, const uint8_t *, int, int64_t, int64_t, int64_t);
    void (*parser_close)(AVCodecParserContext *);
    AVFrame *(*frame_alloc)(void);
    void (*frame_free)(AVFrame **);
    void (*channel_layout_default)(AVChannelLayout *, int);
} ff;
static int g_ff_state;   /* 0 not tried, 1 loaded, -1 missing */

static int load_ffmpeg(void) {
    if (g_ff_state) return g_ff_state > 0;
    g_ff_state = -1;
    HMODULE codec = LoadLibraryA("avcodec-62.dll");
    HMODULE util = LoadLibraryA("avutil-60.dll");
    if (!codec || !util) {
        fprintf(stderr, "mpeg: FFmpeg not found beside the game, so movies are left out\n");
        return 0;
    }
#define FIND(field, lib, name) do { *(FARPROC *)&ff.field = GetProcAddress(lib, name); if (!ff.field) { fprintf(stderr, "mpeg: FFmpeg has no %s\n", name); return 0; } } while (0)
    FIND(find_decoder, codec, "avcodec_find_decoder");
    FIND(alloc_context3, codec, "avcodec_alloc_context3");
    FIND(open2, codec, "avcodec_open2");
    FIND(free_context, codec, "avcodec_free_context");
    FIND(send_packet, codec, "avcodec_send_packet");
    FIND(receive_frame, codec, "avcodec_receive_frame");
    FIND(flush_buffers, codec, "avcodec_flush_buffers");
    FIND(packet_alloc, codec, "av_packet_alloc");
    FIND(packet_free, codec, "av_packet_free");
    FIND(parser_init, codec, "av_parser_init");
    FIND(parser_parse2, codec, "av_parser_parse2");
    FIND(parser_close, codec, "av_parser_close");
    FIND(frame_alloc, util, "av_frame_alloc");
    FIND(frame_free, util, "av_frame_free");
    FIND(channel_layout_default, util, "av_channel_layout_default");
#undef FIND
    g_ff_state = 1;
    fprintf(stderr, "mpeg: FFmpeg loaded\n");
    return 1;
}

/* ---- what has been split from the stream, waiting to be asked for -------- */
typedef struct {
    uint8_t *data;
    int size;
    int64_t pts;
} unit;

typedef struct {
    unit u[QUEUE];
    int head, count;
} queue;

static void queue_push(queue *q, const uint8_t *data, int size, int64_t pts) {
    if (q->count == QUEUE) {   /* full: the oldest goes */
        free(q->u[q->head].data);
        q->head = (q->head + 1) % QUEUE;
        q->count--;
    }
    unit *u = &q->u[(q->head + q->count) % QUEUE];
    u->data = (uint8_t *)malloc((size_t)size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!u->data) return;
    memcpy(u->data, data, (size_t)size);
    memset(u->data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    u->size = size;
    u->pts = pts;
    q->count++;
}

static unit *queue_front(queue *q) { return q->count ? &q->u[q->head] : NULL; }

static void queue_pop(queue *q) {
    if (!q->count) return;
    free(q->u[q->head].data);
    q->u[q->head].data = NULL;
    q->head = (q->head + 1) % QUEUE;
    q->count--;
}

static void queue_clear(queue *q) {
    while (q->count) queue_pop(q);
    q->head = 0;
}

static int queue_bytes(const queue *q) {
    int total = 0;
    for (int i = 0; i < q->count; i++) total += q->u[(q->head + i) % QUEUE].size;
    return total;
}

/* ---- the one movie playing ------------------------------------------------ */
static struct {
    int open;
    uint32_t handle, ring;
    AVCodecContext *video, *audio;
    AVCodecParserContext *parser;
    AVPacket *packet;
    AVFrame *frame;
    queue video_units, audio_units;
    /* Sound frames run on from one packet into the next. */
    uint8_t audio_es[4096];
    int audio_es_len;
    int64_t audio_pts;          /* of the next frame that starts in audio_es, or -1 */
    int64_t audio_next_pts;     /* counted on from the last frame's */
    int64_t video_last_pts;
    int frames_decoded, has_picture;
    uint32_t pictures;
    uint32_t *rgba;             /* the last picture, as the game takes it */
    int rgba_w, rgba_h;
} M;

static void close_movie(void) {
    if (M.video) ff.free_context(&M.video);
    if (M.audio) ff.free_context(&M.audio);
    if (M.parser) ff.parser_close(M.parser);
    if (M.packet) ff.packet_free(&M.packet);
    if (M.frame) ff.frame_free(&M.frame);
    free(M.rgba);
    queue_clear(&M.video_units);
    queue_clear(&M.audio_units);
    memset(&M, 0, sizeof M);
}

static AVCodecContext *open_decoder(enum AVCodecID id, int block_align) {
    const AVCodec *codec = ff.find_decoder(id);
    AVCodecContext *c = codec ? ff.alloc_context3(codec) : NULL;
    if (!c) return NULL;
    if (id == AV_CODEC_ID_H264) {
        c->flags |= AV_CODEC_FLAG_LOW_DELAY;
        c->thread_count = 1;   /* frame threads would hold pictures back */
    } else {
        c->sample_rate = 44100;
        ff.channel_layout_default(&c->ch_layout, 2);
        c->block_align = block_align;
    }
    if (ff.open2(c, codec, NULL) < 0) ff.free_context(&c);
    return c;
}

static int open_movie(uint32_t handle, uint32_t ring) {
    close_movie();
    M.handle = handle;
    M.ring = ring;
    M.video = open_decoder(AV_CODEC_ID_H264, 0);
    M.parser = ff.parser_init(AV_CODEC_ID_H264);
    M.packet = ff.packet_alloc();
    M.frame = ff.frame_alloc();
    M.audio_pts = -1;
    M.audio_next_pts = -1;
    M.video_last_pts = -1;
    M.open = M.video && M.parser && M.packet && M.frame;
    if (!M.open) {
        fprintf(stderr, "mpeg: could not open the H.264 decoder\n");
        close_movie();
    } else {
        fprintf(stderr, "mpeg: movie opened\n");
    }
    return M.open;
}

/* ---- splitting the stream ------------------------------------------------- */
static int64_t read_pts(const uint8_t *p) {
    return ((int64_t)((p[0] >> 1) & 7) << 30) | ((int64_t)p[1] << 22) | ((int64_t)(p[2] >> 1) << 15) | ((int64_t)p[3] << 7) | (p[4] >> 1);
}

static void video_bytes(const uint8_t *data, int size, int64_t pts) {
    while (size > 0) {
        uint8_t *out = NULL;
        int out_size = 0;
        const int used = ff.parser_parse2(M.parser, M.video, &out, &out_size, data, size, pts, pts, 0);
        if (used < 0) return;
        if (out_size > 0) {
            /* A picture with no time of its own comes a frame after the last one. */
            int64_t at = M.parser->pts;
            if (at == AV_NOPTS_VALUE) at = M.video_last_pts >= 0 ? M.video_last_pts + VIDEO_FRAME_TICKS : -1;
            if (at >= 0) M.video_last_pts = at;
            queue_push(&M.video_units, out, out_size, at);
        }
        data += used;
        size -= used;
        pts = AV_NOPTS_VALUE;
    }
}

/* A sound frame: eight bytes of header, 0x0FD0 and its size, then the frame. */
static int audio_frame_size(const uint8_t *p) {
    if (p[0] != 0x0F || p[1] != 0xD0) return -1;
    return ((((p[2] & 3) << 8) | p[3]) * 8 + 8) + 8;
}

static void audio_bytes(const uint8_t *data, int size, int64_t pts) {
    if (pts != AV_NOPTS_VALUE && M.audio_pts < 0) M.audio_pts = pts;
    while (size > 0) {
        const int room = (int)sizeof M.audio_es - M.audio_es_len;
        const int take = size < room ? size : room;
        memcpy(M.audio_es + M.audio_es_len, data, (size_t)take);
        M.audio_es_len += take;
        data += take;
        size -= take;
        int at = 0;
        for (;;) {
            if (M.audio_es_len - at < 8) break;
            int whole = audio_frame_size(M.audio_es + at);
            if (whole < 0) {   /* out of step: find the next header */
                at++;
                continue;
            }
            if (M.audio_es_len - at < whole) break;
            if (!M.audio) {
                M.audio = open_decoder(AV_CODEC_ID_ATRAC3P, whole - 8);
                fprintf(stderr, "mpeg: ATRAC3plus decoder %s, frames of %d bytes\n", M.audio ? "opened" : "missing", whole - 8);
            }
            int64_t frame_pts = M.audio_pts >= 0 ? M.audio_pts : M.audio_next_pts;
            M.audio_pts = -1;
            queue_push(&M.audio_units, M.audio_es + at + 8, whole - 8, frame_pts);
            M.audio_next_pts = frame_pts >= 0 ? frame_pts + (int64_t)ATRAC_SAMPLES * 90000 / 44100 : -1;
            at += whole;
        }
        memmove(M.audio_es, M.audio_es + at, (size_t)(M.audio_es_len - at));
        M.audio_es_len -= at;
    }
}

/* One 2048-byte pack: its header, then PES packets to its end. */
static void split_pack(const uint8_t *p) {
    if (p[0] || p[1] || p[2] != 1 || p[3] != 0xBA) return;
    uint32_t i = 14 + (p[13] & 7);
    while (i + 6 <= PACKET && !p[i] && !p[i + 1] && p[i + 2] == 1) {
        const uint8_t id = p[i + 3];
        const uint32_t length = ((uint32_t)p[i + 4] << 8) | p[i + 5];
        const uint32_t end = i + 6 + length;
        if (end > PACKET) break;
        if ((id == 0xE0 || id == 0xBD) && length >= 3) {
            const uint32_t header = p[i + 8];
            const int64_t pts = (p[i + 7] & 0x80) ? read_pts(p + i + 9) : AV_NOPTS_VALUE;
            uint32_t payload = i + 9 + header;
            if (id == 0xE0) {
                if (payload < end) video_bytes(p + payload, (int)(end - payload), pts);
            } else if (payload + 4 <= end && p[payload] == 0) {
                /* Four bytes of its own: the stream, then where its first frame starts. */
                payload += 4;
                audio_bytes(p + payload, (int)(end - payload), pts);
            }
        }
        i = end;
    }
}

#endif /* PSPRECOMP_FFMPEG */

/* ---- guest memory --------------------------------------------------------- */
static void write_au(uint32_t au, int64_t pts) {
    if (!au) return;
    const uint32_t hi = pts < 0 ? 0xFFFFFFFFu : (uint32_t)(pts >> 32), lo = pts < 0 ? 0xFFFFFFFFu : (uint32_t)pts;
    psp_write32(au + 0, hi);
    psp_write32(au + 4, lo);
    psp_write32(au + 8, hi);
    psp_write32(au + 12, lo);
}

/* A function of the game's, as the console calls a callback: its registers are kept. */
static uint32_t call_guest(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    uint32_t saved[32];
    memcpy(saved, psp_cpu.r, sizeof saved);
    psp_cpu.r[PSP_REG_A0] = a0;
    psp_cpu.r[PSP_REG_A1] = a1;
    psp_cpu.r[PSP_REG_A2] = a2;
    psp_dispatch(fn);
    const uint32_t result = psp_cpu.r[PSP_REG_V0];
    memcpy(psp_cpu.r, saved, sizeof saved);
    return result;
}

#ifdef PSPRECOMP_FFMPEG
/* The frame just decoded, kept as 8888, the way the game takes it. */
static void keep_picture(void) {
    const AVFrame *f = M.frame;
    if (f->format != AV_PIX_FMT_YUV420P || f->width <= 0 || f->height <= 0) return;
    if (f->width != M.rgba_w || f->height != M.rgba_h) {
        free(M.rgba);
        M.rgba = (uint32_t *)malloc((size_t)f->width * f->height * 4);
        M.rgba_w = M.rgba ? f->width : 0;
        M.rgba_h = M.rgba ? f->height : 0;
        if (!M.rgba) return;
    }
    for (int y = 0; y < f->height; y++) {
        const uint8_t *py = f->data[0] + (size_t)y * f->linesize[0];
        const uint8_t *pu = f->data[1] + (size_t)(y / 2) * f->linesize[1];
        const uint8_t *pv = f->data[2] + (size_t)(y / 2) * f->linesize[2];
        uint32_t *row = M.rgba + (size_t)y * f->width;
        for (int x = 0; x < f->width; x++) {
            /* BT.601, studio range, in fixed point. */
            const int c = (py[x] - 16) * 298, d = pu[x / 2] - 128, e = pv[x / 2] - 128;
            int r = (c + 409 * e + 128) >> 8, g = (c - 100 * d - 208 * e + 128) >> 8, b = (c + 516 * d + 128) >> 8;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            row[x] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        }
    }
    M.pictures++;
}

/* Every picture the decoder has ready is taken, and the last of them, or the one before if none
 * is ready, goes into the game's buffer, `width` pixels to a row: the game shows a buffer each time
 * it asks, so it must never be left as it was. Gives back whether there was one to put. */
static int receive_picture(uint32_t buffer, uint32_t width) {
    while (ff.receive_frame(M.video, M.frame) >= 0) {
        keep_picture();
        if (!M.has_picture && M.rgba) fprintf(stderr, "mpeg: first picture, %dx%d, into 0x%08X at %u to a row\n", M.rgba_w, M.rgba_h, buffer, width);
        M.has_picture = M.rgba != NULL;
    }
    if (!M.has_picture || !buffer) return M.has_picture;
    const int w = M.rgba_w < (int)width ? M.rgba_w : (int)width;
    for (int y = 0; y < M.rgba_h; y++)
        psp_mem_write_block(buffer + (uint32_t)y * width * 4, M.rgba + (size_t)y * M.rgba_w, (uint32_t)w * 4);
    return 1;
}
#endif /* PSPRECOMP_FFMPEG */

/* ---- the calls --------------------------------------------------------------- */
int psp_mpeg_available(void) {
#ifdef PSPRECOMP_FFMPEG
    return load_ffmpeg();
#else
    return 0;
#endif
}

static void hle_Init(void) { psp_ret(psp_mpeg_available() ? 0 : MPEG_ERROR_NOT_READY); }
static void hle_Finish(void) {
#ifdef PSPRECOMP_FFMPEG
    if (g_ff_state > 0) close_movie();
#endif
    psp_ret(0);
}
static void hle_QueryMemSize(void) { psp_ret(MPEG_MEMSIZE); }
static void hle_RingbufferQueryMemSize(void) { psp_ret(psp_arg(0) * (PACKET + RING_EXTRA)); }

/* (ring, packets, data, size, callback, argument) */
static void hle_RingbufferConstruct(void) {
    const uint32_t r = psp_arg(0), packets = psp_arg(1);
    psp_write32(r + 0, packets);
    psp_write32(r + 4, 0);
    psp_write32(r + 8, 0);
    psp_write32(r + 12, packets);
    psp_write32(r + 16, PACKET);
    psp_write32(r + 20, psp_arg(2));
    psp_write32(r + 24, psp_arg(4));
    psp_write32(r + 28, psp_arg(5));
    psp_write32(r + 32, psp_arg(2) + packets * PACKET);
    psp_write32(r + 36, 0);
    psp_write32(r + 40, 0);
    psp_ret(0);
}
static void hle_RingbufferDestruct(void) { psp_ret(0); }

/* (handle address, data, size, ring, frame width, mode, ddr top) */
static void hle_Create(void) {
    const uint32_t handle = psp_arg(0), data = psp_arg(1), ring = psp_arg(3);
#ifdef PSPRECOMP_FFMPEG
    if (!load_ffmpeg() || !open_movie(handle, ring)) {
        psp_ret(MPEG_ERROR_NOT_READY);
        return;
    }
#endif
    psp_write32(handle, data + 0x30);
    if (ring) psp_write32(ring + 40, handle);
    psp_ret(0);
}
static void hle_Delete(void) {
#ifdef PSPRECOMP_FFMPEG
    if (g_ff_state > 0) {
        fprintf(stderr, "mpeg: movie closed after %u pictures\n", M.pictures);
        close_movie();
    }
#endif
    psp_ret(0);
}

static uint32_t be32(uint32_t at) {
    return ((uint32_t)psp_read8(at) << 24) | ((uint32_t)psp_read8(at + 1) << 16) | ((uint32_t)psp_read8(at + 2) << 8) | psp_read8(at + 3);
}
static int is_psmf(uint32_t at) { return psp_read32(at) == 0x464D5350u; }   /* "PSMF" */

/* (mpeg, header, offset address) */
static void hle_QueryStreamOffset(void) {
    if (!is_psmf(psp_arg(1))) { psp_ret(MPEG_ERROR_INVALID); return; }
    psp_write32(psp_arg(2), be32(psp_arg(1) + 8));
    psp_ret(0);
}
/* (header, size address) */
static void hle_QueryStreamSize(void) {
    if (!is_psmf(psp_arg(0))) { psp_ret(MPEG_ERROR_INVALID); return; }
    psp_write32(psp_arg(1), be32(psp_arg(0) + 12));
    psp_ret(0);
}
/* (mpeg, kind, number): a handle of the stream's own. */
static void hle_RegistStream(void) { psp_ret(0x34340000u | ((psp_arg(1) & 0xFF) << 8) | (psp_arg(2) & 0xFF)); }
static void hle_UnRegistStream(void) { psp_ret(0); }
static void hle_MallocAvcEsBuf(void) { psp_ret(1); }
static void hle_FreeAvcEsBuf(void) { psp_ret(0); }
/* (mpeg, es size address, out size address) */
static void hle_QueryAtracEsSize(void) {
    if (psp_arg(1)) psp_write32(psp_arg(1), ATRAC_ES_SIZE);
    if (psp_arg(2)) psp_write32(psp_arg(2), ATRAC_OUT_SIZE);
    psp_ret(0);
}
/* (mpeg, es buffer, au) */
static void hle_InitAu(void) {
    const uint32_t au = psp_arg(2);
    write_au(au, -1);
    psp_write32(au + 16, psp_arg(1));
    psp_write32(au + 20, 0);
    psp_ret(0);
}

/* (ring, packets, available): the game's callback fills the ring, and all it gives is split. */
static void hle_RingbufferPut(void) {
    const uint32_t r = psp_arg(0);
    uint32_t wanted = psp_arg(1);
    const uint32_t available = psp_arg(2);
    if (wanted > available) wanted = available;
    const uint32_t packets = psp_read32(r + 0), data = psp_read32(r + 20), callback = psp_read32(r + 24), argument = psp_read32(r + 28);
    uint32_t put = 0;
    while (put < wanted && packets) {
        const uint32_t at = psp_read32(r + 8) % packets;
        uint32_t ask = wanted - put;
        if (ask > packets - at) ask = packets - at;
        const int32_t got = (int32_t)call_guest(callback, data + at * PACKET, ask, argument);
        if (got <= 0) break;
#ifdef PSPRECOMP_FFMPEG
        if (M.open) {
            for (int32_t k = 0; k < got; k++) {
                const uint8_t *pack = (const uint8_t *)psp_mem_ptr(data + (at + (uint32_t)k) * PACKET, PACKET);
                if (pack) split_pack(pack);
            }
        }
#endif
        psp_write32(r + 8, psp_read32(r + 8) + (uint32_t)got);
        put += (uint32_t)got;
        if ((uint32_t)got < ask) break;
    }
    psp_ret(put);
}

/* (ring): packets that can be put, all of them less what waits split and not yet asked for. */
static void hle_RingbufferAvailableSize(void) {
    const uint32_t r = psp_arg(0), packets = psp_read32(r + 0);
    uint32_t waiting = 0;
#ifdef PSPRECOMP_FFMPEG
    if (M.open) waiting = (uint32_t)(queue_bytes(&M.video_units) + queue_bytes(&M.audio_units)) / PACKET;
#endif
    const uint32_t free_packets = waiting >= packets ? 0 : packets - waiting;
    psp_write32(r + 12, free_packets);
    psp_ret(free_packets);
}

/* (mpeg, stream, au, attribute address) */
static void hle_GetAvcAu(void) {
#ifdef PSPRECOMP_FFMPEG
    const unit *u = M.open ? queue_front(&M.video_units) : NULL;
    if (!u) { psp_ret(MPEG_ERROR_NO_DATA); return; }
    write_au(psp_arg(2), u->pts);
    psp_write32(psp_arg(2) + 20, (uint32_t)u->size);
    if (psp_arg(3)) psp_write32(psp_arg(3), 1);
    psp_ret(0);
#else
    psp_ret(MPEG_ERROR_NO_DATA);
#endif
}

/* (mpeg, stream, au, attribute address) */
static void hle_GetAtracAu(void) {
#ifdef PSPRECOMP_FFMPEG
    const unit *u = M.open ? queue_front(&M.audio_units) : NULL;
    if (!u) { psp_ret(MPEG_ERROR_NO_DATA); return; }
    write_au(psp_arg(2), u->pts);
    psp_write32(psp_arg(2) + 20, (uint32_t)u->size);
    if (psp_arg(3)) psp_write32(psp_arg(3), 0);
    psp_ret(0);
#else
    psp_ret(MPEG_ERROR_NO_DATA);
#endif
}

/* (mpeg, au, frame width, buffer address address, init address) */
static void hle_AvcDecode(void) {
    uint32_t got = 0;
#ifdef PSPRECOMP_FFMPEG
    const uint32_t width = psp_arg(2), buffer = psp_arg(3) ? psp_read32(psp_arg(3)) : 0;
    unit *u = M.open ? queue_front(&M.video_units) : NULL;
    if (u) {
        M.packet->data = u->data;
        M.packet->size = u->size;
        M.packet->pts = u->pts;
        ff.send_packet(M.video, M.packet);
        M.packet->data = NULL;
        M.packet->size = 0;
        queue_pop(&M.video_units);
        got = (uint32_t)receive_picture(buffer, width ? width : 512);
        M.frames_decoded++;
    }
#endif
    if (psp_arg(4)) psp_write32(psp_arg(4), got);
    psp_ret(0);
}

/* (mpeg, detail) */
static void hle_AvcDecodeDetail(void) {
    const uint32_t d = psp_arg(1);
    uint32_t frames = 0, status = 0;
#ifdef PSPRECOMP_FFMPEG
    frames = (uint32_t)M.frames_decoded;
    status = (uint32_t)M.has_picture;
#endif
    psp_write32(d + 0, 0);
    psp_write32(d + 4, frames);
    psp_write32(d + 8, VIDEO_W);
    psp_write32(d + 12, VIDEO_H);
    for (uint32_t k = 16; k < 32; k += 4) psp_write32(d + k, 0);
    psp_write32(d + 32, status);
    psp_ret(0);
}

/* (mpeg, frame width, buffer, status address): the end of the pictures. Every access unit has been
 * given its picture as it was decoded, so nothing more is put anywhere, only a status of none left:
 * the third argument is not always a picture's buffer, and a picture written there tramples the
 * game's memory. */
static void hle_AvcDecodeStop(void) {
#ifdef PSPRECOMP_FFMPEG
    if (M.open) ff.flush_buffers(M.video);
#endif
    if (psp_arg(3)) psp_write32(psp_arg(3), 0);
    psp_ret(0);
}

static void hle_AvcDecodeFlush(void) {
#ifdef PSPRECOMP_FFMPEG
    if (M.open) ff.flush_buffers(M.video);
#endif
    psp_ret(0);
}

/* (mpeg, au, buffer, init): 2048 stereo samples of 16 bits. */
static void hle_AtracDecode(void) {
    const uint32_t buffer = psp_arg(2);
#ifdef PSPRECOMP_FFMPEG
    int16_t out[ATRAC_SAMPLES * 2];
    memset(out, 0, sizeof out);
    unit *u = M.open ? queue_front(&M.audio_units) : NULL;
    if (u && M.audio) {
        M.packet->data = u->data;
        M.packet->size = u->size;
        if (ff.send_packet(M.audio, M.packet) >= 0 && ff.receive_frame(M.audio, M.frame) >= 0) {
            const AVFrame *f = M.frame;
            const int n = f->nb_samples < ATRAC_SAMPLES ? f->nb_samples : ATRAC_SAMPLES;
            const int stereo = f->ch_layout.nb_channels > 1;
            for (int i = 0; i < n; i++) {
                for (int c = 0; c < 2; c++) {
                    float s = 0.0f;
                    if (f->format == AV_SAMPLE_FMT_FLTP) s = ((const float *)f->data[stereo ? c : 0])[i];
                    else if (f->format == AV_SAMPLE_FMT_FLT) s = ((const float *)f->data[0])[i * f->ch_layout.nb_channels + (stereo ? c : 0)];
                    int v = (int)(s * 32767.0f);
                    out[i * 2 + c] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
                }
            }
        }
        M.packet->data = NULL;
        M.packet->size = 0;
        queue_pop(&M.audio_units);
    }
    if (buffer) psp_mem_write_block(buffer, out, sizeof out);
#else
    (void)buffer;
#endif
    psp_ret(0);
}

static void hle_FlushAllStream(void) {
#ifdef PSPRECOMP_FFMPEG
    if (M.open) {
        queue_clear(&M.video_units);
        queue_clear(&M.audio_units);
        M.audio_es_len = 0;
        M.audio_pts = M.audio_next_pts = -1;
        ff.flush_buffers(M.video);
        if (M.audio) ff.flush_buffers(M.audio);
    }
#endif
    psp_ret(0);
}

/* ---- sceMp3: MP3 files the game streams from the Memory Stick ---------------------------------
 *
 * The game keeps the file and its buffers. It reserves a handle with where the stream starts and
 * ends in the file, a buffer for the file's bytes and one for the samples. Then, while
 * sceMp3CheckStreamDataNeeded says so, it asks where to put the next bytes and from where in the
 * file (sceMp3GetInfoToAddStreamData), reads them there and says how many (sceMp3NotifyAddStreamData).
 * Each sceMp3Decode gives one frame's samples, 16 bits, interleaved, in its sample buffer.
 * Here the bytes are copied out as they are added, split into frames by FFmpeg's parser and decoded
 * by its MP3 decoder, the same DLLs the movies use.
 *
 * A file that is named .mp3 but is not one, as the M4A (AAC) a phone records, is read whole from
 * the Memory Stick by FFmpeg's own reader (avformat), which needs what an M4A keeps at its end, and
 * decoded by whatever decoder it names, at 44100 Hz in stereo; the bytes the game adds are not
 * needed then. Without avformat, such a file stays silent as before. */
#define MP3_HANDLES 2
#define MP3_FIFO (512u * 1024u)
#define MP3_ERROR_HANDLE 0x80671001u
#define MP3_ERROR_NOT_READY 0x80671002u
#define MP3_ERROR_NEED_DATA 0x80671402u   /* sceMp3Decode with too little of the file added */
#define MP3_NEED_DATA 0xFFFFFFFFu

#ifdef PSPRECOMP_FFMPEG
typedef struct {
    int used, ready;
    uint64_t start, end, readpos;
    uint32_t buf, buf_size, pcm, pcm_size;
    uint8_t *fifo;
    uint32_t fifo_len;
    AVCodecContext *codec;
    AVCodecParserContext *parser;
    AVPacket *packet;
    AVFrame *frame;
    int rate, channels, bitrate, loops, flushed;
    uint64_t decoded;
    /* A file of another kind, read by avformat: its reader, its audio stream, the converter to 44100
     * Hz stereo, and what has been converted but not yet given, in stereo samples. */
    int other, other_sent_eof, other_done, other_stream;
    AVFormatContext *fmt;
    SwrContext *swr;
    int16_t *queue;
    uint32_t queue_len, queue_cap;
} mp3_stream;
static mp3_stream g_mp3[MP3_HANDLES];

static mp3_stream *mp3_of(uint32_t h) { return h < MP3_HANDLES && g_mp3[h].used ? &g_mp3[h] : NULL; }

/* ---- FFmpeg's reader and converter, for files of other kinds ---------------------------------- */
static struct {
    int (*open_input)(AVFormatContext **, const char *, const AVInputFormat *, AVDictionary **);
    int (*find_stream_info)(AVFormatContext *, AVDictionary **);
    int (*find_best_stream)(AVFormatContext *, enum AVMediaType, int, int, const AVCodec **, int);
    int (*read_frame)(AVFormatContext *, AVPacket *);
    int (*seek_frame)(AVFormatContext *, int, int64_t, int);
    void (*close_input)(AVFormatContext **);
    int (*parameters_to_context)(AVCodecContext *, const AVCodecParameters *);
    void (*packet_unref)(AVPacket *);
    int (*swr_alloc_set_opts2)(SwrContext **, const AVChannelLayout *, enum AVSampleFormat, int, const AVChannelLayout *,
                               enum AVSampleFormat, int, int, void *);
    int (*swr_init)(SwrContext *);
    int (*swr_convert)(SwrContext *, uint8_t *const *, int, const uint8_t *const *, int);
    void (*swr_free)(SwrContext **);
} fx;
static int g_fx_state;

static int load_reader(void) {
    if (g_fx_state) return g_fx_state > 0;
    g_fx_state = -1;
    HMODULE format = LoadLibraryA("avformat-62.dll"), resample = LoadLibraryA("swresample-6.dll"), codec = GetModuleHandleA("avcodec-62.dll");
    if (!format || !resample || !codec) {
        fprintf(stderr, "mp3: FFmpeg's reader not found, so only MP3 files play\n");
        return 0;
    }
#define FIND(field, lib, name) do { *(FARPROC *)&fx.field = GetProcAddress(lib, name); if (!fx.field) { fprintf(stderr, "mp3: FFmpeg has no %s\n", name); return 0; } } while (0)
    FIND(open_input, format, "avformat_open_input");
    FIND(find_stream_info, format, "avformat_find_stream_info");
    FIND(find_best_stream, format, "av_find_best_stream");
    FIND(read_frame, format, "av_read_frame");
    FIND(seek_frame, format, "av_seek_frame");
    FIND(close_input, format, "avformat_close_input");
    FIND(parameters_to_context, codec, "avcodec_parameters_to_context");
    FIND(packet_unref, codec, "av_packet_unref");
    FIND(swr_alloc_set_opts2, resample, "swr_alloc_set_opts2");
    FIND(swr_init, resample, "swr_init");
    FIND(swr_convert, resample, "swr_convert");
    FIND(swr_free, resample, "swr_free");
#undef FIND
    g_fx_state = 1;
    return 1;
}

static void mp3_close(mp3_stream *s) {
    if (s->swr) fx.swr_free(&s->swr);
    if (s->fmt) fx.close_input(&s->fmt);
    free(s->queue);
    if (s->codec) ff.free_context(&s->codec);
    if (s->parser) ff.parser_close(s->parser);
    if (s->packet) ff.packet_free(&s->packet);
    if (s->frame) ff.frame_free(&s->frame);
    free(s->fifo);
    memset(s, 0, sizeof *s);
}

/* From the start of the file again, as for a loop or a reset. */
static void mp3_rewind(mp3_stream *s) {
    if (s->other) {
        fx.seek_frame(s->fmt, s->other_stream, 0, AVSEEK_FLAG_BACKWARD);
        ff.flush_buffers(s->codec);
        s->queue_len = 0;
        s->other_sent_eof = s->other_done = 0;
        s->decoded = 0;
        return;
    }
    s->readpos = s->start;
    s->fifo_len = 0;
    s->decoded = 0;
    s->flushed = 0;
    if (s->codec) ff.flush_buffers(s->codec);
    if (s->parser) {
        ff.parser_close(s->parser);
        s->parser = ff.parser_init(AV_CODEC_ID_MP3);
    }
}

/* The first frame's header, past an ID3v2 tag if the stream starts with one. */
static int mp3_header(mp3_stream *s) {
    const uint8_t *p = s->fifo;
    uint32_t i = 0, n = s->fifo_len;
    if (n >= 10 && p[0] == 'I' && p[1] == 'D' && p[2] == '3')
        i = 10 + (((uint32_t)p[6] & 0x7F) << 21 | ((uint32_t)p[7] & 0x7F) << 14 | ((uint32_t)p[8] & 0x7F) << 7 | ((uint32_t)p[9] & 0x7F));
    static const int RATES[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    static const int KBPS_V1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int KBPS_V2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    /* Another kind of file under the name, known by how it begins: MP4 or M4A, Ogg, FLAC, WAV. */
    if (n >= 12 && i == 0 && (!memcmp(p + 4, "ftyp", 4) || !memcmp(p, "OggS", 4) || !memcmp(p, "fLaC", 4) || !memcmp(p, "RIFF", 4)))
        return 0;
    for (; i + 4 <= n; i++) {
        if (p[i] != 0xFF || (p[i + 1] & 0xE0) != 0xE0) continue;
        const int version = (p[i + 1] >> 3) & 3, layer = (p[i + 1] >> 1) & 3, rate = (p[i + 2] >> 2) & 3, kbps = p[i + 2] >> 4;
        if (version == 1 || layer != 1 || rate == 3 || kbps == 0 || kbps == 15) continue;   /* layer III only */
        const int hz = RATES[version][rate], bitrate = version == 3 ? KBPS_V1[kbps] : KBPS_V2[kbps];
        /* A frame only if another begins where it ends, as bytes of any file can look like one. */
        const uint32_t size = (uint32_t)((version == 3 ? 144 : 72) * bitrate * 1000 / hz + ((p[i + 2] >> 1) & 1));
        if (i + size + 2 <= n && (p[i + size] != 0xFF || (p[i + size + 1] & 0xE0) != 0xE0)) continue;
        s->rate = hz;
        s->channels = ((p[i + 3] >> 6) == 3) ? 1 : 2;
        s->bitrate = bitrate;
        return 1;
    }
    return 0;
}

/* The file the game opened last, read whole by avformat, when its bytes are not an MP3's. */
static int other_open(mp3_stream *s) {
    char ansi[1024], utf8[3072];
    wchar_t wide[1024];
    psp_io_last_ms_file(ansi, sizeof ansi);
    if (!ansi[0] || !load_reader()) return 0;
    if (!MultiByteToWideChar(CP_ACP, 0, ansi, -1, wide, 1024) || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof utf8, NULL, NULL)) return 0;
    const AVCodec *codec = NULL;
    if (fx.open_input(&s->fmt, utf8, NULL, NULL) < 0) { s->fmt = NULL; return 0; }
    if (fx.find_stream_info(s->fmt, NULL) < 0) goto fail;
    s->other_stream = fx.find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (s->other_stream < 0 || !codec) goto fail;
    const AVCodecParameters *par = s->fmt->streams[s->other_stream]->codecpar;
    s->codec = ff.alloc_context3(codec);
    if (!s->codec || fx.parameters_to_context(s->codec, par) < 0 || ff.open2(s->codec, codec, NULL) < 0) goto fail;
    AVChannelLayout stereo, in = s->codec->ch_layout;
    ff.channel_layout_default(&stereo, 2);
    if (in.order != AV_CHANNEL_ORDER_NATIVE || in.nb_channels <= 0) ff.channel_layout_default(&in, in.nb_channels > 0 ? in.nb_channels : 2);
    if (fx.swr_alloc_set_opts2(&s->swr, &stereo, AV_SAMPLE_FMT_S16, 44100, &in, s->codec->sample_fmt, s->codec->sample_rate, 0, NULL) < 0 ||
        fx.swr_init(s->swr) < 0)
        goto fail;
    s->other = 1;
    s->rate = 44100;
    s->channels = 2;
    s->bitrate = par->bit_rate > 0 ? (int)(par->bit_rate / 1000) : s->fmt->bit_rate > 0 ? (int)(s->fmt->bit_rate / 1000) : 128;
    s->readpos = s->end;   /* the game has nothing more to add */
    s->fifo_len = 0;
    fprintf(stderr, "mp3: not an MP3 but %s, read whole, %d Hz %d channels into 44100 Hz stereo\n", codec->name,
            s->codec->sample_rate, s->codec->ch_layout.nb_channels);
    return 1;
fail:
    if (s->swr) fx.swr_free(&s->swr);
    if (s->codec) ff.free_context(&s->codec);
    if (s->fmt) fx.close_input(&s->fmt);
    return 0;
}

/* The next samples of a file of another kind, as many as an MP3 frame holds. */
static uint32_t other_decode(mp3_stream *s) {
    uint32_t want = 1152;
    if (want > s->pcm_size / 4) want = s->pcm_size / 4;
    while (s->queue_len < want && !s->other_done) {
        const int r = ff.receive_frame(s->codec, s->frame);
        if (r == 0 || r == AVERROR_EOF) {
            const int in = r == 0 ? s->frame->nb_samples : 0;
            const uint32_t most = (uint32_t)in * 44100u / (uint32_t)(s->codec->sample_rate > 0 ? s->codec->sample_rate : 44100) + 4096u;
            if (s->queue_len + most > s->queue_cap) {
                int16_t *bigger = (int16_t *)realloc(s->queue, (size_t)(s->queue_len + most) * 4);
                if (!bigger) return 0;
                s->queue = bigger;
                s->queue_cap = s->queue_len + most;
            }
            uint8_t *dst = (uint8_t *)(s->queue + s->queue_len * 2);
            const int got = fx.swr_convert(s->swr, &dst, (int)most, r == 0 ? (const uint8_t *const *)s->frame->extended_data : NULL, in);
            if (got > 0) s->queue_len += (uint32_t)got;
            if (r == AVERROR_EOF) s->other_done = 1;
            continue;
        }
        if (s->other_sent_eof) {
            s->other_done = 1;
            break;
        }
        if (fx.read_frame(s->fmt, s->packet) < 0) {
            ff.send_packet(s->codec, NULL);
            s->other_sent_eof = 1;
            continue;
        }
        if (s->packet->stream_index == s->other_stream) ff.send_packet(s->codec, s->packet);
        fx.packet_unref(s->packet);
    }
    const uint32_t n = s->queue_len < want ? s->queue_len : want;
    if (!n) return 0;
    void *pcm = psp_mem_ptr(s->pcm, n * 4);
    if (!pcm) return 0;
    memcpy(pcm, s->queue, (size_t)n * 4);
    memmove(s->queue, s->queue + n * 2, (size_t)(s->queue_len - n) * 4);
    s->queue_len -= n;
    s->decoded += n;
    return n * 4;
}

static void hle_Mp3InitResource(void) { psp_ret(psp_mpeg_available() ? 0 : MP3_ERROR_NOT_READY); }
static void hle_Mp3TermResource(void) {
    for (int i = 0; i < MP3_HANDLES; i++)
        if (g_mp3[i].used) mp3_close(&g_mp3[i]);
    psp_ret(0);
}

/* (SceMp3InitArg *): stream start, end, the file's buffer and size, the samples' buffer and size. */
static void hle_Mp3ReserveMp3Handle(void) {
    const uint32_t a = psp_arg(0);
    if (!psp_mpeg_available()) { psp_ret(MP3_ERROR_NOT_READY); return; }
    for (uint32_t h = 0; h < MP3_HANDLES; h++) {
        mp3_stream *s = &g_mp3[h];
        if (s->used) continue;
        memset(s, 0, sizeof *s);
        s->used = 1;
        s->start = psp_read32(a + 0) | (uint64_t)psp_read32(a + 4) << 32;
        s->end = psp_read32(a + 8) | (uint64_t)psp_read32(a + 12) << 32;
        s->buf = psp_read32(a + 16);
        s->buf_size = psp_read32(a + 20);
        s->pcm = psp_read32(a + 24);
        s->pcm_size = psp_read32(a + 28);
        s->readpos = s->start;
        s->fifo = (uint8_t *)malloc(MP3_FIFO + AV_INPUT_BUFFER_PADDING_SIZE);
        s->packet = ff.packet_alloc();
        s->frame = ff.frame_alloc();
        s->parser = ff.parser_init(AV_CODEC_ID_MP3);
        fprintf(stderr, "mp3: a stream of %llu bytes reserved\n", (unsigned long long)(s->end - s->start));
        psp_ret(h);
        return;
    }
    psp_ret(MP3_ERROR_HANDLE);
}

static void hle_Mp3ReleaseMp3Handle(void) {
    mp3_stream *s = mp3_of(psp_arg(0));
    if (s) mp3_close(s);
    psp_ret(0);
}

/* (handle, u8 **dst, int *towrite, int *srcpos): always the start of the file's buffer, as all
 * that was added before has been taken out. */
static void hle_Mp3GetInfoToAddStreamData(void) {
    mp3_stream *s = mp3_of(psp_arg(0));
    if (!s) { psp_ret(MP3_ERROR_HANDLE); return; }
    uint64_t left = s->end > s->readpos ? s->end - s->readpos : 0;
    uint32_t room = MP3_FIFO - s->fifo_len;
    uint32_t write = s->buf_size;
    if (write > room) write = room;
    if (write > left) write = (uint32_t)left;
    if (psp_arg(1)) psp_write32(psp_arg(1), s->buf);
    if (psp_arg(2)) psp_write32(psp_arg(2), write);
    if (psp_arg(3)) psp_write32(psp_arg(3), (uint32_t)s->readpos);
    psp_ret(0);
}

/* (handle, size) */
static void hle_Mp3NotifyAddStreamData(void) {
    mp3_stream *s = mp3_of(psp_arg(0));
    uint32_t size = psp_arg(1);
    if (!s) { psp_ret(MP3_ERROR_HANDLE); return; }
    if (size > MP3_FIFO - s->fifo_len) size = MP3_FIFO - s->fifo_len;
    if (size && psp_mem_read_block(s->fifo + s->fifo_len, s->buf, size) == 0) s->fifo_len += size;
    s->readpos += psp_arg(1);
    psp_ret(0);
}

static void hle_Mp3CheckStreamDataNeeded(void) {
    mp3_stream *s = mp3_of(psp_arg(0));
    if (!s) { psp_ret(MP3_ERROR_HANDLE); return; }
    psp_ret(s->readpos < s->end && s->fifo_len + s->buf_size <= MP3_FIFO ? 1 : 0);
}

/* (handle): the stream's header, and the decoder for it. */
static void hle_Mp3Init(void) {
    mp3_stream *s = mp3_of(psp_arg(0));
    if (!s) { psp_ret(MP3_ERROR_HANDLE); return; }
    if (!mp3_header(s)) {
        if (other_open(s)) {
            s->ready = 1;
            psp_ret(0);
            return;
        }
        fprintf(stderr, "mp3: no MP3 frame found in the first %u bytes\n", s->fifo_len);
        psp_ret(0x80671100u);
        return;
    }
    const AVCodec *codec = ff.find_decoder(AV_CODEC_ID_MP3);
    s->codec = codec ? ff.alloc_context3(codec) : NULL;
    if (!s->codec || ff.open2(s->codec, codec, NULL) < 0) {
        fprintf(stderr, "mp3: no MP3 decoder in FFmpeg\n");
        if (s->codec) ff.free_context(&s->codec);
        psp_ret(MP3_ERROR_NOT_READY);
        return;
    }
    s->ready = 1;
    fprintf(stderr, "mp3: %d Hz, %d channels, %d kbit/s\n", s->rate, s->channels, s->bitrate);
    psp_ret(0);
}

/* The next frame's samples, as 16 bits interleaved, into the samples' buffer. Gives back the bytes
 * written, 0 once the stream has ended. */
static uint32_t mp3_decode(mp3_stream *s) {
    for (;;) {
        /* What the decoder holds comes first: a frame sent before may be ready only now, and while
         * it waits the decoder takes no more, so a frame sent then would be lost, and the next, and
         * so on through the whole file. */
        if (ff.receive_frame(s->codec, s->frame) < 0) {
            uint8_t *out = NULL;
            int out_size = 0;
            const int eof = s->readpos >= s->end;
            const int used = ff.parser_parse2(s->parser, s->codec, &out, &out_size, s->fifo, (int)s->fifo_len,
                                              AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used > 0) {
                memmove(s->fifo, s->fifo + used, s->fifo_len - (uint32_t)used);
                s->fifo_len -= (uint32_t)used;
            }
            if (!out_size) {
                if (s->fifo_len && used > 0) continue;
                if (!eof) return MP3_NEED_DATA;   /* more of the file is to come first */
                if (s->flushed) return 0;
                /* The parser keeps the last frame, and the decoder its last samples, until told
                 * there is no more. */
                s->flushed = 1;
                ff.parser_parse2(s->parser, s->codec, &out, &out_size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                if (!out_size) {
                    ff.send_packet(s->codec, NULL);
                    continue;
                }
            }
            s->packet->data = out;
            s->packet->size = out_size;
            ff.send_packet(s->codec, s->packet);
            s->packet->data = NULL;
            s->packet->size = 0;
            continue;
        }
        const AVFrame *f = s->frame;
        const int channels = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : s->channels;
        const int out_channels = s->channels;
        uint32_t samples = (uint32_t)f->nb_samples;
        if (samples * (uint32_t)out_channels * 2 > s->pcm_size) samples = s->pcm_size / ((uint32_t)out_channels * 2);
        int16_t *pcm = (int16_t *)psp_mem_ptr(s->pcm, samples * (uint32_t)out_channels * 2);
        if (!pcm) return 0;
        for (uint32_t i = 0; i < samples; i++) {
            for (int c = 0; c < out_channels; c++) {
                const int from = c < channels ? c : channels - 1;
                float v = 0.0f;
                if (f->format == AV_SAMPLE_FMT_FLTP) v = ((const float *)f->data[from])[i];
                else if (f->format == AV_SAMPLE_FMT_FLT) v = ((const float *)f->data[0])[i * channels + from];
                else if (f->format == AV_SAMPLE_FMT_S16P) v = ((const int16_t *)f->data[from])[i] / 32768.0f;
                else if (f->format == AV_SAMPLE_FMT_S16) v = ((const int16_t *)f->data[0])[i * channels + from] / 32768.0f;
                int x = (int)(v * 32767.0f);
                pcm[i * (uint32_t)out_channels + (uint32_t)c] = (int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x);
            }
        }
        s->decoded += samples;
        return samples * (uint32_t)out_channels * 2;
    }
}

/* (handle, s16 **dst) */
static void hle_Mp3Decode(void) {
    mp3_stream *s = mp3_of(psp_arg(0));
    if (!s || !s->ready) { psp_ret(MP3_ERROR_HANDLE); return; }
    uint32_t bytes = s->other ? other_decode(s) : mp3_decode(s);
    if (bytes == MP3_NEED_DATA) {
        /* As the console's: the game adds more of the file and asks again. */
        psp_ret(MP3_ERROR_NEED_DATA);
        return;
    }
    if (!bytes && s->readpos >= s->end && s->loops != 0) {
        if (s->loops > 0) s->loops--;
        mp3_rewind(s);
    }
    if (psp_arg(1)) psp_write32(psp_arg(1), s->pcm);
    psp_ret(bytes);
}

static void hle_Mp3SetLoopNum(void) {
    mp3_stream *s = mp3_of(psp_arg(0));
    if (s) s->loops = (int)psp_arg(1);
    psp_ret(s ? 0 : MP3_ERROR_HANDLE);
}
static void hle_Mp3ResetPlayPosition(void) {
    mp3_stream *s = mp3_of(psp_arg(0));
    if (s) mp3_rewind(s);
    psp_ret(s ? 0 : MP3_ERROR_HANDLE);
}
static void hle_Mp3GetSumDecodedSample(void) {
    mp3_stream *s = mp3_of(psp_arg(0));
    psp_ret(s ? (uint32_t)s->decoded : MP3_ERROR_HANDLE);
}
static void hle_Mp3GetSamplingRate(void) { mp3_stream *s = mp3_of(psp_arg(0)); psp_ret(s ? (uint32_t)s->rate : MP3_ERROR_HANDLE); }
static void hle_Mp3GetMp3ChannelNum(void) { mp3_stream *s = mp3_of(psp_arg(0)); psp_ret(s ? (uint32_t)s->channels : MP3_ERROR_HANDLE); }
static void hle_Mp3GetBitRate(void) { mp3_stream *s = mp3_of(psp_arg(0)); psp_ret(s ? (uint32_t)s->bitrate : MP3_ERROR_HANDLE); }
static void hle_Mp3GetMaxOutputSample(void) {
    mp3_stream *s = mp3_of(psp_arg(0));
    psp_ret(s ? (s->rate >= 32000 ? 1152u : 576u) : MP3_ERROR_HANDLE);
}
#else
static void hle_Mp3Missing(void) { psp_ret(MP3_ERROR_NOT_READY); }
#define hle_Mp3InitResource hle_Mp3Missing
#define hle_Mp3TermResource hle_Mp3Missing
#define hle_Mp3ReserveMp3Handle hle_Mp3Missing
#define hle_Mp3ReleaseMp3Handle hle_Mp3Missing
#define hle_Mp3GetInfoToAddStreamData hle_Mp3Missing
#define hle_Mp3NotifyAddStreamData hle_Mp3Missing
#define hle_Mp3CheckStreamDataNeeded hle_Mp3Missing
#define hle_Mp3Init hle_Mp3Missing
#define hle_Mp3Decode hle_Mp3Missing
#define hle_Mp3SetLoopNum hle_Mp3Missing
#define hle_Mp3ResetPlayPosition hle_Mp3Missing
#define hle_Mp3GetSumDecodedSample hle_Mp3Missing
#define hle_Mp3GetSamplingRate hle_Mp3Missing
#define hle_Mp3GetMp3ChannelNum hle_Mp3Missing
#define hle_Mp3GetBitRate hle_Mp3Missing
#define hle_Mp3GetMaxOutputSample hle_Mp3Missing
#endif

void psp_mpeg_init(void) {}

void psp_mpeg_register(void) {
    psp_hle_register(0x682A619B, "sceMpeg", "sceMpegInit",                    hle_Init);
    psp_hle_register(0x874624D6, "sceMpeg", "sceMpegFinish",                  hle_Finish);
    psp_hle_register(0xC132E22F, "sceMpeg", "sceMpegQueryMemSize",            hle_QueryMemSize);
    psp_hle_register(0xD7A29F46, "sceMpeg", "sceMpegRingbufferQueryMemSize",  hle_RingbufferQueryMemSize);
    psp_hle_register(0x37295ED8, "sceMpeg", "sceMpegRingbufferConstruct",     hle_RingbufferConstruct);
    psp_hle_register(0x13407F13, "sceMpeg", "sceMpegRingbufferDestruct",      hle_RingbufferDestruct);
    psp_hle_register(0xB240A59E, "sceMpeg", "sceMpegRingbufferPut",           hle_RingbufferPut);
    psp_hle_register(0xB5F6DC87, "sceMpeg", "sceMpegRingbufferAvailableSize", hle_RingbufferAvailableSize);
    psp_hle_register(0xD8C5F121, "sceMpeg", "sceMpegCreate",                  hle_Create);
    psp_hle_register(0x606A4649, "sceMpeg", "sceMpegDelete",                  hle_Delete);
    psp_hle_register(0x21FF80E4, "sceMpeg", "sceMpegQueryStreamOffset",       hle_QueryStreamOffset);
    psp_hle_register(0x611E9E11, "sceMpeg", "sceMpegQueryStreamSize",         hle_QueryStreamSize);
    psp_hle_register(0x42560F23, "sceMpeg", "sceMpegRegistStream",            hle_RegistStream);
    psp_hle_register(0x591A4AA2, "sceMpeg", "sceMpegUnRegistStream",          hle_UnRegistStream);
    psp_hle_register(0xA780CF7E, "sceMpeg", "sceMpegMallocAvcEsBuf",          hle_MallocAvcEsBuf);
    psp_hle_register(0xCEB870B1, "sceMpeg", "sceMpegFreeAvcEsBuf",            hle_FreeAvcEsBuf);
    psp_hle_register(0xF8DCB679, "sceMpeg", "sceMpegQueryAtracEsSize",        hle_QueryAtracEsSize);
    psp_hle_register(0x167AFD9E, "sceMpeg", "sceMpegInitAu",                  hle_InitAu);
    psp_hle_register(0xFE246728, "sceMpeg", "sceMpegGetAvcAu",                hle_GetAvcAu);
    psp_hle_register(0xE1CE83A7, "sceMpeg", "sceMpegGetAtracAu",              hle_GetAtracAu);
    psp_hle_register(0x0E3C2E9D, "sceMpeg", "sceMpegAvcDecode",               hle_AvcDecode);
    psp_hle_register(0x0F6C18D7, "sceMpeg", "sceMpegAvcDecodeDetail",         hle_AvcDecodeDetail);
    psp_hle_register(0x740FCCD1, "sceMpeg", "sceMpegAvcDecodeStop",           hle_AvcDecodeStop);
    psp_hle_register(0x4571CC64, "sceMpeg", "sceMpegAvcDecodeFlush",          hle_AvcDecodeFlush);
    psp_hle_register(0x800C44DF, "sceMpeg", "sceMpegAtracDecode",             hle_AtracDecode);
    psp_hle_register(0x707B7629, "sceMpeg", "sceMpegFlushAllStream",          hle_FlushAllStream);
    psp_hle_register(0x35750070, "sceMp3", "sceMp3InitResource",            hle_Mp3InitResource);
    psp_hle_register(0x3C2FA058, "sceMp3", "sceMp3TermResource",            hle_Mp3TermResource);
    psp_hle_register(0x07EC321A, "sceMp3", "sceMp3ReserveMp3Handle",        hle_Mp3ReserveMp3Handle);
    psp_hle_register(0xF5478233, "sceMp3", "sceMp3ReleaseMp3Handle",        hle_Mp3ReleaseMp3Handle);
    psp_hle_register(0xA703FE0F, "sceMp3", "sceMp3GetInfoToAddStreamData",  hle_Mp3GetInfoToAddStreamData);
    psp_hle_register(0x0DB149F4, "sceMp3", "sceMp3NotifyAddStreamData",     hle_Mp3NotifyAddStreamData);
    psp_hle_register(0xD0A56296, "sceMp3", "sceMp3CheckStreamDataNeeded",   hle_Mp3CheckStreamDataNeeded);
    psp_hle_register(0x44E07129, "sceMp3", "sceMp3Init",                    hle_Mp3Init);
    psp_hle_register(0xD021C0FB, "sceMp3", "sceMp3Decode",                  hle_Mp3Decode);
    psp_hle_register(0x3CEF484F, "sceMp3", "sceMp3SetLoopNum",              hle_Mp3SetLoopNum);
    psp_hle_register(0x2A368661, "sceMp3", "sceMp3ResetPlayPosition",       hle_Mp3ResetPlayPosition);
    psp_hle_register(0x354D27EA, "sceMp3", "sceMp3GetSumDecodedSample",     hle_Mp3GetSumDecodedSample);
    psp_hle_register(0x8F450998, "sceMp3", "sceMp3GetSamplingRate",         hle_Mp3GetSamplingRate);
    psp_hle_register(0x7F696782, "sceMp3", "sceMp3GetMp3ChannelNum",        hle_Mp3GetMp3ChannelNum);
    psp_hle_register(0x87677E40, "sceMp3", "sceMp3GetBitRate",              hle_Mp3GetBitRate);
    psp_hle_register(0x87C263D1, "sceMp3", "sceMp3GetMaxOutputSample",      hle_Mp3GetMaxOutputSample);
}
