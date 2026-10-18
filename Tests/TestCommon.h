#pragma once

// The test runner's shared vocabulary: the pass / fail / skip counters every
// test file reports into, the TEST() assertions, and the self-registering
// test-case registry main() runs from.

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
