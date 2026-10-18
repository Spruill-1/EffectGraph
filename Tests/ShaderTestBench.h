#pragma once

// ShaderTestBench
// ===============
//
// A tiny D3D11 compute test harness for unit-testing the HLSL color-math
// helpers in Effects/ColorMath.cpp (shaderlab_colormath.hlsli). Each test author
// writes a few lines of HLSL that compute a value and write it into a
// `RWStructuredBuffer<float4> Result` slot; the bench compiles + dispatches
// + reads back, and the caller asserts on the result with TEST()
// (Tests/TestCommon.h).
//
// Why test the HLSL directly (and not a C++ port)? The HLSL is what ships.
// Maintaining a parallel C++ implementation of every helper would create
// silent-drift opportunities forever. WARP runs the same shaders in CI.
//
// Cost: a single D3DCompile per test (~50-100 ms on WARP). With 60-80 tests
// this is single-digit seconds — fine.
//
// Test body conventions:
//   * The `Result` slot count is passed via `Run(body, numFloat4Slots)`
//     and must match the number of `Result[0..N-1] = ...` writes in the
//     body.
//   * The full color-math header (PQ_EOTF, ScRGBToICtCp, ReinhardCompressI,
//     etc.) is included in every test. Author can call any helper that
//     `Effects::GetColorMathHLSL()` exposes.
//   * The kernel runs as a single 1×1×1 thread group. This is *not* an
//     image-pipeline test; for those, use the regular effect graph tests.

#include "pch_engine.h"
#include <string>
#include <vector>

namespace ShaderLab::Tests
{
    struct TestVec4
    {
        float x{}, y{}, z{}, w{};
    };

    class ShaderTestBench
    {
    public:
        // Initialize with a live D3D11 device. The bench creates and reuses
        // a single result UAV + staging buffer, growing them as needed.
        void Initialize(ID3D11Device* device, ID3D11DeviceContext* ctx);

        // Release all resources. Safe to call when uninitialized.
        void Shutdown();

        bool IsInitialized() const { return m_device != nullptr; }

        // Compile + dispatch a one-thread compute kernel. The `body` string
        // is wrapped in a `[numthreads(1,1,1)] void main()` shell with the
        // full color-math HLSL included. Returns the contents of the
        // Result buffer on success.
        //
        // `preamble` (optional) is inserted at file scope between the
        // color-math library and the kernel shell. Use it to define
        // helper functions that aren't part of GetColorMathHLSL() (e.g.,
        // the Delta E helpers that currently live only inside the Delta E
        // Comparator's pixel shader).
        //
        // On compile failure: prints the D3DCompile error blob to stdout
        // and returns an empty vector. The caller's TEST() assertion will
        // then fail naturally (since the result vector is empty).
        std::vector<TestVec4> Run(const std::string& body,
            uint32_t numFloat4Slots = 1,
            const std::string& preamble = {});

    private:
        void EnsureBuffers(uint32_t numFloat4Slots);

        ID3D11Device* m_device{};
        ID3D11DeviceContext* m_ctx{};

        // Result UAV (default heap, structured stride 16).
        winrt::com_ptr<ID3D11Buffer> m_resultBuf;
        winrt::com_ptr<ID3D11UnorderedAccessView> m_resultUav;

        // CPU-readable mirror.
        winrt::com_ptr<ID3D11Buffer> m_stagingBuf;

        uint32_t m_capacitySlots{ 0 };
    };

    // Common float comparator. Used by every test in Tests/Math/*.cpp.
    inline bool Near(float actual, float expected, float tol)
    {
        // NaN never compares equal — explicit check so a NaN result fails
        // the assertion instead of silently passing via signed-comparison
        // surprises.
        if (std::isnan(actual) || std::isnan(expected)) return false;
        return std::abs(actual - expected) <= tol;
    }
}
