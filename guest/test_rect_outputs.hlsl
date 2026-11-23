struct V {
    float4 uv : TEXCOORD3;
    nointerpolation uint light : LIGHTMAP_ID0;
    nointerpolation uint eye : PACKED_EYE_INDEX0;
    nointerpolation uint view : PACKED_VIEW_INDEX0;
    noperspective float4 p : SV_Position0;
};
uint4 main(V v):SV_Target {
    return uint4(asuint(v.p.z),asuint(v.uv.x)|asuint(v.uv.y)|asuint(v.uv.z)|asuint(v.uv.w),
                 v.light|v.eye|v.view,1u+uint(v.p.x)+256u*uint(v.p.y));
}
