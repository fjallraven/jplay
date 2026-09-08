#include "AudioEngine.h"

#include "AudioSource.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int   kBufferMs      = 200;    // target queued audio ahead of the device
constexpr float kSeekTolSec    = 0.25f;  // playhead jump beyond this => re-cue
constexpr int   kScrubGrainMs  = 60;     // length of the grain played per scrub step
constexpr double kScrubMoveTol = 0.002;  // seconds; playhead move beyond this => new grain
} // namespace

AudioEngine::AudioEngine() {
    openStream(); // default playback device; setDevice() can switch it later
}

AudioEngine::~AudioEngine() {
    if (stream_)
        SDL_DestroyAudioStream(stream_);
}

void AudioEngine::openStream() {
    // We hand SDL mixed F32 stereo at the fixed kMixRate (every channel's
    // AudioSource resamples to it) and SDL converts to the device. A null stream
    // (no device) makes update() a no-op.
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = kMixRate;
    stream_ = SDL_OpenAudioDeviceStream(deviceId_, &spec, nullptr, nullptr);
    if (!stream_)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "audio: no output device (%s)", SDL_GetError());
    else
        SDL_SetAudioStreamGain(stream_, volume_); // a fresh stream starts at unity
    // SDL device streams start paused; update() resumes when playback runs.
}

void AudioEngine::setDevice(SDL_AudioDeviceID dev) {
    if (dev == deviceId_)
        return;
    deviceId_ = dev;
    // Rebuild the stream on the new device and drop all channels so the next
    // update() reopens them and re-cues to the playhead.
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    closeChannels();
    wasActive_ = false;
    openStream();
}

// Master gain rides on the stream rather than mixInto(): SDL applies it as it
// pulls from the queue, so a slider drag is heard at once instead of after the
// ~kBufferMs of already-mixed audio ahead of the device.
void AudioEngine::setVolume(float v) {
    volume_ = std::clamp(v, 0.0f, 1.0f);
    if (stream_)
        SDL_SetAudioStreamGain(stream_, volume_);
}

void AudioEngine::closeChannels() {
    channels_.clear();
    if (stream_)
        SDL_ClearAudioStream(stream_);
}

// Reconcile the channel set with this update's items. A channel opens when its
// id first appears (or its id now resolves to a different file) and closes when
// its id disappears. Returns true when the queued mix no longer matches what
// should be audible: the set changed, or a surviving item's position jumped
// (seek / scrub / loop wrap) beyond tolerance.
bool AudioEngine::syncChannels(const std::vector<FeedItem>& items) {
    bool changed = false;

    // Drop channels whose id is gone.
    for (auto it = channels_.begin(); it != channels_.end();) {
        bool present = false;
        for (const auto& item : items)
            if (item.id == it->first) { present = true; break; }
        if (!present) {
            it = channels_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    for (const auto& item : items) {
        auto it = channels_.find(item.id);
        if (it == channels_.end() || it->second.path != item.path) {
            // New channel, or the same id now plays a different file.
            Channel ch;
            ch.path = item.path;
            std::string err;
            ch.src = AudioSource::open(item.path, err, kMixRate); // null => silent channel
            ch.readSec = ch.feedSec = item.seconds;
            ch.gainStartSec = item.gainStartSec;
            ch.gainHz = item.gainHz;
            ch.gain = item.gain;
            channels_[item.id] = std::move(ch);
            changed = true;
            continue;
        }
        // Continuing channel: a position jump beyond a frame's worth of tolerance
        // means the playhead moved discontinuously — re-cue everything.
        if (std::fabs(item.seconds - it->second.feedSec) > kSeekTolSec)
            changed = true;
        it->second.feedSec = item.seconds;
        // Refresh the envelope every update: it slides forward with the playhead,
        // and an edit to the curve has to reach the mixer. Deliberately not a
        // `changed` trigger — re-cueing on it would clear the queue on every
        // update, and a gain edit landing a buffer's worth late is inaudible.
        it->second.gainStartSec = item.gainStartSec;
        it->second.gainHz = item.gainHz;
        it->second.gain = item.gain;
    }
    return changed;
}

void AudioEngine::cueChannels(const std::vector<FeedItem>& items) {
    for (const auto& item : items) {
        auto it = channels_.find(item.id);
        if (it == channels_.end())
            continue;
        it->second.readSec = item.seconds;
        it->second.feedSec = item.seconds;
        if (it->second.src)
            it->second.src->seek(std::max(item.seconds, 0.0));
    }
}

// Sum `frames` frames from every channel into buf (zero-filled first), each
// scaled by its volume envelope. Channels whose position is still negative
// contribute silence until their start lands inside the chunk; exhausted
// channels stay silent. The sum is hard-clamped.
void AudioEngine::mixInto(std::vector<float>& buf, int frames) {
    buf.assign((size_t)frames * 2, 0.0f);
    std::vector<float> tmp;
    for (auto& kv : channels_) {
        Channel& ch = kv.second;
        int at = 0; // chunk frame the channel's audio starts contributing at
        if (ch.readSec < 0.0)
            at = std::min(frames, (int)std::llround(-ch.readSec * kMixRate));
        if (ch.src && at < frames) {
            int want = frames - at;
            tmp.resize((size_t)want * 2);
            int got = ch.src->read(tmp.data(), want);
            float* dst = buf.data() + (size_t)at * 2;
            // Per-sample volume automation. Chunk frame k is file time
            // ch.readSec + k/kMixRate, which is what makes the envelope land on
            // the right samples despite the mixer running ahead of the picture.
            const bool env = !ch.gain.empty() && ch.gainHz > 0.0;
            for (int j = 0; j < got; ++j) {
                const float g = env
                    ? ch.gainAt(ch.readSec + (double)(at + j) / kMixRate)
                    : 1.0f;
                dst[j * 2]     += tmp[j * 2]     * g;
                dst[j * 2 + 1] += tmp[j * 2 + 1] * g;
            }
        }
        ch.readSec += (double)frames / kMixRate;
    }
    for (float& s : buf)
        s = std::clamp(s, -1.0f, 1.0f);
}

void AudioEngine::update(bool active, bool scrub, bool stalled,
                         const std::vector<FeedItem>& items) {
    if (!stream_)
        return;

    if (!active) {
        if (scrub) {
            updateScrub(items);
        } else {
            // Stopped (and not scrubbing): pause. Unconditional because a grain
            // from a just-ended scrub may have left the device resumed even though
            // wasActive_ is false. Idempotent, so the redundant calls are harmless.
            SDL_PauseAudioStreamDevice(stream_);
            wasActive_ = false;
        }
        return;
    }

    if (stalled) {
        // Video is holding for an uncached frame: freeze audio too and keep our
        // position, so on resume the buffered audio still lines up with the frame.
        SDL_PauseAudioStreamDevice(stream_);
        return;
    }

    // Reconcile the channels with what should be audible. Channel identity is
    // (id, path): audio sliced from one continuous file under a single id plays
    // through clip boundaries seamlessly, while a set change or a position jump
    // invalidates the queued mix and re-cues everything.
    bool recue = syncChannels(items) || !wasActive_;
    if (recue) {
        SDL_ClearAudioStream(stream_);
        cueChannels(items);
    }
    wasActive_ = true;

    if (channels_.empty()) {
        // Gap / nothing audible: silence.
        SDL_PauseAudioStreamDevice(stream_);
        return;
    }

    // Top the queue up to ~kBufferMs of mixed audio.
    const int frameBytes = 2 * (int)sizeof(float);
    const int targetFrames = kMixRate * kBufferMs / 1000;
    int queuedFrames = SDL_GetAudioStreamQueued(stream_) / frameBytes;
    if (queuedFrames < targetFrames) {
        int need = targetFrames - queuedFrames;
        std::vector<float> buf;
        mixInto(buf, need);
        SDL_PutAudioStreamData(stream_, buf.data(), need * frameBytes);
    }

    SDL_ResumeAudioStreamDevice(stream_);
}

// Scrub feed: while the user drags the playhead, play one short grain of the mix
// each time the position moves. Unlike the playback path we do NOT keep the queue
// topped up — a single grain per move plays out and falls silent, so a held mouse
// stops buzzing and a fast drag chirps along with the cursor.
void AudioEngine::updateScrub(const std::vector<FeedItem>& items) {
    if (items.empty()) {
        SDL_PauseAudioStreamDevice(stream_);
        wasActive_ = false;
        return;
    }
    // All items advance in lockstep with the playhead, so any one of them works
    // as the move detector.
    double refSec = items.front().seconds;

    // Only cue a fresh grain when the playhead actually moved; otherwise let the
    // current grain finish so a stationary cursor goes quiet.
    if (std::fabs(refSec - scrubLast_) > kScrubMoveTol) {
        syncChannels(items);
        SDL_ClearAudioStream(stream_);
        cueChannels(items);
        const int grainFrames = kMixRate * kScrubGrainMs / 1000;
        std::vector<float> buf;
        mixInto(buf, grainFrames);
        SDL_PutAudioStreamData(stream_, buf.data(), grainFrames * 2 * (int)sizeof(float));
        SDL_ResumeAudioStreamDevice(stream_);
    }
    scrubLast_ = refSec;
    wasActive_ = false; // force a clean re-cue when normal playback resumes
}
