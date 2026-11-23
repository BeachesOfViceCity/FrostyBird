#!/usr/bin/env python3
"""Cross-build shared FFmpeg libraries for the x64 SHAPE audio provider."""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    build=Path(__file__).resolve().parents[1]/"build"
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("source",type=Path,help="unpacked FFmpeg 8.x source")
    p.add_argument("--prefix",type=Path,default=build/"deps/audio-win64")
    p.add_argument("--build-dir",type=Path,default=build/"audio-ffmpeg")
    p.add_argument("--jobs",type=int,default=min(12,os.cpu_count() or 1))
    a=p.parse_args();a.build_dir.mkdir(parents=True,exist_ok=True)
    source=a.source.resolve(strict=True)
    # Shadow two translation units in a private build directory. Neither
    # the supplied source nor the normal XMA2 file decoder is modified.
    shadow=a.build_dir.resolve()/"xs-source";shadow.mkdir(exist_ok=True)
    for path in source.iterdir():
        target=shadow/path.name
        if path.name in (".git","libavcodec"):continue
        if not target.is_symlink():target.symlink_to(path)
    codec=shadow/"libavcodec";codec.mkdir(exist_ok=True)
    for path in (source/"libavcodec").iterdir():
        target=codec/path.name
        if path.name=="allcodecs.c":
            text=path.read_text();needle="extern const FFCodec ff_xma2_decoder;"
            if text.count(needle)!=1:raise ValueError("unsupported FFmpeg codec registry")
            target.write_text(text.replace(needle,needle+"\nextern const FFCodec ff_xma2frame_decoder;"))
        elif path.name=="wmaprodec.c":
            target.write_bytes(path.read_bytes()+b'\n'+(build.parent/"audio/ffmpeg/xma2frame.inc").read_bytes())
        elif not target.is_symlink():target.symlink_to(path)
    config=shadow/"configure"
    # configure prefers a previous build's src link over its own location.
    src_link=a.build_dir/"src"
    if src_link.is_symlink() and src_link.resolve()!=shadow:src_link.unlink()
    args=[str(config),"--target-os=mingw32","--arch=x86_64","--enable-cross-compile",
          "--cross-prefix=x86_64-w64-mingw32-","--prefix="+str(a.prefix.resolve()),
          "--enable-shared","--disable-static","--disable-programs","--disable-doc",
          "--disable-network","--disable-autodetect","--disable-everything",
          "--disable-avformat","--disable-avdevice","--disable-avfilter",
          "--disable-swscale","--disable-swresample","--enable-decoder=xma2,xma2frame"]
    subprocess.run(args,cwd=a.build_dir,check=True)
    if "#define CONFIG_XMA2FRAME_DECODER 1" not in (a.build_dir/"config_components.h").read_text():
        raise ValueError("FFmpeg did not configure the elementary-frame decoder")
    subprocess.run(["make",f"-j{a.jobs}","install"],cwd=a.build_dir,check=True)

if __name__=="__main__":
    main()
