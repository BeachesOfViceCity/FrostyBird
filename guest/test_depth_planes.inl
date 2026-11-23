bool check_depth_planes(ID3D12Device* d,ID3D12CommandQueue* q){
    constexpr UINT w=61,h=37;
    for(bool stencil:{false,true}){
        DXGI_FORMAT fmt=stencil?DXGI_FORMAT_R32G8X24_TYPELESS:DXGI_FORMAT_R32_TYPELESS;
        D3D12_RESOURCE_DESC xs{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,w,h,1,1,fmt,{1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
        auto desc=desktop_desc(xs);D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
        ComPtr<ID3D12Resource> depth;ok(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,nullptr,IID_PPV_ARGS(&depth)));tag_depth_alias(depth.Get(),xs);
        D3D12_DESCRIPTOR_HEAP_DESC dsv{D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0},uav{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        ComPtr<ID3D12DescriptorHeap> depth_heap,views;ok(d->CreateDescriptorHeap(&dsv,IID_PPV_ARGS(&depth_heap)));ok(d->CreateDescriptorHeap(&uav,IID_PPV_ARGS(&views)));
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=stencil?DXGI_FORMAT_D32_FLOAT_S8X24_UINT:DXGI_FORMAT_D32_FLOAT;dv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        d->CreateDepthStencilView(depth.Get(),&dv,depth_heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=stencil?DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:DXGI_FORMAT_R32_FLOAT;uv.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        if(depth_uav_view(d,depth.Get(),&uv,views->GetCPUDescriptorHandleForHeapStart())!=S_OK)return false;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> fp{};std::array<UINT64,2> widths{};UINT64 span{};
        d->GetCopyableFootprints(&desc,0,stencil?2:1,0,fp.data(),nullptr,widths.data(),&span);
        if(widths[0]!=w*4 || (stencil && (!widths[1] || widths[1]%w)))return false;
        hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC rb{D3D12_RESOURCE_DIMENSION_BUFFER,0,span,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
        ComPtr<ID3D12Resource> read;ok(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rb,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&read)));
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};D3D12_ROOT_PARAMETER par{};par.DescriptorTable={1,&range};
        D3D12_ROOT_SIGNATURE_DESC rsd{1,&par,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> serial,cs;
        ok(D3D12SerializeRootSignature(&rsd,D3D_ROOT_SIGNATURE_VERSION_1,&serial,nullptr));ComPtr<ID3D12RootSignature> rs;
        ok(d->CreateRootSignature(0,serial->GetBufferPointer(),serial->GetBufferSize(),IID_PPV_ARGS(&rs)));
        constexpr char shader[]="RWTexture2D<float> image:register(u0);[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){if(id.x<61&&id.y<37)image[id.xy]+=float(id.x+17*id.y)/1024.0;}";
        ok(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&cs,nullptr));
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=rs.Get();ps.CS={cs->GetBufferPointer(),cs->GetBufferSize()};ComPtr<ID3D12PipelineState> state;
        ok(d->CreateComputePipelineState(&ps,IID_PPV_ARGS(&state)));ComPtr<ID3D12CommandAllocator> a;ComPtr<ID3D12GraphicsCommandList> l;
        ok(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)));ok(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),state.Get(),IID_PPV_ARGS(&l)));
        l->ClearDepthStencilView(depth_heap->GetCPUDescriptorHandleForHeapStart(),stencil?D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL:D3D12_CLEAR_FLAG_DEPTH,.25f,0x5a,0,nullptr);
        if(stencil){D3D12_RECT right{LONG(w/2),0,LONG(w),LONG(h)};l->ClearDepthStencilView(depth_heap->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_STENCIL,0.f,0xb4,1,&right);}
        auto alias=[&](D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};
            b.Transition={depth.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};BarrierBatch batch(l.Get());
            if(depth_barrier(d,batch,b)!=S_OK)throw std::runtime_error("missing depth alias transition");
            batch.flush();};
        alias(D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[]{views.Get()};l->SetDescriptorHeaps(1,heaps);l->SetComputeRootSignature(rs.Get());l->SetComputeRootDescriptorTable(0,views->GetGPUDescriptorHandleForHeapStart());
        l->Dispatch((w+7)/8,(h+7)/8,1);alias(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        for(UINT i=0;i<(stencil?2u:1u);++i){D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=depth.Get();src.SubresourceIndex=i;
            dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp[i];l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}
        ok(l->Close());ID3D12CommandList* ll[]{l.Get()};q->ExecuteCommandLists(1,ll);ComPtr<ID3D12Fence> fence;ok(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        ok(q->Signal(fence.Get(),1));ok(fence->SetEventOnCompletion(1,nullptr));BYTE* p{};ok(read->Map(0,nullptr,reinterpret_cast<void**>(&p)));UINT z=0,s=0;
        for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){float val;std::memcpy(&val,p+fp[0].Offset+y*fp[0].Footprint.RowPitch+4*x,4);z+=val==.25f+float(x+17*y)/1024.f;
            if(stencil){auto stride=widths[1]/w;auto a=p+fp[1].Offset+y*fp[1].Footprint.RowPitch+stride*x;s+=std::find(a,a+stride,BYTE(x<w/2?0x5a:0xb4))!=a+stride;}}
        D3D12_RANGE empty{};read->Unmap(0,&empty);std::printf("Depth alias %u: %u/%u depth, %u/%u stencil texels\n",UINT(fmt),z,w*h,s,stencil?w*h:0);
        if(z!=w*h || (stencil && s!=w*h))return false;
    }
    return true;
}
