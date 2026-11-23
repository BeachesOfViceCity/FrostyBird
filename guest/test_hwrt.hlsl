// Measurement shader for the isolated XS ABI test only.
RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer rays : register(t1);
RWByteAddressBuffer result : register(u0);
#ifdef FROSTY_QUERY_POSITIONS
#include "../src/ray_fetch.hlsli"
#include "../src/ray_visibility.hlsli"
#define RESULT_BYTES 96
#else
#define RESULT_BYTES 20
#endif
[numthreads(1,1,1)]
void main(uint id : SV_DispatchThreadID) {
    uint4 input = rays.Load4(id*16);
    RayDesc r;
    r.Origin=float3(asfloat(input.xy),-160.0);
    r.Direction=float3(0,0,1);
    r.TMin=0; r.TMax=asfloat(input.z);
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, input.w, r);
    while(q.Proceed()) {}
    uint2 hit=uint2(~0u,~0u);
    float3 data=0;
    if(q.CommittedStatus()==COMMITTED_TRIANGLE_HIT) {
        hit=uint2(q.CommittedInstanceID(),q.CommittedPrimitiveIndex());
        data=float3(q.CommittedRayT(),q.CommittedTriangleBarycentrics());
    }
    result.Store2(id*RESULT_BYTES,hit);
    result.Store3(id*RESULT_BYTES+8,asuint(data));
#ifdef FROSTY_QUERY_POSITIONS
    XsTriangle h=xs_closest(scene,input.w,r.Origin,r.Direction,r.TMin,r.TMax,128,RAY_FLAG_NONE);
    [unroll] for(uint k=0;k<3u;++k)result.Store3(id*RESULT_BYTES+20+12*k,asuint(h.vertices[k]));
    uint3 visibility=xs_visibility(scene,input.w,r.Origin,r.Direction,r.TMin,r.TMax,128,RAY_FLAG_NONE);
    uint3 zero=xs_visibility(scene,input.w,r.Origin,r.Direction,r.TMin,r.TMax,0,RAY_FLAG_NONE);
    uint3 beyond=xs_visibility(scene,input.w,r.Origin,r.Direction,300.0,r.TMax,128,RAY_FLAG_NONE);
    result.Store4(id*RESULT_BYTES+56,uint4(h.hit,h.t,h.instanceID,h.instanceIndex));
    result.Store3(id*RESULT_BYTES+72,uint3(visibility.x,visibility.z,zero.z));
    result.Store3(id*RESULT_BYTES+84,uint3(beyond.x,h.geometry,asuint(h.transform[0][3])));
#endif
}
