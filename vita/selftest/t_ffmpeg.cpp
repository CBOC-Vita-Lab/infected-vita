// T9 - FFmpeg decode. libavcodec 7.1.5 built with only atrac3, atrac3plus, h264 (+ h264 parser), NEON asm, no threads
// (build variant "nopt"; "pt" = pthreads, enabled with -DST_FFMPEG_PT). This TU is compiled with -fno-short-enums because the
// FFmpeg headers declare enums inside structs (the vitasdk compiler default is -fshort-enums, FFmpeg is built without).
//
//   H.264 : an embedded 480x272 12-frame Constrained Baseline stream (testsrc2 + noise, made with scripts/gen_t9_streams.py)
//           is pushed through the real h264 parser + decoder; every decoded YUV420p frame is CRC32-checked against the CRC
//           of the reference decoder (H.264 is bit exact), and the time per frame is reported.
//   ATRAC : there is no ATRAC3/3plus encoder available, so this only opens/closes both decoders (which runs their av_tx MDCT
//           init - i.e. proves the float-only av_tx patch did not remove something they need) and feeds a few synthetic
//           blocks to show the error path costs nothing and does not crash. Real ATRAC3+ frames come from the game.
#include "st_common.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}

#include "t9_streams.h"

namespace st {

namespace {

void av_log_cb(void*, int level, const char* fmt, va_list ap) {
    if (level > AV_LOG_WARNING) return;
    char buf[400];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    if (n) plat::logf("[ffmpeg %d] %s", level, buf);
}

uint32_t frame_crc(const AVFrame* f) {
    uint32_t crc = 0;
    for (int p = 0; p < 3; p++) {
        int w = p ? (f->width + 1) / 2 : f->width;
        int h = p ? (f->height + 1) / 2 : f->height;
        for (int y = 0; y < h; y++) crc = crc32_update(crc, f->data[p] + (size_t)y * f->linesize[p], (size_t)w);
    }
    return crc;
}

struct DecResult {
    int frames = 0, mismatches = 0, errors = 0;
    double first_ms = 0, rest_avg_ms = 0, rest_max_ms = 0, total_ms = 0;
    int w = 0, h = 0, pix_fmt = -1;
};

bool decode_h264(int threads, DecResult* out, char* why, size_t why_n) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { snprintf(why, why_n, "avcodec_find_decoder(H264) returned NULL"); return false; }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!ctx || !parser || !pkt || !frame) { snprintf(why, why_n, "allocation failed (ctx %p parser %p pkt %p frame %p)", (void*)ctx, (void*)parser, (void*)pkt, (void*)frame); return false; }
    ctx->thread_count = threads;
    ctx->thread_type = threads > 1 ? FF_THREAD_FRAME : 0;
    int rc = avcodec_open2(ctx, codec, nullptr);
    if (rc < 0) { snprintf(why, why_n, "avcodec_open2 -> %d", rc); avcodec_free_context(&ctx); return false; }

    size_t pos = 0;
    int idx = 0;
    double sum_rest = 0;
    int rest_n = 0;
    auto receive = [&]() {
        for (;;) {
            uint64_t t0 = plat::now_us();
            int r = avcodec_receive_frame(ctx, frame);
            double ms = (double)(plat::now_us() - t0) / 1000.0;
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
            if (r < 0) { out->errors++; return; }
            if (idx == 0) out->first_ms += ms; else { sum_rest += ms; if (ms > out->rest_max_ms) out->rest_max_ms = ms; }
            out->total_ms += ms;
            out->w = frame->width; out->h = frame->height; out->pix_fmt = frame->format;
            uint32_t crc = frame_crc(frame);
            if (idx < kT9Frames && crc != kT9FrameCrc[idx]) out->mismatches++;
            idx++;
            out->frames++;
            av_frame_unref(frame);
        }
    };
    while (pos < kT9H264Size) {
        size_t chunk = kT9H264Size - pos > 4096 ? 4096 : kT9H264Size - pos;
        const uint8_t* in = kT9H264 + pos;
        int left = (int)chunk;
        while (left > 0) {
            uint8_t* data = nullptr;
            int size = 0;
            int used = av_parser_parse2(parser, ctx, &data, &size, in, left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            in += used;
            left -= used;
            if (size > 0) {
                pkt->data = data;
                pkt->size = size;
                uint64_t t0 = plat::now_us();
                int r = avcodec_send_packet(ctx, pkt);
                double ms = (double)(plat::now_us() - t0) / 1000.0;
                // decode work happens inside send_packet for the non-threaded decoder: attribute it to the frame being produced
                if (idx == 0 && out->frames == 0) out->first_ms += ms; else { sum_rest += ms; if (ms > out->rest_max_ms) out->rest_max_ms = ms; }
                out->total_ms += ms;
                if (r < 0) out->errors++;
                receive();
            }
            if (used == 0 && size == 0) break;
        }
        pos += chunk;
    }
    // the parser holds the last access unit until it sees more data: flush it, then drain the decoder
    {
        uint8_t* data = nullptr;
        int size = 0;
        av_parser_parse2(parser, ctx, &data, &size, nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (size > 0) {
            pkt->data = data;
            pkt->size = size;
            uint64_t t0 = plat::now_us();
            int r = avcodec_send_packet(ctx, pkt);
            double ms = (double)(plat::now_us() - t0) / 1000.0;
            sum_rest += ms;
            if (ms > out->rest_max_ms) out->rest_max_ms = ms;
            out->total_ms += ms;
            if (r < 0) out->errors++;
            receive();
        }
    }
    avcodec_send_packet(ctx, nullptr);
    receive();
    rest_n = out->frames > 1 ? out->frames - 1 : 1;
    out->rest_avg_ms = sum_rest / rest_n;
    av_parser_close(parser);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return true;
}

void atrac_open_close() {
    plat::MemInfo m0, m1;
    // ---- ATRAC3plus: stereo 44.1 kHz, 128 kbps (block_align 0x2E8)
    {
        plat::mem_info(&m0);
        const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
        if (!c) { fail("T9", "atrac3plus", "decoder not compiled in"); }
        else {
            AVCodecContext* ctx = avcodec_alloc_context3(c);
            av_channel_layout_default(&ctx->ch_layout, 2);
            ctx->sample_rate = 44100;
            ctx->block_align = 0x2E8;
            ctx->bit_rate = 128000;
            uint64_t t0 = plat::now_us();
            int rc = avcodec_open2(ctx, c, nullptr);
            uint64_t open_us = plat::now_us() - t0;
            plat::mem_info(&m1);
            if (rc < 0) {
                fail("T9", "atrac3plus-open", "avcodec_open2 -> %d", rc);
            } else {
                pass("T9", "atrac3plus-open", "opened '%s' (stereo 44100 Hz, block_align %d) in %llu us; newlib heap grew by %lld KiB (MDCT tables, IPQF, channel units)", c->name, ctx->block_align,
                     (unsigned long long)open_us, (long long)((m1.malloc_used - m0.malloc_used) / 1024));
                // synthetic blocks: start bit 0, channel-unit id 01 (pair), then zeros / noise. They will mostly be rejected - that is the point.
                AVPacket* pkt = av_packet_alloc();
                AVFrame* fr = av_frame_alloc();
                uint8_t* blk = (uint8_t*)av_mallocz(0x2E8 + AV_INPUT_BUFFER_PADDING_SIZE);
                uint32_t lcg = 12345;
                for (int k = 0; k < 4; k++) {
                    memset(blk, 0, 0x2E8);
                    blk[0] = 0x20;   // 0 01 00000
                    if (k >= 2) for (int i = 1; i < 0x2E8; i++) { lcg = lcg * 1664525u + 1013904223u; blk[i] = (uint8_t)(lcg >> 24); }
                    pkt->data = blk;
                    pkt->size = 0x2E8;
                    uint64_t a = plat::now_us();
                    int r1 = avcodec_send_packet(ctx, pkt);
                    int r2 = avcodec_receive_frame(ctx, fr);
                    uint64_t d = plat::now_us() - a;
                    info("T9", "atrac3plus-block", "synthetic block %d (%s): send_packet %d, receive_frame %d (%s), %llu us%s", k, k < 2 ? "start bit + zeros" : "start bit + LCG noise", r1, r2,
                         r2 == 0 ? "got a frame" : "no frame", (unsigned long long)d, r2 == 0 ? "" : "");
                    if (r2 == 0) { info("T9", "atrac3plus-frame", "decoded %d samples x %d ch, fmt %d", fr->nb_samples, fr->ch_layout.nb_channels, fr->format); av_frame_unref(fr); }
                }
                av_free(blk);
                av_frame_free(&fr);
                av_packet_free(&pkt);
            }
            avcodec_free_context(&ctx);
        }
    }
    // ---- ATRAC3 (LP2/LP4 style): needs the 14-byte WAV extradata. joint stereo, 132 kbps -> block_align 384
    {
        const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_ATRAC3);
        if (!c) { fail("T9", "atrac3", "decoder not compiled in"); return; }
        AVCodecContext* ctx = avcodec_alloc_context3(c);
        av_channel_layout_default(&ctx->ch_layout, 2);
        ctx->sample_rate = 44100;
        ctx->block_align = 384;
        ctx->extradata = (uint8_t*)av_mallocz(14 + AV_INPUT_BUFFER_PADDING_SIZE);
        ctx->extradata_size = 14;
        const uint8_t ex[14] = {1, 0, 0x00, 0x04, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0};   // [0..1]=1 samples/ch=1024 coding_mode=1(joint) dup=1 frame_factor=1 [12..13]=0
        memcpy(ctx->extradata, ex, 14);
        uint64_t t0 = plat::now_us();
        int rc = avcodec_open2(ctx, c, nullptr);
        uint64_t open_us = plat::now_us() - t0;
        check(rc >= 0, "T9", "atrac3-open", "avcodec_open2(atrac3, joint stereo, block_align 384, 14-byte WAV extradata) -> %d in %llu us", rc, (unsigned long long)open_us);
        avcodec_free_context(&ctx);
    }
}

}  // namespace

void t9_ffmpeg() {
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(av_log_cb);
    unsigned v = avcodec_version();
    info("T9", "libavcodec", "version %u.%u.%u, configuration: %.300s", v >> 16, (v >> 8) & 255, v & 255, avcodec_configuration());
    {
        char names[256] = "";
        void* it = nullptr;
        int n = 0;
        while (const AVCodec* c = av_codec_iterate(&it)) {
            if (!av_codec_is_decoder(c)) continue;
            snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s", n ? ", " : "", c->name);
            n++;
        }
        const bool ok = n == 3;
        check(ok, "T9", "decoder-list", "%d decoders compiled in: %s (expected exactly atrac3, atrac3plus, h264)", n, names);
    }

    // ---- H.264 through parser + decoder, single thread
    const int loops = cfg_int("t9.loops", 3);
    DecResult best;
    bool have = false;
    char why[160] = "";
    for (int l = 0; l < loops; l++) {
        DecResult r;
        if (!decode_h264(1, &r, why, sizeof(why))) { fail("T9", "h264", "setup failed: %s", why); have = false; break; }
        if (!have || r.total_ms < best.total_ms) best = r;
        have = true;
        poll_abort("T9");
    }
    if (have) {
        bool ok = best.frames == kT9Frames && best.mismatches == 0 && best.errors == 0 && best.w == kT9Width && best.h == kT9Height;
        check(ok, "T9", "h264-decode", "%dx%d pix_fmt %d: %d frames (want %d), %d CRC mismatches, %d errors | single thread, best of %d runs: IDR %.2f ms, P frames avg %.2f ms max %.2f ms, whole stream %.1f ms = %.1f fps equivalent (%.1f%% of a 30 fps real-time budget on one core)",
              best.w, best.h, best.pix_fmt, best.frames, kT9Frames, best.mismatches, best.errors, loops, best.first_ms, best.rest_avg_ms, best.rest_max_ms, best.total_ms, best.total_ms > 0 ? 1000.0 * best.frames / best.total_ms : 0.0,
              (best.total_ms / best.frames) / 33.3 * 100.0);
        info("T9", "h264-note", "synthetic low-motion content at ~%.0f kbit/s; real UMD video bitrates and motion cost more per frame. Stream decode uses NEON asm (h264 idct/pred/qpel/deblock) unless the build log says otherwise.",
             (double)kT9H264Size * 8.0 / (kT9Frames / 30.0) / 1000.0);
    }
#ifdef ST_FFMPEG_PT
    if (have) {
        DecResult r2;
        if (decode_h264(2, &r2, why, sizeof(why))) {
            check(r2.frames == kT9Frames && r2.mismatches == 0, "T9", "h264-decode-2threads", "frame threading (2 threads, pthreads build): %d frames, %d mismatches, whole stream %.1f ms (single thread best %.1f ms)", r2.frames, r2.mismatches, r2.total_ms, best.total_ms);
        } else {
            fail("T9", "h264-decode-2threads", "setup failed: %s", why);
        }
    }
#endif
    atrac_open_close();
}

}  // namespace st
