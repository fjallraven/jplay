#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

class AudioSource;

// Owns the single SDL audio output stream and slaves it to the video-master
// playhead. update() is called once per frame from App::update() with the set of
// audio sources audible at the playhead (the program video's embedded audio plus
// every audio clip under the playhead). Each item keeps its own decoder channel;
// channels are decoded at a fixed device rate, summed, and queued to the one SDL
// stream, so everything mixes. Fed entirely from the main thread (SDL mixes on
// its own thread from the queued data); if the main loop drops below the frame
// rate the audio simply stutters or pauses — which is acceptable here.
class AudioEngine {
public:
    // One audible source at the playhead. `id` is the mixing-channel identity
    // (an audio clip's id, or -1 for the program video's embedded audio): a
    // channel persists while its id keeps resolving to the same path, so audio
    // sliced from one continuous file plays through clip boundaries seamlessly.
    // `seconds` is the time into `path` under the playhead.
    struct FeedItem {
        int id = -1;
        std::string path;
        double seconds = 0.0;
        // Volume automation, baked by the caller into a linear-gain envelope
        // sampled every 1/gainHz of FILE time from gainStartSec. Empty (or
        // gainHz == 0) means unity.
        //
        // Keyed on file time rather than on the playhead on purpose: the mixer
        // runs ~kBufferMs ahead of the picture, so a gain read at the playhead
        // would be heard that late. A channel already tracks the file time of the
        // next sample it will decode (Channel::readSec), so indexing the envelope
        // by that lands each gain value on exactly the samples it belongs to.
        double gainStartSec = 0.0;
        double gainHz = 0.0;
        std::vector<float> gain; // linear; the last value holds past the end
    };

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // active:  playback is running (false => pause, unless scrub is set).
    // scrub:   playback is stopped but the user is dragging the playhead and audio
    //          scrubbing is enabled: play a short grain at each item's `seconds`
    //          each time the playhead moves. Ignored while active.
    // stalled: video is holding for an uncached frame (pause audio to stay synced).
    // items:   everything audible at the playhead; empty => silence.
    void update(bool active, bool scrub, bool stalled, const std::vector<FeedItem>& items);

    // Master output gain, 0..1 (0 = silence). Rides on the output stream, so it
    // covers both the playback feed and the scrub grains and takes effect at once.
    void setVolume(float v);

    // Switch the output to a specific playback device (or
    // SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK). Rebuilds the stream on the new device
    // and drops all channels so the next update() re-cues to the playhead.
    void setDevice(SDL_AudioDeviceID dev);

private:
    // One decoder channel, keyed by FeedItem::id.
    struct Channel {
        std::shared_ptr<AudioSource> src; // null => open failed / no audio (silent)
        std::string path;    // file the channel was opened from (identity check)
        double readSec = 0.0; // file time the next read() will decode from
        double feedSec = 0.0; // item.seconds from the previous update (discontinuity check)
        // This update's volume envelope, copied from the item (see FeedItem).
        double gainStartSec = 0.0;
        double gainHz = 0.0;
        std::vector<float> gain;

        // Linear gain at file time `sec`, interpolated between envelope samples
        // and held flat outside it. Unity when the channel carries no envelope.
        float gainAt(double sec) const {
            if (gain.empty() || gainHz <= 0.0)
                return 1.0f;
            const double x = (sec - gainStartSec) * gainHz;
            if (x <= 0.0)
                return gain.front();
            const size_t i = (size_t)x;
            if (i + 1 >= gain.size())
                return gain.back();
            const float f = (float)(x - (double)i);
            return gain[i] + (gain[i + 1] - gain[i]) * f;
        }
    };

    void openStream(); // (re)open stream_ on deviceId_
    // Reconcile channels_ with `items`; true if the set changed or any item's
    // position jumped (=> the queued mix is stale and must be re-cued).
    bool syncChannels(const std::vector<FeedItem>& items);
    void cueChannels(const std::vector<FeedItem>& items); // seek every channel to its item
    // Sum `frames` frames from all channels into buf (interleaved stereo F32).
    void mixInto(std::vector<float>& buf, int frames);
    void closeChannels();
    void updateScrub(const std::vector<FeedItem>& items);

    static constexpr int kMixRate = 48000; // fixed mix/device rate

    SDL_AudioDeviceID deviceId_ = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    SDL_AudioStream* stream_ = nullptr; // null if no output device is available

    std::map<int, Channel> channels_;

    float volume_ = 1.0f;
    bool wasActive_ = false;
    double scrubLast_ = 0.0; // last scrub position we cued a grain at
};
