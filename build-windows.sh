#!/usr/bin/env bash
# Builds Undertone for 64-bit Windows with MSYS2's ucrt64 g++ from Git Bash.
#   ./build-windows.sh
# Output: dist/Undertone/, a portable folder: the exe, its DLLs, ffmpeg, whisper/, models/.
# Everything is fetched into deps/ and models/ on the first run (about 800 MB) and reused after
# that. Each download is pinned to a version and its SHA-256, and refused if it doesn't match.
set -euo pipefail
cd "$(dirname "$0")"

SDL=2.30.9; TTF=2.22.0
DEPS=deps; OUT=dist/Undertone; TRIPLE=x86_64-w64-mingw32
export PATH="/c/msys64/ucrt64/bin:$PATH"
mkdir -p "$DEPS/dl" models

fetch_tar() {  # fetch_tar <dir> <url>
  [ -d "$DEPS/$1" ] && return
  echo "-> downloading $(basename "$2")"
  curl -fsSL "$2" | tar xz -C "$DEPS"
}
fetch_tar "SDL2-$SDL"     "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL2-devel-$SDL-mingw.tar.gz"
fetch_tar "SDL2_ttf-$TTF" "https://github.com/libsdl-org/SDL_ttf/releases/download/release-$TTF/SDL2_ttf-devel-$TTF-mingw.tar.gz"

fetch() {  # fetch <file> <url> <sha256>
  if [ ! -f "$1" ]; then
    echo "-> downloading $(basename "$1")"
    curl -fsSL -o "$1.part" "$2" && mv "$1.part" "$1"
  fi
  actual=$(sha256sum "$1" | cut -d' ' -f1)
  [ "$3" = "$actual" ] || { echo "!! $(basename "$1") checksum mismatch, refusing to use it"; rm -f "$1"; exit 1; }
}

# FFmpeg: gyan.dev's "essentials" build through its GitHub mirror, as in Sandscope.
FFMPEG=9.0.2
fetch "$DEPS/dl/ffmpeg.zip" "https://github.com/GyanD/codexffmpeg/releases/download/$FFMPEG/ffmpeg-$FFMPEG-essentials_build.zip" 60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba
# ONNX Runtime with DirectML, so the model runs on any DirectX 12 graphics card without CUDA.
fetch "$DEPS/dl/ort.zip" "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.24.4" 57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681
fetch "$DEPS/dl/directml.zip" "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.4" 4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9
# whisper.cpp's own Windows build of whisper-cli.
fetch "$DEPS/dl/whisper.zip" "https://github.com/ggml-org/whisper.cpp/releases/download/b5454/whisper-bin-x64.zip" 6ba69e3482d7826214f90a6a9c84ca07782aec1e1d0c6a7c30c994fd5d816ccb
# The models: UVR's MDX-Net instrumental model, and Whisper "small" for transcribing lyrics.
fetch "models/UVR-MDX-NET-Inst_HQ_3.onnx" "https://github.com/TRvlvr/model_repo/releases/download/all_public_uvr_models/UVR-MDX-NET-Inst_HQ_3.onnx" 317554b07fe1ea5279a77f2b1520a41ea4b93432560c4ffd08792c30fddf9adc
fetch "models/ggml-small.bin" "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin" 1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b
# The model file doesn't carry its FFT size or loudness correction; these come from UVR's own
# table (model_data_new.json, keyed by the file's hash).
[ -f "models/UVR-MDX-NET-Inst_HQ_3.json" ] || echo '{"n_fft": 6144, "hop": 1024, "compensate": 1.022, "primary_stem": "Instrumental"}' > "models/UVR-MDX-NET-Inst_HQ_3.json"

if [ ! -f "$DEPS/whisper/whisper-cli.exe" ] || [ ! -f "$DEPS/onnxruntime/bin/onnxruntime.dll" ] || [ ! -f "$DEPS/ffmpeg/ffmpeg.exe" ]; then
  echo "-> unpacking"
  python - "$DEPS" <<'PY'
import sys, zipfile, os
deps = sys.argv[1]
def put(z, name, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, 'wb') as f: f.write(z.read(name))
with zipfile.ZipFile(f'{deps}/dl/ffmpeg.zip') as z:
    for n in z.namelist():
        b = os.path.basename(n)
        if b in ('ffmpeg.exe', 'ffprobe.exe', 'LICENSE'): put(z, n, f'{deps}/ffmpeg/{b}')
with zipfile.ZipFile(f'{deps}/dl/ort.zip') as z:
    for n in z.namelist():
        if n.startswith('build/native/include/') and n.endswith('.h'): put(z, n, f'{deps}/onnxruntime/include/' + os.path.basename(n))
    for b in ('onnxruntime.dll', 'onnxruntime_providers_shared.dll'): put(z, f'runtimes/win-x64/native/{b}', f'{deps}/onnxruntime/bin/{b}')
    put(z, 'LICENSE', f'{deps}/onnxruntime/LICENSE.txt')
with zipfile.ZipFile(f'{deps}/dl/directml.zip') as z:
    put(z, 'bin/x64-win/DirectML.dll', f'{deps}/onnxruntime/bin/DirectML.dll')
    put(z, 'LICENSE.txt', f'{deps}/onnxruntime/DirectML-LICENSE.txt')
with zipfile.ZipFile(f'{deps}/dl/whisper.zip') as z:
    for n in z.namelist():
        b = os.path.basename(n)
        if b in ('whisper-cli.exe', 'whisper.dll', 'ggml.dll', 'ggml-base.dll') or b.startswith('ggml-cpu-'): put(z, n, f'{deps}/whisper/{b}')
PY
fi

INC=(-Isrc -I"$DEPS/SDL2-$SDL/$TRIPLE/include/SDL2" -I"$DEPS/SDL2_ttf-$TTF/$TRIPLE/include/SDL2" -I"$DEPS/onnxruntime/include")
LIB=(-L"$DEPS/SDL2-$SDL/$TRIPLE/lib" -L"$DEPS/SDL2_ttf-$TTF/$TRIPLE/lib")

mkdir -p build-win "$OUT/whisper" "$OUT/models" "$OUT/licences"
[ -f res/undertone.ico ] || python tools/make_icon.py res/undertone.ico
windres res/undertone.rc -O coff -o build-win/resources.o

echo "-> compiling"
# Each file compiles to its own object in parallel; separate.cpp and player.cpp carry heavy
# header-only libraries and dominate the time.
SRCS=(main ui audio separate player lyrics library job headless proc)
pids=(); OBJS=(); failed=0
for s in "${SRCS[@]}"; do
  OBJS+=("build-win/$s.o")
  if [ ! -f "build-win/$s.o" ] || [ "src/$s.cpp" -nt "build-win/$s.o" ] || [ -n "$(find src -name '*.h' -newer "build-win/$s.o" | head -1)" ]; then
    rm -f "build-win/$s.o"
    g++ -std=c++17 -O3 -Wall -Wextra -DNDEBUG "${INC[@]}" -c "src/$s.cpp" -o "build-win/$s.o" & pids+=($!)
  fi
done
for pid in "${pids[@]}"; do wait "$pid" || failed=1; done
[ $failed -eq 0 ] || { echo "!! compile failed"; exit 1; }
# No -march flag: the exe must start on any 64-bit PC. libgcc, libstdc++ and winpthread are
# static so it doesn't ask for MSYS2's DLLs. onnxruntime.dll is linked directly; MinGW reads
# a DLL's exports without an import library.
g++ "${OBJS[@]}" build-win/resources.o -o "$OUT/Undertone.exe" \
  "${LIB[@]}" "$DEPS/onnxruntime/bin/onnxruntime.dll" -lSDL2_ttf -lSDL2 \
  -lcomdlg32 -ldwmapi -lshell32 -ladvapi32 -lole32 -lwinhttp \
  -mwindows -static-libgcc -static-libstdc++ -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic -s
rm -f "$OUT/engine-test.exe"

copy() { cmp -s "$1" "$2" || cp "$1" "$2"; }  # only when changed: antivirus may hold a file it is scanning
for f in "$DEPS/SDL2-$SDL/$TRIPLE/bin/SDL2.dll" "$DEPS/SDL2_ttf-$TTF/$TRIPLE/bin/SDL2_ttf.dll"; do
  copy "$f" "$OUT/$(basename "$f")"
done
strip --strip-unneeded "$OUT/SDL2.dll" "$OUT/SDL2_ttf.dll" 2>/dev/null || true
for f in "$DEPS"/onnxruntime/bin/*.dll "$DEPS/ffmpeg/ffmpeg.exe" "$DEPS/ffmpeg/ffprobe.exe"; do copy "$f" "$OUT/$(basename "$f")"; done
for f in "$DEPS"/whisper/*; do copy "$f" "$OUT/whisper/$(basename "$f")"; done
# ONNX Runtime and whisper.cpp are built with Visual C++ and need its runtime. Microsoft allows
# shipping these DLLs beside an app, which saves anyone installing the redistributable.
for d in "$OUT" "$OUT/whisper"; do
  for f in msvcp140.dll msvcp140_1.dll vcruntime140.dll vcruntime140_1.dll vcomp140.dll; do
    copy "/c/Windows/System32/$f" "$d/$f"
  done
done
for f in models/UVR-MDX-NET-Inst_HQ_3.onnx models/UVR-MDX-NET-Inst_HQ_3.json models/ggml-small.bin; do
  # Big files: size alone says whether a copy is needed.
  [ -f "$OUT/$f" ] && [ "$(stat -c%s "$f")" = "$(stat -c%s "$OUT/$f")" ] || cp "$f" "$OUT/$f"
done

# Refuse to package a build that needs a DLL which is neither shipped nor part of Windows.
SYSTEM_DLLS=" advapi32.dll comdlg32.dll d3d12.dll dbghelp.dll dwmapi.dll dxgi.dll dxcore.dll gdi32.dll imm32.dll kernel32.dll msvcrt.dll ntdll.dll ole32.dll oleaut32.dll rpcrt4.dll setupapi.dll shell32.dll shlwapi.dll ucrtbase.dll user32.dll usp10.dll uxtheme.dll version.dll winhttp.dll winmm.dll ws2_32.dll "
missing=0
for dir in "$OUT" "$OUT/whisper"; do
  SHIPPED=" $(ls "$dir" | tr 'A-Z' 'a-z' | tr '\n' ' ') "
  for f in "$dir"/*.exe "$dir"/*.dll; do
    case "$(basename "$f")" in ffmpeg.exe|ffprobe.exe) continue ;; esac
    for dll in $(objdump -p "$f" | awk '/DLL Name:/ {print tolower($3)}'); do
      case "$dll" in api-ms-win-*|ext-ms-*) continue ;; esac
      [[ "$SYSTEM_DLLS" == *" $dll "* ]] && continue
      [[ "$SHIPPED" == *" $dll "* ]] && continue
      echo "!! $(basename "$f") needs $dll, which is neither shipped nor part of Windows"; missing=1
    done
  done
done
[ $missing -eq 0 ] || { echo "!! refusing to package a build that won't start on another PC"; exit 1; }
echo "-> dependency check passed"

cp "$DEPS/SDL2-$SDL/LICENSE.txt" "$OUT/licences/SDL2.txt"
cp "$DEPS/SDL2_ttf-$TTF/LICENSE.txt" "$OUT/licences/SDL2_ttf.txt"
cp "$DEPS/ffmpeg/LICENSE" "$OUT/licences/FFmpeg.txt"
cp "$DEPS/onnxruntime/LICENSE.txt" "$OUT/licences/ONNX-Runtime.txt"
cp "$DEPS/onnxruntime/DirectML-LICENSE.txt" "$OUT/licences/DirectML.txt"
cp third_party/signalsmith/LICENSE.txt "$OUT/licences/signalsmith-stretch.txt"
[ -f third_party/LICENSE-whisper.cpp.txt ] && cp third_party/LICENSE-whisper.cpp.txt "$OUT/licences/whisper.cpp.txt"
[ -f README.md ] && cp README.md "$OUT/README.txt"
echo "-> done: $OUT/Undertone.exe"
