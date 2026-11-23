#!/usr/bin/env bash
# Maintainer build step only. The resulting ZIP starts with a native ELF and
# contains neither a scripting-language launcher nor a build tree.
set -euo pipefail
export LC_ALL=C
src=$1 build=$2 winesrc=$3 vk=$4 dxvk=$5 grt=$6 ffmpeg=$7
out="$build/dist"
mkdir -p "$out"
tmp=$(mktemp -d "$out/.pack-XXXXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
app="$tmp/FrostyBird"
mkdir -p "$app"/{assets,guest,lib,libc,libexec,wine-libs,gst,profile,notices}
cp "$build/host/installer/frostybird" "$app/libexec/frostybird-core"
cp "$build/host/installer/frostybird-start" "$app/frostybird"
for soname in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libmvec.so.1 libdl.so.2 libpthread.so.0 librt.so.1 libresolv.so.2 libutil.so.1 libanl.so.1 libnss_dns.so.2 libnss_files.so.2; do
    cp -L "$(cc -print-file-name="$soname")" "$app/libc/$soname"
done
cp "$src/installer/assets/"{installer_frosty.ogg,frostybird.png,workload.json.gz} "$app/assets/"
cp "$src/profiles/play/UserEngine.ini" "$src/profiles/play/rays/dispatch.contract" "$app/profile/"
cp "$src/installer/README.txt" "$app/README.txt"
cp "$build/guest/"{d3d12_xs.dll,AcpHal.dll,avcodec-62.dll,avutil-60.dll,xcrdapi.dll,xmem.dll,frosty-gameinput.dll,frosty-local-grt.dll,frosty-video.dll,frosty-media.exe,frosty-pipeline-check.exe} "$app/guest/"
cp "$build/grt/bin/xgameruntime.dll" "$app/guest/frosty_grt_core.dll"
cp "$build/vkd3d/libs/d3d12/d3d12.dll" "$build/vkd3d/libs/d3d12core/d3d12core.dll" "$build/dxvk/src/dxgi/dxgi.dll" "$app/guest/"
if ! x86_64-w64-mingw32-objdump -p "$app/guest/d3d12core.dll" | grep -E 'VKD3DCommitPipelineCache$' >/dev/null; then
    echo 'vkd3d-proton is missing patches/vkd3d-installer-cache.patch' >&2
    exit 1
fi
# Wine locates its installed PE/unix/nls trees relative to its loader. Never
# copy a developer's prefix, registry, test game or installed game files.
make -s -C "$build/wine64" install-lib "DESTDIR=$app" prefix=/wine >"$tmp/wine-install.log"
test -x "$app/wine/bin/wine" && test -x "$app/wine/bin/wineserver"
plugins=$(pkg-config --variable=pluginsdir gstreamer-1.0)
for name in coreelements audioconvert audioresample audioparsers playback typefindfunctions faad videoconvertscale videoparsersbad volume; do
    cp -L "$plugins/libgst$name.so" "$app/gst/"
done
native_deps(){
    local dest=$1; shift
    local line soname path
    for elf in "$@"; do
        # ldd prints each object's transitive NEEDED closure.
        while IFS= read -r line; do
            if [[ $line =~ ^[[:space:]]*([^[:space:]]+)[[:space:]]+=\>[[:space:]]+(/.+)[[:space:]]+\(0x ]]; then
                soname=${BASH_REMATCH[1]}; path=${BASH_REMATCH[2]}
                [[ $path != "$app/wine/"* ]] || continue
                case "$soname" in libc.so.*|libm.so.*|libmvec.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*|libanl.so.*|libutil.so.*|ld-linux*|ntdll.so|libvulkan.so.*|libGL*.so.*|libEGL.so.*|libdrm*.so.*|libgbm.so.*) continue;; esac
                [[ -f "$dest/$soname" ]] || cp -L -- "$path" "$dest/$soname"
            elif [[ $line == *" => not found"* && $line != *"ntdll.so => not found"* ]]; then
                echo "Unresolved dependency of $elf: $line" >&2;return 1
            fi
        done < <(ldd "$elf" 2>/dev/null)
    done
}
native_deps "$app/lib" "$app/libexec/frostybird-core"
cp -L "$build/deps/dxc/lib/libdxil.so" "$app/lib/"
native_deps "$app/lib" "$app/lib/libdxil.so"
mapfile -d '' wineelf < <(find "$app/wine" -type f -name '*.so' -print0)
LD_LIBRARY_PATH="$app/wine/lib/wine/x86_64-unix${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" native_deps "$app/wine-libs" "$app/wine/bin/wine" "$app/wine/bin/wineserver" "${wineelf[@]}" "$app"/gst/*.so
sdl2=$(pkg-config --variable=libdir sdl2)/libSDL2-2.0.so.0
cp -L "$sdl2" "$app/wine-libs/libSDL2-2.0.so.0";native_deps "$app/wine-libs" "$sdl2"
patchelf --set-rpath '$ORIGIN/../lib' "$app/libexec/frostybird-core"
for f in "$app"/lib/*; do patchelf --set-rpath '$ORIGIN' "$f"; done
# Keep translated packets across installer-only updates. Include both pieces
# of native code, the exact LLVM build and both DXC libraries, not GUI assets.
sha256sum "$build/host/libfrosty-translation.a" "$build/host/installer/libfrostybird-core.a" \
    "$app"/lib/libLLVM*.so* "$app/lib/libdxcompiler.so" "$app/lib/libdxil.so" |
    cut -d ' ' -f 1 | sha256sum | cut -d ' ' -f 1 > "$app/profile/translator.sha256"
# Wine re-execs its architecture loader (including via the Wine preloader).
# Keep immutable templates; setup fills their reserved PT_INTERP slot with
# the final absolute path. No patchelf or other tool is needed on the player.
find "$app/wine/bin" -maxdepth 1 -type l -delete
interp=$(printf '/FROSTYBIRD/%02048d' 0)
for f in "$app/wine/bin/wine" "$app/wine/bin/wineserver" "$app/wine/lib/wine/x86_64-unix/wine"; do
    base=$(realpath --relative-to="$(dirname "$f")" "$app")
    patchelf --set-interpreter "$interp" --force-rpath --set-rpath "\$ORIGIN/$base/libc:\$ORIGIN/$base/wine-libs" "$f"
    mv "$f" "$f.template"
done
# Keep Wine's native dependencies and the GStreamer plug-ins byte-for-byte.
# The launcher already supplies wine-libs via LD_LIBRARY_PATH. Rewriting the
# DSOs' RPATHs can leave glibc's loader lock held during a title's XInput startup.
for entry in "Wine:$winesrc" "vkd3d-proton:$vk" "dxil-spirv:$vk/subprojects/dxil-spirv" "DXVK:$dxvk" "Xodus:$grt" "FFmpeg:$ffmpeg" "DXC:$build/deps/dxc"; do
    name=${entry%%:*}; tree=${entry#*:};mkdir -p "$app/notices/$name"
    for f in "$tree"/{COPYING*,LICENSE*,ANNOUNCE.md}; do if [[ -f $f ]]; then cp "$f" "$app/notices/$name/"; fi; done
done
cp "$src/cmake/DependencySources.cmake" "$src/docs/build.md" "$app/notices/"
cp -r "$src/patches" "$src/audio/ffmpeg" "$app/notices/"
cp "$ffmpeg/COPYING.LGPLv2.1" "$app/notices/glibc-LGPL-2.1.txt"
getconf GNU_LIBC_VERSION > "$app/notices/glibc-version.txt"
if find -L "$app" \( -iname '*.py' -o -iname '*.pyc' -o -iname 'libpython*' -o -iname 'python[0-9]*' -o -iname '*.pak' -o -iname '*.xsrt' -o -iname '*.ucas' -o -iname 'Frosty-XSX-Shipping.exe' \) -print -quit | grep -q .; then echo 'Unexpected development or game file in release' >&2;exit 1;fi
# ZIP stores regular files and their Unix mode. Materialize Wine's relative
# aliases as files so extractors that do not restore symlinks also work.
(cd "$app"; find -L . -type f -printf '%P\0' | sort -z | xargs -0 sha256sum > "$tmp/manifest")
mv "$tmp/manifest" "$app/manifest.sha256"
(cd "$tmp"; zip -q -X -4 -r "$tmp/FrostyBird-linux-x86_64.zip" FrostyBird)
mv "$tmp/FrostyBird-linux-x86_64.zip" "$out/FrostyBird-linux-x86_64.zip"
(cd "$out"; sha256sum FrostyBird-linux-x86_64.zip > FrostyBird-linux-x86_64.zip.sha256)
echo "Release ready: $out/FrostyBird-linux-x86_64.zip"
