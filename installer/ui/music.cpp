#include "music.hpp"
#include <SDL3/SDL.h>
#include <array>
namespace tb {
void Music::open(const std::filesystem::path &path) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO) ||
        !(device_=SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,nullptr))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,"Audio unavailable: %s",SDL_GetError());
        return;
    }
    if (ov_fopen(path.c_str(), &file_) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Can't open Vorbis track: %s", path.c_str());
        return;
    }
    opened_=true;
    auto *info=ov_info(&file_,-1);
    if (!info) { close(); return; }
    SDL_AudioSpec spec{SDL_AUDIO_S16,info->channels,static_cast<int>(info->rate)};
    queueBytes_=spec.freq*spec.channels; // half a second of 16-bit input
    stream_=SDL_CreateAudioStream(&spec,nullptr);
    if (!stream_ || !SDL_SetAudioStreamGain(stream_,0.f) || !SDL_BindAudioStream(device_,stream_)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,"Audio unavailable: %s",SDL_GetError()); close();
    }
}
void Music::update(float volume) {
    if (!stream_) return;
    if (gain_ != volume) { SDL_SetAudioStreamGain(stream_,volume); gain_=volume; }
    std::array<char,16384> pcm;
    int queued=SDL_GetAudioStreamQueued(stream_), section=0;
    if (queued<0) { close(); return; }
    for (int reads=0; queued<queueBytes_ && reads<32; ++reads) {
        long n=ov_read(&file_,pcm.data(),pcm.size(),SDL_BYTEORDER==SDL_BIG_ENDIAN,2,1,&section);
        if (n==0) {
            if (ov_pcm_seek(&file_,0)!=0) { close(); return; }
        } else if (n<0) {
            if (n!=OV_HOLE) { close(); return; }
        } else {
            if (!SDL_PutAudioStreamData(stream_,pcm.data(),static_cast<int>(n))) { close(); return; }
            queued+=static_cast<int>(n);
        }
    }
}
void Music::close() {
    if (stream_) { SDL_DestroyAudioStream(stream_); stream_=nullptr; }
    if (opened_) { ov_clear(&file_); opened_=false; }
}
Music::~Music() { close(); if (device_) SDL_CloseAudioDevice(device_); }
}
