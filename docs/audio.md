# Xbox audio playback

`./Play` starts the shipping audio mixer. Select the output device and volume
in the host sound settings; `./Play --mute` explicitly disables sound.

| Guest interface | Where the sound comes from |
| --- | --- |
| XAudio2 2.9 | Wine/FAudio renders the game's mastering and source voices to the host endpoint. The title submits its own interleaved float mix and receives normal voice callbacks. |
| Media Foundation AAC | The system AAC transform accepts raw access units and AudioSpecificConfig. On the tested system Wine uses GStreamer with `faad` installed. Electra feeds the result into the title's mixer. |
| ACP / SHAPE XMA | The app-local `AcpHal.dll` maps CPU and 32-bit APU addresses, processes XMA context commands, decodes the compressed frames into the caller's PCM ring and signals completion. |

PCM, ADPCM, Bink Audio, movie demux, mixing, and asset selection remain in
the shipping code. The SHAPE provider accepts packet buffers and little-endian
hardware contexts; it doesn't open files or identify an executable, asset, or
engine. Each context owns its decode history. Slots can be refilled as they
are released, including in the middle of a transform frame. The output is
interleaved signed 16-bit PCM in 256-byte ring blocks.

The implemented SHAPE subset is XMA2: one- and two-channel transform
streams, packet skips, two input slots, subframe quanta, in-buffer loops,
single-context and bitmap enable/disable, message queues and completion
writes. Other SHAPE DSP engines/commands return errors. This is not a full
emulation of the audio coprocessor or a verification of every Xbox title.

## Build

Use the x64 MinGW-w64 toolchain, GNU Make, NASM and unpacked
[FFmpeg 8.1.2](https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz) source:

```bash
python3 tools/build_audio_codec.py /path/to/ffmpeg-8.1.2
cmake -S guest -B build/guest -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/mingw64.cmake"
cmake --build build/guest --target acphal xs-shape-check xs-audio-output-check
```

The codec is built as two shared PE libraries, `avcodec-62.dll` and
`avutil-60.dll`, under `build/deps/audio-win64`. The build script adds an
[LGPL frame adapter](../audio/ffmpeg/xma2frame.inc) to a private source view.
The stock XMA2 file decoder trims samples and consumes groups of packets.
SHAPE instead requests individual transform frames at a bit offset and owns
the ring and file-independent sample accounting. Both front ends share
FFmpeg's XMA spectral decoder and overlap-add implementation.

## Independent checks

Use a dedicated Wine prefix and an isolated PulseAudio/PipeWire sink. The
capture tool reports host PCM levels and the process owning each sink input.
It does not save an audio recording or change the desktop default sink.

```bash
export WINEPREFIX="$PWD/build/play-validation/prefix" WINEDEBUG=-all
v=build/audio-validation
python3 tools/audio_fixtures.py --out "$v/fixtures"
python3 tools/audio_monitor.py --out "$v/tones" --tones -- \
  build/wine64/wine build/guest/xs-audio-output-check.exe --console
for layout in stereo 5.1; do
  python3 tools/audio_monitor.py --out "$v/aac-$layout" -- \
    build/wine64/wine build/guest/xs-audio-output-check.exe "$v/fixtures/$layout.aac"
done
```

For SHAPE, pass a short, single-stream 48 kHz XMA2 wave with `--wave`,
or extract a local validation fixture from the installed package:

```bash
python3 tools/audio_fixtures.py --out "$v/fixtures" \
  --iostore "$FROSTY_GAME_ROOT/Frosty/Content/Paks/Frosty-XSX.utoc" \
  --asset Frosty/Content/Audio/Bodyfall_C_04.uasset
build/wine64/wine build/guest/xs-shape-check.exe \
  "$v/fixtures/effect.packets" "$v/fixtures/effect-reference.s16" 2
```

The reference PCM is produced by the host's unmodified file decoder. The
comparison aligns the file decoder's 512+64 samples of startup trim against
the untrimmed SHAPE ring. Neither the fixture nor the reference goes into
the playback runtime.

## Measured playback

The `audio-play-01` trial ran `./Play` through the logo, WakeUp movies and a
20-second movie-free `CityIntro` window. The private 48 kHz stereo sink
had two input streams, both owned by the same shipping EXE process. The
engine logged an eight-channel mixer, successful XMA initialization and
its first XAudio2 buffer submission.

| Section | Endpoint result |
| --- | --- |
| Movies playing | 114.50 s above the audibility threshold in 119.80 s captured. The logo's measured loudness envelope correlated **0.9768** with the original AAC track decoded separately. |
| Movie-free city | **19.70 / 19.70 s** above threshold on both channels after excluding the transition. |

The threshold was RMS `0.0001` per 50 ms of host PCM. Levels are float
samples before the physical device: peaks reached 1.56 in movies and 1.32
in the city. The engine logged one buffer underrun at startup. The trial
establishes title-owned movie and gameplay output; it doesn't identify
which codec supplied each individual gameplay sound.

The independent checks distinguished the 1 kHz left and 1.5 kHz right
signals and received four ordered buffer completions. The AAC transform
produced 58,368 float frames for each of the stereo and six-channel inputs.
SHAPE produced 54,272 stereo frames from the effect fixture: all 53,441
comparable frames matched the file decoder exactly in both channels. The
test crossed nine split-frame handoffs and 94 ring wraps and checked finite
looping, skipped packets, autonomous output, parser errors and tagged
completion messages.

Evidence: `build/audio-validation/audio-play-01/title-result.json`,
`sources.jsonl` and `levels.tsv` in the same directory,
`build/audio-validation/shape-stream-check.log`, and the shipping log in
`build/play-validation/audio-play-01/user/Temp/Logs/Frosty.log`.
To repeat one bounded title session at the end of a work cycle, use a new
run name:

```bash
name=my-audio-check
python3 tools/audio_monitor.py --out "build/audio-validation/$name" -- \
  python3 tools/benchmark_city.py "$name" --seconds 20 --timeout 360
python3 tools/title_audio_result.py "build/audio-validation/$name" \
  "build/play-validation/$name" \
  --logo "$FROSTY_GAME_ROOT/Frosty/Content/Movies/Logo/HEVC/LOGO_VID_HEVC.mp4"
```
