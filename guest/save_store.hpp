#pragma once
#include <windows.h>
namespace local_save {
struct BlobInfo {const char* name;UINT32 size;};
struct Blob {BlobInfo info;BYTE* data;};
struct ContainerInfo {const char* name;const char* display;UINT32 count;UINT64 size;INT64 modified;BOOLEAN needs_sync;};
using ContainerCallback=BOOLEAN(WINAPI*)(const ContainerInfo*,void*);
using BlobCallback=BOOLEAN(WINAPI*)(const BlobInfo*,void*);
HRESULT WINAPI create(void*,void*,const char*,void**);
void WINAPI close(void*,void*);
HRESULT WINAPI remove(void*,void*,const char*);
HRESULT WINAPI info(void*,void*,const char*,void*,ContainerCallback);
HRESULT WINAPI enumerate(void*,void*,void*,ContainerCallback);
HRESULT WINAPI enumerate_by(void*,void*,const char*,void*,ContainerCallback);
HRESULT WINAPI blobs(void*,void*,void*,BlobCallback);
HRESULT WINAPI blobs_by(void*,void*,const char*,void*,BlobCallback);
HRESULT WINAPI read(void*,void*,const char**,UINT*,SIZE_T,Blob*);
HRESULT WINAPI update(void*,void*,const char*,void**);
void WINAPI close_update(void*,void*);
HRESULT WINAPI write(void*,void*,const char*,const BYTE*,SIZE_T);
HRESULT WINAPI erase(void*,void*,const char*);
HRESULT WINAPI submit(void*,void*);
HRESULT initialize(UINT64 user,const char* scid,void** out);
void WINAPI close_provider(void*,void*);
}
