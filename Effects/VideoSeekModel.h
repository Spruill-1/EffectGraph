#pragma once

#include "pch_engine.h"

#include <map>

namespace ShaderLab::Effects
{
    // A frame starting within this of a time counts as starting at it.
    inline constexpr double cVideoTimeTolerance = 1e-4;
    // Frames the decode thread keeps queued ahead of the shown one. Each
    // queued zero-copy frame pins a decoder surface; the H.264 decoder lets a
    // caller hold 9 before ReadSample blocks, and
    // MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT does not raise that, so the queue plus
    // the frame being converted stays well under it.
    inline constexpr size_t cVideoDecodeAheadFrames = 4;

    // When moving a video forward is cheaper as a seek than as decoding on:
    // measured decode and seek costs, and the keyframes seeks have landed on.
    // No Media Foundation; the provider feeds it measurements.
    //
    // Threading: the cost averages are atomics, read from any thread. The
    // keyframe map is not locked here: the provider calls the keyframe
    // members with its state mutex held.
    class VideoSeekModel
    {
    public:
        // Forget everything; `frameRate` sets the threshold until costs are measured.
        void Reset(double frameRate);

        // Measured costs, 0 until measured: seconds per frame over a run of
        // back-to-back reads, seconds from SetCurrentPosition to the first
        // sample (the keyframe), and the mean distance from a seek's keyframe
        // to its target, which a seek has to decode through.
        double DecodeSecondsPerFrame() const { return m_decodeSecondsPerFrame.load(); }
        double SeekOverheadSeconds() const { return m_seekOverheadSeconds.load(); }
        double MeanPrerollSeconds() const { return m_meanPrerollSeconds.load(); }

        void RecordDecodeRate(double secondsPerFrame);
        void RecordSeekOverhead(double seconds);
        void RecordPreroll(double seconds);

        // Frames that decode in the time a seek's overhead takes: a seek pays
        // only if its keyframe is at least this far ahead of the decode head.
        double SeekThresholdFrames() const;

        // A seek to `target` landed on `keyframe`. State mutex held.
        void RecordKeyframe(double keyframe, double target);
        // Keyframes learned so far. State mutex held.
        size_t KnownKeyframeCount() const { return m_keyframeSpans.size(); }

        // Whether a seek to `target` lands far enough ahead of `decodeHead`
        // to beat decoding forward. `seekFrameDecoded` is false while a seek
        // is still decoding up to its frame. State mutex held.
        bool ForwardSeekPays(double target, double decodeHead, double frameDuration, bool seekFrameDecoded) const;

    private:
        double m_frameRate{ 0.0 };
        // Keyframe -> the furthest seek target known to land on it, so no
        // other keyframe lies between the two.
        std::map<double, double> m_keyframeSpans;
        // The longest known span: keyframes are at least this far apart.
        double m_minKeyframeInterval{ 0.0 };
        std::atomic<double> m_decodeSecondsPerFrame{ 0.0 };
        std::atomic<double> m_seekOverheadSeconds{ 0.0 };
        std::atomic<double> m_meanPrerollSeconds{ 0.0 };
    };
}
