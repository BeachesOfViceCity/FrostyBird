#include "cues.hpp"
#include <SDL3/SDL.h>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace {
struct Meter {
    std::atomic<unsigned> peak{0}, frames{0};
};
void SDLCALL measure(void* data, const SDL_AudioSpec*, float* samples, int bytes) {
    auto& m = *static_cast<Meter*>(data);
    unsigned block = 0;
    for (int i = 0; i < bytes / int(sizeof(float)); ++i) {
        auto magnitude = unsigned(std::abs(samples[i]) * 32768);
        if (magnitude > block) block = magnitude;
    }
    auto previous = m.peak.load();
    while (previous < block && !m.peak.compare_exchange_weak(previous, block)) {}
    ++m.frames;
}
bool verify(SDL_AudioDeviceID device) {
    Meter m;
    if (!SDL_SetAudioPostmixCallback(device, measure, &m)) return false;
    struct Callback {
        SDL_AudioDeviceID device;
        ~Callback() { SDL_SetAudioPostmixCallback(device, nullptr, nullptr); }
    } cleanup{device};
    fb::Cues s; s.open(device);
    if (!s.available()) return false;
    s.level(.44f);
    unsigned results[6]{};
    for (unsigned i = 0; i < 6; ++i) {
        m.peak = 0;
        s.play(static_cast<fb::Cue>(i));
        SDL_Delay(i == 5 ? 930 : 440);
        results[i] = m.peak.load();
    }
    s.level(0);
    m.peak = 0;
    s.play(fb::Cue::complete);
    SDL_Delay(250);
    std::printf("SDL postmix peaks: navigation=%u, confirm=%u, back=%u, install=%u, error=%u, finish=%u, muted=%u (%u mix buffers)\n",
                results[0], results[1], results[2], results[3], results[4], results[5],
                m.peak.load(), m.frames.load());
    for (auto n : results) if (n < 300 || n > 27000) return false;
    return m.frames > 0 && m.peak == 0;
}
}
int main() {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_AUDIO)) return 1;
    SDL_AudioSpec spec{SDL_AUDIO_F32,2,48000};
    auto d = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    bool ok = d && verify(d);
    if (d) SDL_CloseAudioDevice(d);
    SDL_Quit();
    return !ok;
}
