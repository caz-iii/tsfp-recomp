// Host audio output: the AX mixer's frames go to an SDL3 audio stream.
#include <SDL3/SDL.h>

#include <cstdlib>

#include "hw/ax.h"
#include "runtime/runtime.h"

namespace {
SDL_AudioStream* g_stream = nullptr;
}

void audio_init() {
    if (getenv("TSFP_MUTE")) return;
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 32000};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!g_stream) {
        rt_log("audio: no output device (%s); running silent", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    rt_log("audio: 32 kHz stereo output");
}

void audio_push(const int16_t* stereo, int frames) {
    if (!g_stream) return;
    // Keep latency bounded if the host falls behind.
    if (SDL_GetAudioStreamQueued(g_stream) > 32000 * 4 / 5) SDL_ClearAudioStream(g_stream);
    SDL_PutAudioStreamData(g_stream, stereo, frames * 4);
}
