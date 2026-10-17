#include "audio.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <cctype>
#include <fstream>
#include <iterator>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
// The implementation is compiled in vorbis.c.
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

// libpsxav.h is C11 (_Static_assert).
#define _Static_assert static_assert
extern "C" {
#include "libpsxav.h"
}
#undef _Static_assert

namespace splash {

namespace {

enum class AudioFormat { Unknown, Wav, Mp3, Flac, Vorbis };

AudioFormat formatOf(const std::filesystem::path& file) {
    std::string e = file.extension().string();
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (e == ".wav" || e == ".aif" || e == ".aiff") return AudioFormat::Wav;  // dr_wav reads both containers
    if (e == ".mp3") return AudioFormat::Mp3;
    if (e == ".flac") return AudioFormat::Flac;
    if (e == ".ogg") return AudioFormat::Vorbis;
    return AudioFormat::Unknown;
}

const char* formatName(AudioFormat f) {
    switch (f) {
        case AudioFormat::Wav: return "WAV or AIFF";
        case AudioFormat::Mp3: return "MP3";
        case AudioFormat::Flac: return "FLAC";
        case AudioFormat::Vorbis: return "Ogg Vorbis";
        default: return "audio";
    }
}

// The whole file, opened through std::filesystem so non-ASCII paths open on
// Windows too; every decoder then reads from memory.
std::vector<unsigned char> readFile(const std::filesystem::path& file, AudioFormat& fmt) {
    const std::string path = file.string();
    fmt = formatOf(file);
    if (fmt == AudioFormat::Unknown)
        throw std::runtime_error(path + ": not an audio file (WAV, AIFF, MP3, Ogg Vorbis or FLAC)");
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error(path + ": cannot open");
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) throw std::runtime_error(path + ": not a readable " + std::string(formatName(fmt)) + " file");
    return bytes;
}

[[noreturn]] void unreadable(const std::filesystem::path& file, AudioFormat fmt) {
    throw std::runtime_error(file.string() + ": not a readable " + std::string(formatName(fmt)) + " file");
}

}  // namespace

bool isAudioFile(const std::filesystem::path& file) { return formatOf(file) != AudioFormat::Unknown; }

MonoAudio loadAudioMono(const std::filesystem::path& file) {
    AudioFormat fmt;
    std::vector<unsigned char> bytes = readFile(file, fmt);
    unsigned channels = 0, rate = 0;
    uint64_t frames = 0;
    std::vector<float> interleaved;
    if (fmt == AudioFormat::Wav) {
        drwav_uint64 n = 0;
        float* d = drwav_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &channels, &rate, &n, nullptr);
        if (!d) unreadable(file, fmt);
        frames = n;
        interleaved.assign(d, d + size_t(n) * channels);
        drwav_free(d, nullptr);
    } else if (fmt == AudioFormat::Mp3) {
        drmp3_config cfg{};
        drmp3_uint64 n = 0;
        float* d = drmp3_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &cfg, &n, nullptr);
        if (!d) unreadable(file, fmt);
        channels = cfg.channels;
        rate = cfg.sampleRate;
        frames = n;
        interleaved.assign(d, d + size_t(n) * channels);
        drmp3_free(d, nullptr);
    } else if (fmt == AudioFormat::Flac) {
        drflac_uint64 n = 0;
        float* d = drflac_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &channels, &rate, &n, nullptr);
        if (!d) unreadable(file, fmt);
        frames = n;
        interleaved.assign(d, d + size_t(n) * channels);
        drflac_free(d, nullptr);
    } else {
        int err = 0;
        stb_vorbis* v = stb_vorbis_open_memory(bytes.data(), int(bytes.size()), &err, nullptr);
        if (!v) unreadable(file, fmt);
        stb_vorbis_info vi = stb_vorbis_get_info(v);
        channels = unsigned(vi.channels);
        rate = vi.sample_rate;
        std::vector<float> buf(4096 * size_t(std::max(1, vi.channels)));
        for (;;) {
            int got = stb_vorbis_get_samples_float_interleaved(v, vi.channels, buf.data(), int(buf.size()));
            if (got <= 0) break;
            interleaved.insert(interleaved.end(), buf.begin(), buf.begin() + std::ptrdiff_t(got) * vi.channels);
        }
        stb_vorbis_close(v);
        frames = channels ? interleaved.size() / channels : 0;
    }
    if (channels == 0 || rate == 0) unreadable(file, fmt);
    MonoAudio a;
    a.rate = int(rate);
    a.samples.resize(size_t(frames));
    for (size_t i = 0; i < size_t(frames); i++) {
        float sum = 0;
        for (unsigned c = 0; c < channels; c++) sum += interleaved[i * channels + c];
        a.samples[i] = channels > 1 ? sum / float(channels) : sum;
    }
    return a;
}

AudioFileInfo audioFileInfo(const std::filesystem::path& file) {
    AudioFormat fmt;
    std::vector<unsigned char> bytes = readFile(file, fmt);
    AudioFileInfo info;
    if (fmt == AudioFormat::Wav) {
        drwav wav;
        if (!drwav_init_memory(&wav, bytes.data(), bytes.size(), nullptr)) unreadable(file, fmt);
        info = {int(wav.sampleRate), int(wav.channels), uint64_t(wav.totalPCMFrameCount)};
        drwav_uninit(&wav);
    } else if (fmt == AudioFormat::Mp3) {
        drmp3 mp3;
        if (!drmp3_init_memory(&mp3, bytes.data(), bytes.size(), nullptr)) unreadable(file, fmt);
        // MP3 has no length in its header: this decodes the frames to count them.
        info = {int(mp3.sampleRate), int(mp3.channels), uint64_t(drmp3_get_pcm_frame_count(&mp3))};
        drmp3_uninit(&mp3);
    } else if (fmt == AudioFormat::Flac) {
        drflac* fl = drflac_open_memory(bytes.data(), bytes.size(), nullptr);
        if (!fl) unreadable(file, fmt);
        info = {int(fl->sampleRate), int(fl->channels), uint64_t(fl->totalPCMFrameCount)};
        drflac_close(fl);
    } else {
        int err = 0;
        stb_vorbis* v = stb_vorbis_open_memory(bytes.data(), int(bytes.size()), &err, nullptr);
        if (!v) unreadable(file, fmt);
        stb_vorbis_info vi = stb_vorbis_get_info(v);
        info = {int(vi.sample_rate), vi.channels, uint64_t(stb_vorbis_stream_length_in_samples(v))};
        stb_vorbis_close(v);
    }
    if (info.rate <= 0 || info.channels <= 0) unreadable(file, fmt);
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
