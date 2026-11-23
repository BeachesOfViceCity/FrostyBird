#pragma once
#include "shape.hpp"
#include <array>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
namespace shape {
// Private state per hardware context: overlap-add, a partially assembled
// elementary frame, and PCM not yet consumed in 128-sample quanta.
class Xma {
    struct Cursor { u32 bit{},slot{},consumed{}; };
    AVCodecContext* decoder{};
    AVFrame* frame{};
    AVPacket* packet{};
    std::array<unsigned char,4096> bits{};
    std::array<short,1024> pcm{};
    u32 have{},length{},begin_bit{},begin_slot{},sub{},end_sub{},skip{};
    Cursor next{};
    bool loop{},tracking{};
    u32 expected_bit{},expected_slot{},expected_write{},expected_input{},expected_output{},expected_work{},format{};
    void clear();
    unsigned pull(Context&);
    bool transform(unsigned channels,unsigned rate);
    void save_position(Context&,Cursor);
public:
    Xma()=default;~Xma();
    Xma(const Xma&)=delete;Xma& operator=(const Xma&)=delete;
    // Returns emitted subframes. Error/status bits are published in the context.
    unsigned run(Context&);
};
}
