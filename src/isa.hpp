#pragma once

#include "shader.hpp"
#include <array>
#include <map>
#include <iosfwd>

namespace frosty::isa {
enum Stage : u32 { pixel = 0, vertex = 1, geometry = 2, compute = 5 };

struct Semantic {
    std::string name;
    unsigned index, system, type, reg, mask, used, interpolation{};
};
struct Resource {
    unsigned table, offset, words; // initial user SGPR, byte offset, SRD width
    unsigned reg, type, kind, stride; // D3D register, PSV type/kind, Xbox element bytes
};
struct Attribute { unsigned reg, mask, base; };
struct Program {
    Stage stage;
    Bytes code; // borrows the inflated object; never points into DXC output
    std::vector<Resource> resources;
    std::vector<Semantic> inputs, outputs;
    std::vector<Attribute> fetch;
    std::map<unsigned, unsigned> parameters, interpolants;
    std::map<unsigned,unsigned> layer_parameters,clip_parameters;
    std::vector<unsigned> dead_parameter_regs;
    std::array<unsigned, 3> threads{};
    u32 cs_rsrc2{}, ps_input{}, wave{}, lds_bytes{}, max_vertices{}, output_vectors{};
    bool native_compute_wave{},flexible_pixel_wave{},native_half{},native_group_id{};
    bool depth_before_pixel{};
};

Program interface(const Shader&, Buffer& object);
Bytes machine_code(const Shader&,const Buffer& object); // raw, before interface/export linkage
unsigned machine_wave(const Shader&,const Buffer& object);
struct Instruction {
    unsigned pc, size;
    std::string op, modifiers;
    std::vector<std::string> args;
    bool field_decode=false; // vendor MIMG with format-dependent register widths
};
Instruction decode(Bytes code,unsigned pc,unsigned wave=0);
struct Graph {
    std::map<unsigned,Instruction> ins;
    std::map<unsigned,std::vector<unsigned>> edges;
    std::vector<unsigned> order; // topological order, empty for cyclic graphs
};
Graph decode_graph(Bytes code,std::vector<std::string>* gaps=nullptr,unsigned wave=0,bool vertex_fetch=false);
void emit_resolved_flow(std::ostream&,const std::map<unsigned,std::string>& blocks,
        const std::map<unsigned,std::vector<unsigned>>& edges,const std::map<unsigned,std::string>& conditions,unsigned entry);
std::string listing(const Shader&);

struct Translation {
    Stage stage;
    std::string source;
    std::vector<u32> views; // SRV register or (0x100 | UAV register), in specialization order
    unsigned required_wave{};
    bool inline_rays{};
    bool native_half{};
};
Translation recompile(const Shader&,unsigned compute_wave_caps=0);
Translation recompile(const Program&); // also used by isolated ISA conformance kernels
Translation rectangle_assembly(const Shader&); // XS RECTLIST fixed-function assembly after the original VS
Buffer compile_dxil(const Translation&, std::string_view numeric={});
inline const char* stage_name(Stage s) { return s==vertex?"vs":s==pixel?"ps":s==geometry?"gs":"cs"; }
} // namespace frosty::isa
