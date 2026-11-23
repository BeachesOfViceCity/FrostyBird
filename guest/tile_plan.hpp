#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <string>

// One exact XS descriptor/subresource. Offsets are measured by the packaged
// XG address computer. Only addressing is reusable; resource bytes never are.
using TileKey=std::array<UINT64,18>;
bool tile_sha(const void*,size_t,std::array<BYTE,32>&);
std::wstring tile_path(const TileKey&,const wchar_t* extension);
struct TileRun {UINT src,n;};
struct TilePlan {
    std::vector<UINT> ends;
    std::vector<TileRun> runs;
    UINT lo=UINT_MAX,hi{};
    size_t size()const{return 4*ends.size()+sizeof(TileRun)*runs.size();}
};
std::shared_ptr<const TilePlan> exact_tile_plan(const TileKey&,UINT64 source_size,UINT row_bytes,UINT rows,
    const std::function<std::shared_ptr<TilePlan>()>& build);
