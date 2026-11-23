// Independent oracle: encode array slice, face-local texel and mip directly
// in an R32_FLOAT cube array. No part of this resource is used by the title.
struct CubeFixture {
    static constexpr UINT levels=5,slices=12;
    ComPtr<ID3D12Resource> image,upload;
    ComPtr<ID3D12DescriptorHeap> heap;
    static float texel(UINT face,UINT mip,UINT x,UINT y){return float(10000*face+1000*mip+32*y+x);}
    static float expected(UINT x,UINT y,UINT mip){
        UINT face=(x/4<6?x/4:5)+6*((y/4)&1u);
        return texel(face,mip,(4+(x&3))>>mip,(4+(y&3))>>mip);
    }
    void bind(ID3D12Device* d,ID3D12GraphicsCommandList* list,bool planar=false){
        D3D12_HEAP_PROPERTIES hp{};hp.CreationNodeMask=hp.VisibleNodeMask=1;hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,16,16,UINT16(planar?1:slices),levels,DXGI_FORMAT_R32_FLOAT,
                                {1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE};
        CHECK(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&image)));
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,slices*levels> fp{};UINT64 size{};
        UINT count=td.DepthOrArraySize*levels;
        d->GetCopyableFootprints(&td,0,count,0,fp.data(),nullptr,nullptr,&size);
        D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER,0,size,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
        hp.Type=D3D12_HEAP_TYPE_UPLOAD;
        CHECK(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
        BYTE* p{};CHECK(upload->Map(0,nullptr,reinterpret_cast<void**>(&p)));
        for(UINT i=0;i<count;++i){auto& f=fp[i].Footprint;
            for(UINT y=0;y<f.Height;++y){auto row=reinterpret_cast<float*>(p+fp[i].Offset+y*f.RowPitch);
                for(UINT x=0;x<f.Width;++x)row[x]=texel(i/levels,i%levels,x,y);}
        }
        upload->Unmap(0,nullptr);
        for(UINT i=0;i<count;++i){D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=upload.Get();
            a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=fp[i];b.pResource=image.Get();b.SubresourceIndex=i;
            list->CopyTextureRegion(&b,0,0,0,&a,nullptr);}
        D3D12_RESOURCE_BARRIER tr{};tr.Transition={image.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        list->ResourceBarrier(1,&tr);
        D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        CHECK(d->CreateDescriptorHeap(&h,IID_PPV_ARGS(&heap)));
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=td.Format;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if(planar){v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.Texture2D.MipLevels=levels;}
        else{v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;v.TextureCubeArray.MipLevels=levels;v.TextureCubeArray.NumCubes=2;}
        d->CreateShaderResourceView(image.Get(),&v,heap->GetCPUDescriptorHandleForHeapStart());
        auto ptr=heap.Get();list->SetDescriptorHeaps(1,&ptr);list->SetGraphicsRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
    }
};
