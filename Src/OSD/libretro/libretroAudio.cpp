#include <OSD/Audio.h>
#include "Supermodel.h"
#include <cmath>
#include <algorithm>
#include <mutex>
#include "libretro_cbs.h"
#include "LibretroTiming.h"

// Model 3 audio output is 44.1 kHz, mixed to stereo for Libretro.
#define SAMPLE_RATE_M3     (LibretroTiming::kAudioSampleRate)
#define MAX_SND_FREQ       (75)
#define MIN_SND_FREQ       (45)
#define MAX_LATENCY        (100)
#define NUM_CHANNELS_M3 (4)

Game::AudioTypes AudioType;
int nbHostAudioChannels = NUM_CHANNELS_M3;      // Number of channels on host

#define SAMPLES_PER_FRAME_M3  (INT32)(LibretroTiming::kDefaultAudioFramesPerVideoFrame)
#define BYTES_PER_SAMPLE_M3   (NUM_CHANNELS_M3 * sizeof(INT16))
#define BYTES_PER_FRAME_M3   (SAMPLES_PER_FRAME_M3 * BYTES_PER_SAMPLE_M3)


int samples_per_frame_host = SAMPLES_PER_FRAME_M3;
static int bytes_per_sample_host = BYTES_PER_SAMPLE_M3;
int bytes_per_frame_host = BYTES_PER_FRAME_M3;

// Balance percents for mixer
float BalanceLeftRight = 0;                     // 0 mid balance, 100: left only,  -100:right only 
float BalanceFrontRear = 0;                     // 0 mid balance, 100: front only, -100:right only 
// Mixer factor (depends on values above)
float balanceFactorFrontLeft  = 1.0f;
float balanceFactorFrontRight = 1.0f;
float balanceFactorRearLeft   = 1.0f;
float balanceFactorRearRight  = 1.0f;

static bool enabled = true;                     // True if sound output is enabled
static constexpr bool underRunLoop = true;      // True if should loop back to beginning of buffer on under-run, otherwise sound is just skipped
static UINT32 audioBufferSize = 0;              // Size (in bytes) of audio buffer
static INT8* audioBuffer = NULL;                // Audio buffer
static UINT32 writePos = 0;                     // Current position at which writing into buffer
static UINT32 playPos = 0;                      // Current position at which playing data in buffer via callback
static bool writeWrapped = false;               // True if write position has wrapped around at end of buffer but play position has not done so yet
static unsigned underRuns = 0;                  // Number of buffer under-runs that have occured
static unsigned overRuns = 0;                   // Number of buffer over-runs that have occured
static unsigned shortWrites = 0;                // Frontend batch callbacks that accepted fewer frames than requested
static AudioCallbackFPtr callback = NULL;       // Pointer to audio callback that is called when audio buffer is less than half empty
static void* callbackData = NULL;               // Pointer to data to be passed to audio callback when it is called
static std::mutex s_audioMutex;
static const Util::Config::Node* s_config = 0;

UINT32 GetAvailableAudioLen()
{
    // Simple circular buffer distance
    if (writePos >= playPos)
    {
        return writePos - playPos;
    }
    else
    {
        // Write has wrapped around, play has not
        return (audioBufferSize - playPos) + writePos;
    }
}
void SetAudioCallback(AudioCallbackFPtr newCallback, void* newData)
{
    // Scoped lock: Automatically unlocks when it goes out of scope
    std::lock_guard<std::mutex> lock(s_audioMutex);

    callback = newCallback;
    callbackData = newData;
}

void SetAudioEnabled(bool newEnabled)
{
    enabled = newEnabled;
}

/// <summary>
/// Set game audio mixing type
/// </summary>
/// <param name="type"></param>
void SetAudioType(Game::AudioTypes type)
{
    AudioType = type;
}

static INT16 MixINT16(float x, float y)
{
    INT32 sum = (INT32)((x + y)*0.5f); //!! dither
    if (sum > INT16_MAX) {
        sum = INT16_MAX;
    }
    if (sum < INT16_MIN) {
        sum = INT16_MIN;
    }
    return (INT16)sum;
}

static float MixFloat(float x, float y)
{
    return (x + y)*0.5f;
}

static INT16 ClampINT16(float x)
{
    INT32 xi = (INT32)x; //!! dither
    if (xi > INT16_MAX) {
        xi = INT16_MAX;
    }
    if (xi < INT16_MIN) {
        xi = INT16_MIN;
    }
    return (INT16)xi;
}
void PlayCallback(void* /*data*/, uint8_t* /*stream*/, int len)
{
    std::lock_guard<std::mutex> lock(s_audioMutex);
    if (!enabled || !audio_batch_cb) return;

    UINT32 avail = GetAvailableAudioLen();
    UINT32 to_read = (avail < (UINT32)len) ? avail : (UINT32)len;
    
    INT8* src1 = nullptr;
    INT8* src2 = nullptr;
    UINT32 len1 = 0, len2 = 0;

    if (to_read > 0)
    {
        if (playPos + to_read > audioBufferSize)
        {
            src1 = audioBuffer + playPos;
            len1 = audioBufferSize - playPos;
            src2 = audioBuffer;
            len2 = to_read - len1;
        }
        else
        {
            src1 = audioBuffer + playPos;
            len1 = to_read;
        }

        UINT32 consumed_bytes = 0;
        bool frontend_short_write = false;

        if (len1 > 0)
        {
            const size_t requested_frames = len1 / bytes_per_sample_host;
            const size_t accepted_frames = std::min(
                audio_batch_cb((const int16_t*)src1, requested_frames),
                requested_frames);
            consumed_bytes += static_cast<UINT32>(
                accepted_frames * bytes_per_sample_host);
            frontend_short_write = accepted_frames != requested_frames;
        }

        // Preserve submission order across the ring-buffer boundary. If the
        // frontend only accepted part of the first span, leave both the
        // unaccepted frames and the wrapped span queued for the next call.
        if (!frontend_short_write && len2 > 0)
        {
            const size_t requested_frames = len2 / bytes_per_sample_host;
            const size_t accepted_frames = std::min(
                audio_batch_cb((const int16_t*)src2, requested_frames),
                requested_frames);
            consumed_bytes += static_cast<UINT32>(
                accepted_frames * bytes_per_sample_host);
            frontend_short_write = accepted_frames != requested_frames;
        }

        if (frontend_short_write)
            ++shortWrites;

        const UINT32 old_play_pos = playPos;
        playPos = (playPos + consumed_bytes) % audioBufferSize;
        if (old_play_pos + consumed_bytes >= audioBufferSize)
            writeWrapped = false;

        // A short frontend write is not an emulator under-run. Do not append
        // synthetic samples after data the frontend explicitly deferred.
        if (frontend_short_write)
            to_read = static_cast<UINT32>(len);
    }

    int missing_bytes = len - to_read;
    if (missing_bytes > 0)
    {
        // Get last two samples for interpolation
        int16_t* lastSample = (int16_t*)(audioBuffer + ((playPos - 4 + audioBufferSize) % audioBufferSize));
        
        static int16_t fade_buf[4096];
        int samples_to_fill = missing_bytes / 4;
        
        // Linear fade to silence (less jarring than sudden repeat or silence)
        for (int i = 0; i < samples_to_fill && i < 2048; i++) {
            float fade = 1.0f - (float)i / (float)samples_to_fill;
            fade_buf[i*2]   = (int16_t)(lastSample[0] * fade);  // Left
            fade_buf[i*2+1] = (int16_t)(lastSample[1] * fade);  // Right
        }
        
        const size_t requested_frames = static_cast<size_t>(samples_to_fill);
        const size_t accepted_frames = std::min(
            audio_batch_cb(fade_buf, requested_frames), requested_frames);
        if (accepted_frames != requested_frames)
            ++shortWrites;
    }

    if (callback) callback(callbackData);
}

static void MixChannels(unsigned numSamples, const float* leftFrontBuffer, const float* rightFrontBuffer, const float* leftRearBuffer, const float* rightRearBuffer, void* dest, bool flipStereo)
{
    INT16* p = (INT16*)dest;

    if (nbHostAudioChannels == 1) {
        for (unsigned i = 0; i < numSamples; i++) {
            INT16 monovalue = MixINT16(
                MixFloat(leftFrontBuffer[i] * balanceFactorFrontLeft,rightFrontBuffer[i] * balanceFactorFrontRight),
                MixFloat(leftRearBuffer[i]  * balanceFactorRearLeft, rightRearBuffer[i]  * balanceFactorRearRight));
            *p++ = monovalue;
        }
    } else {
        // Flip again left/right if configured in audio
        switch (AudioType) {
        case Game::STEREO_RL:
        case Game::QUAD_1_FRL_2_RRL:
        case Game::QUAD_1_RRL_2_FRL:
            flipStereo = !flipStereo;
            break;
        default:
            break;
        }

        // Now order channels according to audio type
        if (nbHostAudioChannels == 2) {
            for (unsigned i = 0; i < numSamples; i++) {
                INT16 leftvalue = MixINT16(leftFrontBuffer[i] * balanceFactorFrontLeft, leftRearBuffer[i] * balanceFactorRearLeft);
                INT16 rightvalue = MixINT16(rightFrontBuffer[i]*balanceFactorFrontRight, rightRearBuffer[i]*balanceFactorRearRight);
                if (flipStereo) // swap left and right channels
                {
                    *p++ = rightvalue;
                    *p++ = leftvalue;
                } else {
                    *p++ = leftvalue;
                    *p++ = rightvalue;
                }
            }
        } else if (nbHostAudioChannels == 4) {
            for (unsigned i = 0; i < numSamples; i++) {
                float frontLeftValue = leftFrontBuffer[i]*balanceFactorFrontLeft;
                float frontRightValue = rightFrontBuffer[i]*balanceFactorFrontRight;
                float rearLeftValue = leftRearBuffer[i]*balanceFactorRearLeft;
                float rearRightValue = rightRearBuffer[i]*balanceFactorRearRight;

                // Check game audio type
                switch (AudioType) {
                case Game::MONO: {
                    INT16 monovalue = MixINT16(MixFloat(frontLeftValue, frontRightValue), MixFloat(rearLeftValue, rearRightValue));
                    *p++ = monovalue;
                    *p++ = monovalue;
                    *p++ = monovalue;
                    *p++ = monovalue;
                } break;

                case Game::STEREO_LR:
                case Game::STEREO_RL: {
                    INT16 leftvalue =  MixINT16(frontLeftValue, frontRightValue);
                    INT16 rightvalue = MixINT16(rearLeftValue,  rearRightValue);
                    if (flipStereo) // swap left and right channels
                    {
                        *p++ = rightvalue;
                        *p++ = leftvalue;
                        *p++ = rightvalue;
                        *p++ = leftvalue;
                    } else {
                        *p++ = leftvalue;
                        *p++ = rightvalue;
                        *p++ = leftvalue;
                        *p++ = rightvalue;
                    }
                } break;

                case Game::QUAD_1_FLR_2_RLR:
                case Game::QUAD_1_FRL_2_RRL: {
                    // Normal channels Front Left/Right then Rear Left/Right
                    if (flipStereo) // swap left and right channels
                    {
                        *p++ = ClampINT16(frontRightValue);
                        *p++ = ClampINT16(frontLeftValue);
                        *p++ = ClampINT16(rearRightValue);
                        *p++ = ClampINT16(rearLeftValue);
                    } else {
                        *p++ = ClampINT16(frontLeftValue);
                        *p++ = ClampINT16(frontRightValue);
                        *p++ = ClampINT16(rearLeftValue);
                        *p++ = ClampINT16(rearRightValue);
                    }
                } break;

                case Game::QUAD_1_RLR_2_FLR:
                case Game::QUAD_1_RRL_2_FRL:
                    // Reversed channels Front/Rear Left then Front/Rear Right
                    if (flipStereo) // swap left and right channels
                    {
                        *p++ = ClampINT16(rearRightValue);
                        *p++ = ClampINT16(rearLeftValue);
                        *p++ = ClampINT16(frontRightValue);
                        *p++ = ClampINT16(frontLeftValue);
                    } else {
                        *p++ = ClampINT16(rearLeftValue);
                        *p++ = ClampINT16(rearRightValue);
                        *p++ = ClampINT16(frontLeftValue);
                        *p++ = ClampINT16(frontRightValue);
                    }
                    break;

                case Game::QUAD_1_LR_2_FR_MIX:
                    // Split mix: one goes to left/right, other front/rear (mono)
                    // =>Remix all!
                    INT16 newfrontLeftValue = MixINT16(frontLeftValue, rearLeftValue);
                    INT16 newfrontRightValue = MixINT16(frontLeftValue, rearRightValue);
                    INT16 newrearLeftValue = MixINT16(frontRightValue, rearLeftValue);
                    INT16 newrearRightValue = MixINT16(frontRightValue, rearRightValue);

                    if (flipStereo) // swap left and right channels
                    {
                        *p++ = newfrontRightValue;
                        *p++ = newfrontLeftValue;
                        *p++ = newrearRightValue;
                        *p++ = newrearLeftValue;
                    } else {
                        *p++ = newfrontLeftValue;
                        *p++ = newfrontRightValue;
                        *p++ = newrearLeftValue;
                        *p++ = newrearRightValue;
                    }
                    break;
                }
            }
        }
    }
}

/*
static void LogAudioInfo(SDL_AudioSpec *fmt)
{
    InfoLog("Audio device information:");
    InfoLog("    Frequency: %d", fmt->freq);
    InfoLog("     Channels: %d", fmt->channels);
    InfoLog("Sample Format: %d", fmt->format);
    InfoLog("");
}
*/

/// <summary>
/// Prepare audio subsystem on host.
/// The requested channels is deduced, and SDL will make sure it is compatible with this.
/// </summary>
/// <param name="config"></param>
/// <returns></returns>
Result OpenAudio(const Util::Config::Node& config)
{
    s_config = &config;

    // 1. Channel Configuration
    // We force this to 2 (Stereo) for Libretro unless you specifically 
    // want to support 4-channel surround in RetroArch (which is complex).
    nbHostAudioChannels = 2; 

    // Mixer Balance Logic (Keep this - it's math, not SDL)
    float balancelr = std::max(-100.f, std::min(100.f, s_config->Get("BalanceLeftRight").ValueAs<float>()));
    balancelr *= 0.01f;
    BalanceLeftRight = balancelr;

    float balancefr = std::max(-100.f, std::min(100.f, s_config->Get("BalanceFrontRear").ValueAs<float>()));
    balancefr *= 0.01f;
    BalanceFrontRear = balancefr;

    balanceFactorFrontLeft  = (BalanceLeftRight < 0.f ? 1.f + BalanceLeftRight : 1.f) * (BalanceFrontRear < 0 ? 1.f + BalanceFrontRear : 1.f);
    balanceFactorFrontRight = (BalanceLeftRight > 0.f ? 1.f - BalanceLeftRight : 1.f) * (BalanceFrontRear < 0 ? 1.f + BalanceFrontRear : 1.f);
    balanceFactorRearLeft   = (BalanceLeftRight < 0.f ? 1.f + BalanceLeftRight : 1.f) * (BalanceFrontRear > 0 ? 1.f - BalanceFrontRear : 1.f);
    balanceFactorRearRight  = (BalanceLeftRight > 0.f ? 1.f - BalanceLeftRight : 1.f) * (BalanceFrontRear > 0 ? 1.f - BalanceFrontRear : 1.f);

    // 2. Timing Calculations
    // Model 3 internal rate is 44100Hz.
    float soundFreq_Hz = (float)s_config->Get("SoundFreq").ValueAs<float>();
    if (soundFreq_Hz > MAX_SND_FREQ) soundFreq_Hz = MAX_SND_FREQ;
    if (soundFreq_Hz < MIN_SND_FREQ) soundFreq_Hz = MIN_SND_FREQ;

    samples_per_frame_host = (INT32)(SAMPLE_RATE_M3 / soundFreq_Hz);
    bytes_per_sample_host = (nbHostAudioChannels * sizeof(INT16));
    bytes_per_frame_host = (samples_per_frame_host * bytes_per_sample_host);

    // 3. Buffer Allocation
    // We still need a small temporary mix buffer to interleave L/R samples
    // before sending them to the Libretro callback.
    if (audioBuffer) {
        delete[] audioBuffer;
        audioBuffer = nullptr;
    }

    // Allocate enough for roughly one frame of audio (standard is ~735 samples for 60fps)
    // We'll be safe and allocate 4096 bytes.
    audioBufferSize = 8192 * bytes_per_sample_host;  // Double the buffer
    audioBuffer = new(std::nothrow) INT8[audioBufferSize];
    
    if (audioBuffer == NULL) {
        return Result::FAIL;
    }
    memset(audioBuffer, 0, audioBufferSize);

    // 4. State initialization. The ring buffer cannot represent full and
    // empty with identical read/write positions. Seed two silent packets so
    // the first frontend callback has valid data while it wakes the
    // asynchronous SoundBoard thread. That thread then replaces the initial
    // silence with real audio and keeps the buffer filled.
    playPos = 0;
    writePos = 2 * bytes_per_frame_host;
    writeWrapped = false;
    underRuns = 0;
    overRuns = 0;
    shortWrites = 0;

    // In Libretro, "starting" audio just means we are ready to accept calls.
    enabled = true; 

    return Result::OKAY;
}

bool OutputAudio(unsigned numSamples, const float* leftFrontBuffer, const float* rightFrontBuffer, const float* leftRearBuffer, const float* rightRearBuffer, bool flipStereo)
{
    INT16* src;

    // 1. Bound Check
    if (numSamples > (unsigned)samples_per_frame_host)
        numSamples = samples_per_frame_host;

    // 2. Mix Phase (This is CPU heavy, we do it BEFORE the lock to keep FPS high)
    // Using a static buffer to avoid stack overflow (Model 3 buffers can be large)
    static INT16 mixBuffer[NUM_CHANNELS_M3 * 4096]; 
    MixChannels(numSamples, leftFrontBuffer, rightFrontBuffer, leftRearBuffer, rightRearBuffer, mixBuffer, flipStereo);

    // 3. Thread Safety: Replace SDL_LockAudio
    std::lock_guard<std::mutex> lock(s_audioMutex);

    UINT32 numBytes = numSamples * bytes_per_sample_host;
    UINT32 playEndPos = playPos + bytes_per_frame_host;

    // Logic for wrap-around and under-run remains identical to preserve your "85% fix"
    if (playEndPos > writePos && writeWrapped)
        writePos += audioBufferSize;

    if (playEndPos > writePos)
    {
        underRuns++;
        if (underRunLoop)
        {
            playPos = writePos + numBytes + bytes_per_frame_host;
            if (playPos >= audioBufferSize)
                playPos -= audioBufferSize;
            else
            {
                writeWrapped = true;
                writePos += audioBufferSize;
            }
        }
        else
        {
            do { writePos += numBytes; } while (playEndPos > writePos);
        }
    }

    bool overRun = writePos + numBytes > playPos + audioBufferSize;
    bool bufferFull = writePos + 2 * bytes_per_frame_host > playPos + audioBufferSize;

    if (writePos >= audioBufferSize)
        writePos -= audioBufferSize;

    if (overRun)
    {
        overRuns++;
        bufferFull = true;
        // Discarding chunk...
    }
    else
    {
        src = mixBuffer;
        INT8* dst1;
        INT8* dst2;
        UINT32 len1;
        UINT32 len2;

        if (writePos + numBytes > audioBufferSize)
        {
            dst1 = audioBuffer + writePos;
            dst2 = audioBuffer;
            len1 = audioBufferSize - writePos;
            len2 = numBytes - len1;
        }
        else
        {
            dst1 = audioBuffer + writePos;
            dst2 = nullptr;
            len1 = numBytes;
            len2 = 0;
        }

        // Copy to ring buffer
        memcpy(dst1, src, len1);
        if (len2 > 0 && dst2 != nullptr)
        {
            memcpy(dst2, (UINT8*)src + len1, len2);
        }

        writePos += numBytes;
        if (writePos >= audioBufferSize)
        {
            writePos -= audioBufferSize;
            writeWrapped = true;
        }
    }

    return bufferFull;
}


void CloseAudio()
{
    if (shortWrites > 0)
        InfoLog("[Supermodel] Audio frontend reported %u partial batch submission(s); unaccepted ring-buffer frames were preserved.",
                shortWrites);
}
