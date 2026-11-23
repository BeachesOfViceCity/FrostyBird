// Loopback-only check for deferred WinHTTP headers. The peer cannot send
// the headers until the asynchronous caller has returned to its event pump.
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <thread>

struct Notice{HANDLE sent,headers,body,error,closed;DWORD size{},operation{};};
static void CALLBACK notice(HINTERNET,DWORD_PTR user,DWORD status,void* data,DWORD size){
    auto* n=reinterpret_cast<Notice*>(user);if(!n)return;
    switch(status){
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:SetEvent(n->sent);break;
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:SetEvent(n->headers);break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:n->size=size;SetEvent(n->body);break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:n->operation=DWORD(static_cast<WINHTTP_ASYNC_RESULT*>(data)->dwResult);SetEvent(n->error);break;
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:SetEvent(n->closed);break;
    }
}
int main(){
    WSADATA w{};if(WSAStartup(MAKEWORD(2,2),&w))return 2;
    for(unsigned mode:{0u,1u,2u}){
        const bool async=mode!=2;
        sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        SOCKET server=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);int len=sizeof(addr);
        if(server==INVALID_SOCKET || bind(server,reinterpret_cast<sockaddr*>(&addr),len) || listen(server,1) ||
           getsockname(server,reinterpret_cast<sockaddr*>(&addr),&len))return 2;
        auto ev=[](){return CreateEventW(nullptr,TRUE,FALSE,nullptr);};HANDLE returned=ev();
        Notice n{ev(),ev(),ev(),ev(),ev()};bool peer_ok=false;
        std::thread peer([&]{
            SOCKET client=accept(server,nullptr,nullptr);if(client==INVALID_SOCKET)return;
            std::array<char,2048> req{};int used=0;
            while(used+1<int(req.size()) && !std::strstr(req.data(),"\r\n\r\n")){
                int count=recv(client,req.data()+used,int(req.size())-used-1,0);if(count<=0)break;used+=count;}
            peer_ok=used && (!async || WaitForSingleObject(returned,2000)==WAIT_OBJECT_0);
            if(mode!=1){constexpr char message[]="HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nGPU";
                send(client,message,sizeof(message)-1,0);}
            closesocket(client);
        });
        auto session=WinHttpOpen(L"Frosty pacing fixture",WINHTTP_ACCESS_TYPE_NO_PROXY,nullptr,nullptr,async?WINHTTP_FLAG_ASYNC:0);
        auto conn=WinHttpConnect(session,L"127.0.0.1",ntohs(addr.sin_port),0);
        auto req=WinHttpOpenRequest(conn,L"GET",L"/headers",nullptr,nullptr,nullptr,0);
        if(!req)return 2;
        if(async)WinHttpSetStatusCallback(req,notice,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0);
        bool ok=WinHttpSendRequest(req,nullptr,0,nullptr,0,0,async?reinterpret_cast<DWORD_PTR>(&n):0) &&
            (!async || WaitForSingleObject(n.sent,3000)==WAIT_OBJECT_0) && WinHttpReceiveResponse(req,nullptr);
        SetEvent(returned);
        if(mode==1)ok=ok && WaitForSingleObject(n.error,3000)==WAIT_OBJECT_0 && n.operation==API_RECEIVE_RESPONSE;
        else{
            DWORD status{},size=sizeof(status),got{};char body[4]{};
            ok=ok && (!async || WaitForSingleObject(n.headers,3000)==WAIT_OBJECT_0) &&
                WinHttpQueryHeaders(req,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr) && status==200 &&
                WinHttpReadData(req,body,3,async?nullptr:&got) && (!async || WaitForSingleObject(n.body,3000)==WAIT_OBJECT_0) &&
                (async?n.size:got)==3 && !std::memcmp(body,"GPU",3);
        }
        WinHttpCloseHandle(req);if(async)ok&=WaitForSingleObject(n.closed,3000)==WAIT_OBJECT_0;
        WinHttpCloseHandle(conn);WinHttpCloseHandle(session);closesocket(server);peer.join();
        for(auto h:{returned,n.sent,n.headers,n.body,n.error,n.closed})CloseHandle(h);
        std::printf("%s: %s\n",mode==0?"asynchronous headers and body":mode==1?"asynchronous peer-close error":"synchronous headers and body",
            ok && peer_ok?"passed":"failed");if(!ok || !peer_ok)return 1;
    }
    WSACleanup();
}
