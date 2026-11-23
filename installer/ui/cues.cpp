#include "cues.hpp"
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace fb {
namespace {
constexpr int rate = 48000;
constexpr float tau = 2 * std::numbers::pi_v<float>;

struct Synth {
    std::vector<float> samples;
    std::uint32_t rng = 0xb391f28du;
    explicit Synth(float seconds) : samples(static_cast<size_t>(seconds * rate)) {}
    void tone(float start, float length, float from, float to, float gain, float index, float ratio,
              float decay, float grit = 0) {
        auto offset = static_cast<size_t>(start * rate), count = static_cast<size_t>(length * rate);
        float filtered = 0, previous = 0;
        for (size_t i = 0; i < count && offset + i < samples.size(); ++i) {
            float t = float(i) / rate, u = t / length;
            float phase = tau * t * (from + (to - from) * u * .5f);
            float envelope =
                std::min(t / .004f, 1.f) * std::min((length - t) / .017f, 1.f) * std::exp(-u * decay);
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            float white = 2.f * float(rng >> 8) / 16777215.f - 1.f;
            filtered += .23f * (white - filtered);
            float burst = (filtered - previous) * std::exp(-t * 46.f) * 2.f;
            previous = filtered;
            samples[offset + i] +=
                gain * envelope *
                (std::sin(phase + index * (1 - u) * std::sin(phase * ratio)) + grit * burst);
        }
    }
};

std::vector<float> synthesize(Cue kind) {
    switch (kind) {
    case Cue::tick: {
        Synth s(.044f);
        s.tone(0, .043f, 1150, 780, .22f, .85f, 2.44f, 4.f, .25f);
        return std::move(s.samples);
    }
    case Cue::accept: {
        Synth s(.174f);
        s.tone(0, .104f, 330, 420, .20f, 1.4f, 2.01f, 3.6f, .21f);
        s.tone(.056f, .111f, 626, 703, .15f, .95f, 3.03f, 3.9f);
        s.tone(.088f, .078f, 113, 79, .12f, .22f, 2.f, 3.5f);
        return std::move(s.samples);
    }
    case Cue::back: {
        Synth s(.107f);
        s.tone(0, .106f, 422, 205, .21f, .82f, 2.77f, 3.4f, .21f);
        return std::move(s.samples);
    }
    case Cue::link: {
        Synth s(.36f);
        s.tone(0, .31f, 83, 47, .19f, 1.05f, 4.1f, 3.3f, .7f);
        for (int i = 0; i < 4; ++i) {
            float hz = 294.f * std::exp2(i * .2f);
            s.tone(.035f + i * .042f, .116f, hz, hz * 1.12f, .13f, 1.38f, 2.77f, 3.6f, .11f);
        }
        return std::move(s.samples);
    }
    case Cue::error: {
        Synth s(.218f);
        s.tone(0, .084f, 228, 145, .21f, 1.86f, 2.09f, 2.4f, .14f);
        s.tone(.1f, .112f, 171, 112, .22f, 1.71f, 2.31f, 3.3f, .23f);
        return std::move(s.samples);
    }
    case Cue::complete: {
        Synth s(.83f);
        s.tone(0, .59f, 98, 49, .16f, .78f, 2.41f, 3.3f, .32f);
        constexpr float notes[] = {293.66f, 440.f, 587.33f, 698.46f};
        for (int i = 0; i < 4; ++i) {
            s.tone(.065f * i, .59f, notes[i], notes[i], .13f, 1.17f, 2.01f, 3.8f);
            s.tone(.16f + .065f * i, .39f, notes[i] * 2, notes[i] * 2, .035f, .48f, 2.f, 4.1f);
        }
        return std::move(s.samples);
    }
    case Cue::count:
        return {};
    }
    return {};
}
} // namespace

void Cues::open(SDL_AudioDeviceID device) {
    if (!device)
        return;
    SDL_AudioSpec spec{SDL_AUDIO_F32, 1, rate};
    for (auto& stream : streams) {
        stream = SDL_CreateAudioStream(&spec, nullptr);
        if (!stream)
            continue;
        if (!SDL_BindAudioStream(device, stream)) {
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
        } else
            SDL_SetAudioStreamGain(stream, volume);
    }
    for (unsigned i = 0; i < bank.size(); ++i)
        bank[i] = synthesize(static_cast<Cue>(i));
}

void Cues::level(float value) {
    if (value == volume)
        return;
    volume = value;
    for (auto stream : streams)
        if (stream) {
            SDL_SetAudioStreamGain(stream, volume);
            if (!volume)
                SDL_ClearAudioStream(stream);
        }
}

void Cues::play(Cue cue) {
    if (!volume || cue == Cue::count)
        return;
    bool tick = cue == Cue::tick;
    auto now = SDL_GetTicks();
    if (tick && last_tick && now - last_tick < 100)
        return;
    auto stream = streams[tick ? 0 : cue == Cue::complete ? 2 : 1];
    if (!stream)
        return;
    if (SDL_GetAudioStreamQueued(stream) > 0) {
        if (cue != Cue::error && cue != Cue::link)
            return;
        SDL_ClearAudioStream(stream);
    }
    auto& wave = bank[static_cast<unsigned>(cue)];
    if (!SDL_PutAudioStreamData(stream, wave.data(), int(wave.size() * sizeof(float))) ||
        !SDL_FlushAudioStream(stream))
        SDL_ClearAudioStream(stream);
    if (tick)
        last_tick = now;
}

Cues::~Cues() {
    for (auto stream : streams)
        if (stream)
            SDL_DestroyAudioStream(stream);
}
} // namespace fb
