#include "audio.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

// libpsxav.h is C11 (_Static_assert).
#define _Static_assert static_assert
extern "C" {
#include "libpsxav.h"
}
#undef _Static_assert

namespace splash {

MonoAudio loadWavMono(const std::filesystem::path& file) {
    unsigned channels = 0, rate = 0;
    drwav_uint64 frames = 0;
    std::string path = file.string();
    float* data = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &rate, &frames, nullptr);
    if (!data) throw std::runtime_error(path + ": not a readable WAV file");
    MonoAudio a;
    a.rate = int(rate);
    a.samples.resize(size_t(frames));
    for (size_t i = 0; i < size_t(frames); i++) {
        float sum = 0;
        for (unsigned c = 0; c < channels; c++) sum += data[i * channels + c];
        a.samples[i] = channels > 1 ? sum / float(channels) : sum;
    }
    drwav_free(data, nullptr);
    return a;
}

WavInfo wavInfo(const std::filesystem::path& file) {
    std::string path = file.string();
    drwav wav;
    if (!drwav_init_file(&wav, path.c_str(), nullptr)) throw std::runtime_error(path + ": not a readable WAV file");
    WavInfo info{int(wav.sampleRate), int(wav.channels), uint64_t(wav.totalPCMFrameCount)};
    drwav_uninit(&wav);
    return info;
}

void trimLeadingSilence(MonoAudio& a) {
    const float floor = 1.f / 512.f;
    size_t first = a.samples.size();
    for (size_t i = 0; i < a.samples.size(); i++)
        if (a.samples[i] > floor || a.samples[i] < -floor) {
            first = i;
            break;
        }
    if (first == a.samples.size()) return;
    size_t guard = size_t(a.rate) * 4 / 1000;
    first = first > guard ? first - guard : 0;
    a.samples.erase(a.samples.begin(), a.samples.begin() + std::ptrdiff_t(first));
}

size_t resampledCount(size_t n, int from, int to) {
    if (to == from || n == 0) return n;
    return size_t(std::llround(double(n) * (double(to) / double(from))));
}

MonoAudio resample(const MonoAudio& a, int rate) {
    if (rate == a.rate || a.samples.empty()) return {a.samples, rate};
    const double ratio = double(rate) / double(a.rate);
    const double cutoff = 0.5 * std::min(1.0, ratio) * 0.95;  // cycles per input sample
    const int halfTaps = int(std::ceil(16.0 / std::min(1.0, ratio)));
    const double pi = 3.14159265358979323846;
    size_t outCount = resampledCount(a.samples.size(), a.rate, rate);
    MonoAudio out;
    out.rate = rate;
    out.samples.resize(outCount);
    const long n = long(a.samples.size());
    for (size_t i = 0; i < outCount; i++) {
        double t = double(i) / ratio;
        long centre = long(std::floor(t));
        double acc = 0, wsum = 0;
        for (long j = centre - halfTaps + 1; j <= centre + halfTaps; j++) {
            double x = t - double(j);
            double s = x == 0 ? 2 * cutoff : std::sin(2 * pi * cutoff * x) / (pi * x);
            double w = 0.5 + 0.5 * std::cos(pi * x / halfTaps);  // Hann
            double k = s * w;
            wsum += k;
            if (j >= 0 && j < n) acc += k * a.samples[size_t(j)];
        }
        out.samples[i] = float(wsum != 0 ? acc / wsum : 0);
    }
    return out;
}

std::vector<int16_t> toPcm16(const std::vector<float>& samples) {
    std::vector<int16_t> out(samples.size());
    for (size_t i = 0; i < samples.size(); i++) {
        float v = std::clamp(samples[i], -1.f, 1.f) * 32767.f;
        out[i] = int16_t(v);  // truncates toward zero
    }
    return out;
}

std::vector<uint8_t> encodeSpuAdpcm(const std::vector<int16_t>& pcm, bool loop) {
    const int samplesPerBlock = PSX_AUDIO_SPU_SAMPLES_PER_BLOCK, blockSize = PSX_AUDIO_SPU_BLOCK_SIZE;
    std::vector<uint8_t> out(blockSize, 0);  // leading silent block
    psx_audio_encoder_channel_state_t state;
    std::memset(&state, 0, sizeof(state));
    uint8_t block[PSX_AUDIO_SPU_BLOCK_SIZE];
    for (size_t pos = 0; pos < pcm.size(); pos += samplesPerBlock) {
        int n = int(std::min<size_t>(samplesPerBlock, pcm.size() - pos));
        int len = psx_audio_spu_encode(&state, pcm.data() + pos, n, 1, block);
        if (loop && pos + samplesPerBlock >= pcm.size()) block[1] |= PSX_AUDIO_SPU_LOOP_REPEAT;
        out.insert(out.end(), block, block + len);
    }
    if (!loop) {
        uint8_t trap[PSX_AUDIO_SPU_BLOCK_SIZE] = {};
        trap[1] = PSX_AUDIO_SPU_LOOP_TRAP;
        out.insert(out.end(), trap, trap + blockSize);
    }
    out.resize((out.size() + 63) / 64 * 64, 0);
    return out;
}

size_t spuAdpcmBytes(size_t samples, bool loop) {
    const size_t samplesPerBlock = PSX_AUDIO_SPU_SAMPLES_PER_BLOCK, blockSize = PSX_AUDIO_SPU_BLOCK_SIZE;
    size_t bytes = blockSize + (samples + samplesPerBlock - 1) / samplesPerBlock * blockSize;  // silent lead + data
    if (!loop) bytes += blockSize;                                                         // trap
    return (bytes + 63) / 64 * 64;
}

std::vector<int16_t> decodeSpuAdpcm(const std::vector<uint8_t>& adpcm) {
    static const int f0[5] = {0, 60, 115, 98, 122}, f1[5] = {0, 0, -52, -55, -60};
    std::vector<int16_t> out;
    int old = 0, older = 0;
    for (size_t b = 0; b + 16 <= adpcm.size(); b += 16) {
        int shift = adpcm[b] & 15, filter = std::min(adpcm[b] >> 4, 4);
        if (shift > 12) shift = 9;
        for (int i = 0; i < 28; i++) {
            int nib = (adpcm[b + 2 + i / 2] >> ((i & 1) * 4)) & 15;
            int s = int16_t(uint16_t(nib << 12)) >> shift;
            s += (old * f0[filter] + older * f1[filter] + 32) >> 6;
            s = std::clamp(s, -32768, 32767);
            out.push_back(int16_t(s));
            older = old;
            old = s;
        }
    }
    return out;
}

}  // namespace splash
