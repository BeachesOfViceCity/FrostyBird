// Use the cook's small-frame raygen, miss and closest-hit programs, including
// the observed local status descriptor. A third pipeline is the same raygen
// with an intentionally incompatible route; it verifies that traces ran.
template<class Buffer,class Resource,class Upload,class Transition,class Cpu>
bool compare_city_shadow(const std::filesystem::path& dir,ID3D12Device5* d,ID3D12CommandQueue* q,ID3D12GraphicsCommandList4* l,
        ID3D12RootSignature* root,ID3D12DescriptorHeap* heap,ID3D12Resource* accel,Buffer& buf,Resource& resource,
        Upload& upload,Transition& transition,Cpu& cpu){
    constexpr UINT w=256,h=128,iters=8;
    std::array<std::vector<char>,5> codes;std::array<std::wstring,5> names;
    std::ifstream manifest(dir/"city-shadow.libs");
    for(UINT i=0;i<5;++i){std::string path,name;if(!(manifest>>path>>name))throw std::runtime_error("missing city ray library");
        codes[i]=file(dir/path);names[i].assign(name.begin(),name.end());}
    D3D12_RESOURCE_DESC tex{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,w,h,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,{1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;UINT64 span{};d->GetCopyableFootprints(&tex,0,1,0,&fp,nullptr,nullptr,&span);
    auto linear=[&](ID3D12Resource* r){D3D12_TEXTURE_COPY_LOCATION loc{};loc.pResource=r;loc.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;loc.PlacedFootprint=fp;return loc;};
    for(UINT i=0;i<6;++i){auto desc=tex;bool integer=i==1 || i==5;if(integer)desc.Format=DXGI_FORMAT_R32G32B32A32_UINT;
        auto image=resource(D3D12_HEAP_TYPE_DEFAULT,desc,D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<BYTE> data(span);
        for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){
            Floats v{};
            if(i==0)v[0]=(x%29)?1.f:0.f; // depth, with holes
            else if(i==2)v={.5f,.5f,1.f,0.f};
            else if(i==3)v[3]=float(y%7==0?7:1)/255.f;
            else if(i==5)v[0]=std::bit_cast<float>(1u);
            std::memcpy(data.data()+y*fp.Footprint.RowPitch+16*x,v.data(),16);
        }
        auto src=linear(upload(data.data(),UINT(span)).Get());src.PlacedFootprint.Footprint.Format=desc.Format;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=image.Get();l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        transition(image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        d->CreateShaderResourceView(image.Get(),nullptr,cpu(i));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    sv.RaytracingAccelerationStructure.Location=accel->GetGPUVirtualAddress();d->CreateShaderResourceView(nullptr,&sv,cpu(6));
    std::array<UINT,128> b0{};std::array<float,640> b1{};
    b0[0x28/4]=1;b0[12]=1;b0[13]=1;b0[14]=w-2;b0[15]=h-2;
    b0[0x70/4]=std::bit_cast<UINT>(7.f);b0[0x74/4]=std::bit_cast<UINT>(11.f);b0[0x78/4]=std::bit_cast<UINT>(600.f);
    auto m=b1.data()+0x2c0/4;m[0]=2;m[5]=2;m[12]=7.f-float(w+1);m[13]=11.f-float(h+1);m[14]=-160;m[15]=1;
    b1[0x440/4]=7;b1[0x444/4]=11;b1[0x448/4]=-161;
    auto cb=[&](UINT index,const auto& data){auto r=upload(data.data(),sizeof(data));D3D12_CONSTANT_BUFFER_VIEW_DESC v{r->GetGPUVirtualAddress(),sizeof(data)};d->CreateConstantBufferView(&v,cpu(index));};
    cb(14,b0);cb(15,b1);
    tex.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;auto image=resource(D3D12_HEAP_TYPE_DEFAULT,tex,D3D12_RESOURCE_STATE_COPY_DEST);
    d->CreateUnorderedAccessView(image.Get(),nullptr,nullptr,cpu(12));
    std::vector<UINT> seed(span/4,0x427c0000u);auto blank=upload(seed.data(),UINT(span));
    auto first=linear(blank.Get());D3D12_TEXTURE_COPY_LOCATION img{};img.pResource=image.Get();
    D3D12_ROOT_PARAMETER par{};par.ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;par.Descriptor={0,21301};ComPtr<ID3DBlob> bytes;
    D3D12_ROOT_SIGNATURE_DESC rd{1,&par,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE};check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,nullptr));
    ComPtr<ID3D12RootSignature> local;check(d->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&local)));
    std::array<ComPtr<ID3D12StateObject>,3> state;std::array<ComPtr<ID3D12Resource>,2> pixels;
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,4*iters,0};ComPtr<ID3D12QueryHeap> queries;
    check(d->CreateQueryHeap(&qd,IID_PPV_ARGS(&queries)));auto timings=buf(D3D12_HEAP_TYPE_READBACK,8*qd.Count,D3D12_RESOURCE_STATE_COPY_DEST);
    RayPacketPool pool;std::array<std::shared_ptr<RayPacket>,3> packets;std::array<BYTE,256> passing_table{};
    l->SetDescriptorHeaps(1,&heap);l->SetComputeRootSignature(root);l->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
    for(UINT k=0;k<3;++k){
        D3D12_DXIL_LIBRARY_DESC libs[3]{};D3D12_STATE_SUBOBJECT subs[9]{};
        for(UINT j=0;j<3;++j){auto& data=codes[j?j+2:k];libs[j].DXILLibrary={data.data(),data.size()};subs[j]={D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&libs[j]};}
        D3D12_RAYTRACING_SHADER_CONFIG config{64,8};D3D12_RAYTRACING_PIPELINE_CONFIG1 depth{1,D3D12_RAYTRACING_PIPELINE_FLAG_NONE};
        D3D12_GLOBAL_ROOT_SIGNATURE gr{root};D3D12_LOCAL_ROOT_SIGNATURE lr{local.Get()};D3D12_HIT_GROUP_DESC hit{};
        hit.HitGroupExport=L"CityPairTriangle";hit.ClosestHitShaderImport=names[4].c_str();
        subs[3]={D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&config};subs[4]={D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1,&depth};
        subs[5]={D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&gr};subs[6]={D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&hit};
        subs[7]={D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE,&lr};LPCWSTR name=names[k].c_str();D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{&subs[7],1,&name};
        subs[8]={D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,&association};
        D3D12_STATE_OBJECT_DESC so{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,9,subs};check(d->CreateStateObject(&so,IID_PPV_ARGS(&state[k])));
        ComPtr<ID3D12StateObjectProperties> props;check(state[k].As(&props));std::array<BYTE,256> table{};UINT slot=0;
        for(auto entry:{name,names[3].c_str(),hit.HitGroupExport,hit.HitGroupExport}){
            auto id=props->GetShaderIdentifier(entry);if(!id)throw std::runtime_error("missing city shadow export");std::memcpy(table.data()+64*slot++,id,32);}
        if(k==1)passing_table=table;
        bool reused=false;auto& packet=packets[k];check(pool.take(d,table.size()+16,packet,reused));if(reused)return false;
        check(packet->write(table));auto va=packet->table->GetGPUVirtualAddress();
        D3D12_DISPATCH_RAYS_DESC rays{{va,64},{va+64,32,32},{va+128,128,64},{},w,h,1};
        packet->begin(l,table.size());
        if(k)transition(image.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        l->CopyTextureRegion(&img,0,0,0,&first,nullptr);transition(image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        l->SetPipelineState1(state[k].Get());
        for(UINT j=0;j<(k==2?1:iters);++j){
            if(j){D3D12_RESOURCE_BARRIER order{};order.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;l->ResourceBarrier(1,&order);}
            if(k<2)l->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2*(k*iters+j));
            l->DispatchRays(&rays);if(k<2)l->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2*(k*iters+j)+1);
        }
        transition(image.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        packet->finish(l);
        if(k<2){pixels[k]=buf(D3D12_HEAP_TYPE_READBACK,span,D3D12_RESOURCE_STATE_COPY_DEST);auto dst=linear(pixels[k].Get());l->CopyTextureRegion(&dst,0,0,0,&img,nullptr);}
    }
    l->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,qd.Count,timings.Get(),0);
    check(l->Close());ID3D12CommandList* lists[]{l};q->ExecuteCommandLists(1,lists);ComPtr<ID3D12Fence> done;
    check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));check(q->Signal(done.Get(),1));check(done->SetEventOnCompletion(1,nullptr));
    UINT* a{},*b{};check(pixels[0]->Map(0,nullptr,reinterpret_cast<void**>(&a)));check(pixels[1]->Map(0,nullptr,reinterpret_cast<void**>(&b)));
    std::array<std::array<UINT,4>,3> status;for(UINT i=0;i<3;++i)check(packets[i]->result(status[i]));
    UINT matched=0,changed=0;std::set<std::array<UINT,4>> colors;
    for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){auto j=y*fp.Footprint.RowPitch/4+4*x;
        matched+=std::equal(a+j,a+j+4,b+j);changed+=!std::equal(a+j,a+j+4,seed.data());
        colors.insert({a[j],a[j+1],a[j+2],a[j+3]});}
    UINT64* t{},hz{};check(timings->Map(0,nullptr,reinterpret_cast<void**>(&t)));check(q->GetTimestampFrequency(&hz));std::array<double,2> ms{};
    for(UINT k=0;k<2;++k){std::array<UINT64,iters-2> dt;for(UINT j=2;j<iters;++j){auto i=2*(k*iters+j);dt[j-2]=t[i+1]-t[i];}
        std::sort(dt.begin(),dt.end());ms[k]=500.*(dt[2]+dt[3])/hz;}
    bool good=matched==w*h && changed>w*h/2 && colors.size()>1 && !status[0][0] && !status[1][0] &&
        status[2][0]==3 && (status[2][1]>>8)==0x21 && !(status[2][2]&2) && status[2][3]==0x800000;
    std::printf("City checked shadow: %u/%u RGBA pixels bit-identical, %u changed, %zu results, GPU %.3f/%.3f ms. Status %u/%u, negative %u route %#x word9 %#x\n",
        matched,w*h,changed,colors.size(),ms[0],ms[1],status[0][0],status[1][0],status[2][0],status[2][1],status[2][3]);
    D3D12_RANGE empty{};for(auto& v:pixels)v->Unmap(0,&empty);timings->Unmap(0,&empty);
    if(!good)return false;

    // Release only the failed slot after the original recording is gone.
    // Reuse its dirty status on compute with a different SBT, then legally
    // execute that closed recording twice. Each dispatch must zero all words.
    ComPtr<ID3D12CommandAllocator> reset;check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&reset)));
    check(l->Reset(reset.Get(),nullptr));check(l->Close());
    auto dirty_va=packets[2]->status->GetGPUVirtualAddress();packets[2].reset();bool reused=false;
    std::shared_ptr<RayPacket> recycled;check(pool.take(d,passing_table.size()+16,recycled,reused));
    if(!reused || dirty_va!=recycled->status->GetGPUVirtualAddress())return false;
    check(recycled->write(passing_table));UINT64 va=recycled->table->GetGPUVirtualAddress();
    D3D12_COMMAND_QUEUE_DESC cd{};cd.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;ComPtr<ID3D12CommandQueue> compute;
    check(d->CreateCommandQueue(&cd,IID_PPV_ARGS(&compute)));ComPtr<ID3D12CommandAllocator> ca;
    check(d->CreateCommandAllocator(cd.Type,IID_PPV_ARGS(&ca)));ComPtr<ID3D12GraphicsCommandList4> cl;
    check(d->CreateCommandList(0,cd.Type,ca.Get(),nullptr,IID_PPV_ARGS(&cl)));
    cl->SetDescriptorHeaps(1,&heap);cl->SetComputeRootSignature(root);cl->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
    D3D12_RESOURCE_BARRIER target_barrier{};target_barrier.Transition={image.Get(),0,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    cl->ResourceBarrier(1,&target_barrier);recycled->begin(cl.Get(),passing_table.size());cl->SetPipelineState1(state[1].Get());
    D3D12_DISPATCH_RAYS_DESC rays{{va,64},{va+64,32,32},{va+128,128,64},{},w,h,1};cl->DispatchRays(&rays);recycled->finish(cl.Get());
    std::swap(target_barrier.Transition.StateBefore,target_barrier.Transition.StateAfter);cl->ResourceBarrier(1,&target_barrier);check(cl->Close());
    check(compute->Wait(done.Get(),1));ID3D12CommandList* repeat[]{cl.Get()};
    for(UINT i=2;i<=3;++i){compute->ExecuteCommandLists(1,repeat);check(compute->Signal(done.Get(),i));check(done->SetEventOnCompletion(i,nullptr));
        std::array<UINT,4> clean;check(recycled->result(clean));if(clean!=std::array<UINT,4>{})return false;}
    std::puts("Fenced SBT packet reused on compute: both replays reset all four dirty status words.");return true;
}
