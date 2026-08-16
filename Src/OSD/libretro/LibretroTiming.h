#ifndef SUPERMODEL_LIBRETRO_TIMING_H
#define SUPERMODEL_LIBRETRO_TIMING_H

#include <cstdint>

namespace LibretroTiming {

// Supermodel defaults to 60 Hz to avoid judder on ordinary displays, while
// native Model 3 timing is 57.524160 Hz. Libretro always exposes the original
// 44.1 kHz audio stream; native timing packetizes it into a deterministic
// 766/767-frame cadence instead of changing or resampling the stream.
inline constexpr std::uint64_t kMicroHzScale = 1000000;
inline constexpr std::uint64_t kDefaultFrameRateMicroHz = 60000000;
inline constexpr std::uint64_t kNativeFrameRateMicroHz = 57524160;
inline constexpr double kDefaultFramesPerSecond = 60.0;
inline constexpr double kNativeFramesPerSecond = 57.524160;
inline constexpr unsigned kAudioSampleRate = 44100;
inline constexpr unsigned kDefaultAudioFramesPerVideoFrame =
   kAudioSampleRate / 60;
inline constexpr unsigned kStereoAudioBytesPerFrame =
   2 * sizeof(std::int16_t);

inline constexpr std::uint64_t FrameRateMicroHz(bool nativeTiming)
{
   return nativeTiming ? kNativeFrameRateMicroHz
                       : kDefaultFrameRateMicroHz;
}

inline constexpr double FramesPerSecond(bool nativeTiming)
{
   return nativeTiming ? kNativeFramesPerSecond
                       : kDefaultFramesPerSecond;
}

inline unsigned NextAudioFrames(std::uint64_t frameRateMicroHz,
                                std::uint64_t &remainder)
{
   remainder += static_cast<std::uint64_t>(kAudioSampleRate) * kMicroHzScale;
   const unsigned frames = static_cast<unsigned>(remainder / frameRateMicroHz);
   remainder %= frameRateMicroHz;
   return frames;
}

} // namespace LibretroTiming

#endif
