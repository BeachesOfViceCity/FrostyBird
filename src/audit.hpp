#pragma once
#include "bytes.hpp"
#include <filesystem>

namespace frosty {
bool audit(Bytes pak,const std::filesystem::path& dir,unsigned jobs,bool dxil);
void probe_catalog(const std::filesystem::path& file);
}
