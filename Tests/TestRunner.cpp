#include "pch_engine.h"
#include "TestSupport.h"
// Every built-in shader effect, registered at startup.
#include "EffectGraphBuiltIns.h"
#include "Rendering/DeviceStack.h"
#include "Engine/EngineBootstrap.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

// Test runner: runs every case registered with SL_TEST_CASE or
// SL_REGISTER_TEST (Tests/TestCommon.h) on one shared device, after any
// SL_TEST_SETUP start-up. EffectGraphTests and ShaderLabTests both build it.
//
//   <runner>.exe [--adapter warp|default] [--tag <tag>[,<tag>...]]
//                [--filter <text>] [--list] [--require-fixtures]
//
// Exit code = number of failures (0 = all passed).

namespace
{
    using namespace ShaderLab::Tests;

    struct TagName
    {
        const char* name;
        Tag tag;
    };

    constexpr TagName cTagNames[] = {
        { "fast", Tag::Fast }, { "gpu", Tag::Gpu }, { "video", Tag::Video }, { "timing", Tag::Timing },
        { "mcp", Tag::Mcp }, { "math", Tag::Math }, { "manual", Tag::Manual }, { "last", Tag::Last },
    };

    struct RunnerOptions
    {
        bool warp = false;
        bool list = false;
        bool requireFixtures = false;
        Tag tags = Tag::None;     // any of these; None selects every case
        std::string filter;       // substring of the case name
        std::string error;
    };

    // The executable's name, for messages.
    std::string RunnerName()
    {
        wchar_t path[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).stem().string();
    }

    void PrintUsage()
    {
        std::printf("Usage: %s.exe [--adapter warp|default] [--tag <tag>[,<tag>...]]\n"
                    "         [--filter <text>] [--list] [--require-fixtures]\n"
                    "Tags:", RunnerName().c_str());
        for (const auto& entry : cTagNames) std::printf(" %s", entry.name);
        std::printf("\nA manual case runs only when --filter matches it.\n");
    }

    bool ParseTags(const std::string& list, Tag& tags)
    {
        size_t start = 0;
        while (start <= list.size())
        {
            const size_t comma = list.find(',', start);
            const std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            const auto match = std::find_if(std::begin(cTagNames), std::end(cTagNames),
                                            [&](const TagName& entry) { return name == entry.name; });
            if (match == std::end(cTagNames)) return false;
            tags = tags | match->tag;
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return true;
    }

    RunnerOptions ParseArgs(int argc, char* argv[])
    {
        RunnerOptions options;
        for (int i = 1; i < argc && options.error.empty(); ++i)
        {
            const std::string arg = argv[i];
            const bool hasValue = i + 1 < argc;
            if (arg == "--adapter" && hasValue)
            {
                const std::string value = argv[++i];
                if (value != "warp" && value != "default") options.error = "unknown adapter '" + value + "'";
                options.warp = value == "warp";
            }
            else if (arg == "--tag" && hasValue)
            {
                if (!ParseTags(argv[++i], options.tags)) options.error = std::string("unknown tag in '") + argv[i] + "'";
            }
            else if (arg == "--filter" && hasValue)
            {
                options.filter = argv[++i];
            }
            else if (arg == "--list")
            {
                options.list = true;
            }
            else if (arg == "--require-fixtures")
            {
                options.requireFixtures = true;
            }
            else
            {
                options.error = "unknown or incomplete argument '" + arg + "'";
            }
        }
        return options;
    }

    bool Selected(const TestCase& testCase, const RunnerOptions& options)
    {
        const bool nameMatches = !options.filter.empty() &&
                                 std::string(testCase.name).find(options.filter) != std::string::npos;
        if (HasTag(testCase.tags, Tag::Manual)) return nameMatches;
        if (!options.filter.empty() && !nameMatches) return false;
        return options.tags == Tag::None || HasTag(testCase.tags, options.tags);
    }

    // Cases tagged Last run after the rest; otherwise by source file, then
    // by line, so the order does not depend on link order.
    std::vector<TestCase> SortedCases()
    {
        std::vector<TestCase> cases = TestRegistry();
        std::stable_sort(cases.begin(), cases.end(), [](const TestCase& a, const TestCase& b) {
            const bool aLast = HasTag(a.tags, Tag::Last);
            const bool bLast = HasTag(b.tags, Tag::Last);
            if (aLast != bLast) return bLast;
            const int byFile = _stricmp(a.file, b.file);
            if (byFile != 0) return byFile < 0;
            return a.line < b.line;
        });
        return cases;
    }

    std::string TagList(Tag tags)
    {
        std::string out;
        for (const auto& entry : cTagNames)
        {
            if (!HasTag(tags, entry.tag)) continue;
            if (!out.empty()) out += ',';
            out += entry.name;
        }
        return out;
    }

    // Each case gets a fresh context and the process-default evaluator
    // options as they were before it. A throw fails the case and the run
    // goes on.
    void RunCase(const TestCase& testCase)
    {
        g_currentCase = testCase.name;
        ScopedPerformanceSettings settings;
        BeginCaseContext();
        std::string thrown;
        try
        {
            testCase.run();
        }
        catch (const winrt::hresult_error& error)
        {
            char code[16]{};
            std::snprintf(code, sizeof(code), "0x%08X", static_cast<uint32_t>(error.code()));
            thrown = std::string("hresult_error ") + code + " " + winrt::to_string(error.message());
        }
        catch (const std::exception& error)
        {
            thrown = error.what();
        }
        catch (...)
        {
            thrown = "unknown exception";
        }
        if (!thrown.empty())
        {
            const std::string name = std::string(testCase.name) + " threw: " + thrown;
            ReportResult(name.c_str(), false, testCase.file, testCase.line);
        }
        EndCaseContext();
        g_currentCase = "";
    }

    void ConfigureTimingAssertions(bool warp)
    {
        char timing[8]{};
        char ci[8]{};
        const DWORD timingLength = ::GetEnvironmentVariableA("SHADERLAB_TIMING_TESTS", timing, sizeof(timing));
        if (timingLength > 0 && timingLength < sizeof(timing))
            g_timingAssertions = std::string(timing) != "0";
        else
            g_timingAssertions = !warp && ::GetEnvironmentVariableA("CI", ci, sizeof(ci)) == 0;
        std::printf("Timing assertions: %s\n", g_timingAssertions
            ? "on" : "off (WARP or CI; SHADERLAB_TIMING_TESTS=1 forces them)");
    }

    // The D3D11 device every case's context is created on. Any failure here
    // is fatal: it would surface later as a null dereference inside some test.
    bool CreateTestDevice(bool warp)
    {
        ShaderLab::Rendering::DeviceStackOptions deviceOptions;
        deviceOptions.choice = warp ? ShaderLab::Rendering::AdapterChoice::Warp : ShaderLab::Rendering::AdapterChoice::Hardware;
        ShaderLab::Rendering::DeviceStack device;
        std::wstring failedStep;
        HRESULT hr = ShaderLab::Rendering::CreateDeviceStack(deviceOptions, device, &failedStep);
        if (FAILED(hr))
        {
            std::printf("FATAL: %ls failed 0x%08X\n", failedStep.c_str(), static_cast<uint32_t>(hr));
            return false;
        }
        g_d3dDevice = device.d3dDevice;
        g_d3dContext = device.d3dContext;
        g_useWarp = warp;

        // A context must be creatable on it, or every case would fail alike.
        winrt::com_ptr<IEffectGraphContext> probe;
        hr = EffectGraphCreateContext(g_d3dDevice.get(), nullptr, IID_PPV_ARGS(probe.put()));
        if (FAILED(hr))
        {
            std::printf("FATAL: EffectGraphCreateContext failed 0x%08X\n", static_cast<uint32_t>(hr));
            return false;
        }
        return true;
    }

    // Releases the test device before main returns. Left to the globals'
    // destructors, the last release ran inside exit() holding the CRT exit
    // lock, and the driver's FreeLibrary then waited on the loader lock that a
    // Media Foundation thread held while unloading a video decoder, which was
    // itself waiting on the exit lock.
    void ReleaseTestDevice()
    {
        g_dc = nullptr;
        g_d2dFactory = nullptr;
        g_d3dContext = nullptr;
        g_d3dDevice = nullptr;
    }
}

int main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const RunnerOptions options = ParseArgs(argc, argv);
    if (!options.error.empty())
    {
        std::printf("%s: %s\n", RunnerName().c_str(), options.error.c_str());
        PrintUsage();
        return 1;
    }

    std::vector<TestCase> cases;
    for (const auto& testCase : SortedCases())
        if (Selected(testCase, options)) cases.push_back(testCase);

    if (options.list)
    {
        for (const auto& testCase : cases)
            std::printf("%-48s %-24s %s:%d\n", testCase.name, TagList(testCase.tags).c_str(),
                        TestSourceName(testCase.file), testCase.line);
        std::printf("%zu cases\n", cases.size());
        return 0;
    }
    if (cases.empty())
    {
        std::printf("%s: no test case matches\n", RunnerName().c_str());
        return 1;
    }

    winrt::init_apartment();
    MFStartup(MF_VERSION);
    // Joins the engine's worker threads on every return path.
    struct EngineServicesScope
    {
        ~EngineServicesScope() { ShaderLab::Engine::ShutdownEngineServices(); }
    } engineServices;
    for (const auto setup : TestSetups())
        setup();

    std::printf("%s\n", RunnerName().c_str());
    std::printf("=====================\n");
    if (!CreateTestDevice(options.warp))
        return 1;
    std::printf("Device: %s\n", options.warp ? "WARP" : "Hardware");
    ConfigureTimingAssertions(options.warp);
    g_requireFixtures = options.requireFixtures;
    std::printf("Cases: %zu of %zu\n", cases.size(), TestRegistry().size());

    for (const auto& testCase : cases)
        RunCase(testCase);
    ShutdownBench();
    ReleaseTestDevice();

    std::printf("\n========================================\n");
    if (g_skipped > 0)
        std::printf("SKIPPED %d (missing fixtures; --require-fixtures fails them)\n", g_skipped);
    if (g_failed == 0)
        std::printf("ALL %d TESTS PASSED\n", g_passed);
    else
        std::printf("%d PASSED, %d FAILED out of %d\n", g_passed, g_failed, g_passed + g_failed);
    std::printf("========================================\n");

    MFShutdown();
    winrt::uninit_apartment();
    return g_failed;
}
