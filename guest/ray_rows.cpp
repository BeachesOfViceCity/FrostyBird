#define NOMINMAX
#include "ray_rows.hpp"
#include <algorithm>
#include <bit>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;
#ifdef FROSTY_VK_RAY_ROWS
#define VK_NO_PROTOTYPES
#include <vulkan.h>
#include "ray_rows_spv.h"

struct RayRowPacker::State{
    ComPtr<IUnknown> ext;
    VkDevice device{};VkPipelineLayout layout{};VkPipeline pipeline{};
    PFN_vkDestroyPipeline destroy{};PFN_vkDestroyPipelineLayout destroy_layout{};
    PFN_vkCmdBindPipeline bind{};PFN_vkCmdPushConstants push{};PFN_vkCmdDispatch dispatch{};
    PFN_vkCmdPipelineBarrier barrier{};PFN_vkGetBufferDeviceAddress address{};
    std::mutex lock;std::vector<std::shared_ptr<RayRowBuffers>> pool;
    template<class F>F slot(UINT i){return std::bit_cast<F>((*reinterpret_cast<void***>(ext.Get()))[i]);}
    ~State(){if(pipeline)destroy(device,pipeline,nullptr);if(layout)destroy_layout(device,layout,nullptr);}
    VkDeviceAddress va(ID3D12Resource* r){
        UINT64 handle{},off{};
        if(FAILED(slot<HRESULT(WINAPI*)(IUnknown*,ID3D12Resource*,UINT64*,UINT64*)>(10)(ext.Get(),r,&handle,&off)))return 0;
        VkBufferDeviceAddressInfo info{};info.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;info.buffer=std::bit_cast<VkBuffer>(handle);
        auto base=address(device,&info);return base && off<=UINT64_MAX-base?base+off:0;
    }
    std::shared_ptr<RayRowBuffers> buffers(ID3D12Device* dev,UINT64 need){
        std::lock_guard g(lock);
        for(auto& b:pool)if(b.use_count()==1 && b->capacity>=need)return b;
        auto b=std::make_shared<RayRowBuffers>();b->capacity=std::bit_ceil(need);
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER,0,b->capacity,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        D3D12_HEAP_PROPERTIES h{};h.CreationNodeMask=h.VisibleNodeMask=1;h.Type=D3D12_HEAP_TYPE_DEFAULT;
        if(FAILED(dev->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&b->packed))))return {};
        h.Type=D3D12_HEAP_TYPE_READBACK;d.Flags=D3D12_RESOURCE_FLAG_NONE;
        if(FAILED(dev->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&b->readback))))return {};
        // Never recycle a recording or submission still retaining its slot.
        if(pool.size()>=8)if(auto i=std::ranges::find_if(pool,[](auto& e){return e.use_count()==1;});i!=pool.end())pool.erase(i);
        if(pool.size()<8)pool.push_back(b);
        return b;
    }
};

bool RayRowPacker::init(ID3D12Device* dev)try{
    if(impl_)return true;
    auto s=std::make_unique<State>();
    constexpr GUID interop1{0x902d8115,0x59eb,0x4406,{0x95,0x18,0xfe,0x00,0xf9,0x91,0xee,0x65}};
    if(FAILED(dev->QueryInterface(interop1,reinterpret_cast<void**>(s->ext.GetAddressOf()))))return false;
    const VkPhysicalDeviceFeatures2* features{};
    if(FAILED(s->slot<HRESULT(WINAPI*)(IUnknown*,const VkPhysicalDeviceFeatures2**)>(6)(s->ext.Get(),&features)) || !features)return false;
    bool bda=false;
    for(auto p=static_cast<const VkBaseInStructure*>(features->pNext);p;p=p->pNext){
        if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)bda|=reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(p)->bufferDeviceAddress!=0;
        if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)bda|=reinterpret_cast<const VkPhysicalDeviceBufferDeviceAddressFeatures*>(p)->bufferDeviceAddress!=0;
    }
    if(!bda)return false;
    VkInstance instance{};VkPhysicalDevice physical{};
    if(FAILED(s->slot<HRESULT(WINAPI*)(IUnknown*,VkInstance*,VkPhysicalDevice*,VkDevice*)>(7)(s->ext.Get(),&instance,&physical,&s->device)))return false;
    // Same loader and handles as vkd3d. Command buffer interop flushes its
    // pending AS builds and invalidates native bindings before our dispatch.
    auto mod=GetModuleHandleW(L"winevulkan.dll");if(!mod)mod=GetModuleHandleW(L"vulkan-1.dll");if(!mod)return false;
    auto gip=std::bit_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(mod,"vkGetInstanceProcAddr"));if(!gip)return false;
    auto gp=std::bit_cast<PFN_vkGetDeviceProcAddr>(gip(instance,"vkGetDeviceProcAddr"));if(!gp)return false;
#define LOAD(name) std::bit_cast<PFN_vk##name>(gp(s->device,"vk" #name))
    auto make_shader=LOAD(CreateShaderModule);auto free_shader=LOAD(DestroyShaderModule);
    auto make_pipeline=LOAD(CreateComputePipelines);auto make_layout=LOAD(CreatePipelineLayout);
    s->destroy=LOAD(DestroyPipeline);s->destroy_layout=LOAD(DestroyPipelineLayout);
    s->bind=LOAD(CmdBindPipeline);s->push=LOAD(CmdPushConstants);s->dispatch=LOAD(CmdDispatch);
    s->barrier=LOAD(CmdPipelineBarrier);s->address=LOAD(GetBufferDeviceAddress);
#undef LOAD
    if(!make_shader || !free_shader || !make_pipeline || !make_layout || !s->destroy || !s->destroy_layout ||
       !s->bind || !s->push || !s->dispatch || !s->barrier || !s->address)return false;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,20};
    VkPipelineLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;li.pushConstantRangeCount=1;li.pPushConstantRanges=&range;
    if(make_layout(s->device,&li,nullptr,&s->layout)!=VK_SUCCESS)return false;
    VkShaderModuleCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;ci.codeSize=sizeof(frosty_ray_rows);ci.pCode=frosty_ray_rows;
    VkShaderModule module{};if(make_shader(s->device,&ci,nullptr,&module)!=VK_SUCCESS)return false;
    VkComputePipelineCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;pi.layout=s->layout;
    pi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pi.stage.module=module;pi.stage.pName="main";
    auto result=make_pipeline(s->device,VK_NULL_HANDLE,1,&pi,nullptr,&s->pipeline);free_shader(s->device,module,nullptr);
    if(result!=VK_SUCCESS)return false;
    impl_=std::move(s);return true;
}catch(const std::bad_alloc&){return false;}

std::shared_ptr<RayRowBuffers> RayRowPacker::record(ID3D12Device* dev,ID3D12GraphicsCommandList* list,ID3D12Resource* src,UINT64 offset,UINT count)try{
    if(!impl_ || !src || !count || count>=1000000 || offset%16 ||
       (list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT && list->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE))return {};
    auto d=src->GetDesc();if(d.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER || offset>d.Width || UINT64(count)*64>d.Width-offset)return {};
    auto& s=*impl_;auto srcva=s.va(src);if(!srcva || srcva>UINT64_MAX-offset)return {};
    auto n=bytes(count);auto b=s.buffers(dev,n);if(!b)return {};
    auto dstva=s.va(b->packed.Get());if(!dstva)return {};
    VkCommandBuffer cb{};
    if(FAILED(s.slot<HRESULT(WINAPI*)(IUnknown*,ID3D12CommandList*,VkCommandBuffer*)>(16)(s.ext.Get(),list,&cb)))return {};
    VkMemoryBarrier dep{};dep.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    dep.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;dep.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    s.barrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&dep,0,nullptr,0,nullptr);
    struct{UINT64 source,dest;UINT count;} args{srcva+offset,dstva,count};
    s.bind(cb,VK_PIPELINE_BIND_POINT_COMPUTE,s.pipeline);s.push(cb,s.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,20,&args);s.dispatch(cb,groups(count),1,1);
    dep.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;dep.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    // Order the snapshot read before any later writer to the original rows.
    s.barrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&dep,0,nullptr,0,nullptr);
    if(FAILED(s.slot<HRESULT(WINAPI*)(IUnknown*,ID3D12CommandList*)>(17)(s.ext.Get(),list)))ExitProcess(ERROR_DEVICE_NOT_AVAILABLE);
    D3D12_RESOURCE_BARRIER t{};t.Transition={b->packed.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1,&t);list->CopyBufferRegion(b->readback.Get(),0,b->packed.Get(),0,n);
    std::swap(t.Transition.StateBefore,t.Transition.StateAfter);list->ResourceBarrier(1,&t);
    return b;
}catch(const std::bad_alloc&){return {};}
#else
struct RayRowPacker::State{};
bool RayRowPacker::init(ID3D12Device*){return false;}
std::shared_ptr<RayRowBuffers> RayRowPacker::record(ID3D12Device*,ID3D12GraphicsCommandList*,ID3D12Resource*,UINT64,UINT){return {};}
#endif
RayRowPacker::RayRowPacker()=default;
RayRowPacker::~RayRowPacker()=default;
