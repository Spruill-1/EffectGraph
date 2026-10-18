#include "pch_engine.h"
#include "ClockModel.h"
#include "GraphModel.h"

namespace ShaderLab::Graph
{
    namespace
    {
        float ReadFloat(const EffectNode& node, const wchar_t* key, float fallback)
        {
            auto it = node.properties.find(key);
            if (it == node.properties.end()) return fallback;
            if (auto* f = std::get_if<float>(&it->second)) return *f;
            if (auto* u = std::get_if<uint32_t>(&it->second)) return static_cast<float>(*u);
            if (auto* i = std::get_if<int32_t>(&it->second)) return static_cast<float>(*i);
            return fallback;
        }

        bool IsBoundTo(const EffectNode& node, uint32_t sourceId)
        {
            return AnyBindingSource(node, [sourceId](uint32_t id) { return id == sourceId; });
        }

        // StopTime follows the longest Duration field among the nodes bound to
        // the clock. Binding StopTime switches AutoDuration off for good.
        void ApplyAutoDuration(const GraphModel& graph, EffectNode& clock)
        {
            if (ReadFloat(clock, L"AutoDuration", 1.0f) <= 0.5f) return;
            if (clock.propertyBindings.count(L"StopTime"))
            {
                clock.properties[L"AutoDuration"] = 0.0f;
                return;
            }
            float longest = 0.0f;
            for (const auto& other : graph.Nodes())
            {
                if (other.id == clock.id || !IsBoundTo(other, clock.id)) continue;
                for (const auto& field : other.analysisOutput.fields)
                    if (field.name == L"Duration" && field.components[0] > longest)
                        longest = field.components[0];
            }
            if (longest > 0.0f)
                clock.properties[L"StopTime"] = longest;
        }
    }

    double ClockParams::Duration() const
    {
        const double duration = stopTime - startTime;
        return duration > 0.0 ? duration : 1.0;
    }

    ClockParams ReadClockParams(const EffectNode& clock)
    {
        ClockParams params;
        params.startTime  = ReadFloat(clock, L"StartTime", 0.0f);
        params.stopTime   = ReadFloat(clock, L"StopTime", 10.0f);
        params.speed      = ReadFloat(clock, L"Speed", 1.0f);
        params.loop       = ReadFloat(clock, L"Loop", 1.0f) > 0.5f;
        params.updateRate = ReadFloat(clock, L"UpdateRate", 0.0f);
        return params;
    }

    double WrapClockTime(const ClockParams& params, double clockTime)
    {
        const double duration = params.Duration();
        if (!params.loop)
            return std::clamp(clockTime, 0.0, duration);
        clockTime = std::fmod(clockTime, duration);
        if (clockTime < 0.0) clockTime += duration;
        return clockTime;
    }

    double ClockTimeAt(const ClockParams& params, double elapsedSeconds)
    {
        return WrapClockTime(params, elapsedSeconds * params.speed);
    }

    bool AdvanceClock(EffectNode& clock, double dt)
    {
        const ClockParams params = ReadClockParams(clock);
        clock.clockTime = WrapClockTime(params, clock.clockTime + dt * params.speed);
        if (!params.loop && clock.clockTime >= params.Duration())
            clock.isPlaying = false;

        if (params.updateRate <= 0.0)
            return true;
        const double step = 1.0 / params.updateRate;
        const long long bucket = static_cast<long long>(std::floor(clock.clockTime / step));
        const bool emit = bucket != clock.clockTickBucket;
        clock.clockTickBucket = bucket;
        return emit;
    }

    void SetClockPlaying(EffectNode& clock, bool play)
    {
        if (play && !clock.isPlaying)
        {
            const ClockParams params = ReadClockParams(clock);
            if (!params.loop && clock.clockTime >= params.Duration())
                clock.clockTime = 0.0;
        }
        clock.isPlaying = play;
    }

    void SeekClock(EffectNode& clock, double normalizedTime)
    {
        clock.clockTime = std::clamp(normalizedTime, 0.0, 1.0) * ReadClockParams(clock).Duration();
        clock.clockTickBucket = -1;
        clock.dirty = true;
    }

    void AdvanceClocks(GraphModel& graph, double dt)
    {
        for (auto& node : graph.MutableNodes())
        {
            if (!node.IsClock()) continue;
            ApplyAutoDuration(graph, node);
            if (node.isPlaying && AdvanceClock(node, dt))
                node.dirty = true;
        }
    }

    uint32_t SetClocksToTime(GraphModel& graph, double seconds)
    {
        uint32_t count = 0;
        for (auto& node : graph.MutableNodes())
        {
            if (!node.IsClock()) continue;
            node.clockTime = ClockTimeAt(ReadClockParams(node), seconds);
            node.clockTickBucket = -1;
            node.dirty = true;
            ++count;
        }
        return count;
    }

    double LongestClockDuration(const GraphModel& graph)
    {
        double longest = 0.0;
        for (const auto& node : graph.Nodes())
        {
            if (!node.IsClock()) continue;
            const ClockParams params = ReadClockParams(node);
            if (std::abs(params.speed) < 1e-6) continue;
            longest = (std::max)(longest, params.Duration() / std::abs(params.speed));
        }
        return longest;
    }
}
