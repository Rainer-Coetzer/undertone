"""Undertone test runner.

Runs the built app (dist/Undertone/Undertone.exe) through its headless and scripted modes:

  selftest      STFT round trip, LRC parser, key/tempo stretcher
  separate      vocal removal on tests/out/mix.wav, scored against tests/out/backing.wav
  lrclib        online lyrics lookup for a well-known song (needs internet)
  process       a full in-app run on the tagged test MP3 into a fresh library
  reopen        opening that song again comes straight from the library, no processing
  screenshots   empty state, dialogs and the playing stage, written to tests/out/shots/

The audio fixtures live in tests/out/ (git-ignored). Checks whose fixture is missing are
skipped, not failed. Usage:  python tests/run-tests.py [name ...]
"""
import os
import shutil
import subprocess
import sys
import time
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "dist", "Undertone", "Undertone.exe")
OUT = os.path.join(ROOT, "tests", "out")
MIX = os.path.join(OUT, "mix.wav")
BACKING = os.path.join(OUT, "backing.wav")
SONG = os.path.join(OUT, "Test Choir - Fellow Americans.mp3")


class Skip(Exception):
    pass


def run(args, timeout=600):
    p = subprocess.run([EXE] + args, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=timeout, cwd=ROOT)
    return p.returncode, p.stdout + p.stderr


def values(text):
    out = {}
    for line in text.splitlines():
        if "=" in line and not line.startswith(" "):
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def need(path):
    if not os.path.exists(path):
        raise Skip("missing " + os.path.relpath(path, ROOT))


def read_wav(path):
    import numpy as np
    with wave.open(path) as w:
        assert w.getsampwidth() == 2, path
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64) / 32768
    return data


def sdr(reference, estimate):
    import numpy as np
    n = min(len(reference), len(estimate))
    r, e = reference[:n], estimate[:n]
    return 10 * np.log10(np.sum(r * r) / max(np.sum((r - e) ** 2), 1e-12))


def fresh_library(name):
    path = os.path.join(OUT, name)
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path)
    return path


# --- checks -------------------------------------------------------------------------------

def test_selftest():
    code, text = run(["--selftest"], timeout=120)
    print(text.rstrip())
    assert code == 0 and "SELFTEST OK" in text, "self-test failed"


def test_separate():
    need(MIX)
    need(BACKING)
    outdir = os.path.join(OUT, "sep")
    os.makedirs(outdir, exist_ok=True)
    code, text = run(["--separate", MIX, outdir])
    v = values(text)
    assert code == 0, text
    assert float(v["sum_error"]) < 1e-4, "instrumental + vocals should equal the mix"
    base = sdr(read_wav(BACKING), read_wav(MIX))
    got = sdr(read_wav(BACKING), read_wav(os.path.join(outdir, "instrumental.wav")))
    print(f"device={v.get('device')} separate_seconds={v.get('separate_seconds')}")
    print(f"instrumental SDR {got:.1f} dB (unprocessed mix scores {base:.1f} dB)")
    assert got > base + 10, "the instrumental should be far closer to the backing track than the mix is"


def test_lrclib():
    # A silent file of the song's length stands in for it; the lookup uses its name and duration.
    path = os.path.join(OUT, "Queen - Bohemian Rhapsody.wav")
    if not os.path.exists(path):
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(1)
            w.setframerate(8000)
            w.writeframes(b"\x80" * 8000 * 354)
    code, text = run(["--lyrics", path], timeout=60)
    v = values(text)
    print(f"source={v.get('source')} lines={v.get('lines')}")
    if "Could not reach" in text:
        raise Skip("lrclib.net could not be reached")
    assert code == 0 and v.get("source") == "lrclib" and int(v.get("lines", 0)) > 20, text


def test_process():
    need(SONG)
    lib = fresh_library("lib-run")
    t = time.time()
    code, text = run([SONG, "--library", lib, "--shot", os.path.join(OUT, "shots", "processed.bmp")])
    took = time.time() - t
    assert code == 0, text
    songs = [d for d in os.listdir(lib) if os.path.isdir(os.path.join(lib, d))]
    assert len(songs) == 1, f"expected one song in the library, found {songs}"
    files = set(os.listdir(os.path.join(lib, songs[0])))
    for f in ("instrumental.flac", "vocals.flac", "meta.json", "lyrics.lrc"):
        assert f in files, f"{f} missing from the library entry"
    print(f"processed into library entry {songs[0]} in {took:.1f} s")


def test_reopen():
    lib = os.path.join(OUT, "lib-run")
    if not os.path.isdir(lib) or not os.listdir(lib):
        raise Skip("run 'process' first")
    song = [d for d in os.listdir(lib) if os.path.isdir(os.path.join(lib, d))][0]
    t = time.time()
    code, text = run(["--library", lib, "--song", song, "--shot", os.path.join(OUT, "shots", "reopened.bmp")], timeout=60)
    took = time.time() - t
    assert code == 0, text
    print(f"reopened in {took:.1f} s")
    assert took < 15, "reopening should load from the library, not process again"


def test_screenshots():
    shots = os.path.join(OUT, "shots")
    lib = os.path.join(OUT, "lib-run")
    empty = fresh_library("lib-empty-run")
    cases = [("empty", ["--library", empty])]
    if os.path.isdir(lib) and os.listdir(lib):
        song = os.listdir(lib)[0]
        base = ["--library", lib, "--song", song]
        cases += [
            ("playing", base + ["--time", "2", "--play", "2"]),
            ("export", base + ["--modal", "export"]),
            ("lyrics", base + ["--modal", "lyrics"]),
            ("settings", base + ["--modal", "settings"]),
        ]
    for name, args in cases:
        path = os.path.join(shots, name + ".bmp")
        code, text = run(args + ["--shot", path], timeout=60)
        assert code == 0 and os.path.exists(path), f"{name}: {text}"
        try:
            from PIL import Image
            Image.open(path).save(path[:-4] + ".png")
        except ImportError:
            pass
        print(f"wrote {os.path.relpath(path, ROOT)}")


TESTS = [("selftest", test_selftest), ("separate", test_separate), ("lrclib", test_lrclib),
         ("process", test_process), ("reopen", test_reopen), ("screenshots", test_screenshots)]


def main():
    if not os.path.exists(EXE):
        print("Build first: ./build-windows.sh")
        return 2
    os.makedirs(os.path.join(OUT, "shots"), exist_ok=True)
    wanted = sys.argv[1:] or [n for n, _ in TESTS]
    results = []
    for name, fn in TESTS:
        if name not in wanted:
            continue
        print(f"\n== {name}")
        try:
            fn()
            results.append((name, "ok"))
        except Skip as s:
            results.append((name, f"skipped ({s})"))
        except Exception as e:  # noqa: BLE001  a failed check should not stop the rest
            print(f"FAILED: {e}")
            results.append((name, "FAILED"))
    print("\n== summary")
    for name, r in results:
        print(f"  {name:12} {r}")
    return 1 if any(r == "FAILED" for _, r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
