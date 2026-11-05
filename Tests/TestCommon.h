#pragma once

// The test runner's shared vocabulary: the pass / fail / skip counters every
// test file reports into, the TEST() assertions, and the self-registering
// test-case registry main() runs from.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "../Effects/Performance.h"

namespace ShaderLab::Tests
{
    inline int g_passed = 0;
    inline int g_failed = 0;
    inline int g_skipped = 0;

    // The registered case running now; failures name it.
    inline const char* g_currentCase = "";

    // --require-fixtures: a skipped test counts as a failure (CI sets it).
    inline bool g_requireFixtures = false;

    // --plugin: the effect plugin main() loaded before the first case
    // (EffectGraphLoadPlugin), for cases that need effects kept outside this
    // repository. Empty when none was given.
    inline std::wstring g_pluginPath;

    // --recording: a folder of desktop-duplication recordings (the PRIV
    // recorder's format, docs/development/dda-recording.md) the replay cases
    // play. Empty when none was given.
    inline std::wstring g_recordingPath;

    // Cases that need a runner option they were not given. Not a missing
    // fixture, so --require-fixtures does not fail them.
    inline int g_notRun = 0;

    // The path below Tests\ (or the bare file name), for failure lines.
    inline const char* TestSourceName(const char* file)
    {
        const char* tests = std::strstr(file, "Tests\\");
        if (!tests) tests = std::strstr(file, "Tests/");
        if (tests) return tests;
        const char* slash = std::strrchr(file, '\\');
        return slash ? slash + 1 : file;
    }

    inline void ReportResult(const char* name, bool result, const char* file, int line)
    {
        if (result)
        {
            std::printf("  [PASS] %s\n", name);
            ++g_passed;
        }
        else
        {
            std::printf("  [FAIL] %s  (%s, %s:%d)\n", name, g_currentCase, TestSourceName(file), line);
            ++g_failed;
        }
        std::fflush(stdout);
    }

    // TEST_EQ: a failure also prints both values.
    template <typename Actual, typename Expected>
    void ReportEqual(const char* name, const Actual& actual, const Expected& expected, const char* file, int line)
    {
        const bool equal = actual == expected;
        ReportResult(name, equal, file, line);
        if (equal) return;
        if constexpr (std::is_arithmetic_v<Actual> && std::is_arithmetic_v<Expected>)
            std::printf("         actual %s, expected %s\n",
                        std::to_string(actual).c_str(), std::to_string(expected).c_str());
    }

    // Real-time assertions (rates, deadlines, playback tracking) need an
    // otherwise idle machine. Off on WARP and under CI (shared runners);
    // SHADERLAB_TIMING_TESTS=1 or 0 overrides. Set by main().
    inline bool g_timingAssertions = true;

    // TEST() when timing assertions are on; otherwise the result is printed
    // as [info] and not counted.
    inline void ReportTimingResult(const char* name, bool result, const char* file, int line)
    {
        if (g_timingAssertions)
        {
            ReportResult(name, result, file, line);
            return;
        }
        std::printf("  [info] %s: %s (timing, not asserted)\n", name, result ? "met" : "missed");
        std::fflush(stdout);
    }

    // Two timed arms sampled in pairs: a[i] and b[i] back to back, with the
    // order alternating, so clock-state drift lands in both. Holds the paired
    // differences d[i] = a[i] - (ratio * b[i] + margin): their median and a
    // distribution-free 99% confidence interval for it (sign-test order
    // statistics; the whole sample range below 9 pairs).
    struct PairedTiming
    {
        size_t pairs{ 0 };
        double median{ 0.0 };
        double lower{ 0.0 };
        double upper{ 0.0 };
        // a is dearer than ratio * b + margin with 99% confidence.
        bool ConfidentlyAbove() const { return pairs > 0 && lower > 0.0; }
        // a is cheaper than ratio * b + margin with 99% confidence.
        bool ConfidentlyBelow() const { return pairs > 0 && upper < 0.0; }
    };

    inline PairedTiming ComparePaired(const std::vector<double>& a, const std::vector<double>& b,
                                      double ratio = 1.0, double margin = 0.0)
    {
        PairedTiming result;
        const size_t n = (std::min)(a.size(), b.size());
        if (n == 0) return result;
        std::vector<double> d(n);
        for (size_t i = 0; i < n; ++i) d[i] = a[i] - (ratio * b[i] + margin);
        std::sort(d.begin(), d.end());
        result.pairs = n;
        result.median = (n % 2) ? d[n / 2] : 0.5 * (d[n / 2 - 1] + d[n / 2]);
        // The largest k with P(Binomial(n, 1/2) < k) <= 0.005: [d(k), d(n-k+1)]
        // then covers the median with at least 99% probability.
        double cumulative = 0.0, term = std::pow(0.5, static_cast<double>(n));
        size_t k = 0;
        for (size_t j = 0; j < n; ++j)
        {
            if (cumulative + term > 0.005) break;
            cumulative += term;
            k = j + 1;
            term = term * static_cast<double>(n - j) / static_cast<double>(j + 1);
        }
        result.lower = d[k == 0 ? 0 : k - 1];
        result.upper = d[k == 0 ? n - 1 : n - k];
        return result;
    }

    inline void PrintPairedTiming(const char* name, const PairedTiming& t, const char* unit = "ms")
    {
        std::printf("  [info] %s: %zu interleaved pairs, paired difference median %+.4f %s, 99%% CI [%+.4f, %+.4f]\n",
                    name, t.pairs, t.median, unit, t.lower, t.upper);
        std::fflush(stdout);
    }

    // A test (or part of one) that cannot run here, typically for a missing
    // fixture. Counted, and a failure under --require-fixtures.
    inline void ReportSkip(const std::string& reason)
    {
        ++g_skipped;
        if (g_requireFixtures)
        {
            std::printf("  [FAIL] %s  (%s, skipped under --require-fixtures)\n", reason.c_str(), g_currentCase);
            ++g_failed;
        }
        else
        {
            std::printf("  [SKIP] %s\n", reason.c_str());
        }
        std::fflush(stdout);
    }

    // A case that cannot run without a runner option (--plugin): counted
    // apart from fixture skips.
    inline void ReportNotRun(const std::string& reason)
    {
        ++g_notRun;
        std::printf("  [SKIP] %s\n", reason.c_str());
        std::fflush(stdout);
    }

    // Restores every process-default evaluator option (Performance::) on
    // scope exit, whatever path the test leaves by, so a setting a test
    // changes cannot leak into the tests after it.
    class ScopedPerformanceSettings
    {
    public:
        ScopedPerformanceSettings() : m_saved(::ShaderLab::Performance::DefaultEvaluatorOptions()) {}
        ~ScopedPerformanceSettings() { ::ShaderLab::Performance::SetDefaultEvaluatorOptions(m_saved); }
        ScopedPerformanceSettings(const ScopedPerformanceSettings&) = delete;
        ScopedPerformanceSettings& operator=(const ScopedPerformanceSettings&) = delete;

    private:
        ::ShaderLab::Rendering::EvaluatorOptions m_saved;
    };

    // ---- Test-case registry ------------------------------------------------

    // What a case needs and how it behaves; `--tag` selects on these.
    enum class Tag : uint32_t
    {
        None   = 0,
        Fast   = 1u << 0,   // no device work, well under a second
        Gpu    = 1u << 1,   // renders or dispatches on the shared device
        Video  = 1u << 2,   // decodes the video fixtures
        Timing = 1u << 3,   // real-time assertions (TIMING_TEST)
        Mcp    = 1u << 4,   // MCP router, routes, transport
        Math   = 1u << 5,   // HLSL math on the ShaderTestBench
        Manual = 1u << 6,   // runs only when --filter names it
        Last   = 1u << 7,   // changes process-wide state; runs after the rest
    };

    constexpr Tag operator|(Tag a, Tag b) { return static_cast<Tag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }
    constexpr bool HasTag(Tag tags, Tag tag) { return (static_cast<uint32_t>(tags) & static_cast<uint32_t>(tag)) != 0; }

    struct TestCase
    {
        const char* name;
        Tag tags;
        void (*run)();
        const char* file;
        int line;
    };

    // Every registered case, in registration order; main() sorts it.
    inline std::vector<TestCase>& TestRegistry()
    {
        static std::vector<TestCase> registry;
        return registry;
    }

    struct TestRegistration
    {
        TestRegistration(const char* name, Tag tags, void (*run)(), const char* file, int line)
        {
            TestRegistry().push_back({ name, tags, run, file, line });
        }
    };
}

namespace ShaderLab::Tests
{
    // Start-up a runner's host adds (ShaderLab registers its support library):
    // each runs once, after COM, Media Foundation and the engine start, before
    // the device is created.
    inline std::vector<void (*)()>& TestSetups()
    {
        static std::vector<void (*)()> setups;
        return setups;
    }

    struct TestSetupRegistration
    {
        explicit TestSetupRegistration(void (*setup)()) { TestSetups().push_back(setup); }
    };
}

#define SL_TEST_CONCAT_INNER(a, b) a##b
#define SL_TEST_CONCAT(a, b) SL_TEST_CONCAT_INNER(a, b)

// Defines and registers a test case:  SL_TEST_CASE(TestThing, Tag::Gpu) { ... }
#define SL_TEST_CASE(name, tags)                                                        \
    static void name();                                                                 \
    static const ::ShaderLab::Tests::TestRegistration SL_TEST_CONCAT(s_register_, name){ \
        #name, (tags), &name, __FILE__, __LINE__ };                                     \
    static void name()

// Registers an existing function or captureless lambda as a test case.
#define SL_REGISTER_TEST(name, tags, run)                                               \
    static const ::ShaderLab::Tests::TestRegistration SL_TEST_CONCAT(s_register_, name){ \
        #name, (tags), (run), __FILE__, __LINE__ }

// Registers start-up for the runner:  SL_TEST_SETUP(RegisterMine) { ... }
#define SL_TEST_SETUP(name)                                                              \
    static void name();                                                                  \
    static const ::ShaderLab::Tests::TestSetupRegistration SL_TEST_CONCAT(s_setup_, name){ &name }; \
    static void name()

#define TEST(name, ...) ::ShaderLab::Tests::ReportResult((name), static_cast<bool>(__VA_ARGS__), __FILE__, __LINE__)
#define TEST_EQ(name, actual, expected) ::ShaderLab::Tests::ReportEqual((name), (actual), (expected), __FILE__, __LINE__)
#define TIMING_TEST(name, ...) ::ShaderLab::Tests::ReportTimingResult((name), static_cast<bool>(__VA_ARGS__), __FILE__, __LINE__)
