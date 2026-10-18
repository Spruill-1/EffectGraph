#pragma once

#include "pch_engine.h"
#include "EffectNode.h"

// Clock-node time semantics, shared by every host: the live tick (GUI render
// worker), fixed-timeline stepping (video export, headless --time) and tests.
namespace ShaderLab::Graph
{
    class GraphModel;

    // A Clock node's timing properties.
    struct ClockParams
    {
        double startTime{ 0.0 };
        double stopTime{ 10.0 };
        double speed{ 1.0 };
        bool   loop{ true };
        // Ticks per second that dirty consumers; 0 dirties them every tick.
        double updateRate{ 0.0 };

        // Seconds of clock time in one pass: StopTime - StartTime, or 1 when
        // that is not positive.
        double Duration() const;
    };

    ClockParams ReadClockParams(const EffectNode& clock);

    // A clock time wrapped into [0, Duration) when looping, else clamped to
    // [0, Duration].
    double WrapClockTime(const ClockParams& params, double clockTime);

    // The clock time after `elapsedSeconds` of playback from 0 at the clock's
    // Speed.
    double ClockTimeAt(const ClockParams& params, double elapsedSeconds);

    // Advances one playing clock by dt. A non-looping clock stops at its end.
    // Returns true when the tick should dirty the clock's consumers: every
    // tick, or only when clockTime enters a new 1/UpdateRate bucket.
    // GraphEvaluator quantises Time with the same bucket rule.
    bool AdvanceClock(EffectNode& clock, double dt);

    // Starts or pauses a clock. Playing a non-looping clock that has reached
    // its end rewinds it first, as a transport control does; otherwise the
    // next tick would stop it again at once.
    void SetClockPlaying(EffectNode& clock, bool play);

    // Moves a clock to `normalizedTime` (0 = StartTime, 1 = StopTime, clamped)
    // and marks it dirty.
    void SeekClock(EffectNode& clock, double normalizedTime);

    // The live per-tick update of every Clock node: AutoDuration (StopTime
    // follows the longest Duration field among nodes bound to the clock,
    // unless StopTime is itself bound), then AdvanceClock for playing clocks,
    // marking a clock dirty when AdvanceClock says so.
    void AdvanceClocks(GraphModel& graph, double dt);

    // Sets every Clock node to the time it shows after `seconds` of playback
    // from 0, whether it is playing or not, and marks it dirty. Returns the
    // number of clocks set.
    uint32_t SetClocksToTime(GraphModel& graph, double seconds);

    // The span one pass of the longest Clock takes at its Speed, in seconds;
    // 0 when the graph has no Clock with a non-zero speed.
    double LongestClockDuration(const GraphModel& graph);
}
