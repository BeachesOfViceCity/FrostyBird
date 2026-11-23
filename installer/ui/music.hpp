#pragma once
#include <SDL3/SDL_audio.h>
#include <vorbis/vorbisfile.h>
#include <filesystem>
namespace tb {
class Music {
public:
    Music() = default;
    ~Music();
    Music(const Music &) = delete;
    Music &operator=(const Music &) = delete;
    void open(const std::filesystem::path &path);
    void update(float volume);
    bool available() const { return stream_ != nullptr; }
    SDL_AudioDeviceID device() const { return device_; }
private:
    void close();
    OggVorbis_File file_{};
    SDL_AudioDeviceID device_{};
    SDL_AudioStream *stream_{};
    bool opened_{};
    int queueBytes_{};
    float gain_ = -1;
};
}
