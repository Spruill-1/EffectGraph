#include "pch_engine.h"
#include "VideoSeekModel.h"

namespace ShaderLab::Effects
{
    namespace
    {
        // Weight of each new measurement in the decode and seek cost averages.
        constexpr double cCostSmoothing = 0.2;
        // Seek overhead, in seconds of video, until the costs have been measured.
        constexpr double cDefaultSeekThresholdSeconds = 0.25;
        // Keyframe-to-target distance a seek is assumed to decode through until
        // one has been measured: half a typical 2 s keyframe interval.
        constexpr double cDefaultPrerollSeconds = 1.0;
        constexpr size_t cMaxKnownKeyframes = 4096;

        void Smooth(std::atomic<double>& average, double sample)
        {
            const double previous = average.load();
            average = previous > 0.0 ? previous + cCostSmoothing * (sample - previous) : sample;
        }
    }

    void VideoSeekModel::Reset(double frameRate)
    {
        m_frameRate = frameRate;
        m_keyframeSpans.clear();
        m_minKeyframeInterval = 0.0;
        m_decodeSecondsPerFrame = 0.0;
        m_seekOverheadSeconds = 0.0;
        m_meanPrerollSeconds = 0.0;
    }

    void VideoSeekModel::RecordDecodeRate(double secondsPerFrame) { Smooth(m_decodeSecondsPerFrame, secondsPerFrame); }
    void VideoSeekModel::RecordSeekOverhead(double seconds)        { Smooth(m_seekOverheadSeconds, seconds); }
    void VideoSeekModel::RecordPreroll(double seconds)             { Smooth(m_meanPrerollSeconds, seconds); }

    double VideoSeekModel::SeekThresholdFrames() const
    {
        const double frameSeconds = m_decodeSecondsPerFrame.load();
        const double overheadSeconds = m_seekOverheadSeconds.load();
        const double frames = (frameSeconds > 0.0 && overheadSeconds > 0.0)
            ? overheadSeconds / frameSeconds
            : cDefaultSeekThresholdSeconds * m_frameRate;
        return (std::max)(static_cast<double>(cVideoDecodeAheadFrames), frames);
    }

    void VideoSeekModel::RecordKeyframe(double keyframe, double target)
    {
        if (keyframe < 0.0 || target < keyframe - cVideoTimeTolerance) return;
        m_minKeyframeInterval = (std::max)(m_minKeyframeInterval, target - keyframe);
        auto it = m_keyframeSpans.find(keyframe);
        if (it != m_keyframeSpans.end())
            it->second = (std::max)(it->second, target);
        else if (m_keyframeSpans.size() < cMaxKnownKeyframes)
            m_keyframeSpans.emplace(keyframe, target);
    }

    bool VideoSeekModel::ForwardSeekPays(double target, double decodeHead, double frameDuration, bool seekFrameDecoded) const
    {
        if (frameDuration <= 0.0) return false;
        const double threshold = SeekThresholdFrames() * frameDuration;
        if (target - decodeHead <= threshold) return false;

        // The latest known keyframe at or before the target. A seek lands on
        // it exactly when the target is inside its known span, and at or
        // after it otherwise.
        double knownKeyframe = -1.0;
        double knownSpanEnd = -1.0;
        bool landingKnown = false;
        auto it = m_keyframeSpans.upper_bound(target + cVideoTimeTolerance);
        if (it != m_keyframeSpans.begin())
        {
            --it;
            knownKeyframe = it->first;
            knownSpanEnd = it->second;
            landingKnown = target <= knownSpanEnd + cVideoTimeTolerance;
        }

        // While a seek is still decoding up to its frame, only a keyframe known
        // to be further ahead justifies another; an estimate would restart it
        // on every tick.
        if (!seekFrameDecoded || landingKnown)
            return knownKeyframe - decodeHead > threshold;

        // Otherwise the seek lands on the known keyframe if the next one
        // cannot be that close yet, or the usual preroll before the target if
        // that falls past the known span, which holds no other keyframe.
        if (target < knownKeyframe + m_minKeyframeInterval)
            return knownKeyframe - decodeHead > threshold;
        const double meanPreroll = m_meanPrerollSeconds.load();
        const double expected = target - (meanPreroll > 0.0 ? meanPreroll : cDefaultPrerollSeconds);
        const double landing = expected > knownSpanEnd ? expected : knownKeyframe;
        return landing - decodeHead > threshold;
    }
}
