#pragma once
#include <SDL3/SDL_audio.h>
#include <array>
#include <cstdint>
#include <vector>

namespace fb {
enum class Cue { tick, accept, back, link, error, complete, count };

// Short original sounds synthesized once, then played on the music device.
// Separate streams keep navigation from interrupting the finish cue.
class Cues {
  public:
    Cues() = default;
    Cues(const Cues&) = delete;
    Cues& operator=(const Cues&) = delete;
    ~Cues();
    void open(SDL_AudioDeviceID);
    void level(float);
    void play(Cue);
    bool available() const { return streams[0] != nullptr; }

  private:
    std::array<SDL_AudioStream*, 3> streams{};
    std::array<std::vector<float>, static_cast<unsigned>(Cue::count)> bank;
    std::uint64_t last_tick{};
    float volume{};
};
} // namespace fb
