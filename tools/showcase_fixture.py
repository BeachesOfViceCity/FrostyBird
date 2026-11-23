"""Stage an opt-in developer level shortcut in a private IoStore partition."""
import ctypes
from ctypes.util import find_library
import hashlib
import re
import struct
import zlib

from frosty import link_siblings
from retail_mesh_fields import IoStore,u32

GAMESTATE="Frosty/Content/Blueprint/Gameplay/BP_FrostyGameState.uasset"
WORLD="Frosty/Content/Map/P_Construct_WP.umap"
OUTRO="Frosty/Content/Cinematic/CarChase/FinalChase/99_Outro/OutroMaster.uasset"


def names(b,at):
    count,size=struct.unpack_from("<II",b,at)
    off=at+16+8*count;pos=off+2*count;start=pos;out=[]
    for j in range(count):
        v=int.from_bytes(b[off+2*j:off+2*j+2],"big");end=pos+(v&32767)*(1+(v>>15))
        out.append(b[pos:end].decode("utf-16le" if v>>15 else "utf8"));pos=end
    if pos-start!=size:raise ValueError("invalid cooked name batch")
    return out,pos


def iohash(data):
    # This container uses FIoHash (BLAKE3-160), padded to 32 bytes.
    lib=ctypes.CDLL(find_library("blake3") or "libblake3.so");ctx=ctypes.create_string_buffer(4096)
    result=ctypes.create_string_buffer(20);lib.blake3_hasher_init(ctx)
    lib.blake3_hasher_update(ctx,data,ctypes.c_size_t(len(data)))
    lib.blake3_hasher_finalize(ctx,result,ctypes.c_size_t(20));return result.raw+bytes(12)


def script_import(global_data,owner,name):
    gn,start=names(global_data,0);count=u32(global_data,start)
    script={k:(gn[n&0x3fffffff],outer) for n,_,k,outer,_ in struct.iter_unpack(
        "<IIQQQ",global_data[start+4:start+4+32*count])}
    key,=[k for k,(n,o) in script.items() if n==name and script.get(o,("",))[0]==owner]
    return key


def exports_for(package):
    nm,_=names(package,40);h=struct.unpack_from("<10I",package)
    pos=h[0];exports={}
    for idx,cmd in struct.iter_unpack("<II",package[h[7]:h[8]]):
        if cmd!=1:continue
        at=h[6]+72*idx;size=struct.unpack_from("<Q",package,at+8)[0]
        name=nm[u32(package,at+16)&0x3fffffff];number=u32(package,at+20)
        if number:name+=f"_{number-1}"
        exports[name]=(pos,size,idx);pos+=size
    return h,exports


def forwarder(package,entries,event,graph):
    pos,size,_=entries[event];body=package[pos:pos+size]
    if (size!=52 or struct.unpack_from("<II",body,18)!=(18,14) or
        body[26:32]!=b"\x46"+struct.pack("<i",entries[graph][2]+1)+b"\x1d" or body[36:40]!=b"\x16\x04\x0b\x53"):
        raise ValueError("unrecognized event forwarder: "+event)
    return pos


def direct_sandbox(original,timer):
    h,entries=exports_for(original)
    start=forwarder(original,entries,"ReceiveEnterStartflow","ExecuteUbergraph_BP_FrostyGameState")
    # The boot movie's completion itself selects Startflow. Let the
    # 21-second movie and frontend fade complete before requesting Sandbox.
    imports=(h[6]-h[5])//8
    call=(b"\x68"+struct.pack("<i",-imports-1)+b"\x17\x1fRequestStateChangeToSandbox\0"+
          b"\x1e"+struct.pack("<f",32.)+b"\x28"+(b"\x1e"+bytes(4))*2+b"\x16")
    data=bytearray(original)
    struct.pack_into("<II",data,start+18,18+len(call)+4,14+len(call))
    at=h[6]+72*entries["ReceiveEnterStartflow"][2];before=struct.unpack_from("<Q",data,at)[0]
    struct.pack_into("<Q",data,at+8,52+len(call))
    for e in range(h[6],h[7],72):
        disk=struct.unpack_from("<Q",data,e)[0]
        if disk>before:struct.pack_into("<Q",data,e,disk+len(call))
    data[start+37:start+37]=call # between EndFunctionParms and Return
    data[h[6]:h[6]]=struct.pack("<Q",timer)
    for field in (0,6,7,8):struct.pack_into("<I",data,field*4,h[field]+8)
    return bytes(data)


def select_showcase(original):
    _,entries=exports_for(original);graph="ExecuteUbergraph_P_Construct_WP"
    a=forwarder(original,entries,"PlaySandboxIntro",graph)
    b=forwarder(original,entries,"PlayChaseOutro",graph)
    # Select the existing post-chase entry in the original level graph rather
    # than the short main-menu intro. Only the integer entry point changes.
    return original[:a+32]+original[b+32:b+36]+original[a+36:]


def director_events(package):
    ns,_=names(package,40);_,entries=exports_for(package);events={}
    for name,(pos,size,_) in entries.items():
        if not re.fullmatch(r"SequenceEvent__ENTRYPOINTSequenceDirector_\d+",name) or size!=51:continue
        # EX_LocalVirtualFunction FName, EndParms, Return(Nothing), EndOfScript.
        body=package[pos:pos+size]
        if body[26]!=0x45 or u32(body,31) or body[35:39]!=b"\x16\x04\x0b\x53":continue
        events[name]=ns[u32(body,27)&0x3fffffff]
    if not {"PlayMetaSoundsCallout","PlayMetaHumanCallout","SD_RestoreDefaultDeviceProfile"}<=set(events.values()):
        raise ValueError("cannot identify the packaged showcase director's finish/callout events")
    return events


def stage(mount,base):
    root=base/"package";dst=root/"Mount";dst.mkdir(parents=True)
    (root/"EmbeddedXvd").symlink_to(mount.parent/"EmbeddedXvd")
    src=mount
    for part in ("Frosty","Content","Paks"):
        skip={part.casefold()}
        if part=="Content":skip.add("config")
        link_siblings(src,dst,skip)
        if part=="Content":
            config=dst/"Config";config.mkdir()
            if (src/"Config").is_dir():link_siblings(src/"Config",config,{"userengine.ini"})
            ini=src/"Config/UserEngine.ini";text=ini.read_text() if ini.is_file() else ""
            (config/"UserEngine.ini").write_text(text+"\n[Core.Log]\nLogMovieScene=VeryVerbose\n")
        src=src/part;dst=dst/part;dst.mkdir()
    tocname="Frosty-XSX.utoc";link_siblings(src,dst,{tocname.casefold()})
    store=IoStore(src/tocname)
    try:
        toc=bytearray(store.toc);files=dict(store.files_in())
        if toc[80]!=9 or u32(toc,52)!=1:raise ValueError("unsupported developer fixture container")
        meta=store.blocks+12*u32(toc,28)+u32(toc,36)*u32(toc,40)+u32(toc,48)
        if len(toc)-meta!=33*u32(toc,24):raise ValueError("unknown chunk metadata")
        partition=1<<(src/"Frosty-XSX.ucas").stat().st_size.bit_length()
        struct.pack_into("<I",toc,52,2);struct.pack_into("<Q",toc,88,partition)
        timer=script_import((src/"global.ucas").read_bytes(),"KismetSystemLibrary","K2_SetTimer")
        payload=bytearray();report={}
        for asset,edit in ((GAMESTATE,lambda b:direct_sandbox(b,timer)),(WORLD,select_showcase)):
            i=files[asset];p=store.lengths+10*i;offset=int.from_bytes(toc[p:p+5],"big");size=int.from_bytes(toc[p+5:p+10],"big")
            first,skip=divmod(offset,store.block_size);orig=bytes(store.read(i,0,size));at=meta+33*i
            if skip or iohash(orig)!=toc[at:at+32]:raise ValueError("unexpected original chunk: "+asset)
            data=edit(orig);block_size=store.block_size
            if (size-1)//block_size!=(len(data)-1)//block_size:raise ValueError("fixture exceeds allocated blocks")
            toc[at:at+32]=iohash(data);toc[p+5:p+10]=len(data).to_bytes(5,"big");modified=[]
            for pos in range(0,len(data),block_size):
                raw=data[pos:pos+block_size]
                if raw==orig[pos:pos+block_size]:continue
                block=first+pos//block_size;entry=store.blocks+block*12
                if store.block(block)!=orig[pos:pos+block_size]:raise ValueError("shared fixture block")
                compressed=zlib.compress(raw);toc[entry:entry+5]=(partition+len(payload)).to_bytes(5,"little")
                toc[entry+5:entry+8]=len(compressed).to_bytes(3,"little");toc[entry+8:entry+11]=len(raw).to_bytes(3,"little")
                toc[entry+11]=store.methods.index(b"Zlib");payload+=compressed+bytes((-len(compressed))%65536);modified.append(block)
            report[asset]=dict(blocks=modified,original_sha256=hashlib.sha256(orig).hexdigest(),private_sha256=hashlib.sha256(data).hexdigest())
        (dst/"Frosty-XSX_s1.ucas").write_bytes(payload)
        (dst/tocname).write_bytes(toc)
        i=files[OUTRO];p=store.lengths+10*i
        return {"overrides":report,"director":director_events(bytes(store.read(i,0,int.from_bytes(toc[p+5:p+10],"big"))))}
    finally:store.data.close()
