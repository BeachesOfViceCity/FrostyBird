// dxil-spirv lowers extension opcode 98 to the vendor-neutral
// OpRayQueryGetIntersectionTriangleVertexPositionsKHR. The extension UAV
// is a compiler protocol, not a resource read by the generated SPIR-V.
struct XsFetchCommand { uint opcode; uint pad0[18]; uint query; uint pad1[44]; };
RWStructuredBuffer<XsFetchCommand> xs_fetch_ext : register(u0,space31282);
float3x3 xs_fetch_committed(uint queryFlags){
    uint i=xs_fetch_ext.IncrementCounter();
    xs_fetch_ext[i].opcode=98;
    xs_fetch_ext[i].query=queryFlags;
    float3x3 v;
    [unroll] for(uint a=0;a<3u;++a)
        [unroll] for(uint b=0;b<3u;++b)v[a][b]=asfloat(xs_fetch_ext.IncrementCounter());
    return v;
}

struct XsTriangle {
    uint hit, t, status, instanceID, instanceIndex, contribution, geometry;
    float3x4 transform;
    float3x3 vertices;
};
XsTriangle xs_closest(RaytracingAccelerationStructure accel,uint mask,float3 origin,float3 direction,float nearT,float farT,uint budget,uint flags){
    XsTriangle h=(XsTriangle)0;
    h.t=asuint(farT);
    if(!budget){h.status=3u;return h;}
    if(!(nearT>=0 && farT>nearT) || any(isnan(origin)) || any(isnan(direction)) || all(direction==0))return h;
    RayDesc ray;ray.Origin=origin;ray.Direction=direction;ray.TMin=nearT;ray.TMax=farT;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(accel,flags,mask,ray);
    bool more;uint n=0;
    do {more=q.Proceed();++n;}while(more && n<budget);
    h.status=more?3u:0u;
    if(q.CommittedStatus()==COMMITTED_TRIANGLE_HIT && q.CommittedRayT()<farT){
        h.hit=1u;h.t=asuint(q.CommittedRayT());
        h.instanceID=q.CommittedInstanceID();h.instanceIndex=q.CommittedInstanceIndex();
        h.contribution=q.CommittedInstanceContributionToHitGroupIndex();h.geometry=q.CommittedGeometryIndex();
        h.transform=q.CommittedObjectToWorld3x4();h.vertices=xs_fetch_committed(q.RayFlags());
    }
    return h;
}
