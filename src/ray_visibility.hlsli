uint3 xs_visibility(RaytracingAccelerationStructure accel, uint mask, float3 origin, float3 direction, float nearT, float farT, uint budget, uint flags) {
    if(!budget)return uint3(0u,asuint(farT),3u);
    if(!(nearT>=0 && farT>nearT) || any(isnan(origin)) || any(isnan(direction)) || all(direction==0))
        return uint3(0u,asuint(farT),0u);
    RayDesc ray;ray.Origin=origin;ray.Direction=direction;ray.TMin=nearT;ray.TMax=farT;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(accel,flags,mask,ray);
    uint n=0;bool more;
    do {more=q.Proceed();++n;}while(more && n<budget);
    bool hit=q.CommittedStatus()==COMMITTED_TRIANGLE_HIT && q.CommittedRayT()<farT;
    return uint3(uint(hit),asuint(hit?q.CommittedRayT():farT),more?3u:0u);
}
