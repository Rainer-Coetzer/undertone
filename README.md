# Undertone

A karaoke player for Windows. Give it any song file and it removes the vocals with a machine-learning model, then plays the backing track with synced lyrics that light up line by line as the song plays.

It looks like Dusk Reader and Sandscope, with a white accent.

## Using it

1. Drag a song onto the window, or click **Add a song** (Ctrl+O). MP3, FLAC, WAV, M4A, OGG and anything else ffmpeg reads will work.
2. Undertone separates the vocals. This takes a few seconds per song on the graphics card, or about the length of the song on the processor alone. Then it finds the lyrics.
3. The song is saved to your library, so opening it again is instant.

While a song plays:

| Control | What it does |
|---|---|
| Guide vocal | Brings some of the original singer back in (0% is pure karaoke). ↑ and ↓ change it. |
| Key | Moves the song up or down by up to 6 semitones without changing its speed. |
| Tempo | Plays from 50% to 125% speed without changing the key. The lyrics stay in time. |
| Earlier / Later | Shifts the lyrics by a quarter of a second if they run ahead of or behind the song. |
| Lyrics | Searches again under a different title or artist, or transcribes the singing instead. |
| Export | Saves the instrumental, the vocals, or both as WAV, FLAC or MP3. You can keep the guide vocal and the key and tempo changes. |

Click a lyric line to jump to it. Space plays and pauses; ← and → skip 5 seconds.

Each song remembers its own guide vocal, key, tempo and lyric timing.

## Where the lyrics come from

1. A `.lrc` file with the same name next to the song, if there is one.
2. [LRCLIB](https://lrclib.net), a free lyrics database. The lookup sends the song's title, artist, album and length, and nothing else.
3. If neither has the song, Whisper transcribes the separated vocals on your PC. These lyrics are marked **Auto-transcribed** and may contain misheard words. On the processor this takes about as long as the song itself. You can turn it off in Settings.

## Everything stays in this folder

Undertone is portable. The models, settings (`settings.json`) and the song library (`library\`) live beside `Undertone.exe`. Nothing is written to C: and nothing is downloaded when the app runs. Each song takes about 50 MB in the library. You can delete a song from the library list, or change the library location in Settings.

## Models

| Job | Model |
|---|---|
| Vocal removal | UVR-MDX-NET-Inst_HQ_3 (Ultimate Vocal Remover project), run with ONNX Runtime. It uses the graphics card through DirectML, or the processor if that fails. |
| Transcription | Whisper small (whisper.cpp) |

No model removes vocals perfectly. Expect faint traces on reverb tails and dense backing harmonies.

## Building

Needs MSYS2 with the ucrt64 g++ toolchain at `C:\msys64`, plus Git Bash and Python 3.

```bash
./build-windows.sh
```

On the first run it downloads SDL2, ONNX Runtime, DirectML, ffmpeg, whisper.cpp and both models (about 800 MB) into `deps\` and `models\`. Each download is checked against a pinned SHA-256. The finished app goes to `dist\Undertone\`.

Tests:

```bash
python tests/run-tests.py
```

They cover the transform and parser self-test, vocal removal quality, the LRCLIB lookup, a full run into a fresh library, reopening a song from the library, and screenshots of each screen. The audio fixtures live in `tests\out\`, which git ignores; checks whose fixtures are missing are skipped.

## Licences

- Undertone's own code: MIT, see `LICENSE`.
- SDL2, SDL2_ttf: zlib. ONNX Runtime, DirectML redistributable: MIT / Microsoft. whisper.cpp: MIT. signalsmith-stretch, nlohmann/json, pocketfft: MIT / BSD.
- The bundled ffmpeg is a GPL build. That's fine for personal use, but it matters if you distribute the app.
- Licence texts are in `licences\` in the built app.
