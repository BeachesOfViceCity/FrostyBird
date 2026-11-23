#include <windows.h>
#include <cstdio>
#include <cwchar>

int wmain(int argc,wchar_t** argv) {
    if(argc!=4)return 2;
    wchar_t *end{};auto buttons=std::wcstoul(argv[2],&end,0);if(*end||buttons>0x3fff)return 2;
    auto duration=std::wcstoul(argv[3],&end,10);if(*end||!duration||duration>10000)return 2;
    HANDLE h=OpenFileMappingW(FILE_MAP_WRITE,FALSE,argv[1]);if(!h)return 1;
    auto p=static_cast<LONG*>(MapViewOfFile(h,FILE_MAP_WRITE,0,0,4));if(!p){CloseHandle(h);return 1;}
    InterlockedExchange(p,LONG(buttons));Sleep(duration);InterlockedExchange(p,0);
    UnmapViewOfFile(p);CloseHandle(h);return 0;
}
