RWByteAddressBuffer output : register(u0);
RWByteAddressBuffer record : register(u0,space21301);
[shader("raygeneration")]
void CheckedEntry(){
    uint i=DispatchRaysIndex().x;
    output.Store(4*i,103u+5u*i);
    record.Store(4*i,271u-11u*i);
}
