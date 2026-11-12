// Plays the two stems of a song as one: instrumental plus as much of the vocals as the slider
// asks for, optionally shifted in key and tempo.
#pragma once
#include <SDL.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

struct Stems {
    std::vector<float> instrumental, vocals;  // interleaved stereo at kModelRate, same length
    size_t frames() const { return instrumental.size() / 2; }
};

class Player {
public:
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool open(std::string& error);
    void close();
    void setStems(std::shared_ptr<const Stems> stems);  // stops, and rewinds to the start

    void play();
    void pause();
    bool playing() const { return isPlaying; }
    bool hasSong() const { return current != nullptr; }
    double duration() const;
    // Where the listener is in the song, in seconds of the original recording. Lyrics follow
    // this, so they stay in time at any tempo.
    double position() const;
    void seek(double seconds);

    std::atomic<float> vocalLevel{0.f};  // 0 = instrumental only, 1 = the original mix
    std::atomic<float> semitones{0.f};
    std::atomic<float> tempo{1.f};       // 1 = original speed

private:
    struct Stretcher;
    static void callback(void* user, Uint8* stream, int len);
    void fill(float* out, int frames);

    SDL_AudioDeviceID device = 0;
    std::shared_ptr<const Stems> current;
    std::unique_ptr<Stretcher> stretcher;
    std::atomic<bool> isPlaying{false};
    std::atomic<long long> readPos{0};     // next source frame to read
    std::atomic<long long> seekRequest{-1};
    std::atomic<int> latencyFrames{0};     // source frames still inside the stretcher
    bool stretching = false;
    double inputCarry = 0;
    float lastLevel = 0;
    std::vector<float> inL, inR, outL, outR;
};

// The whole song through the same key and tempo change, for exports. Returns interleaved stereo.
std::vector<float> renderStretched(const std::vector<float>& stereo, float semitones, float tempo);
