#include "pch_engine.h"
#include "ShaderTestBench.h"
#include "Effects/ShaderCompiler.h"

#include <cstdio>
#include <cstring>

namespace ShaderLab::Tests
{
    namespace
    {
        // The fixed shell wrapped around every test body. The user body is
        // inserted at $BODY$. The color-math include goes ahead of this
        // shell so the body has access to every helper.
        constexpr const char* kKernelShell = R"HLSL(

// ---- Test bench shell -----------------------------------------------------
// Disable the optimizer's "value cannot be NaN/Inf" diagnostic (X3577),
// the "floating-point division by zero" warning (X4008), and the
// corresponding warnings-as-errors wrap (X3129). Test bodies routinely
// call isnan/isinf/isfinite on values the static analyzer can prove are
// finite at compile time but can become non-finite at runtime in WARP /
// hardware (e.g., PQ_EOTF past its V~=1.16 pole, or pow() of a negative).
// They also pass literal edge-case inputs (zero, near-zero) that would
// trip a divide-by-zero static check inside guarded branches even when
// the runtime ternary correctly avoids the divide. IEEE_STRICTNESS is
// set on the compile flags so the runtime calls do execute.
#pragma warning(disable: 3577)
#pragma warning(disable: 4008)
#pragma warning(disable: 3129)

RWStructuredBuffer<float4> Result : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 dt : SV_DispatchThreadID)
{
$BODY$
}
)HLSL";

        std::string AssembleSource(const std::string& body, const std::string& preamble)
        {
            std::string src;
            src.reserve(preamble.size() + std::strlen(kKernelShell) + body.size() + 64);
            src.append("#include \"shaderlab_colormath.hlsli\"\n");
            src.append(preamble);
            std::string shell = kKernelShell;
            const std::string marker = "$BODY$";
            auto pos = shell.find(marker);
            shell.replace(pos, marker.size(), body);
            src.append(shell);
            return src;
        }
    }

    void ShaderTestBench::Initialize(ID3D11Device* device, ID3D11DeviceContext* ctx)
    {
        m_device = device;
        m_ctx = ctx;
    }

    void ShaderTestBench::Shutdown()
    {
        m_resultUav = nullptr;
        m_resultBuf = nullptr;
        m_stagingBuf = nullptr;
        m_capacitySlots = 0;
        m_device = nullptr;
        m_ctx = nullptr;
    }

    void ShaderTestBench::EnsureBuffers(uint32_t numFloat4Slots)
    {
        if (numFloat4Slots == 0) numFloat4Slots = 1;
        if (numFloat4Slots <= m_capacitySlots && m_resultBuf && m_stagingBuf) return;

        m_resultUav = nullptr;
        m_resultBuf = nullptr;
        m_stagingBuf = nullptr;

        const UINT byteStride = sizeof(float) * 4;
        const UINT byteSize = byteStride * numFloat4Slots;

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = byteSize;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = byteStride;
        winrt::check_hresult(m_device->CreateBuffer(&bd, nullptr, m_resultBuf.put()));

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0;
        ud.Buffer.NumElements = numFloat4Slots;
        winrt::check_hresult(m_device->CreateUnorderedAccessView(
            m_resultBuf.get(), &ud, m_resultUav.put()));

        D3D11_BUFFER_DESC sd = bd;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        winrt::check_hresult(m_device->CreateBuffer(&sd, nullptr, m_stagingBuf.put()));

        m_capacitySlots = numFloat4Slots;
    }

    std::vector<TestVec4> ShaderTestBench::Run(const std::string& body,
        uint32_t numFloat4Slots, const std::string& preamble)
    {
        if (!m_device || !m_ctx)
        {
            std::printf("    [bench] ERROR: not initialized\n");
            return {};
        }
        if (numFloat4Slots == 0) numFloat4Slots = 1;

        EnsureBuffers(numFloat4Slots);

        std::string src = AssembleSource(body, preamble);

        winrt::com_ptr<ID3DBlob> bytecode, errors;
        UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3
            | D3DCOMPILE_WARNINGS_ARE_ERRORS
            | D3DCOMPILE_IEEE_STRICTNESS;  // allow isnan/isinf/isfinite
        HRESULT hr = D3DCompile(src.data(), src.size(),
            "shader_test_bench", nullptr, ShaderLab::Effects::ShaderLabIncludeHandler(),
            "main", "cs_5_0", flags, 0,
            bytecode.put(), errors.put());
        if (FAILED(hr) || !bytecode)
        {
            std::printf("    [bench] D3DCompile failed (0x%08X):\n%s\n",
                static_cast<uint32_t>(hr),
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no error blob)");
            return {};
        }

        winrt::com_ptr<ID3D11ComputeShader> cs;
        hr = m_device->CreateComputeShader(bytecode->GetBufferPointer(),
            bytecode->GetBufferSize(), nullptr, cs.put());
        if (FAILED(hr) || !cs)
        {
            std::printf("    [bench] CreateComputeShader failed 0x%08X\n",
                static_cast<uint32_t>(hr));
            return {};
        }

        // Save+restore minimal compute state so we don't disturb the rest
        // of the test runner (which mostly drives the D2D path on the same
        // device).
        winrt::com_ptr<ID3D11ComputeShader> prevCs;
        UINT prevCsCount = 1;
        m_ctx->CSGetShader(prevCs.put(), nullptr, &prevCsCount);
        winrt::com_ptr<ID3D11UnorderedAccessView> prevUav;
        m_ctx->CSGetUnorderedAccessViews(0, 1, prevUav.put());

        m_ctx->CSSetShader(cs.get(), nullptr, 0);
        ID3D11UnorderedAccessView* uavs[] = { m_resultUav.get() };
        UINT initialCounts[] = { 0 };
        m_ctx->CSSetUnorderedAccessViews(0, 1, uavs, initialCounts);

        m_ctx->Dispatch(1, 1, 1);

        // Unbind UAV before the staging copy (D3D11 complains otherwise).
        ID3D11UnorderedAccessView* nullUav[] = { nullptr };
        m_ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);

        m_ctx->CopyResource(m_stagingBuf.get(), m_resultBuf.get());

        // Restore prior compute state.
        m_ctx->CSSetShader(prevCs.get(), nullptr, 0);
        ID3D11UnorderedAccessView* restoreUavs[] = { prevUav.get() };
        m_ctx->CSSetUnorderedAccessViews(0, 1, restoreUavs, nullptr);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = m_ctx->Map(m_stagingBuf.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
        {
            std::printf("    [bench] Map(staging) failed 0x%08X\n",
                static_cast<uint32_t>(hr));
            return {};
        }
        std::vector<TestVec4> out(numFloat4Slots);
        std::memcpy(out.data(), mapped.pData, sizeof(TestVec4) * numFloat4Slots);
        m_ctx->Unmap(m_stagingBuf.get(), 0);

        return out;
    }
}
