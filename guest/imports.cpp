#define NOMINMAX
#include <windows.h>
#include "bytes.hpp"
#include <algorithm>
#include <iostream>

using namespace frosty;

namespace {
// Ask the same Wine loader as the title, in the same application directory.
// Map the EXE as an image without binding its imports or running its code.
struct Module {
    HMODULE h;
    ~Module() { if (h) FreeLibrary(h); }
};
std::string name_at(Bytes image, u64 rva) {
    auto t = image.tail(rva).data;
    auto end = std::find(t.begin(), t.end(), 0);
    if (end == t.end()) throw Error("unterminated PE string");
    return {reinterpret_cast<const char*>(t.data()), size_t(end - t.begin())};
}
struct Totals { unsigned modules{}, imports{}, missing{}; };
Totals check(Bytes image, IMAGE_DATA_DIRECTORY dir, bool delay) {
    Totals stats;
    if (!dir.Size) return stats;
    Cursor desc{image.slice(dir.VirtualAddress, dir.Size)};
    for (;;) {
        auto d = desc.take(delay ? 32 : 20);
        auto name_rva = d.at<u32>(delay ? 4 : 12);
        if (!name_rva) break;
        if (delay && d.at<u32>(0) != 1) throw Error("unsupported VA-based delay imports");
        auto dll = name_at(image, name_rva);
        auto table = d.at<u32>(delay ? 16 : 0);
        if (!table) table = d.at<u32>(delay ? 12 : 16);
        if (!table) throw Error("missing PE import lookup table");
        const char* label = delay ? "delay" : "load";
        Module m{LoadLibraryA(dll.c_str())};
        if (!m.h) {
            auto error = GetLastError();
            std::cout << label << ": " << dll << " unavailable (Win32 " << error << ")\n";
        }
        ++stats.modules;
        Cursor thunks{image.tail(table)};
        while (auto t = thunks.take(8).at<u64>(0)) {
            ++stats.imports;
            if (!m.h) { ++stats.missing; continue; }
            const bool ordinal = (t & IMAGE_ORDINAL_FLAG64) != 0;
            std::string symbol = ordinal ? "#" + std::to_string(WORD(t)) : name_at(image, t + 2);
            if (!GetProcAddress(m.h, ordinal ? MAKEINTRESOURCEA(WORD(t)) : symbol.c_str())) {
                ++stats.missing;
                std::cout << label << ": " << dll << '!' << symbol << " unavailable\n";
            }
        }
    }
    return stats;
}
} // namespace

int wmain(int argc, wchar_t** argv) try {
    if (argc != 2) throw Error("usage: frosty-imports.exe <title.exe>");
    Module exe{LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES)};
    if (!exe.h) throw Error("cannot map PE image, Win32 " + std::to_string(GetLastError()));
    auto base = reinterpret_cast<const u8*>(exe.h);
    // The OS image loader validated the NT header and mapped sections.
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT)
        throw Error("expected an AMD64 PE32+ title");
    const auto& h = nt->OptionalHeader;
    std::cout << "AMD64 PE32+ mapped at 0x" << std::hex << reinterpret_cast<std::uintptr_t>(base) << " entry RVA 0x"
              << h.AddressOfEntryPoint << std::dec << '\n';
    Bytes image{{base, h.SizeOfImage}};
    auto normal = check(image, h.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT], false);
    auto delayed = check(image, h.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT], true);
    for (auto [label, s] : {std::pair{"load", normal}, {"delay", delayed}})
        std::cout << label << ": " << s.imports << " imports from " << s.modules
                  << " modules; " << s.missing << " unresolved\n";
    return normal.missing ? 3 : 0; // unresolved delay imports don't block startup
} catch (const std::exception& e) {
    std::cerr << "frosty-imports: " << e.what() << '\n';
    return 1;
}
