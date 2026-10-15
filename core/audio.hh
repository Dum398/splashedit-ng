// Audio clips: WAV -> mono 16-bit PCM at the clip's rate -> SPU-ADPCM, laid
// out the way `psxavenc -t spu -f <rate> [-L]` writes it (SplashEdit 2.4).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace splash {

struct MonoAudio {
    std::vector<float> samples;  // -1..1, channels averaged
    int rate = 0;
};

// Any PCM or float WAV dr_wav reads. Channels are averaged.
MonoAudio loadWavMono(const std::filesystem::path& file);

// A WAV file's format and length, from its header alone.
struct WavInfo {
    int rate = 0;
    int channels = 0;
    uint64_t frames = 0;
};
WavInfo wavInfo(const std::filesystem::path& file);

// SplashEdit 2.4's "Trim Leading Silence": drop samples below 1/512 at the
// start, keeping 4 ms before the first loud one. A fully silent clip is kept.
void trimLeadingSilence(MonoAudio& a);

// Windowed-sinc resample to `rate`. Identity when the rates match.
MonoAudio resample(const MonoAudio& a, int rate);
// How many samples resample() makes of `n` samples at `from` Hz.
size_t resampledCount(size_t n, int from, int to);

// (short)(clamp(x, -1, 1) * 32767), truncating like the C# cast.
std::vector<int16_t> toPcm16(const std::vector<float>& samples);

// psxavenc's SPU layout: a silent leading block, the ADPCM blocks (the last
// one flagged LOOP_REPEAT when looping), a LOOP_TRAP block when not looping,
// zero padding to 64 bytes.
std::vector<uint8_t> encodeSpuAdpcm(const std::vector<int16_t>& pcm, bool loop);
// encodeSpuAdpcm's output size for `samples` samples, without encoding.
size_t spuAdpcmBytes(size_t samples, bool loop);

// Decode SPU-ADPCM back to PCM (for measuring the encoder).
std::vector<int16_t> decodeSpuAdpcm(const std::vector<uint8_t>& adpcm);

}  // namespace splash
