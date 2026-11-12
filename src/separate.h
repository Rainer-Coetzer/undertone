// Splits a stereo mix into an instrumental and a vocal stem with an MDX-Net model run through
// ONNX Runtime. The model sees the song as spectrogram tiles and returns the same tiles with the
// singer removed; this file does the STFT on the way in, the inverse on the way out, and the
// stitching of tiles back into one track.
#pragma once
#include <atomic>
#include <string>
#include <vector>

constexpr int kModelRate = 44100;  // every MDX-Net model was trained on 44.1 kHz stereo

class Separator {
public:
    Separator() = default;
    ~Separator();
    Separator(const Separator&) = delete;
    Separator& operator=(const Separator&) = delete;

    // modelPath is the .onnx file; a .json beside it carries the values the file itself doesn't
    // (FFT size, loudness compensation, which stem the model outputs).
    bool load(const std::string& modelPath, bool allowGpu, std::string& error);
    bool loaded() const { return session != nullptr; }
    const std::string& device() const { return deviceName; }  // "GPU (DirectML)" or "CPU"

    // mix: interleaved stereo at kModelRate. instrumental and vocals come back the same length,
    // and always add up to the mix exactly. Returns false on error or when cancel was set.
    bool run(const std::vector<float>& mix, std::vector<float>& instrumental, std::vector<float>& vocals,
             std::atomic<float>& progress, const std::atomic<bool>& cancel, std::string& error);

private:
    bool open(const std::string& modelPath, bool gpu, std::string& error);
    void close();

    void* env = nullptr;      // OrtEnv
    void* session = nullptr;  // OrtSession
    std::string inputName, outputName, deviceName;
    int nFft = 6144, hop = 1024, dimF = 3072, dimT = 256;
    float compensate = 1.f;
    bool outputsInstrumental = true;
};

// For the tests: forward then inverse transform of one chunk, to prove the pair is transparent.
// Returns the largest absolute error away from the chunk's edges.
float stftRoundTripError(int nFft, int hop, int frames);
