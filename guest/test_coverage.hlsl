#ifndef COVERAGE_UV
#define COVERAGE_UV 1
#endif
struct Output {
    float4 a : TEXCOORD10_centroid0;
    float4 b : TEXCOORD11_centroid0;
#if COVERAGE_UV
    float4 uv : TEXCOORD0;
#endif
    nointerpolation uint prim : PRIMITIVE_ID0;
    float4 pos : SV_Position0;
};
Output main(uint id : SV_VertexID) {
    Output o;
    o.pos=float4(id==1?3:-1,id==2?3:-1,0,1);
    o.a=o.b=o.pos;
#if COVERAGE_UV
    o.uv=o.pos;
#endif
    o.prim=id;
    return o;
}
