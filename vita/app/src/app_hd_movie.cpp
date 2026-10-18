// app_hd_movie.cpp - the hardware source of the HD replacement movies (see app_hd_movie.h and docs/HD_MOVIES.md).
//
// How a picture gets from the file to the screen on the Vita:
//
//   1. The framework's HdSession asks frame_for() for the picture of access unit n (the game is decoding that unit).
//   2. The source reads sample n of the replacement MP4 (host/movie/mp4_reader: plain stdio, ux0:data/infected/movies_hd/),
//      copies it as Annex B into a physically contiguous buffer and hands it to the hardware decoder (sceAvcdecDecode). The
//      decoder writes 4:2:0 into one of a few frame slots (physically contiguous memory, mapped for the GPU).
//   3. The frame goes to the framework as a "native" HdPicture. When the game's own draw samples the marker texture, the
//      renderer asks the binder below for a GL texture; the binder keeps one GL texture per frame slot, each of which is a
//      vitaGL texture whose GXM descriptor was re-pointed at the slot (YUV420 format). The GPU converts YUV to RGB while it
//      samples: no CPU conversion, no copy.
//
// Lockstep, not free-running: nothing here has a clock. A picture is decoded when the game asks for it, exactly as the
// desktop's software source does, so the replacement cannot drift from the audio or from the game's skip and pause handling.
// A jump (skip, a movie started in the middle, the setting switched on mid-movie) restarts from the sync sample before the
// wanted picture; the encoder makes one about every second (build_movies.sh: -g 30).
//
// The facts the picture format rests on, and where each comes from (nothing here has run on a console yet; the Vita3K emulator
// implements the same facts, which is what the first run could check):
//
//  1. Output layout. vitasdk psp2/videodec.h: SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER (0x20) is the luma plane followed by one
//     plane of interleaved chroma, SCE_AVCDEC_PIXELFORMAT_YUV420_RASTER (0x10) three planes. Vita3K (vita3k/modules/SceVideodec/
//     SceVideodecUser.cpp, vita3k/codec/src/h264.cpp copy_yuv_data_from_frame) writes the packed one as Y, then U V U V ... (NV12) and
//     the planar one as Y, U, V (I420). Default here: packed, U first.
//  2. How GXM names them. A GXM texture format has a base (YUV420P2: luma plane + one chroma plane, YUV420P3: three planes), a chroma
//     order and a colour conversion slot (CSC0 or CSC1). For the three-plane format the names read as they say (I420 = YUV420P3). For the
//     two-plane format SCE's names are the other way round: NV12 (U first) is YVU420P2. Sources: SDL's Vita renderer (SDL_render_vita_gxm.c:
//     NV12 -> YVU420P2_CSC0, IYUV -> YUV420P3_CSC0, with the comment "should be the other way around. looks like SCE bug"), vitaGL
//     (source/textures.c: VGL_YUV420P_NV12_BT601 -> YVU420P2_CSC0, VGL_YVU420P_NV21_BT601 -> YUV420P2_CSC0, VGL_YUV420P_BT601 -> YUV420P3_CSC0),
//     vitaGL's samples/video_playback (frames of sceAvPlayer, NV12, bound as YVU420P2).
//  3. Which matrix. vitaGL maps its *_BT601 formats to CSC0 and *_BT709 to CSC1; SDL programs the CSC0 slot with sceGxmSetYuvProfile(ctx, 0,
//     SCE_GXM_YUV_PROFILE_BT601_STANDARD) for limited-range BT.601 content (vitasdk gxm.h lists BT601/BT709 x STANDARD/FULL_RANGE). vitaGL
//     never calls sceGxmSetYuvProfile, so what its BT601 formats get is the context's default for slot 0. The replacement movies are
//     limited-range BT.601 (build_movies.sh), so the default here is CSC0.
//  4. The descriptor's mip_count field must stay 15 ("no mip chain": what sceGxmTextureInitLinear(.., 0) writes, as in vitaGL's video_playback).
//     vitaGL's glTexParameteri(GL_TEXTURE_MIN_FILTER) on a GL_TEXTURE_2D rewrites it to 0 (textures.c: vglSetTexMipmapCount(.., use_mips ?
//     mip_count : 1)), and the texture unit places the chroma plane right behind the luma plane only for 15; with 0 Vita3K looks for it behind a
//     power-of-two sized luma area (renderer/src/texture/cache.cpp, layout_width/layout_height): the green and magenta picture of the first
//     emulator run, whose chroma came out of the neighbouring frame slots. The renderer therefore never calls glTexParameteri on these textures
//     (gl_renderer.cpp, resolve_hd_movie seeds its sampler cache) and the binder below writes the descriptor again every time it is used.
//
// What a real console may still do differently: the chroma byte order of the decoder, the default profile of CSC0. hd_movie_layout,
// hd_movie_chroma and hd_movie_csc in config.txt are the escape hatches (docs/HD_MOVIES_HARDWARE_TEST.md).
//
// When something goes wrong the original picture is shown, never a stuck or broken one: a replacement file that cannot be opened or does
// not fit (open() checks it before the decoder is touched), a sample that cannot be read or decoded (the pictures of that group of pictures
// that follow it refer to it, so the original shows the rest of the group and the replacement returns at the next sync sample), frame slots
// all held (the original shows that picture), eight failures in a row (the original plays for the rest of the movie). The framework feeds the
// original decoder the units it has not seen whenever it takes over (HdSession::show, the replay), so its own pictures are right.
//
// Memory: the decoder, its stream buffer and the frame slots (about 13 MiB of physically contiguous memory) exist only while a movie
// with a replacement is playing, plus a second or so after the last picture: the renderer calls Binder::release_frames() when nothing has
// drawn a replacement picture for 120 frames and no movie exists; the GL textures of the slots are deleted, the memory freed, the decoder
// library terminated and the module unloaded. The next movie sets it all up again (about the cost of loading a module).
#include "app_hd_movie.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>
#include <vitaGL.h>

#include "app_config.h"
#include "gpu/gl/gl_hd_movie.hpp"
#include "movie/hd_movie.hpp"
#include "movie/mp4_reader.hpp"
#include "platform.h"

namespace app {
namespace {

using portablekit::movie::HdPicture;
using portablekit::movie::HdSource;
using portablekit::movie::kTicksPerPicture;
using portablekit::movie::Mp4Reader;

constexpr uint32_t kMiB = 1024u * 1024u;
constexpr uint32_t kWidth = 960u, kHeight = 544u;
constexpr uint32_t kLumaBytes = kWidth * kHeight;
constexpr uint32_t kFrameBytes = kLumaBytes * 3u / 2u;      // 783360
constexpr uint32_t kSlotBytes = 0xC0000u;                   // 768 KiB: a frame, rounded up to 4 KiB (twelve fill 9 MiB)
constexpr int kSlots = 12;                                  // the framework's store holds 8, the renderer 1, the source 1 + 1 being decoded
constexpr uint32_t kEsBytes = 1u * kMiB;                    // the biggest sample of a 2.8 Mbit/s movie is well below
constexpr uint32_t kRefFrames = 4u;                         // the encoder uses 3 (build_movies.sh: -refs 3)
constexpr uint32_t kPixelPlanar = SCE_AVCDEC_PIXELFORMAT_YUV420_RASTER;           // Y, U, V
constexpr uint32_t kPixelPacked = SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER;    // Y, UV interleaved

struct Block {
    SceUID uid{-1};
    void* base{};
    uint32_t size{};
};

// Blocks of this file that exist right now. The log prints it with every memory line: after the last movie it is 0. On a console the
// kernel's own free-memory figure shows the same; Vita3K subtracts the wrong size when a block is freed (SceSysmemForDriver.cpp takes
// KernelMemBlock::size, which holds sizeof(SceKernelMemBlockInfo), not the size of the block), so only this counter proves the release there.
int g_blocks_live = 0;

bool alloc_phycont(const char* name, uint32_t bytes, bool gpu, Block& out, std::string& error) {
    const uint32_t size = (bytes + kMiB - 1u) & ~(kMiB - 1u);   // phycont blocks are 1 MiB granular
    const SceUID uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, size, nullptr);
    if (uid < 0) {
        char text[96];
        snprintf(text, sizeof(text), "no physically contiguous memory for %s (%u KiB, error 0x%08X)", name, size / 1024u, (unsigned)uid);
        error = text;
        return false;
    }
    void* base = nullptr;
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || base == nullptr) {
        sceKernelFreeMemBlock(uid);
        error = std::string("cannot map the block of ") + name;
        return false;
    }
    if (gpu) {
        const int r = sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_RW);
        if (r < 0) {
            sceKernelFreeMemBlock(uid);
            char text[96];
            snprintf(text, sizeof(text), "sceGxmMapMemory failed for %s (0x%08X)", name, (unsigned)r);
            error = text;
            return false;
        }
    }
    out.uid = uid;
    out.base = base;
    out.size = size;
    ++g_blocks_live;
    return true;
}

void free_block(Block& block, bool gpu) {
    if (block.uid < 0) return;
    if (gpu && block.base != nullptr) sceGxmUnmapMemory(block.base);
    const int r = sceKernelFreeMemBlock(block.uid);
    if (r < 0)
        plat::logf("hdmovie: sceKernelFreeMemBlock failed (0x%08X)", (unsigned)r);
    else
        --g_blocks_live;
    block = Block{};
}

// One slot of decoded frames. `texture` belongs to the render thread (the binder fills it in and deletes it).
struct Frame {
    uint8_t* data{};
    int slot{};
    GLuint texture{};
};

// The process-wide state of the decoder: library, memory, frame slots. Set up when the first source of a movie opens (guest thread),
// given back by trim() (render thread) once no source is open, no frame is held and the renderer has stopped drawing replacements.
class Device {
public:
    static Device& get() {
        static Device device;
        return device;
    }

    // Guest thread. One user per open source; the first one sets everything up (again, after a trim). False and `error` when the
    // decoder cannot be had: a failed module load or library init is final for the process, a failed allocation is tried again at the
    // next movie.
    bool acquire(std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_) {
            error = error_;
            return false;
        }
        if (!ready_ && !setup()) {
            error = error_;
            return false;
        }
        ++users_;
        return true;
    }
    void release_user() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (users_ > 0) --users_;
    }

    // A free frame slot, null when all are held. Returns to the pool when the last reference goes (any thread).
    std::shared_ptr<void> acquire_frame() {
        Frame* frame = nullptr;
        {
            std::lock_guard<std::mutex> lock(frames_mutex_);
            if (free_.empty()) return nullptr;
            frame = free_.front();   // oldest released first: the GPU has long finished with it
            free_.pop_front();
        }
        return std::shared_ptr<void>(frame, [this](void* p) {
            std::lock_guard<std::mutex> lock(frames_mutex_);
            free_.push_back(static_cast<Frame*>(p));
        });
    }

    uint8_t* es() const { return static_cast<uint8_t*>(es_.base); }

    // One decoder at a time (numOfStreams = 1).
    bool open_decoder(SceAvcdecCtrl& ctrl, std::string& error) {
        if (decoder_busy_.exchange(true)) {
            error = "the hardware decoder is in use";
            return false;
        }
        SceAvcdecQueryDecoderInfo query{};
        query.horizontal = kWidth;
        query.vertical = kHeight;
        query.numOfRefFrames = kRefFrames;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.frameBuf.pBuf = decoder_.base;
        ctrl.frameBuf.size = decoder_.size;
        const int r = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &ctrl, &query);
        if (r < 0) {
            decoder_busy_ = false;
            char text[96];
            snprintf(text, sizeof(text), "sceAvcdecCreateDecoder failed (0x%08X)", (unsigned)r);
            error = text;
            return false;
        }
        return true;
    }
    void close_decoder(SceAvcdecCtrl& ctrl) {
        sceAvcdecDeleteDecoder(&ctrl);
        memset(&ctrl, 0, sizeof(ctrl));
        decoder_busy_ = false;
    }

    // Render thread (GL context current). Gives everything back when nothing uses it: no open source, every frame home (the frame store
    // and the renderer have let go of their pictures). True when the device is not set up any more; false when it still is (asked again).
    bool trim() {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return false;   // a movie is being set up right now
        if (!ready_) return true;
        if (users_ != 0) return false;
        {
            std::lock_guard<std::mutex> frames_lock(frames_mutex_);
            if (free_.size() != static_cast<size_t>(kSlots)) return false;   // a picture is still alive somewhere
        }
        for (Frame& frame : slots_) {
            if (frame.texture != 0u) {
                // The GXM descriptor points at the slot and vitaGL's data pointer was cleared: the delete frees only the texture object.
                glDeleteTextures(1, &frame.texture);
                frame.texture = 0u;
            }
        }
        teardown();
        ready_ = false;
        log_memory("after the last movie, decoder given back");
        return true;
    }

    uint32_t pixel_type() const { return planar_ ? kPixelPlanar : kPixelPacked; }
    bool probe() const { return probe_; }

    // The GXM texture format that reads what the decoder writes (see the facts at the top of this file). The chroma order knob is the
    // byte order in memory: "uv" is NV12 (packed) or I420 (planar). SCE names the two-plane NV12 "YVU420P2" and the three-plane I420 "YUV420P3".
    SceGxmTextureFormat texture_format() const {
        if (planar_) {
            if (chroma_vu_) return csc_ ? SCE_GXM_TEXTURE_FORMAT_YVU420P3_CSC1 : SCE_GXM_TEXTURE_FORMAT_YVU420P3_CSC0;
            return csc_ ? SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC1 : SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC0;
        }
        if (chroma_vu_) return csc_ ? SCE_GXM_TEXTURE_FORMAT_YUV420P2_CSC1 : SCE_GXM_TEXTURE_FORMAT_YUV420P2_CSC0;
        return csc_ ? SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1 : SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC0;
    }
    const char* format_name() const {
        if (planar_) return chroma_vu_ ? (csc_ ? "YVU420P3_CSC1" : "YVU420P3_CSC0") : (csc_ ? "YUV420P3_CSC1" : "YUV420P3_CSC0");
        return chroma_vu_ ? (csc_ ? "YUV420P2_CSC1" : "YUV420P2_CSC0") : (csc_ ? "YVU420P2_CSC1" : "YVU420P2_CSC0");
    }

private:
    bool fail(const std::string& text) {
        error_ = text;
        plat::logf("hdmovie: hardware decoder unavailable: %s", text.c_str());
        return false;
    }

    static void log_memory(const char* when) {
        plat::MemInfo m;
        if (!plat::mem_info(&m)) return;
        plat::logf("hdmovie: memory %s: phycont_free=%d KiB cdram_free=%d KiB user_free=%d KiB, %d phycont blocks of the decoder", when,
                   (int)(m.phycont_free / 1024), (int)(m.cdram_free / 1024), (int)(m.user_free / 1024), g_blocks_live);
    }

    // Mutex held (acquire).
    bool setup() {
        // The knobs for what could not be checked without a device (docs/HD_MOVIES_HARDWARE_TEST.md).
        const std::string layout = cfg_str("hd_movie_layout", "packed");
        planar_ = layout == "planar";
        chroma_vu_ = cfg_str("hd_movie_chroma", "uv") == "vu";
        csc_ = cfg_int("hd_movie_csc", 0) != 0 ? 1 : 0;
        probe_ = cfg_int("hd_movie_probe", 0) != 0;

        log_memory("before the first picture, decoder not set up");
        int r = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
        if (r < 0) {
            // Vita3K in its automatic module mode tries to load a system module file for AVCDEC that its firmware does not
            // have (the decoder itself is built into the emulator) and reports SCE_SYSMODULE_ERROR_FATAL. On a console the load
            // succeeds. hd_movie_ignore_load_error=1 (config.txt) goes on regardless, for that emulator only: on a console a
            // failed load would leave the imports unresolved, so it is not the default.
            if (cfg_int("hd_movie_ignore_load_error", 0) == 0) {
                char text[80];
                snprintf(text, sizeof(text), "sceSysmoduleLoadModule(AVCDEC) failed (0x%08X)", (unsigned)r);
                failed_ = true;
                return fail(text);
            }
            plat::logf("hdmovie: sceSysmoduleLoadModule(AVCDEC) failed (0x%08X); going on (hd_movie_ignore_load_error)", (unsigned)r);
        } else {
            module_loaded_ = true;
        }
        SceVideodecQueryInitInfoHwAvcdec init{};
        init.size = sizeof(init);
        init.horizontal = kWidth;
        init.vertical = kHeight;
        init.numOfRefFrames = kRefFrames;
        init.numOfStreams = 1;
        r = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
        if (r < 0) {
            char text[80];
            snprintf(text, sizeof(text), "sceVideodecInitLibrary failed (0x%08X)", (unsigned)r);
            unload_module();
            failed_ = true;
            return fail(text);
        }
        library_ = true;
        SceAvcdecQueryDecoderInfo query{};
        query.horizontal = kWidth;
        query.vertical = kHeight;
        query.numOfRefFrames = kRefFrames;
        SceAvcdecDecoderInfo info{};
        r = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &info);
        if (r < 0) {
            char text[80];
            snprintf(text, sizeof(text), "sceAvcdecQueryDecoderMemSize failed (0x%08X)", (unsigned)r);
            teardown();
            failed_ = true;
            return fail(text);
        }
        std::string error;
        if (!alloc_phycont("hdmovie_decoder", info.frameMemSize, false, decoder_, error) ||
            !alloc_phycont("hdmovie_es", kEsBytes, false, es_, error) ||
            !alloc_phycont("hdmovie_frames", kSlotBytes * kSlots, true, frames_, error)) {
            teardown();   // not final: the memory may be there at the next movie
            return fail(error);
        }
        {
            std::lock_guard<std::mutex> lock(frames_mutex_);
            free_.clear();
            for (int i = 0; i < kSlots; ++i) {
                slots_[i] = Frame{};
                slots_[i].slot = i;
                slots_[i].data = static_cast<uint8_t*>(frames_.base) + static_cast<size_t>(i) * kSlotBytes;
                free_.push_back(&slots_[i]);
            }
        }
        ready_ = true;
        plat::logf("hdmovie: hardware decoder ready: decoder %u KiB + stream %u KiB + %d frames of %u KiB in phycont; pixel type %s, chroma %s, GXM format %s",
                   decoder_.size / 1024u, es_.size / 1024u, kSlots, kSlotBytes / 1024u, planar_ ? "planar" : "packed",
                   chroma_vu_ ? "VU" : "UV", format_name());
        log_memory("with the decoder set up");
        return true;
    }

    void unload_module() {
        if (module_loaded_) sceSysmoduleUnloadModule(SCE_SYSMODULE_AVCDEC);
        module_loaded_ = false;
    }

    // Everything setup() took, in reverse order. Safe to call with part of it missing.
    void teardown() {
        free_block(frames_, true);
        free_block(es_, false);
        free_block(decoder_, false);
        if (library_) sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
        library_ = false;
        unload_module();
        std::lock_guard<std::mutex> lock(frames_mutex_);
        free_.clear();
    }

    std::mutex mutex_;
    bool ready_{}, failed_{};
    bool module_loaded_{}, library_{};
    int users_{};
    std::string error_;
    Block decoder_, es_, frames_;
    Frame slots_[kSlots];
    std::mutex frames_mutex_;
    std::deque<Frame*> free_;
    std::atomic<bool> decoder_busy_{false};
    bool planar_{}, chroma_vu_{}, probe_{};
    int csc_{0};
};

class AvcdecSource final : public HdSource {
public:
    ~AvcdecSource() override { close(); }

    bool open(const std::string& path) override {
        close();
        // The file is checked first: a bad replacement never touches the decoder or its memory.
        if (!mp4_.open(path)) return fail(mp4_.error());
        if (mp4_.width() != kWidth || mp4_.height() != kHeight) {
            mp4_.close();
            return fail("the replacement is not 960x544");
        }
        if (mp4_.profile() != 66u && mp4_.profile() != 77u && mp4_.profile() != 100u) {
            mp4_.close();
            return fail("the replacement is not Baseline, Main or High profile (the hardware decoder takes those)");
        }
        if (mp4_.level() > 41u) {
            mp4_.close();
            return fail("the replacement is above level 4.1");
        }
        if (mp4_.longest_gop() > 600u) {
            mp4_.close();
            return fail("sync samples are too far apart (more than 600 pictures)");
        }
        if (mp4_.largest_sample() + 1024u > kEsBytes) {
            mp4_.close();
            return fail("a sample is bigger than the stream buffer of the decoder");
        }
        Device& device = Device::get();
        if (!device.acquire(error_)) {
            mp4_.close();
            return false;
        }
        device_user_ = true;
        if (!device.open_decoder(ctrl_, error_)) {
            mp4_.close();
            device.release_user();
            device_user_ = false;
            return false;
        }
        decoder_open_ = true;
        next_ = 0;
        failures_ = 0;
        poison_begin_ = poison_end_ = 0;
        last_.reset();
        last_index_ = -1;
        return true;
    }

    void close() override {
        last_.reset();
        last_index_ = -1;
        if (decoder_open_) {
            Device::get().close_decoder(ctrl_);
            decoder_open_ = false;
        }
        mp4_.close();
        if (device_user_) {
            Device::get().release_user();
            device_user_ = false;
        }
    }

    const std::string& error() const override { return error_; }
    uint32_t width() const override { return mp4_.width(); }
    uint32_t height() const override { return mp4_.height(); }
    uint32_t frame_count() const override { return static_cast<uint32_t>(mp4_.sample_count()); }
    uint64_t seeks() const override { return seeks_; }
    uint64_t pictures_decoded() const override { return decoded_; }
    uint64_t decode_cpu_us() const override { return cpu_us_; }

    std::shared_ptr<const HdPicture> frame_for(int64_t time) override {
        if (!decoder_open_ || mp4_.sample_count() == 0u || failures_ >= kMaxFailures) return nullptr;
        int64_t index = (time + kTicksPerPicture / 2) / kTicksPerPicture;
        index = std::clamp<int64_t>(index, 0, static_cast<int64_t>(mp4_.sample_count()) - 1);
        if (index == last_index_ && last_) return last_;
        // A picture of this group of pictures failed to decode: the pictures after it refer to the broken one, so the original shows the rest
        // of the group and the replacement comes back at the next sync sample.
        if (index >= poison_begin_ && index < poison_end_) return nullptr;
        const uint64_t cpu_begin = portablekit::movie::thread_cpu_us();
        // The decoder holds pictures up to next_ - 1. Going on from there is right unless a sync sample lies between (or the
        // picture asked for is behind): then start at that sync sample. An IDR picture clears the decoder's references, so
        // no reset is needed.
        const int64_t sync = static_cast<int64_t>(mp4_.sync_at_or_before(static_cast<size_t>(index)));
        if (index < next_ || sync > next_) {
            ++seeks_;
            next_ = sync;
        }
        std::shared_ptr<void> slot = Device::get().acquire_frame();
        if (!slot) {
            if (!starved_reported_) {
                starved_reported_ = true;
                plat::logf("hdmovie: all frame slots are held; the original picture is shown");
            }
            return nullptr;
        }
        Frame* frame = static_cast<Frame*>(slot.get());
        bool got = true;
        for (; next_ <= index; ++next_) {
            if (!decode(next_, *frame)) {
                got = false;
                break;
            }
        }
        cpu_us_ += portablekit::movie::thread_cpu_us() - cpu_begin;
        if (!got) {
            ++failures_;
            poison_begin_ = sync;
            poison_end_ = next_sync_after(index);
            next_ = -1;   // the decoder state is unknown: start again at a sync sample next time
            if (failures_ < kMaxFailures)
                plat::logf("hdmovie: picture %d could not be decoded; the original shows pictures %d to %d", (int)index, (int)index, (int)poison_end_ - 1);
            if (failures_ >= kMaxFailures)
                plat::logf("hdmovie: %d pictures in a row could not be decoded; the original movie plays from here on", kMaxFailures);
            return nullptr;
        }
        failures_ = 0;
        auto picture = std::make_shared<HdPicture>();
        picture->width = kWidth;
        picture->height = kHeight;
        picture->native = std::move(slot);
        picture->native_kind = 1u;
        last_ = picture;
        last_index_ = index;
        return picture;
    }

private:
    static constexpr int kMaxFailures = 8;

    // The first sync sample after `index` (the sample count when there is none).
    int64_t next_sync_after(int64_t index) const {
        const int64_t count = static_cast<int64_t>(mp4_.sample_count());
        for (int64_t i = index + 1; i < count; ++i)
            if (static_cast<int64_t>(mp4_.sync_at_or_before(static_cast<size_t>(i))) == i) return i;
        return count;
    }

    bool fail(const std::string& text) {
        error_ = text;
        return false;
    }

    // Decodes sample `sample` into `frame`. True when the decoder returned the picture of exactly that sample.
    bool decode(int64_t sample, Frame& frame) {
        if (sample < 0 || !mp4_.read_annex_b(static_cast<size_t>(sample), unit_)) return false;
        Device& device = Device::get();
        memcpy(device.es(), unit_.data(), unit_.size());
        SceAvcdecAu au{};
        au.pts.upper = 0u;
        au.pts.lower = static_cast<uint32_t>(sample);
        au.dts = au.pts;
        au.es.pBuf = device.es();
        au.es.size = static_cast<uint32_t>(unit_.size());
        SceAvcdecPicture picture{};
        picture.size = sizeof(picture);
        picture.frame.pixelType = device.pixel_type();
        picture.frame.framePitch = kWidth;
        picture.frame.frameWidth = kWidth;
        picture.frame.frameHeight = kHeight;
        picture.frame.pPicture[0] = frame.data;
        picture.frame.pPicture[1] = frame.data + kLumaBytes;
        SceAvcdecPicture* pictures[1] = {&picture};
        SceAvcdecArrayPicture array{};
        array.numOfElm = 1u;
        array.pPicture = pictures;
        const int r = sceAvcdecDecode(&ctrl_, &au, &array);
        ++decoded_;
        if (r < 0) {
            if (errors_logged_++ < 8) plat::logf("hdmovie: sceAvcdecDecode of sample %d failed (0x%08X)", (int)sample, (unsigned)r);
            return false;
        }
        if (array.numOfOutput == 0u) return false;   // the decoder keeps it back (it does not with the movies of build_movies.sh)
        const bool invalid = picture.info.pts.upper == 0xFFFFFFFFu && picture.info.pts.lower == 0xFFFFFFFFu;
        if (!invalid && picture.info.pts.lower != static_cast<uint32_t>(sample)) {
            if (errors_logged_++ < 8)
                plat::logf("hdmovie: decoder returned picture %u for sample %d", (unsigned)picture.info.pts.lower, (int)sample);
            return false;
        }
        if (device.probe() && sample % 100 == 0) probe_layout(frame, sample);
        return true;
    }

    // hd_movie_probe=1 (config.txt): what the decoder wrote after the luma plane, to tell the byte layout apart without a
    // screen. Interleaved chroma (UV or VU): the even and odd bytes of the area have different means in a coloured scene.
    // Planar chroma: the first and the second half of the area do (a warm scene has Cr above 128 and Cb below).
    void probe_layout(const Frame& frame, int64_t sample) {
        const uint8_t* c = frame.data + kLumaBytes;
        const uint32_t n = kLumaBytes / 2u;
        double even = 0, odd = 0, h1 = 0, h2 = 0;
        for (uint32_t i = 0; i < n; i += 2u) {
            even += c[i];
            odd += c[i + 1u];
        }
        for (uint32_t i = 0; i < n / 2u; ++i) {
            h1 += c[i];
            h2 += c[n / 2u + i];
        }
        const double k = 2.0 / n, k2 = 1.0 / (n / 2u);
        plat::logf("hdmovie: probe sample %d: luma[0..3] %u %u %u %u; chroma area mean of even bytes %.1f, odd bytes %.1f, first half %.1f, second half %.1f",
                   (int)sample, frame.data[0], frame.data[1], frame.data[2], frame.data[3], even * k, odd * k, h1 * k2, h2 * k2);
    }

    Mp4Reader mp4_;
    std::string error_;
    SceAvcdecCtrl ctrl_{};
    bool decoder_open_{};
    bool device_user_{};
    int64_t next_{0};          // the sample the decoder gets next
    int failures_{};
    int64_t poison_begin_{0}, poison_end_{0};   // [begin, end): pictures after a failed decode up to the next sync sample
    bool starved_reported_{};
    int errors_logged_{};
    std::shared_ptr<const HdPicture> last_;
    int64_t last_index_{-1};
    std::vector<uint8_t> unit_;
    uint64_t seeks_{}, decoded_{}, cpu_us_{};
};

// The renderer's side: a GL texture per frame slot, each a vitaGL texture whose GXM descriptor points at the slot.
class Binder final : public portablekit::gpu::HdNativeBinder {
public:
    GLuint texture_for(const HdPicture& picture) override {
        if (picture.native_kind != 1u || !picture.native) return 0u;
        Frame* frame = static_cast<Frame*>(picture.native.get());
        Device& device = Device::get();
        if (frame->texture == 0u) {
            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            // A real (tiny) texture first so that vitaGL has a valid texture object; its storage is given back at once and the
            // descriptor re-pointed at the slot, as in vitaGL's own video_playback sample. The data pointer is cleared so that
            // vitaGL never frees the slot when it replaces or deletes the texture.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            vglFree(vglGetTexDataPointer(GL_TEXTURE_2D));
            vglOverloadTexDataPointer(GL_TEXTURE_2D, nullptr);
            frame->texture = texture;
            plat::logf("hdmovie: GL texture %u for frame slot %d", (unsigned)texture, frame->slot);
        } else {
            glBindTexture(GL_TEXTURE_2D, frame->texture);
        }
        // The descriptor is written at every use, not once: whatever vitaGL did to it in between (a glTexParameteri rewrites the mip count
        // field, which moves the chroma plane, see the facts at the top of the file) is undone. Mip count 0 gives the field 15.
        SceGxmTexture* gxm = vglGetGxmTexture(GL_TEXTURE_2D);
        sceGxmTextureInitLinear(gxm, frame->data, device.texture_format(), kWidth, kHeight, 0);
        sceGxmTextureSetMinFilter(gxm, SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetMagFilter(gxm, SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetUAddrMode(gxm, SCE_GXM_TEXTURE_ADDR_CLAMP);
        sceGxmTextureSetVAddrMode(gxm, SCE_GXM_TEXTURE_ADDR_CLAMP);
        if (device.probe() && probes_++ < 24) {
            const uint32_t* words = reinterpret_cast<const uint32_t*>(gxm);
            plat::logf("hdmovie: descriptor of slot %d: %08X %08X %08X %08X (format %s, mip count field %u)", frame->slot, (unsigned)words[0],
                       (unsigned)words[1], (unsigned)words[2], (unsigned)words[3], device.format_name(), (unsigned)((words[0] >> 17) & 0xFu));
        }
        return frame->texture;
    }

    bool release_frames() override { return Device::get().trim(); }

private:
    int probes_{};
};

std::unique_ptr<HdSource> make_source() { return std::make_unique<AvcdecSource>(); }

}  // namespace

void hd_movie_install() {
    static Binder binder;
    portablekit::movie::set_hd_source_factory(&make_source);
    portablekit::gpu::set_hd_native_binder(&binder);
    plat::logf("hdmovie: hardware source registered (loaded on the first replacement movie)");
}

}  // namespace app
