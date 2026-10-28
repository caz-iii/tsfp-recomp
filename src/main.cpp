// tsfp-recomp entry point: boot the recompiled main.dol natively.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "funcs.h"
#include "gx/gx_fifo.h"
#include "hw/hw.h"
#include "runtime/runtime.h"
#include "video/video.h"

static PPCState g_cpu;

int main(int argc, char** argv) {
    RtConfig cfg;
    std::string root = argc > 1 ? argv[1] : "assets/GCN - TimeSplitters - Future Perfect (USA)";
    cfg.dol_path = root + "/sys/main.dol";
    cfg.files_root = root + "/files";
    cfg.sys_root = root + "/sys";
    if (FILE* f = fopen((root + "/sys/boot.bin").c_str(), "rb")) {
        cfg.disc_header.resize(0x440);
        cfg.disc_header.resize(fread(cfg.disc_header.data(), 1, 0x440, f));
        fclose(f);
    }
    if (!video_init(getenv("TSFP_HEADLESS") != nullptr)) {
        fprintf(stderr, "video initialisation failed (OpenGL 3.3 required)\n");
        return 1;
    }
    if (!rt_boot(cfg, &g_cpu)) {
        fprintf(stderr, "boot failed; pass the extracted GameCube disc directory as the first argument\n");
        return 1;
    }
    // Watchdog: periodically show where the guest is (TSFP_WATCHDOG=seconds, 0 = off).
    int every = getenv("TSFP_WATCHDOG") ? atoi(getenv("TSFP_WATCHDOG")) : 0;
    std::thread([every] {
        for (int t = 1; every; t++) {
            std::this_thread::sleep_for(std::chrono::seconds(every));
            rt_log("watchdog %ds: frames %u", t * every, hw_frame_count());
            irq_status();
            void backtrace_from(const PPCState&);
            rt_log("live main thread:");
            backtrace_from(g_cpu);
            if (getenv("TSFP_DEBUG_DVD")) {  // SDK dvd.c state
                uint32_t ex = rd32(0x80613180);
                rt_log("dvd: executing 0x%08X (cmd %u state %d len 0x%X done 0x%X) CurrCommand %u Pause %u/%u Fatal %u Canceling %u Reset %u Motor %u queue0 0x%08X",
                       ex, ex ? rd32(ex + 8) : 0, ex ? (int32_t)rd32(ex + 0xC) : 0, ex ? rd32(ex + 0x14) : 0, ex ? rd32(ex + 0x20) : 0,
                       rd32(0x8061319C), rd32(0x8061318C), rd32(0x80613190), rd32(0x80613198), rd32(0x806131A0), rd32(0x806131B8),
                       rd32(0x806131C0), rd32(0x805F35F8));
            }
            if (getenv("TSFP_DEBUG_DVD")) {  // game dvd2 handle table (0x805A3238, 0x54 per handle)
                std::string t;
                for (int h = 0; h < 0x1C; h++) {
                    uint32_t b = 0x805A3238 + 0x54 * h;
                    if (rd8(b + 0x50)) {
                        char buf[64];
                        snprintf(buf, sizeof buf, " [%d use %u done %u busy %u]", h, rd8(b + 0x50), rd8(b + 0x51), rd8(b + 0x52));
                        t += buf;
                    }
                }
                rt_log("dvd2: cs handle %d, open:%s", (int32_t)rd32(0x8060FB28), t.c_str());
            }
            if (getenv("TSFP_DEBUG_STREAM")) {  // stream slot 0 (stream_player.cpp)
                uint32_t voice = rd32(0x805BE588);
                rt_log("stream: state %u in_use %u voice 0x%08X voice.state %u paused %u",
                       rd32(0x805AD480), rd32(0x805AD484), voice, voice ? rd16(voice + 0x146) : 0, rd32(0x805AD4A0));
            }
            const GxStats& g = gx_stats();
            rt_log("video: frames presented %u", video_frames_presented());
            rt_log("gx: commands %u primitives %u vertices %u display lists %u draw-done %u tokens %u copies %u",
                   g.commands, g.primitives, g.vertices, g.display_lists, g.draw_done, g.tokens, g.copies);
            rt_dump_trace(getenv("TSFP_TRACE_N") ? atoi(getenv("TSFP_TRACE_N")) : 24);
        }
    }).detach();
    rt_log("entering guest entry point 0x%08X", kPpcEntry);
    ppc_call(&g_cpu, kPpcEntry);
    rt_log("guest entry point returned");
    return 0;
}
