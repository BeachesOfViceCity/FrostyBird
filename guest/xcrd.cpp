// The DirectStorage subset used by Frosty's FXSXFileIoStoreImpl. Submit
// publishes bounded work. Status/fence markers retire in original order.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include "storage_pack.hpp"
#ifdef FROSTY_FAST_INFLATE
#include <libdeflate.h>
#else
#include <zlib.h>
#endif
#include <atomic>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace {
constexpr GUID factory_iid{0xfb3b54ac,0xbf82,0x463a,{0xb7,0x77,0x73,0x19,0x58,0x37,0x86,0x4d}};
constexpr GUID queue_iid{0xa3f6649a,0x8c2c,0x4b3a,{0x8b,0x6e,0x10,0x18,0x6a,0x04,0xb9,0xbc}};
constexpr GUID status_iid{0xcd66d752,0x2786,0x44f7,{0x90,0x70,0xe2,0xa6,0xfd,0xad,0x3f,0x1b}};
constexpr GUID file_iid{0x43704f9f,0x27c6,0x4a9f,{0x88,0x15,0xd1,0x65,0x27,0x89,0x4d,0xf4}};

template<class T, const GUID& id>
struct Object : IUnknown {
    std::atomic<ULONG> refs{1};
    HRESULT WINAPI QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != id && iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = static_cast<T*>(this);
        AddRef();
        return S_OK;
    }
    ULONG WINAPI AddRef() override { return ++refs; }
    ULONG WINAPI Release() override {
        auto n = --refs;
        if (!n) delete static_cast<T*>(this);
        return n;
    }
};

struct File final : Object<File, file_iid> {
    HANDLE handle;
    std::mutex lock;
    unsigned in_flight{};bool closed{},train{};
    File(HANDLE h,const wchar_t* path) : handle(h) {
        auto p=wcsrchr(path,L'/'),q=wcsrchr(path,L'\\');auto leaf=p && (!q || p>q)?p+1:q?q+1:path;
        train=!_wcsicmp(leaf,L"Frosty-XSX.ucas");
    }
    ~File() { Close(); }
    virtual void WINAPI Close() {
        std::lock_guard g(lock);closed=true;
        if (!in_flight && handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); handle = INVALID_HANDLE_VALUE; }
    }
    bool retain_read(){
        std::lock_guard g(lock);if(closed)return false;++in_flight;AddRef();return true;
    }
    void release_read(){
        {std::lock_guard g(lock);if(!--in_flight && closed && handle!=INVALID_HANDLE_VALUE){CloseHandle(handle);handle=INVALID_HANDLE_VALUE;}}
        Release();
    }
    HRESULT read(void* dest, DWORD size, std::uint64_t offset) {
        OVERLAPPED op{};
        op.Offset = DWORD(offset);
        op.OffsetHigh = DWORD(offset >> 32);
        struct Done{HANDLE h=CreateEventW(nullptr,TRUE,FALSE,nullptr);~Done(){if(h)CloseHandle(h);}};
        static thread_local Done done;
        op.hEvent=done.h;
        if (!op.hEvent) return HRESULT_FROM_WIN32(GetLastError());
        if(!ResetEvent(op.hEvent))return HRESULT_FROM_WIN32(GetLastError());
        DWORD got{};
        HRESULT hr = S_OK;
        if (!ReadFile(handle, dest, size, &got, &op) &&
            (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(handle, &op, &got, TRUE)))
            hr = HRESULT_FROM_WIN32(GetLastError());
        else if (got != size) hr = HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        return hr;
    }
};
struct Status final : Object<Status, status_iid> {
    std::vector<std::atomic<UINT64>> slots;
    explicit Status(UINT n) : slots(n) { for (auto& s : slots) s = S_OK; }
    virtual bool WINAPI IsComplete(UINT i) { return i < slots.size() && HRESULT(UINT(slots[i].load())) != E_PENDING; }
    virtual HRESULT WINAPI GetHResult(UINT i) { return i < slots.size() ? HRESULT(UINT(slots[i].load())) : E_INVALIDARG; }
    UINT64 begin(UINT i){
        auto old=slots[i].load();UINT64 v;
        do{v=((old&0xffffffff00000000ull)+(1ull<<32))|UINT(E_PENDING);}while(!slots[i].compare_exchange_weak(old,v));
        return v;
    }
    void complete(UINT i,UINT64 ticket,HRESULT result){slots[i].compare_exchange_strong(ticket,(ticket&0xffffffff00000000ull)|UINT(result));}
};
struct Request {
    std::uint64_t options;
    void* dest;
    std::uint64_t dest_page_offset;
    UINT dest_size, pad;
    File* file;
    std::uint64_t offset;
    UINT size, intermediate;
    std::uint64_t tag;
    const char* name;
};
static_assert(sizeof(Request) == 72 && offsetof(Request, size) == 48);
struct Desc { std::uint64_t source; UINT16 capacity; std::int8_t priority; };
StoragePack& store(){static StoragePack s;return s;}
void trace_blocks(const Request& r,bool hit){
    struct Trace{
        std::mutex lock;FILE* f{};
        Trace(){wchar_t p[2048];auto n=GetEnvironmentVariableW(L"FROSTY_STORAGE_TRACE",p,2048);if(n && n<2048)f=_wfopen(p,L"wb");}
        ~Trace(){if(f)std::fclose(f);}
    };
    static Trace trace;if(!trace.f || !r.file->train)return;
    FILETIME ft{};GetSystemTimePreciseAsFileTime(&ft);
    struct Row{UINT64 us,offset;UINT compressed,decoded,hit,pad;} row{(((UINT64(ft.dwHighDateTime)<<32)|ft.dwLowDateTime)-116444736000000000ull)/10,
        r.offset,r.size,r.dest_size,UINT(hit),0};static_assert(sizeof(Row)==32);
    std::lock_guard g(trace.lock);std::fwrite(&row,sizeof(row),1,trace.f);
}

struct Queue final : Object<Queue, queue_iid> {
    struct Item { Request request{}; Status* status{}; UINT index{}; UINT64 value{}; ID3D12Fence* fence{};HRESULT error=S_OK; };
    std::vector<Item> items;
    std::deque<std::vector<Item>> submitted;
    std::mutex lock;
    std::condition_variable cv;
    HRESULT first_error = S_OK;
    size_t capacity,outstanding{};bool closing{};
    std::thread worker;
    explicit Queue(size_t n) : capacity(n) { items.reserve(n);worker=std::thread([this]{work();}); }
    ~Queue() {
        {std::lock_guard g(lock);flush();closing=true;}
        cv.notify_all();worker.join();
        if(GetEnvironmentVariableA("FROSTY_TEST_STORAGE_GATHER",nullptr,0))
            std::fprintf(stderr,"frosty/storage-fixture: %llu gathered reads for %llu blocks, %u persistent decoders\n",gathered,gathered_blocks,decoders.made);
    }

    static HRESULT decode(const Request& r,const BYTE* compressed){
        if(store().copy(r.offset,compressed,r.size,r.dest,r.dest_size)){trace_blocks(r,true);return S_OK;}
#ifdef FROSTY_FAST_INFLATE
            using Decoder=std::unique_ptr<libdeflate_decompressor,decltype(&libdeflate_free_decompressor)>;
            static thread_local Decoder decoder(libdeflate_alloc_decompressor(),libdeflate_free_decompressor);
            if(!decoder)return E_OUTOFMEMORY;
            size_t actual{};
            auto result=libdeflate_zlib_decompress(decoder.get(),compressed,r.size,r.dest,r.dest_size,&actual);
            auto hr=result==LIBDEFLATE_SUCCESS && actual==r.dest_size?S_OK:HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
#else
            z_stream zs{};
            // Wine's PE zlib is a Z_SOLO build: allocator callbacks are mandatory.
            zs.zalloc = [](void*, uInt n, uInt s) -> void* { return std::calloc(n, s); };
            zs.zfree = [](void*, void* p) { std::free(p); };
            zs.next_in = const_cast<BYTE*>(compressed);
            zs.avail_in = r.size;
            zs.next_out = static_cast<BYTE*>(r.dest);
            zs.avail_out = r.dest_size;
            int init = inflateInit(&zs);
            if (init != Z_OK) return init == Z_MEM_ERROR ? E_OUTOFMEMORY : E_FAIL;
            int rc = inflate(&zs, Z_FINISH);
            bool ok = rc == Z_STREAM_END && zs.total_out == r.dest_size;
            inflateEnd(&zs);
            auto hr=ok ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
#endif
        if(SUCCEEDED(hr))trace_blocks(r,false);
        return hr;
    }
    static HRESULT read(const Request& r,const BYTE* packed=nullptr) {
        if (!r.dest || !r.file || !r.size || !r.dest_size) return E_INVALIDARG;
        if (!r.options && r.size == r.dest_size) return r.file->read(r.dest, r.size, r.offset);
        if (r.options != 1 || r.intermediate) return E_NOTIMPL;
        if(packed)return decode(r,packed);
        try {
            static thread_local std::vector<BYTE> block;
            std::vector<BYTE> large;auto& compressed=r.size<=(2u<<20)?block:large;compressed.resize(r.size);
            auto hr=r.file->read(compressed.data(),r.size,r.offset);
            return FAILED(hr)?hr:decode(r,compressed.data());
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    }
    void record_error(HRESULT hr){if(FAILED(hr)&&SUCCEEDED(first_error))first_error=hr;}
    // Files are immutable while open. A status marker or an overlapping
    // destination ends the parallel portion of a batch.
    static bool independent(const Request& a,const Request& b){
        auto x=reinterpret_cast<UINT_PTR>(a.dest),y=reinterpret_cast<UINT_PTR>(b.dest);
        return x && y && a.dest_size<=UINTPTR_MAX-x && b.dest_size<=UINTPTR_MAX-y &&
            (x+a.dest_size<=y || y+b.dest_size<=x);
    }
    static bool private_output(const Request& r){
        MEMORY_BASIC_INFORMATION m{};auto x=reinterpret_cast<UINT_PTR>(r.dest);
        // Distinct mapped Xbox addresses can alias one physical page. Keep
        // those writes in order; only private storage may be grouped by VA.
        return VirtualQuery(r.dest,&m,sizeof(m)) && m.State==MEM_COMMIT && m.Type==MEM_PRIVATE &&
            x>=reinterpret_cast<UINT_PTR>(m.BaseAddress) && r.dest_size<=m.RegionSize-(x-reinterpret_cast<UINT_PTR>(m.BaseAddress));
    }
    struct Job{const Request* r{};const BYTE* packed{};HRESULT hr=E_FAIL;};
    // One coordinator per queue; slots are borrowed until join(). Persistent
    // decoder threads avoid creating/waiting/closing a Windows work object
    // for every 64 KiB block. Markers still retire on the coordinator.
    struct DecodePool{
        SRWLOCK lock=SRWLOCK_INIT;CONDITION_VARIABLE ready=CONDITION_VARIABLE_INIT,done=CONDITION_VARIABLE_INIT;
        std::array<std::thread,3> threads;std::array<Job*,3> next{};
        unsigned made{},remaining{};bool closing{};
        void run(unsigned i){
            for(;;){
                AcquireSRWLockExclusive(&lock);
                while(!closing && !next[i])SleepConditionVariableSRW(&ready,&lock,INFINITE,0);
                if(closing){ReleaseSRWLockExclusive(&lock);return;}
                auto j=next[i];next[i]=nullptr;ReleaseSRWLockExclusive(&lock);
                j->hr=read(*j->r,j->packed);
                AcquireSRWLockExclusive(&lock);if(!--remaining)WakeConditionVariable(&done);ReleaseSRWLockExclusive(&lock);
            }
        }
        unsigned start(Job* first,unsigned n){
            if(!n)return 0;
            try{while(made<n){threads[made]=std::thread([this,i=made]{run(i);});++made;}}
            catch(const std::system_error&){/* coordinator handles any remaining jobs */}
            catch(const std::bad_alloc&){}
            n=std::min(n,made);AcquireSRWLockExclusive(&lock);remaining=n;
            for(unsigned i=0;i<n;++i)next[i]=first+i;
            WakeAllConditionVariable(&ready);ReleaseSRWLockExclusive(&lock);return n;
        }
        void join(){AcquireSRWLockExclusive(&lock);while(remaining)SleepConditionVariableSRW(&done,&lock,INFINITE,0);ReleaseSRWLockExclusive(&lock);}
        ~DecodePool(){AcquireSRWLockExclusive(&lock);closing=true;WakeAllConditionVariable(&ready);ReleaseSRWLockExclusive(&lock);
            for(unsigned i=0;i<made;++i)threads[i].join();}
    } decoders;
    std::vector<BYTE> packed_input;UINT64 gathered{},gathered_blocks{};
    void gather(Job* jobs,unsigned count){
        if(count<2)return;
        auto file=jobs[0].r->file;UINT64 low=UINT64_MAX,high=0,sum=0;
        for(unsigned j=0;j<count;++j){const auto& r=*jobs[j].r;
            if(r.file!=file || r.options!=1 || r.intermediate || !r.size || r.offset>UINT64_MAX-r.size)return;
            low=std::min(low,r.offset);high=std::max(high,r.offset+r.size);sum+=r.size;}
        if(high-low>(1u<<20) || high-low>sum+4096*(count-1))return;
        try{packed_input.resize(size_t(high-low));}catch(const std::bad_alloc&){return;}
        // A partial/error read must recover per original request, so a bad
        // neighbor cannot turn an otherwise valid status into a failure.
        if(FAILED(file->read(packed_input.data(),DWORD(high-low),low)))return;
        for(unsigned j=0;j<count;++j)jobs[j].packed=packed_input.data()+(jobs[j].r->offset-low);
        ++gathered;gathered_blocks+=count;
    }
    void drain(std::vector<Item>& batch) {
        for(size_t i=0;i<batch.size();){
            auto& item=batch[i];
            if (item.status) {
                item.status->complete(item.index,item.value,first_error);
                item.status->Release();
                first_error = S_OK;++i;
            }else if(item.fence){
                record_error(item.fence->Signal(item.value));item.fence->Release();++i;
            }else if(FAILED(item.error)){
                record_error(item.error);++i;
            } else {
                std::array<Job,4> jobs{};unsigned count=1;jobs[0].r=&item.request;
                bool parallel=i+1<batch.size() && item.request.dest_size>=32768 && private_output(item.request);
                while(parallel && count<jobs.size() && i+count<batch.size()){
                    auto& next=batch[i+count];if(!next.request.file || next.status || next.fence || FAILED(next.error) ||
                       next.request.dest_size<32768 || !private_output(next.request))break;
                    bool ok=true;for(unsigned j=0;j<count;++j)ok&=independent(*jobs[j].r,next.request);
                    if(!ok)break;
                    jobs[count++].r=&next.request;
                }
                gather(jobs.data(),count);auto other=decoders.start(jobs.data()+1,count-1);
                jobs[0].hr=read(*jobs[0].r,jobs[0].packed);
                for(unsigned j=other+1;j<count;++j)jobs[j].hr=read(*jobs[j].r,jobs[j].packed);
                if(other)decoders.join();
                for(unsigned j=0;j<count;++j){
                    auto& job=jobs[j];
                    if(FAILED(job.hr))std::fprintf(stderr,"frosty/storage: read at %#llx (%u -> %u bytes): %#lx\n",
                        job.r->offset,job.r->size,job.r->dest_size,static_cast<unsigned long>(job.hr));
                    record_error(job.hr);job.r->file->release_read();
                }
                i+=count;
            }
        }
    }
    void flush(){
        if(items.empty())return;
        submitted.emplace_back();submitted.back().swap(items);cv.notify_all();
    }
    void work(){
        for(;;){
            std::vector<Item> batch;
            {std::unique_lock g(lock);cv.wait(g,[&]{return closing || !submitted.empty();});
                if(submitted.empty())return;
                batch=std::move(submitted.front());submitted.pop_front();}
            drain(batch);
            {std::lock_guard g(lock);outstanding-=batch.size();}
            cv.notify_all();
        }
    }
    void room(std::unique_lock<std::mutex>& g){
        if(outstanding>=capacity){flush();cv.wait(g,[&]{return outstanding<capacity;});}
    }
    void push(Item item){
        items.push_back(item);++outstanding;
        if(items.size()>=capacity/2)flush();
    }
    virtual void WINAPI EnqueueRequest(const Request* req) {
        std::unique_lock g(lock);room(g);Item item{};
        if(!req || !req->file)item.error=E_POINTER;
        else if(!req->file->retain_read())item.error=E_HANDLE;
        else item.request=*req;
        push(item);
    }
    virtual void WINAPI EnqueueStatus(Status* status, UINT index) {
        std::unique_lock g(lock);room(g);Item item{};
        if(!status || index>=status->slots.size())item.error=E_INVALIDARG;
        else{status->AddRef();item.status=status;item.index=index;item.value=status->begin(index);}
        push(item);
    }
    virtual void WINAPI EnqueueSignal(IUnknown* fence,UINT64 value) {
        std::unique_lock g(lock);room(g);Item item{};item.value=value;
        item.error=fence?fence->QueryInterface(IID_PPV_ARGS(&item.fence)):E_POINTER;push(item);
    }
    virtual void WINAPI Submit() { std::lock_guard guard(lock);flush(); }
};
struct Factory final : Object<Factory, factory_iid> {
    virtual HRESULT WINAPI CreateQueue(const Desc* desc, REFIID iid, void** out) {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != queue_iid) return E_NOINTERFACE;
        if (!desc || desc->source || desc->capacity < 2) return E_INVALIDARG;
        try { *out = new Queue(desc->capacity); }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (const std::system_error&) { return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY); }
        return S_OK;
    }
    virtual HRESULT WINAPI OpenFile(const wchar_t* path, REFIID iid, void** out) {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != file_iid) return E_NOINTERFACE;
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        *out = new(std::nothrow) File(h,path);
        if (!*out) { CloseHandle(h); return E_OUTOFMEMORY; }
        std::fprintf(stderr, "frosty/storage: opened %ls\n", path);
        return S_OK;
    }
    virtual HRESULT WINAPI CreateStatusArray(UINT n, const char*, REFIID iid, void** out) {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != status_iid) return E_NOINTERFACE;
        if (!n || n > 65536) return E_INVALIDARG;
        try { *out = new Status(n); }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        return S_OK;
    }
};
}

extern "C" __declspec(dllexport) void WINAPI FrostyStorageCacheStats(UINT64* hits,UINT64* bytes,UINT64* rejected){
    *hits=store().hits();*bytes=store().bytes();*rejected=store().rejected();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DStorageGetFactory(REFIID id, void** out) {
    static Factory factory; // holds its original reference for process lifetime
    return factory.QueryInterface(id, out);
}
