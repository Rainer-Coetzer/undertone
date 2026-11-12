#include "player.h"

#include <algorithm>
#include <cmath>

#include "separate.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "../third_party/signalsmith/signalsmith-stretch.h"
#pragma GCC diagnostic pop

struct Player::Stretcher {
    signalsmith::stretch::SignalsmithStretch<float> s;
};

namespace {
bool needsStretch(float semitones, float tempo) { return std::abs(semitones) > 0.01f || std::abs(tempo - 1.f) > 0.005f; }
}

Player::Player() : stretcher(new Stretcher) {
    stretcher->s.presetDefault(2, (float)kModelRate);
}

Player::~Player() { close(); }

bool Player::open(std::string& error) {
    if (device) return true;
    SDL_AudioSpec want{}, got{};
    want.freq = kModelRate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = callback;
    want.userdata = this;
    // No allowed changes: SDL converts if the sound card runs at another rate.
    device = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (!device) { error = std::string("No audio output: ") + SDL_GetError(); return false; }
    SDL_PauseAudioDevice(device, 0);
    return true;
}

void Player::close() {
    if (device) SDL_CloseAudioDevice(device);
    device = 0;
}

void Player::setStems(std::shared_ptr<const Stems> stems) {
    if (device) SDL_LockAudioDevice(device);
    isPlaying = false;
    current = std::move(stems);
    readPos = 0;
    seekRequest = -1;
    latencyFrames = 0;
    stretching = false;
    inputCarry = 0;
    stretcher->s.reset();
    if (device) SDL_UnlockAudioDevice(device);
}

void Player::play() {
    if (!current) return;
    if (position() >= duration() - 0.01) seek(0);
    isPlaying = true;
}

void Player::pause() { isPlaying = false; }

double Player::duration() const { return current ? (double)current->frames() / kModelRate : 0; }

double Player::position() const {
    const long long pending = seekRequest.load();
    if (pending >= 0) return (double)pending / kModelRate;
    const long long heard = readPos.load() - latencyFrames.load();
    return std::clamp((double)heard / kModelRate, 0.0, duration());
}

void Player::seek(double seconds) {
    if (!current) return;
    seekRequest = (long long)(std::clamp(seconds, 0.0, duration()) * kModelRate);
    // While paused the callback isn't reading, so apply it here; it is the only writer then.
    if (!isPlaying) {
        if (device) SDL_LockAudioDevice(device);
        readPos = seekRequest.exchange(-1);
        latencyFrames = 0;
        stretching = false;
        stretcher->s.reset();
        if (device) SDL_UnlockAudioDevice(device);
    }
}

void Player::callback(void* user, Uint8* stream, int len) {
    ((Player*)user)->fill((float*)stream, len / (int)(2 * sizeof(float)));
}

void Player::fill(float* out, int frames) {
    std::fill(out, out + (size_t)frames * 2, 0.f);
    const Stems* s = current.get();
    if (!s || !isPlaying) return;
    const long long total = (long long)s->frames();
    const long long req = seekRequest.exchange(-1);
    if (req >= 0) { readPos = req; stretcher->s.reset(); stretching = false; latencyFrames = 0; inputCarry = 0; }

    const float semis = semitones.load(), speed = std::clamp(tempo.load(), 0.25f, 2.f);
    const float level = std::clamp(vocalLevel.load(), 0.f, 1.f);
    const bool want = needsStretch(semis, speed);
    if (want != stretching) {
        stretching = want;
        stretcher->s.reset();
        inputCarry = 0;
        latencyFrames = 0;
    }
    long long pos = readPos.load();
    const float* in = s->instrumental.data();
    const float* vo = s->vocals.data();

    if (!stretching) {
        const int n = (int)std::clamp<long long>(total - pos, 0, frames);
        for (int i = 0; i < n; ++i) {
            // The slider is ramped across the buffer; a step in level is heard as a click.
            const float g = lastLevel + (level - lastLevel) * (float)(i + 1) / frames;
            const size_t k = (size_t)(pos + i) * 2;
            out[i * 2] = in[k] + g * vo[k];
            out[i * 2 + 1] = in[k + 1] + g * vo[k + 1];
        }
        lastLevel = level;
        readPos = pos + n;
        if (n < frames) isPlaying = false;
        return;
    }

    auto& st = stretcher->s;
    st.setTransposeSemitones(semis);
    inputCarry += (double)frames * speed;
    const int inN = (int)inputCarry;
    inputCarry -= inN;
    if ((int)inL.size() < inN) { inL.resize(inN); inR.resize(inN); }
    if ((int)outL.size() < frames) { outL.resize(frames); outR.resize(frames); }
    for (int i = 0; i < inN; ++i) {
        const long long p = pos + i;
        if (p >= total) { inL[i] = inR[i] = 0.f; continue; }
        const float g = lastLevel + (level - lastLevel) * (float)(i + 1) / std::max(inN, 1);
        const size_t k = (size_t)p * 2;
        inL[i] = in[k] + g * vo[k];
        inR[i] = in[k + 1] + g * vo[k + 1];
    }
    lastLevel = level;
    float* ins[2] = {inL.data(), inR.data()};
    float* outs[2] = {outL.data(), outR.data()};
    st.process(ins, inN, outs, frames);
    for (int i = 0; i < frames; ++i) { out[i * 2] = outL[i]; out[i * 2 + 1] = outR[i]; }
    const int latency = st.inputLatency() + (int)(st.outputLatency() * speed);
    latencyFrames = latency;
    readPos = pos + inN;
    // Keep going past the end until what is still inside the stretcher has been played.
    if (pos + inN >= total + latency) isPlaying = false;
}

std::vector<float> renderStretched(const std::vector<float>& stereo, float semitones, float tempo) {
    if (!needsStretch(semitones, tempo)) return stereo;
    signalsmith::stretch::SignalsmithStretch<float> st;
    st.presetDefault(2, (float)kModelRate);
    st.setTransposeSemitones(semitones);
    const size_t inFrames = stereo.size() / 2;
    const size_t outFrames = (size_t)((double)inFrames / tempo);
    // The stretcher answers late by a fixed amount. Render that much extra and drop it from the
    // front, so the export starts where the song does.
    const size_t latency = (size_t)(st.outputLatency() + st.inputLatency() / tempo);
    const int block = 4096;
    std::vector<float> out(outFrames * 2), inL, inR, outL(block), outR(block);
    double carry = 0;
    size_t inPos = 0, produced = 0;
    while (produced < outFrames + latency) {
        carry += (double)block * tempo;
        const int inN = (int)carry;
        carry -= inN;
        inL.assign(inN, 0.f); inR.assign(inN, 0.f);
        for (int i = 0; i < inN && inPos + i < inFrames; ++i) { inL[i] = stereo[(inPos + i) * 2]; inR[i] = stereo[(inPos + i) * 2 + 1]; }
        inPos += inN;
        float* ins[2] = {inL.data(), inR.data()};
        float* outs[2] = {outL.data(), outR.data()};
        st.process(ins, inN, outs, block);
        for (int i = 0; i < block; ++i) {
            const size_t at = produced + i;
            if (at < latency || at - latency >= outFrames) continue;
            out[(at - latency) * 2] = outL[i];
            out[(at - latency) * 2 + 1] = outR[i];
        }
        produced += block;
    }
    return out;
}
