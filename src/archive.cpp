#include "archive.hpp"
#include <lz4.h>
#include <zlib.h>

namespace frosty {
namespace {
std::string_view fstring(Cursor& c) {
    auto length = std::bit_cast<std::int32_t>(c.word());
    if (length == 0) return {};
    if (length < 0) { // The two names we look for are ANSI.
        c.take(static_cast<u64>(-std::int64_t(length)) * 2);
        return {};
    }
    auto raw = c.take(length);
    if (raw.data.back() != 0) throw Error("unterminated Pak index string");
    return {reinterpret_cast<const char*>(raw.data.data()), raw.size() - 1};
}

Bytes uncompressed_entry(Bytes pak, Bytes encoded) {
    Cursor c{encoded};
    const auto flags = c.word();
    if ((flags & 0x1fffffff) != 0)
        throw Error("expected an uncompressed, unencrypted Pak shader library");
    const auto field = [&](u32 mask) -> u64 {
        return flags & mask ? c.word() : c.take(8).at<u64>(0);
    };
    const auto file = pak.tail(field(1u << 31));
    const auto size = field(1u << 30);
    // FPakEntry v11 on disk: offset, stored/raw lengths, method, SHA1,
    // flags, block size. Check the independent on-disk record as well.
    if (file.at<u64>(8) != size || file.at<u64>(16) != size ||
        file.at<u32>(24) || file.at<u8>(48))
        throw Error("Pak index disagrees with the shader library's entry header");
    return file.slice(53, size);
}
Bytes directory_entry(Bytes pak,std::string_view directory,std::string_view filename) {
    constexpr size_t footer_size = 221;
    if (pak.size() < footer_size) throw Error("truncated Pak footer");
    auto footer = pak.tail(pak.size() - footer_size);
    if (footer.at<u32>(17) != 0x5a6f12e1 || footer.at<u32>(21) != 11)
        throw Error("expected a UE Pak v11 footer");
    if (footer.at<u8>(16)) throw Error("encrypted Pak index is unsupported");
    Cursor primary{pak.slice(footer.at<u64>(25), footer.at<u64>(33))};
    fstring(primary); // mount point
    primary.take(12); // entry count and hash seed
    if (primary.word()) primary.take(36); // optional path hash index
    if (!primary.word()) throw Error("Pak has no full directory index");
    auto dir_info = primary.take(36);
    Cursor dirs{pak.slice(dir_info.at<u64>(0), dir_info.at<u64>(8))};
    auto encoded = primary.array(1);
    const auto dir_count = dirs.word();
    for (u32 i = 0; i < dir_count; ++i) {
        auto dir = fstring(dirs);
        const auto file_count = dirs.word();
        for (u32 j = 0; j < file_count; ++j) {
            auto file = fstring(dirs);
            auto entry = dirs.word();
            if(dir==directory && file==filename)return encoded.tail(entry);
        }
    }
    throw Error("entry missing from Pak directory: "+std::string(directory)+std::string(filename));
}
} // namespace
ShaderArchive open_library(Bytes pak,std::string_view library) {
    if(library!="Global" && library!="Frosty")throw Error("library must be Global or Frosty");
    auto name="ShaderArchive-"+std::string(library)+"-SF_XSX_SM6.ushaderbytecode";
    return ShaderArchive(uncompressed_entry(pak,directory_entry(pak,"Frosty/Content/",name)));
}
Buffer global_shader_cache(Bytes pak){
    Cursor c{directory_entry(pak,"Engine/","GlobalShaderCache-SF_XSX_SM6.bin")};
    auto flags=c.word();auto field=[&](u32 bit)->u64{return flags&bit?c.word():c.take(8).at<u64>(0);};
    auto file=pak.tail(field(1u<<31));auto raw=field(1u<<30),stored=field(1u<<29);
    if(!raw || raw>128u<<20 || file.at<u64>(8)!=stored || file.at<u64>(16)!=raw || file.at<u32>(24)!=1)
        throw Error("unexpected packaged global-cache compression or size");
    auto count=file.at<u32>(48);auto ranges=file.slice(52,16ull*count);auto control=file.tail(52+ranges.size());
    auto block=control.at<u32>(1);if(control.at<u8>(0) || !block || count!=(raw+block-1)/block)throw Error("invalid global-cache compression blocks");
    Buffer result(raw);u64 written=0,consumed=0;
    for(u32 i=0;i<count;++i){
        auto lo=ranges.at<u64>(16ull*i),hi=ranges.at<u64>(16ull*i+8);if(hi<=lo || lo<57+ranges.size())throw Error("invalid global-cache block range");
        auto part=file.slice(lo,hi-lo);uLongf expected=std::min<u64>(block,raw-written),n=expected;uLong src=part.size();
        if(uncompress2(result.data()+written,&n,part.data.data(),&src)!=Z_OK || n!=expected || src!=part.size())throw Error("bad global-cache zlib stream");
        written+=n;consumed+=src;
    }
    if(written!=raw || consumed!=stored)throw Error("global-cache size mismatch");
    return result;
}

ShaderArchive::ShaderArchive(Bytes archive) {
    Cursor c{archive};
    if (c.word() != 2) throw Error("unsupported UE shader archive version");
    map_hashes_ = c.array(20);
    shader_hashes_ = c.array(20);
    maps_ = c.array(16);
    entries_ = c.array(17); // serialized fields, no C++ struct padding
    if (shader_hashes_.size() / 20 != size() || map_hashes_.size() / 20 != maps_.size() / 16)
        throw Error("inconsistent UE shader archive counts");
    c.array(16); // preload ranges
    indices_=c.array(4);
    payload_ = c.rest;
}
Bytes ShaderArchive::map_indices(u32 i) const{
    auto map=maps_.slice(16ull*i,16);return indices_.slice(4ull*map.at<u32>(0),4ull*map.at<u32>(4));
}

ShaderEntry ShaderArchive::operator[](u32 i) const {
    if (i >= size()) throw Error("shader index out of range");
    auto e = entries_.slice(u64(i) * 17, 17);
    return {payload_.slice(e.at<u64>(0), e.at<u32>(8)), e.at<u32>(12), e.at<u8>(16)};
}

Buffer ShaderEntry::unpack() const {
    constexpr size_t limit = 64 * 1024 * 1024;
    if (!unpacked_size || unpacked_size > limit || stored.size() > limit)
        throw Error("invalid shader record size");
    if (stored.size() == unpacked_size) return {stored.data.begin(), stored.data.end()};
    Buffer result(unpacked_size);
    const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(stored.data.data()),
        reinterpret_cast<char*>(result.data()), int(stored.size()), int(result.size()));
    if (n != int(result.size())) throw Error("invalid LZ4 shader record");
    return result;
}
} // namespace frosty
