#include "separate.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

#define POCKETFFT_NO_MULTITHREADING
#define POCKETFFT_CACHE_SIZE 8
#include "../third_party/pocketfft_hdronly.h"
#include "../third_party/json.hpp"
#include <onnxruntime_c_api.h>

#include "proc.h"
#include "threads.h"

// Declared in dml_provider_factory.h, which drags in the Direct3D headers for overloads this
// program doesn't use.
extern "C" OrtStatus* ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_DML(OrtSessionOptions* options, int device_id);

namespace {

const OrtApi* ort() {
    static const OrtApi* api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    return api;
}

// Returns true when the call failed, and leaves the reason in error.
bool failed(OrtStatus* st, std::string& error) {
    if (!st) return false;
    error = ort()->GetErrorMessage(st);
    ort()->ReleaseStatus(st);
    return true;
}

std::vector<float> hannWindow(int n) {
    std::vector<float> w(n);
    for (int i = 0; i < n; ++i) w[i] = (float)(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / n));
    return w;
}

// One channel of one chunk into the model's layout: re and im are [dimF][frames], row-major.
// Matches torch.stft(center=True, pad_mode="reflect") with a periodic Hann window, which is what
// the model was trained against.
void stft(const float* x, int len, int nFft, int hop, int frames, int dimF, const std::vector<float>& window, float* re, float* im) {
    const int half = nFft / 2;
    parallelRange(frames, [&](int t0, int t1) {
        std::vector<float> buf(nFft);
        std::vector<std::complex<float>> spec(half + 1);
        for (int t = t0; t < t1; ++t) {
            const int start = t * hop - half;
            for (int i = 0; i < nFft; ++i) {
                int idx = start + i;
                if (idx < 0) idx = -idx;
                if (idx >= len) idx = 2 * (len - 1) - idx;
                buf[i] = x[std::clamp(idx, 0, len - 1)] * window[i];
            }
            pocketfft::r2c(pocketfft::shape_t{(size_t)nFft}, pocketfft::stride_t{sizeof(float)}, pocketfft::stride_t{sizeof(std::complex<float>)},
                           0, pocketfft::FORWARD, buf.data(), spec.data(), 1.f, 1);
            for (int f = 0; f < dimF; ++f) {
                re[(size_t)f * frames + t] = spec[f].real();
                im[(size_t)f * frames + t] = spec[f].imag();
            }
        }
    });
}

void istft(const float* re, const float* im, int len, int nFft, int hop, int frames, int dimF, const std::vector<float>& window, float* out,
           std::vector<float>& work, std::vector<float>& acc, std::vector<float>& norm) {
    const int half = nFft / 2;
    work.resize((size_t)frames * nFft);
    parallelRange(frames, [&](int t0, int t1) {
        std::vector<std::complex<float>> spec(half + 1);
        for (int t = t0; t < t1; ++t) {
            for (int f = 0; f < dimF; ++f) spec[f] = {re[(size_t)f * frames + t], im[(size_t)f * frames + t]};
            for (int f = dimF; f <= half; ++f) spec[f] = 0;  // the model never sees the top bins
            float* frame = work.data() + (size_t)t * nFft;
            pocketfft::c2r(pocketfft::shape_t{(size_t)nFft}, pocketfft::stride_t{sizeof(std::complex<float>)}, pocketfft::stride_t{sizeof(float)},
                           0, pocketfft::BACKWARD, spec.data(), frame, 1.f / nFft, 1);
            for (int i = 0; i < nFft; ++i) frame[i] *= window[i];
        }
    });
    const size_t padded = (size_t)len + nFft;
    acc.assign(padded, 0.f);
    norm.assign(padded, 0.f);
    for (int t = 0; t < frames; ++t) {
        const float* frame = work.data() + (size_t)t * nFft;
        float* a = acc.data() + (size_t)t * hop;
        float* n = norm.data() + (size_t)t * hop;
        for (int i = 0; i < nFft; ++i) { a[i] += frame[i]; n[i] += window[i] * window[i]; }
    }
    for (int i = 0; i < len; ++i) {
        const float n = norm[(size_t)i + half];
        out[i] = n > 1e-8f ? acc[(size_t)i + half] / n : 0.f;
    }
}

}  // namespace

float stftRoundTripError(int nFft, int hop, int frames) {
    const int len = hop * (frames - 1), dimF = nFft / 2 + 1;
    std::vector<float> x(len), y(len), re((size_t)dimF * frames), im((size_t)dimF * frames), work, acc, norm;
    unsigned seed = 12345;
    for (float& v : x) { seed = seed * 1664525u + 1013904223u; v = (float)((seed >> 8) & 0xFFFF) / 32768.f - 1.f; }
    const std::vector<float> window = hannWindow(nFft);
    stft(x.data(), len, nFft, hop, frames, dimF, window, re.data(), im.data());
    istft(re.data(), im.data(), len, nFft, hop, frames, dimF, window, y.data(), work, acc, norm);
    float worst = 0;
    for (int i = nFft / 2; i < len - nFft / 2; ++i) worst = std::max(worst, std::abs(x[i] - y[i]));
    return worst;
}

Separator::~Separator() { close(); }

void Separator::close() {
    if (session) ort()->ReleaseSession((OrtSession*)session);
    if (env) ort()->ReleaseEnv((OrtEnv*)env);
    session = env = nullptr;
}

bool Separator::open(const std::string& modelPath, bool gpu, std::string& error) {
    close();
    OrtEnv* e = nullptr;
    if (failed(ort()->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "undertone", &e), error)) return false;
    env = e;
    OrtSessionOptions* so = nullptr;
    if (failed(ort()->CreateSessionOptions(&so), error)) return false;
    ort()->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL);
    bool ok = true;
    if (gpu) {
        // DirectML needs both of these or it refuses the session.
        ort()->DisableMemPattern(so);
        ort()->SetSessionExecutionMode(so, ORT_SEQUENTIAL);
        ok = !failed(OrtSessionOptionsAppendExecutionProvider_DML(so, 0), error);
    }
    OrtSession* s = nullptr;
    if (ok) ok = !failed(ort()->CreateSession(e, widen(modelPath).c_str(), so, &s), error);
    ort()->ReleaseSessionOptions(so);
    if (!ok) { close(); return false; }
    session = s;

    OrtAllocator* alloc = nullptr;
    ort()->GetAllocatorWithDefaultOptions(&alloc);
    char* name = nullptr;
    if (failed(ort()->SessionGetInputName(s, 0, alloc, &name), error)) { close(); return false; }
    inputName = name; alloc->Free(alloc, name);
    if (failed(ort()->SessionGetOutputName(s, 0, alloc, &name), error)) { close(); return false; }
    outputName = name; alloc->Free(alloc, name);

    // The tile size is baked into the file: [batch, 4, frequency bins, frames].
    OrtTypeInfo* ti = nullptr;
    if (failed(ort()->SessionGetInputTypeInfo(s, 0, &ti), error)) { close(); return false; }
    const OrtTensorTypeAndShapeInfo* shape = nullptr;
    ort()->CastTypeInfoToTensorInfo(ti, &shape);
    size_t rank = 0;
    ort()->GetDimensionsCount(shape, &rank);
    int64_t dims[8] = {};
    if (rank == 4) ort()->GetDimensions(shape, dims, 4);
    ort()->ReleaseTypeInfo(ti);
    if (rank != 4 || dims[1] != 4 || dims[2] <= 0 || dims[3] <= 0) {
        error = "This model is not an MDX-Net stereo separation model.";
        close();
        return false;
    }
    dimF = (int)dims[2];
    dimT = (int)dims[3];
    deviceName = gpu ? "GPU (DirectML)" : "CPU";
    return true;
}

bool Separator::load(const std::string& modelPath, bool allowGpu, std::string& error) {
    const size_t dot = modelPath.find_last_of('.');
    const std::string side = readTextFile(modelPath.substr(0, dot) + ".json");
    const nlohmann::json j = nlohmann::json::parse(side, nullptr, false);
    if (j.is_discarded() || !j.contains("n_fft")) {
        error = "The model's settings file is missing: " + modelPath.substr(0, dot) + ".json";
        return false;
    }
    nFft = j.value("n_fft", 6144);
    hop = j.value("hop", 1024);
    compensate = j.value("compensate", 1.f);
    outputsInstrumental = j.value("primary_stem", std::string("Instrumental")) == "Instrumental";

    std::string gpuError;
    if (allowGpu && open(modelPath, true, gpuError)) {
        // A graphics driver can accept the model and still fail the first real run, so prove it
        // works on a tile of silence before trusting it with a song.
        std::vector<float> silence((size_t)hop * (dimT - 1) * 2, 0.f), a, b;
        std::atomic<float> p{0};
        std::atomic<bool> c{false};
        if (run(silence, a, b, p, c, gpuError)) return true;
    }
    if (!open(modelPath, false, error)) return false;
    return true;
}

bool Separator::run(const std::vector<float>& mix, std::vector<float>& instrumental, std::vector<float>& vocals,
                    std::atomic<float>& progress, const std::atomic<bool>& cancel, std::string& error) {
    if (!session) { error = "The separation model is not loaded."; return false; }
    if (nFft / 2 + 1 < dimF) { error = "The model's settings don't match the model file."; return false; }
    const size_t frames = mix.size() / 2;
    const int chunk = hop * (dimT - 1);
    // The outer nFft/2 samples of every tile are unreliable (the transform has nothing beyond the
    // edge to work with), so tiles overlap by that much plus a short crossfade, which hides any
    // small level step between one tile's answer and the next.
    const int trim = nFft / 2, fade = 8192;
    const int step = chunk - 2 * trim - fade;
    const size_t tiles = frames == 0 ? 0 : (frames + step - 1) / step;

    std::vector<double> sum(frames * 2, 0.0);
    std::vector<float> weight(frames, 0.f);
    const std::vector<float> window = hannWindow(nFft);
    const size_t plane = (size_t)dimF * dimT;
    std::vector<float> in(4 * plane), tileL(chunk), tileR(chunk), outL(chunk), outR(chunk), work, acc, norm;

    OrtMemoryInfo* mem = nullptr;
    if (failed(ort()->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem), error)) return false;
    const int64_t shape[4] = {1, 4, dimF, dimT};
    const char* inNames[1] = {inputName.c_str()};
    const char* outNames[1] = {outputName.c_str()};
    bool ok = true;

    for (size_t k = 0; k < tiles && ok; ++k) {
        if (cancel) { error = "Cancelled."; ok = false; break; }
        // Tile k's trusted region starts at k*step, so the tile itself starts trim earlier.
        const long long origin = (long long)k * step - trim;
        for (int i = 0; i < chunk; ++i) {
            const long long s = origin + i;
            const bool inRange = s >= 0 && s < (long long)frames;
            tileL[i] = inRange ? mix[(size_t)s * 2] : 0.f;
            tileR[i] = inRange ? mix[(size_t)s * 2 + 1] : 0.f;
        }
        stft(tileL.data(), chunk, nFft, hop, dimT, dimF, window, in.data(), in.data() + plane);
        stft(tileR.data(), chunk, nFft, hop, dimT, dimF, window, in.data() + 2 * plane, in.data() + 3 * plane);

        OrtValue* inVal = nullptr;
        OrtValue* outVal = nullptr;
        if (failed(ort()->CreateTensorWithDataAsOrtValue(mem, in.data(), in.size() * sizeof(float), shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal), error)) { ok = false; break; }
        const OrtValue* inputs[1] = {inVal};
        if (failed(ort()->Run((OrtSession*)session, nullptr, inNames, inputs, 1, outNames, 1, &outVal), error)) ok = false;
        float* res = nullptr;
        if (ok && failed(ort()->GetTensorMutableData(outVal, (void**)&res), error)) ok = false;
        if (ok) {
            istft(res, res + plane, chunk, nFft, hop, dimT, dimF, window, outL.data(), work, acc, norm);
            istft(res + 2 * plane, res + 3 * plane, chunk, nFft, hop, dimT, dimF, window, outR.data(), work, acc, norm);
            for (int i = trim; i < chunk - trim; ++i) {
                const long long s = origin + i;
                if (s < 0 || s >= (long long)frames) continue;
                const int fromEdge = std::min(i - trim, chunk - trim - 1 - i);
                const float w = fromEdge < fade ? (fromEdge + 1) / (float)(fade + 1) : 1.f;
                sum[(size_t)s * 2] += (double)outL[i] * w;
                sum[(size_t)s * 2 + 1] += (double)outR[i] * w;
                weight[(size_t)s] += w;
            }
        }
        if (outVal) ort()->ReleaseValue(outVal);
        if (inVal) ort()->ReleaseValue(inVal);
        progress = (float)(k + 1) / tiles;
    }
    ort()->ReleaseMemoryInfo(mem);
    if (!ok) return false;

    instrumental.resize(frames * 2);
    vocals.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const float w = weight[i] > 0 ? weight[i] : 1.f;
        for (int c = 0; c < 2; ++c) {
            const float stem = (float)(sum[i * 2 + c] / w) * compensate;
            const float rest = mix[i * 2 + c] - stem;
            instrumental[i * 2 + c] = outputsInstrumental ? stem : rest;
            vocals[i * 2 + c] = outputsInstrumental ? rest : stem;
        }
    }
    return true;
}
