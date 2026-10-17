// Audio clips: WAV, AIFF, MP3, Ogg Vorbis or FLAC -> mono 16-bit PCM at the clip's rate -> SPU-ADPCM, laid
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

// True for the extensions loadAudioMono reads (.wav .aif .aiff .mp3 .ogg
// .flac, any case).
bool isAudioFile(const std::filesystem::path& file);
// The extensions isAudioFile accepts, for file dialog filters.
inline constexpr const char* kAudioExtensions = "wav;aif;aiff;mp3;ogg;flac";

// Decodes the whole clip: WAV and AIFF (dr_wav), MP3 (dr_mp3), FLAC (dr_flac),
// Ogg Vorbis (stb_vorbis), chosen by extension. Channels are averaged.
MonoAudio loadAudioMono(const std::filesystem::path& file);

// A clip's rate, channels and length. An MP3 is decoded to count its frames.
struct AudioFileInfo {
    int rate = 0;
    int channels = 0;
    uint64_t frames = 0;
};
AudioFileInfo audioFileInfo(const std::filesystem::path& file);

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
