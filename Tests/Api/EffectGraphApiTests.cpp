#include "pch_engine.h"
#include "../TestSupport.h"
#include "Engine/Api/EffectGraph.h"
#include "Engine/Api/EffectGraphInternal.h"
#include "Engine/Utf8.h"
#include "Effects/EffectBlob.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/SourceNodeFactory.h"
#include "Graph/GraphEdits.h"
#include "Graph/GraphModel.h"
#include "Rendering/DisplayProfile.h"
#include "Rendering/GraphEvaluator.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <d3d12.h>
#pragma comment(lib, "d3d12.lib")

// The public EffectGraph API, used as an outside program would: a context on
// the test device, texture inputs, D2D and ShaderLab effects, and results read
// back through D3D11.
namespace
{
    using namespace ShaderLab::Tests;

    constexpr UINT cWidth = 64, cHeight = 32;

    uint16_t ToHalf(float f)
    {
        uint32_t b; std::memcpy(&b, &f, 4);
        if ((b & 0x7FFFFFFF) == 0) return static_cast<uint16_t>((b >> 16) & 0x8000);
        return static_cast<uint16_t>(((b >> 16) & 0x8000) | ((((b >> 23) & 0xFF) - 112) << 10) | ((b >> 13) & 0x3FF));
    }

    float FromHalf(uint16_t h)
    {
        const uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
        uint32_t bits = exponent == 0 ? sign : sign | ((exponent + 112) << 23) | (mantissa << 13);
        float f; std::memcpy(&f, &bits, 4);
        return f;
    }

    // Grey FP16 texels: the left half `left`, the right half `right`, alpha 1.
    std::vector<uint16_t> GreyTexels(float left, float right)
    {
        std::vector<uint16_t> texels(cWidth * cHeight * 4);
        for (UINT y = 0; y < cHeight; ++y)
            for (UINT x = 0; x < cWidth; ++x)
            {
                uint16_t* p = &texels[(y * cWidth + x) * 4];
                p[0] = p[1] = p[2] = ToHalf(x < cWidth / 2 ? left : right);
                p[3] = ToHalf(1.0f);
            }
        return texels;
    }

    // A texture on `device`, the test device unless given.
    winrt::com_ptr<ID3D11Texture2D> MakeTexture(DXGI_FORMAT format, UINT bind, const void* texels, UINT pitch,
                                                ID3D11Device* device = nullptr)
    {
        if (!device) device = g_d3dDevice.get();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = cWidth; desc.Height = cHeight; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = format; desc.SampleDesc.Count = 1; desc.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA init{ texels, pitch, 0 };
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(device->CreateTexture2D(&desc, texels ? &init : nullptr, texture.put()));
        return texture;
    }

    // The texture's bytes, through a staging copy on the texture's own device.
    std::vector<uint8_t> ReadBack(ID3D11Texture2D* texture, UINT bytesPerPixel)
    {
        winrt::com_ptr<ID3D11Device> device;
        texture->GetDevice(device.put());
        winrt::com_ptr<ID3D11DeviceContext> context;
        device->GetImmediateContext(context.put());
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, staging.put()));
        context->CopyResource(staging.get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<uint8_t> bytes(desc.Width * desc.Height * bytesPerPixel);
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(&bytes[y * desc.Width * bytesPerPixel], static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                        desc.Width * bytesPerPixel);
        context->Unmap(staging.get(), 0);
        return bytes;
    }

    float RedAt(const std::vector<uint8_t>& fp16, UINT x, UINT y)
    {
        uint16_t h; std::memcpy(&h, &fp16[(y * cWidth + x) * 8], 2);
        return FromHalf(h);
    }

    winrt::com_ptr<IEffectGraphContext> MakeContext()
    {
        winrt::com_ptr<IEffectGraphContext> context;
        winrt::check_hresult(EffectGraphCreateContext(g_d3dDevice.get(), nullptr, IID_PPV_ARGS(context.put())));
        return context;
    }

    float FieldOf(IEffectGraphNode* node, const wchar_t* name)
    {
        winrt::com_ptr<IEffectGraphValues> values;
        float value = -1.0f;
        if (SUCCEEDED(node->GetValues(values.put()))) values->GetFloat(name, &value);
        return value;
    }

    SL_TEST_CASE(TestEffectGraphContext, Tag::Gpu)
    {
        printf("\n=== EffectGraph: context ===\n");

        // A device without BGRA support cannot host Direct2D.
        winrt::com_ptr<ID3D11Device> noBgra;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                          noBgra.put(), nullptr, nullptr);
        winrt::com_ptr<IEffectGraphContext> refused;
        TEST("Api_NoBgraRefused", EffectGraphCreateContext(noBgra.get(), nullptr, IID_PPV_ARGS(refused.put())) ==
                                      EFFECTGRAPH_E_UNSUPPORTED_DEVICE && !refused);

        auto context = MakeContext();
        winrt::com_ptr<ID3D11Device> device;
        context->GetD3DDevice(device.put());
        TEST("Api_UsesCallersDevice", device.get() == static_cast<ID3D11Device*>(g_d3dDevice.get()));
        winrt::com_ptr<ID2D1Device> d2dDevice;
        context->GetD2DDevice(d2dDevice.put());
        TEST("Api_HasD2DDevice", d2dDevice != nullptr);

        winrt::com_ptr<IEffectGraphNode> unknown;
        TEST("Api_UnknownEffect", context->CreateEffect(L"No Such Effect", unknown.put()) == EFFECTGRAPH_E_UNKNOWN_EFFECT);

        // ShaderLab reaches its own objects through the internal interface.
        auto internal = context.try_as<IEffectGraphContextInternal>();
        winrt::com_ptr<IEffectGraphNode> stats;
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        winrt::com_ptr<IEffectGraphNode> again;
        const bool sameObject = internal && SUCCEEDED(internal->NodeFor(internal->NodeId(stats.get()), again.put())) &&
                                again.get() == stats.get();
        TEST("Api_InternalSameNodeObject", sameObject);
        TEST("Api_InternalSeesGraph", internal && internal->Model().Nodes().size() == 1);
    }

    SL_TEST_CASE(TestEffectGraphStatistics, Tag::Gpu)
    {
        printf("\n=== EffectGraph: statistics from a texture ===\n");
        auto context = MakeContext();
        auto texels = GreyTexels(1.0f, 3.0f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);

        winrt::com_ptr<IEffectGraphNode> input, stats;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        TEST("Stats_Connect", SUCCEEDED(stats->SetInput(0, input.get())));
        const HRESULT realized = context->Realize(stats.get(), nullptr);
        TEST("Stats_Realize", SUCCEEDED(realized));
        TEST("Stats_MatchTexture", FieldOf(stats.get(), L"Samples") == cWidth * cHeight &&
             std::abs(FieldOf(stats.get(), L"Max") - 240.0f) < 0.05f && std::abs(FieldOf(stats.get(), L"Mean") - 160.0f) < 0.05f);

        // New texels in the same texture, then Invalidate: the next Realize sees them.
        texels = GreyTexels(2.0f, 6.0f);
        g_d3dContext->UpdateSubresource(texture.get(), 0, nullptr, texels.data(), cWidth * 8, 0);
        auto asInput = input.try_as<IEffectGraphInput>();
        if (asInput) asInput->Invalidate();
        context->Realize(stats.get(), nullptr);
        TEST("Stats_FollowInvalidate", asInput && std::abs(FieldOf(stats.get(), L"Max") - 480.0f) < 0.1f);
        TEST("Stats_PlainNodeIsNotInput", !stats.try_as<IEffectGraphInput>());

        winrt::com_ptr<IEffectGraphValues> values;
        stats->GetValues(values.put());
        EFFECTGRAPH_FIELD_INFO info{};
        const UINT32 index = values ? values->GetFieldIndex(L"Max") : EFFECTGRAPH_INVALID_INDEX;
        TEST("Stats_FieldInfo", index != EFFECTGRAPH_INVALID_INDEX && SUCCEEDED(values->GetFieldInfo(index, &info)) &&
             info.type == EFFECTGRAPH_FIELD_TYPE_FLOAT && info.componentCount == 1 && info.elementCount == 1);
    }

    SL_TEST_CASE(TestEffectGraphRealizeTo, Tag::Gpu)
    {
        printf("\n=== EffectGraph: rendering into a texture ===\n");
        auto context = MakeContext();
        auto texels = GreyTexels(0.25f, 0.5f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);
        auto target = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                  nullptr, 0);
        winrt::com_ptr<IEffectGraphNode> input;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));

        // The input alone comes back exactly.
        TEST("RealizeTo_InputRoundTrips", SUCCEEDED(context->RealizeTo(input.get(), target.get())));
        auto pixels = ReadBack(target.get(), 8);
        TEST("RealizeTo_ExactCopy", RedAt(pixels, 3, 3) == 0.25f && RedAt(pixels, 60, 20) == 0.5f);

        // A D2D effect by CLSID, its property set by D2D's own index.
        winrt::com_ptr<IEffectGraphNode> matrix;
        winrt::check_hresult(context->CreateD2DEffect(CLSID_D2D1ColorMatrix, matrix.put()));
        TEST("D2D_IndexMatchesD2D", matrix->GetPropertyIndex(L"ColorMatrix") == D2D1_COLORMATRIX_PROP_COLOR_MATRIX &&
             matrix->GetType(D2D1_COLORMATRIX_PROP_COLOR_MATRIX) == D2D1_PROPERTY_TYPE_MATRIX_5X4);
        const D2D1_MATRIX_5X4_F doubled = D2D1::Matrix5x4F(2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0);
        TEST("D2D_SetByIndex", SUCCEEDED(matrix->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, doubled)));
        matrix->SetInput(0, input.get());
        TEST("RealizeTo_D2DEffect", SUCCEEDED(context->RealizeTo(matrix.get(), target.get())));
        pixels = ReadBack(target.get(), 8);
        TEST("RealizeTo_Doubled", RedAt(pixels, 3, 3) == 0.5f && RedAt(pixels, 60, 20) == 1.0f);
        if (RedAt(pixels, 3, 3) != 0.5f) printf("    (debug) red = %.6f, %.6f\n", RedAt(pixels, 3, 3), RedAt(pixels, 60, 20));

        // An sRGB 8-bit target gamma-encodes: linear 0.25 is sRGB 137, 0.5 is 188.
        auto srgb = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                nullptr, 0);
        TEST("RealizeTo_Srgb", SUCCEEDED(context->RealizeTo(input.get(), srgb.get())));
        auto bytes = ReadBack(srgb.get(), 4);
        const int left = bytes[(3 * cWidth + 3) * 4 + 2], right = bytes[(20 * cWidth + 60) * 4 + 2];
        TEST("RealizeTo_SrgbEncoded", std::abs(left - 137) <= 1 && std::abs(right - 188) <= 1);
        if (std::abs(left - 137) > 1 || std::abs(right - 188) > 1) printf("    (debug) sRGB red = %d, %d\n", left, right);
    }

    SL_TEST_CASE(TestEffectGraphBindings, Tag::Gpu)
    {
        printf("\n=== EffectGraph: bindings and the graph rules ===\n");
        auto context = MakeContext();
        auto texels = GreyTexels(1.0f, 3.0f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);
        winrt::com_ptr<IEffectGraphNode> input, stats, toneMap;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        winrt::check_hresult(context->CreateEffect(L"ICtCp Tone Map (HDR -> SDR)", toneMap.put()));
        stats->SetInput(0, input.get());
        toneMap->SetInput(0, input.get());

        TEST("Bind_Accepted", SUCCEEDED(toneMap->Bind(L"SourcePeakNits", stats.get(), L"Max")));
        TEST("Bind_UnknownFieldRefused", FAILED(toneMap->Bind(L"SourcePeakNits", stats.get(), L"NoSuchField")));
        ID2D1Image* image = nullptr;
        const HRESULT realized = context->Realize(toneMap.get(), &image);
        TEST("Bind_Realize", SUCCEEDED(realized) && image);
        if (image) image->Release();
        float peak = 0.0f;
        toneMap->GetValueByName(L"SourcePeakNits", D2D1_PROPERTY_TYPE_FLOAT, reinterpret_cast<BYTE*>(&peak), sizeof(peak));
        TEST("Bind_ValueArrives", std::abs(peak - 240.0f) < 0.05f);
        if (std::abs(peak - 240.0f) >= 0.05f) printf("    (debug) SourcePeakNits = %.3f\n", peak);

        // A loop is refused, through an edge or a binding.
        TEST("Graph_EdgeCycleRefused", input.try_as<IEffectGraphNode>() && toneMap->SetInput(0, toneMap.get()) == EFFECTGRAPH_E_CYCLE);
        TEST("Graph_BindCycleRefused", stats->Bind(L"ClipNits", stats.get(), L"Max") == EFFECTGRAPH_E_CYCLE);

        // A node from another context, and a removed node.
        auto other = MakeContext();
        winrt::com_ptr<IEffectGraphNode> foreign;
        winrt::check_hresult(other->CreateEffect(L"Luminance Statistics", foreign.put()));
        TEST("Graph_ForeignNodeRefused", toneMap->SetInput(0, foreign.get()) == EFFECTGRAPH_E_WRONG_CONTEXT);
        TEST("Graph_Remove", SUCCEEDED(context->RemoveNode(stats.get())));
        wchar_t error[64];
        TEST("Graph_RemovedNodeSays", stats->GetError(error, 64) == EFFECTGRAPH_E_NODE_REMOVED &&
             context->Realize(stats.get(), nullptr) == EFFECTGRAPH_E_NODE_REMOVED);
    }

    // No draw session to think about: values come from GetValues alone, and
    // outputs are drawn with the caller's own device context, which may be
    // drawing while the graph evaluates.
    SL_TEST_CASE(TestEffectGraphDrawing, Tag::Gpu)
    {
        printf("\n=== EffectGraph: drawing outputs ===\n");
        auto context = MakeContext();
        auto texels = GreyTexels(1.0f, 3.0f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);
        winrt::com_ptr<IEffectGraphNode> input, stats, matrix, highlight;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        winrt::check_hresult(context->CreateD2DEffect(CLSID_D2D1ColorMatrix, matrix.put()));
        winrt::check_hresult(context->CreateEffect(L"Gamut Highlight", highlight.put()));
        stats->SetInput(0, input.get());
        matrix->SetInput(0, input.get());
        matrix->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,
                         D2D1::Matrix5x4F(2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0));
        highlight->SetInput(0, matrix.get());
        TEST("Values_EvaluateOnDemand", std::abs(FieldOf(stats.get(), L"Max") - 240.0f) < 0.05f);

        // The caller's own device context on the context's D2D device.
        winrt::com_ptr<ID2D1Device> d2dDevice;
        context->GetD2DDevice(d2dDevice.put());
        winrt::com_ptr<ID2D1DeviceContext> mine;
        winrt::check_hresult(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, mine.put()));
        auto target = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        winrt::check_hresult(mine->CreateBitmapFromDxgiSurface(target.as<IDXGISurface>().get(), &props, bitmap.put()));
        mine->SetTarget(bitmap.get());

        // Evaluation while the caller's context is drawing: a compute node,
        // and a D2D effect into a ShaderLab pixel shader drawn there.
        texels = GreyTexels(1.0f, 4.0f);
        g_d3dContext->UpdateSubresource(texture.get(), 0, nullptr, texels.data(), cWidth * 8, 0);
        input.as<IEffectGraphInput>()->Invalidate();
        mine->BeginDraw();
        mine->Clear(D2D1::ColorF(0, 0, 0, 0));
        const float peak = FieldOf(stats.get(), L"Max");
        ID2D1Image* image = nullptr;
        const HRESULT realized = context->Realize(highlight.get(), &image);
        if (image)
        {
            mine->DrawImage(image, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
            image->Release();
        }
        const HRESULT drawn = mine->EndDraw();
        mine->SetTarget(nullptr);
        const auto pixels = ReadBack(target.get(), 8);
        TEST("Draw_ComputeWhileCallerDraws", std::abs(peak - 320.0f) < 0.05f);
        TEST("Draw_OnCallersContext", SUCCEEDED(realized) && SUCCEEDED(drawn) &&
             RedAt(pixels, 3, 3) == 2.0f && RedAt(pixels, 60, 20) == 8.0f);
    }

    SL_TEST_CASE(TestEffectGraphMoveToDevice, Tag::Gpu)
    {
        printf("\n=== EffectGraph: moving a context to another device ===\n");
        auto context = MakeContext();
        auto internal = context.as<IEffectGraphContextInternal>();
        auto texels = GreyTexels(1.0f, 3.0f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);
        winrt::com_ptr<IEffectGraphNode> input, stats, matrix;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        winrt::check_hresult(context->CreateD2DEffect(CLSID_D2D1ColorMatrix, matrix.put()));
        stats->SetInput(0, input.get());
        matrix->SetInput(0, input.get());
        matrix->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,
                         D2D1::Matrix5x4F(2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0));
        context->Realize(stats.get(), nullptr);
        TEST("Move_BeforeMove", std::abs(FieldOf(stats.get(), L"Max") - 240.0f) < 0.05f);

        // A device D2D cannot use is refused, and the context carries on where it was.
        winrt::com_ptr<ID3D11Device> noBgra;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                          noBgra.put(), nullptr, nullptr);
        TEST("Move_NoBgraRefused", internal->MoveToDevice(noBgra.get()) == EFFECTGRAPH_E_UNSUPPORTED_DEVICE);
        TEST("Move_StillRendersAfterRefusal", SUCCEEDED(context->Realize(stats.get(), nullptr)) &&
             std::abs(FieldOf(stats.get(), L"Max") - 240.0f) < 0.05f);

        winrt::com_ptr<ID3D11Device> other;
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                               nullptr, 0, D3D11_SDK_VERSION, other.put(), nullptr, nullptr));
        TEST("Move_Succeeds", SUCCEEDED(internal->MoveToDevice(other.get())));
        winrt::com_ptr<ID3D11Device> current;
        context->GetD3DDevice(current.put());
        TEST("Move_UsesNewDevice", current.get() == other.get());

        // The node interfaces are kept; the input waits for a texture on the new device.
        auto asInput = input.as<IEffectGraphInput>();
        TEST("Move_InputEmpty", context->Realize(input.get(), nullptr) == EFFECTGRAPH_E_NODE_FAILED);
        TEST("Move_OldTextureRefused", asInput->SetTexture(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR) == E_INVALIDARG);
        texels = GreyTexels(2.0f, 6.0f);
        auto moved = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8,
                                 other.get());
        TEST("Move_NewTexture", SUCCEEDED(asInput->SetTexture(moved.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR)));
        TEST("Move_ComputeOnNewDevice", SUCCEEDED(context->Realize(stats.get(), nullptr)) &&
             std::abs(FieldOf(stats.get(), L"Max") - 480.0f) < 0.1f);

        auto target = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                  nullptr, 0, other.get());
        TEST("Move_RealizeToOnNewDevice", SUCCEEDED(context->RealizeTo(matrix.get(), target.get())));
        const auto pixels = ReadBack(target.get(), 8);
        TEST("Move_D2DEffectKeptItsValue", RedAt(pixels, 3, 3) == 4.0f && RedAt(pixels, 60, 20) == 12.0f);
        auto oldTarget = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                     nullptr, 0);
        TEST("Move_OldDeviceTargetRefused", context->RealizeTo(matrix.get(), oldTarget.get()) == E_INVALIDARG);
    }

    // A D3D11 device on 11on12 over WARP, whose D3D12 device can be removed on
    // purpose (ID3D12Device5::RemoveDevice): a real removal, affecting nothing
    // else on the machine.
    struct RemovableDevice
    {
        winrt::com_ptr<ID3D12Device> d3d12;
        winrt::com_ptr<ID3D12CommandQueue> queue;
        winrt::com_ptr<ID3D11Device> d3d11;
    };

    bool MakeRemovableDevice(RemovableDevice& device)
    {
        winrt::com_ptr<IDXGIFactory4> factory;
        winrt::com_ptr<IDXGIAdapter> warp;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put()))) ||
            FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(warp.put()))) ||
            FAILED(D3D12CreateDevice(warp.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.d3d12.put()))))
            return false;
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device.d3d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(device.queue.put())))) return false;
        IUnknown* queues[] = { device.queue.get() };
        return SUCCEEDED(D3D11On12CreateDevice(device.d3d12.get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                               queues, 1, 0, device.d3d11.put(), nullptr, nullptr));
    }

    SL_TEST_CASE(TestEffectGraphDeviceRemoved, Tag::Gpu)
    {
        printf("\n=== EffectGraph: recovering from a removed device ===\n");
        RemovableDevice removable;
        if (!MakeRemovableDevice(removable))
        {
            SkipMissingFixture(L"a D3D12 WARP device for 11on12 (removable device)");
            return;
        }
        winrt::com_ptr<IEffectGraphContext> context;
        winrt::check_hresult(EffectGraphCreateContext(removable.d3d11.get(), nullptr, IID_PPV_ARGS(context.put())));
        auto texels = GreyTexels(1.0f, 3.0f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8,
                                   removable.d3d11.get());
        winrt::com_ptr<IEffectGraphNode> input, stats;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        stats->SetInput(0, input.get());
        TEST("Removed_RendersBefore", SUCCEEDED(context->Realize(stats.get(), nullptr)) &&
             std::abs(FieldOf(stats.get(), L"Max") - 240.0f) < 0.05f);

        winrt::com_ptr<ID3D12Device5> d3d12;
        removable.d3d12.as(d3d12);
        d3d12->RemoveDevice();
        const HRESULT reason = removable.d3d11->GetDeviceRemovedReason();
        TEST("Removed_DeviceReportsIt", FAILED(reason));
        TEST("Removed_RealizeSaysWhy", context->Realize(stats.get(), nullptr) == reason);
        winrt::com_ptr<IEffectGraphValues> lost;
        TEST("Removed_GetValuesSaysWhy", stats->GetValues(lost.put()) == reason && !lost);

        // A new device: the graph and its interfaces carry over.
        winrt::com_ptr<ID3D11Device> fresh;
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                               nullptr, 0, D3D11_SDK_VERSION, fresh.put(), nullptr, nullptr));
        TEST("Removed_MovesToNewDevice", SUCCEEDED(context->MoveToDevice(fresh.get())));
        texels = GreyTexels(1.0f, 5.0f);
        auto replacement = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8,
                                       fresh.get());
        TEST("Removed_InputTakesNewTexture",
             SUCCEEDED(input.as<IEffectGraphInput>()->SetTexture(replacement.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR)));
        TEST("Removed_RendersAfter", SUCCEEDED(context->Realize(stats.get(), nullptr)) &&
             std::abs(FieldOf(stats.get(), L"Max") - 400.0f) < 0.05f);
    }

    // Element `element` of a StructuredBuffer<float4>, read by a compute
    // shader of the caller's own: what a program does with a values buffer.
    float ReadWithOwnShader(ID3D11ShaderResourceView* values, UINT element)
    {
        const std::string index = std::to_string(element);
        const D3D_SHADER_MACRO macros[] = { { "ELEMENT", index.c_str() }, { nullptr, nullptr } };
        const char* source =
            "StructuredBuffer<float4> Values : register(t0);\n"
            "RWStructuredBuffer<float4> Result : register(u0);\n"
            "[numthreads(1, 1, 1)] void main() { Result[0] = Values[ELEMENT]; }\n";
        winrt::com_ptr<ID3DBlob> code, errors;
        if (FAILED(D3DCompile(source, std::strlen(source), "caller", macros, nullptr, "main", "cs_5_0", 0, 0,
                              code.put(), errors.put())))
            return -1.0f;
        winrt::com_ptr<ID3D11ComputeShader> shader;
        winrt::check_hresult(g_d3dDevice->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.put()));
        D3D11_BUFFER_DESC desc{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16 };
        winrt::com_ptr<ID3D11Buffer> result;
        winrt::check_hresult(g_d3dDevice->CreateBuffer(&desc, nullptr, result.put()));
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = 1;
        winrt::com_ptr<ID3D11UnorderedAccessView> uav;
        winrt::check_hresult(g_d3dDevice->CreateUnorderedAccessView(result.get(), &uavDesc, uav.put()));

        g_d3dContext->CSSetShader(shader.get(), nullptr, 0);
        g_d3dContext->CSSetShaderResources(0, 1, &values);
        ID3D11UnorderedAccessView* uavs[] = { uav.get() };
        g_d3dContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        g_d3dContext->Dispatch(1, 1, 1);
        ID3D11ShaderResourceView* noSrv[] = { nullptr };
        ID3D11UnorderedAccessView* noUav[] = { nullptr };
        g_d3dContext->CSSetShaderResources(0, 1, noSrv);
        g_d3dContext->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
        g_d3dContext->CSSetShader(nullptr, nullptr, 0);

        D3D11_BUFFER_DESC stagingDesc{ 16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
        winrt::com_ptr<ID3D11Buffer> staging;
        winrt::check_hresult(g_d3dDevice->CreateBuffer(&stagingDesc, nullptr, staging.put()));
        g_d3dContext->CopyResource(staging.get(), result.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(g_d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        const float x = static_cast<const float*>(mapped.pData)[0];
        g_d3dContext->Unmap(staging.get(), 0);
        return x;
    }

    SL_TEST_CASE(TestEffectGraphValuesBuffer, Tag::Gpu)
    {
        printf("\n=== EffectGraph: analysis values on the GPU ===\n");
        auto context = MakeContext();
        auto internal = context.as<IEffectGraphContextInternal>();
        auto texels = GreyTexels(1.0f, 3.0f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);
        winrt::com_ptr<IEffectGraphNode> input, stats;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        stats->SetInput(0, input.get());

        winrt::com_ptr<IEffectGraphValuesBuffer> buffer;
        TEST("Buffer_Created", stats->GetValuesBuffer(buffer.put()) == S_OK && buffer);
        if (!buffer) return;
        const UINT32 maxField = buffer->GetFieldIndex(L"Max");
        EFFECTGRAPH_FIELD_INFO info{};
        TEST("Buffer_HasMax", maxField != EFFECTGRAPH_INVALID_INDEX && SUCCEEDED(buffer->GetFieldInfo(maxField, &info)));
        // Nothing was read back to get it: the node's CPU values are untouched.
        const auto* node = internal->Model().FindNode(internal->NodeId(stats.get()));
        TEST("Buffer_NoCpuReadback", node && node->analysisOutput.fields.empty());

        winrt::com_ptr<ID3D11ShaderResourceView> view;
        winrt::com_ptr<ID3D11Buffer> raw;
        buffer->GetView(view.put());
        buffer->GetBuffer(raw.put());
        winrt::com_ptr<ID3D11Resource> viewed;
        if (view) view->GetResource(viewed.put());
        TEST("Buffer_RawObjects", view && raw && viewed.get() == static_cast<ID3D11Resource*>(raw.get()));
        const float ownRead = ReadWithOwnShader(view.get(), info.bufferElement);
        TEST("Buffer_CallersShaderReadsMax", std::abs(ownRead - 240.0f) < 0.05f);
        printf("  [info] Max at element %u, read by the caller's shader: %.3f\n", info.bufferElement, ownRead);

        winrt::com_ptr<IEffectGraphValues> read;
        float readMax = -1.0f;
        if (SUCCEEDED(buffer->ReadValues(read.put()))) read->GetFloat(L"Max", &readMax);
        TEST("Buffer_ReadValues", std::abs(readMax - 240.0f) < 0.05f);

        // A tone mapper bound to the statistics, and one with its peak set by
        // hand, for comparing what a write does.
        winrt::com_ptr<IEffectGraphNode> bound, manual;
        winrt::check_hresult(context->CreateEffect(L"ICtCp Tone Map (HDR -> SDR)", bound.put()));
        winrt::check_hresult(context->CreateEffect(L"ICtCp Tone Map (HDR -> SDR)", manual.put()));
        bound->SetInput(0, input.get());
        manual->SetInput(0, input.get());
        winrt::check_hresult(bound->Bind(L"SourcePeakNits", stats.get(), L"Max"));
        auto target = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
        auto render = [&](IEffectGraphNode* toneMap)
        {
            return SUCCEEDED(context->RealizeTo(toneMap, target.get())) ? ReadBack(target.get(), 8) : std::vector<uint8_t>{};
        };
        const auto at240 = render(bound.get());

        const float written = 400.0f;
        TEST("Write_Accepted", SUCCEEDED(buffer->SetFloats(maxField, &written, 1)));
        TEST("Write_WrongCountRefused", buffer->SetFloats(maxField, &written, 2) == E_INVALIDARG);
        TEST("Write_CallersShaderSeesIt", std::abs(ReadWithOwnShader(view.get(), info.bufferElement) - 400.0f) < 0.05f);
        TEST("Write_GetValuesSeesIt", std::abs(FieldOf(stats.get(), L"Max") - 400.0f) < 0.05f);
        const auto boundAt400 = render(bound.get());
        manual->SetValueByName(L"SourcePeakNits", 400.0f);
        const auto manualAt400 = render(manual.get());
        TEST("Write_ReachesBoundNode", !boundAt400.empty() && boundAt400 == manualAt400 && boundAt400 != at240);

        // The next evaluation of the node writes its own values back.
        texels = GreyTexels(1.0f, 6.0f);
        g_d3dContext->UpdateSubresource(texture.get(), 0, nullptr, texels.data(), cWidth * 8, 0);
        input.as<IEffectGraphInput>()->Invalidate();
        TEST("Write_NextEvaluationReplacesIt", std::abs(FieldOf(stats.get(), L"Max") - 480.0f) < 0.05f);

        // A node whose values are worked out on the CPU gets an uploaded buffer.
        winrt::com_ptr<IEffectGraphNode> parameter;
        winrt::check_hresult(context->CreateEffect(L"Float Parameter", parameter.put()));
        parameter->SetValueByName(L"Value", 0.75f);
        winrt::com_ptr<IEffectGraphValuesBuffer> parameterBuffer;
        TEST("Cpu_BufferCreated", parameter->GetValuesBuffer(parameterBuffer.put()) == S_OK && parameterBuffer);
        if (!parameterBuffer) return;
        winrt::com_ptr<ID3D11ShaderResourceView> parameterView;
        parameterBuffer->GetView(parameterView.put());
        TEST("Cpu_CallersShaderReadsIt", std::abs(ReadWithOwnShader(parameterView.get(), 0) - 0.75f) < 1e-6f);
        const float half = 0.5f;
        TEST("Cpu_Write", SUCCEEDED(parameterBuffer->SetFloats(0, &half, 1)) &&
             std::abs(ReadWithOwnShader(parameterView.get(), 0) - 0.5f) < 1e-6f);
    }

    // One patch over cWidth x cHeight: a horizontal ramp from black to
    // `peak` in scRGB, beyond SDR white, with hard edges.
    winrt::com_ptr<ID2D1GradientMesh> HdrRamp(ID2D1DeviceContext2* dc, float peak)
    {
        D2D1_GRADIENT_MESH_PATCH patch{};
        D2D1_POINT_2F* points = &patch.point00;   // point<column><row>, column-major
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                points[column * 4 + row] = D2D1::Point2F(cWidth * column / 3.0f, cHeight * row / 3.0f);
        patch.color00 = patch.color03 = D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f);
        patch.color30 = patch.color33 = D2D1::ColorF(peak, peak, peak, 1.0f);
        patch.topEdgeMode = patch.leftEdgeMode = patch.bottomEdgeMode = patch.rightEdgeMode = D2D1_PATCH_EDGE_MODE_ALIASED;
        winrt::com_ptr<ID2D1GradientMesh> mesh;
        winrt::check_hresult(dc->CreateGradientMesh(&patch, 1, mesh.put()));
        return mesh;
    }

    // Worked example: content a program draws with Direct2D, given to a graph.
    SL_TEST_CASE(TestEffectGraphDrawnInput, Tag::Gpu)
    {
        printf("\n=== EffectGraph: a gradient mesh drawn by the caller as an input ===\n");
        auto context = MakeContext();
        winrt::com_ptr<ID2D1Device> d2dDevice;
        context->GetD2DDevice(d2dDevice.put());
        winrt::com_ptr<ID2D1DeviceContext> created;
        winrt::check_hresult(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, created.put()));
        auto mine = created.as<ID2D1DeviceContext2>();
        const float peak = 4.0f;   // 320 nits
        auto mesh = HdrRamp(mine.get(), peak);

        auto target = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
        // The exact ramp: linear between the corner colours, at pixel centres.
        auto rampError = [&](const std::vector<uint8_t>& pixels)
        {
            float worst = 0.0f;
            for (UINT x = 0; x < cWidth; ++x)
                worst = (std::max)(worst, std::abs(RedAt(pixels, x, cHeight / 2) - peak * (x + 0.5f) / cWidth));
            return worst;
        };
        const float expectedMax = peak * (cWidth - 0.5f) / cWidth * 80.0f;

        // 1. A command list: recorded once, drawn by the graph wherever it is
        //    used, no pixels stored. Bounds come from what was drawn.
        winrt::com_ptr<ID2D1CommandList> list;
        winrt::check_hresult(mine->CreateCommandList(list.put()));
        mine->SetTarget(list.get());
        mine->BeginDraw();
        mine->DrawGradientMesh(mesh.get());
        winrt::check_hresult(mine->EndDraw());
        winrt::check_hresult(list->Close());
        mine->SetTarget(nullptr);

        winrt::com_ptr<IEffectGraphNode> drawn, stats;
        TEST("List_Input", SUCCEEDED(context->CreateImageInput(list.get(), drawn.put())));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        stats->SetInput(0, drawn.get());
        TEST("List_RendersTheRamp", SUCCEEDED(context->RealizeTo(drawn.get(), target.get())) &&
             rampError(ReadBack(target.get(), 8)) < 0.01f);
        const float listMax = FieldOf(stats.get(), L"Max");
        TEST("List_HdrSurvives", std::abs(listMax - expectedMax) < 0.5f);
        printf("  [info] command list: ramp error %.5f, Max %.3f nits (expected %.3f)\n",
               rampError(ReadBack(target.get(), 8)), listMax, expectedMax);

        // 2. A bitmap the caller draws into: FP16, premultiplied, so compute
        //    reads it in place. Redraw it and Invalidate for each frame.
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        winrt::check_hresult(mine->CreateBitmap(D2D1::SizeU(cWidth, cHeight), nullptr, 0, &props, bitmap.put()));
        auto drawInto = [&](ID2D1GradientMesh* content)
        {
            mine->SetTarget(bitmap.get());
            mine->BeginDraw();
            mine->Clear(D2D1::ColorF(0, 0, 0, 0));
            mine->DrawGradientMesh(content);
            const HRESULT hr = mine->EndDraw();
            mine->SetTarget(nullptr);
            return hr;
        };
        TEST("Bitmap_Drawn", SUCCEEDED(drawInto(mesh.get())));
        winrt::com_ptr<IEffectGraphNode> painted, paintedStats;
        TEST("Bitmap_Input", SUCCEEDED(context->CreateImageInput(bitmap.get(), painted.put())));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", paintedStats.put()));
        paintedStats->SetInput(0, painted.get());
        TEST("Bitmap_RendersTheRamp", SUCCEEDED(context->RealizeTo(painted.get(), target.get())) &&
             rampError(ReadBack(target.get(), 8)) < 0.01f);
        TEST("Bitmap_HdrSurvives", std::abs(FieldOf(paintedStats.get(), L"Max") - expectedMax) < 0.5f);

        // A new frame: draw again, then say so.
        auto brighter = HdrRamp(mine.get(), 2.0f * peak);
        TEST("Bitmap_Redrawn", SUCCEEDED(drawInto(brighter.get())));
        painted.as<IEffectGraphInput>()->Invalidate();
        TEST("Bitmap_NextFrameSeen", std::abs(FieldOf(paintedStats.get(), L"Max") - 2.0f * expectedMax) < 1.0f);
    }

    // ---- 1.1: dirty rects --------------------------------------------------

    bool RectIs(const RECT& rect, LONG left, LONG top, LONG right, LONG bottom)
    {
        return rect.left == left && rect.top == top && rect.right == right && rect.bottom == bottom;
    }

    bool Inside(const std::vector<RECT>& rects, UINT x, UINT y)
    {
        for (const auto& rect : rects)
            if (static_cast<LONG>(x) >= rect.left && static_cast<LONG>(x) < rect.right && static_cast<LONG>(y) >= rect.top &&
                static_cast<LONG>(y) < rect.bottom)
                return true;
        return false;
    }

    // Pixels (4 bytes each) that differ between `a` and `b`; of them, how
    // many lie outside `rects`.
    std::pair<size_t, size_t> PixelsDiffering(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                                              const std::vector<RECT>& rects)
    {
        size_t differing = 0, outside = 0;
        for (UINT y = 0; y < cHeight; ++y)
            for (UINT x = 0; x < cWidth; ++x)
            {
                const size_t at = (static_cast<size_t>(y) * cWidth + x) * 4;
                if (std::memcmp(&a[at], &b[at], 4) == 0) continue;
                ++differing;
                if (!Inside(rects, x, y)) ++outside;
            }
        return { differing, outside };
    }

    // A grey ramp, brighter to the right, so every column renders differently.
    std::vector<uint16_t> RampTexels(float scale)
    {
        std::vector<uint16_t> texels(cWidth * cHeight * 4);
        for (UINT y = 0; y < cHeight; ++y)
            for (UINT x = 0; x < cWidth; ++x)
            {
                uint16_t* p = &texels[(y * cWidth + x) * 4];
                p[0] = p[1] = p[2] = ToHalf(scale * (0.1f + 0.1f * x + 0.01f * y));
                p[3] = ToHalf(1.0f);
            }
        return texels;
    }

    // Rewrites `rect` of an FP16 texture with `nits`.
    void Paint(ID3D11Texture2D* texture, const RECT& rect, float nits)
    {
        const UINT width = rect.right - rect.left, height = rect.bottom - rect.top;
        std::vector<uint16_t> texels(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i < texels.size(); i += 4)
        {
            texels[i] = texels[i + 1] = texels[i + 2] = ToHalf(nits / 80.0f);
            texels[i + 3] = ToHalf(1.0f);
        }
        const D3D11_BOX box{ static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0,
                             static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom), 1 };
        g_d3dContext->UpdateSubresource(texture, 0, &box, texels.data(), width * 8, 0);
    }

    // The rects RealizeToIncremental drew, or none when it failed.
    struct Incremental
    {
        HRESULT hr{ E_FAIL };
        std::vector<RECT> drawn;
        UINT32 moved{ 99 };
    };

    Incremental RealizeIncremental(IEffectGraphContext2* context, IEffectGraphNode* node, ID3D11Texture2D* target,
                                   UINT32 flags = EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL, UINT32 capacity = 16)
    {
        Incremental result;
        std::vector<RECT> drawn(capacity);
        EFFECTGRAPH_MOVE_RECT moves[4]{};
        UINT32 count = 0;
        result.hr = context->RealizeToIncremental(node, target, flags, drawn.data(), capacity, &count, moves, 4,
                                                  &result.moved);
        drawn.resize(SUCCEEDED(result.hr) ? count : 0);
        result.drawn = std::move(drawn);
        return result;
    }

    struct InvalidRects
    {
        HRESULT hr{ E_FAIL };
        std::vector<RECT> rects;
        bool whole{ false };
    };

    InvalidRects InvalidOf(IEffectGraphNode* node)
    {
        InvalidRects result;
        winrt::com_ptr<IEffectGraphNode2> node2;
        if (FAILED(node->QueryInterface(__uuidof(IEffectGraphNode2), node2.put_void()))) return result;
        RECT rects[16]{};
        UINT32 count = 0;
        BOOL whole = FALSE;
        result.hr = node2->GetInvalidRects(rects, 16, &count, &whole);
        result.rects.assign(rects, rects + (SUCCEEDED(result.hr) ? count : 0));
        result.whole = whole != FALSE;
        return result;
    }

    SL_TEST_CASE(TestEffectGraphV2Interfaces, Tag::Gpu)
    {
        printf("\n=== EffectGraph 1.1: the dirty-rect interfaces ===\n");
        auto context = MakeContext();
        auto context2 = context.try_as<IEffectGraphContext2>();
        TEST("V2_ContextAnswers", context2 != nullptr);

        auto texels = GreyTexels(1.0f, 2.0f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);
        winrt::com_ptr<IEffectGraphNode> input, shader;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Nit Map", shader.put()));

        // An input answers both, and each reaches the other: one object.
        auto input2 = input.try_as<IEffectGraphInput2>();
        auto inputNode2 = input2 ? input2.try_as<IEffectGraphNode2>() : nullptr;
        auto backToInput2 = inputNode2 ? inputNode2.try_as<IEffectGraphInput2>() : nullptr;
        TEST("V2_InputAnswersBoth", input2 && inputNode2 && backToInput2 &&
                                    input2.as<IUnknown>().get() == inputNode2.as<IUnknown>().get());
        TEST("V2_InputIsStillV1", input2 && input2.try_as<IEffectGraphInput>() && input2.try_as<IEffectGraphNode>());
        // Any node answers IEffectGraphNode2; only an input answers IEffectGraphInput2.
        winrt::com_ptr<IEffectGraphInput2> notInput;
        TEST("V2_NodeAnswersNode2", shader.try_as<IEffectGraphNode2>() != nullptr);
        TEST("V2_NodeIsNoInput2", shader->QueryInterface(__uuidof(IEffectGraphInput2), notInput.put_void()) == E_NOINTERFACE &&
                                  !notInput);

        // Moves (stage 5): taken; an empty one, or none, changes nothing.
        const EFFECTGRAPH_MOVE_RECT move{ { 0, 0 }, { 4, 0, 8, 4 } };
        const EFFECTGRAPH_MOVE_RECT emptyMove{ { 0, 0 }, { 4, 0, 4, 4 } };
        winrt::com_ptr<IEffectGraphValues> values;
        const UINT32 field = 0;
        const float value = 1.0f;
        TEST("V2_MoveRectsAccepted", input2 && input2->MoveRects(&move, 1) == S_OK &&
                                     input2->MoveRects(&emptyMove, 1) == S_FALSE && input2->MoveRects(nullptr, 0) == S_FALSE &&
                                     input2->MoveRects(nullptr, 1) == E_INVALIDARG);
        // The deferred commit (stage 3) needs its context flag; without it
        // there is nothing landed, committed or to poll.
        auto shader2 = shader.as<IEffectGraphNode2>();
        TEST("V2_TryGetValuesNeedsFlag", shader2->TryGetValues(values.put()) == E_NOT_VALID_STATE && !values);
        TEST("V2_CommitFieldsNeedsFlag", shader2->CommitFields(&field, &value, 1) == E_NOT_VALID_STATE);
        TEST("V2_GetCommittedValuesNeedsFlag", shader2->GetCommittedValues(values.put()) == E_NOT_VALID_STATE && !values);
        TEST("V2_PollReadbacksNeedsFlag", context2 && context2->PollReadbacks() == E_NOT_VALID_STATE);
        TEST("V2_SetCommitPolicyUnbound", shader2->SetCommitPolicy(L"MinNits", EFFECTGRAPH_COMMIT_POLICY_HOSTED) == E_INVALIDARG);
        {
            const EFFECTGRAPH_CONTEXT_DESC desc{ EFFECTGRAPH_CONTEXT_FLAGS_DEFERRED_ANALYSIS_COMMIT, nullptr };
            winrt::com_ptr<IEffectGraphContext> accepted;
            const HRESULT hr = EffectGraphCreateContext(g_d3dDevice.get(), &desc, IID_PPV_ARGS(accepted.put()));
            TEST("V2_DeferredCommitFlagAccepted", SUCCEEDED(hr) && accepted);
            auto accepted2 = accepted ? accepted.try_as<IEffectGraphContext2>() : nullptr;
            TEST("V2_DeferredPollNothingPending", accepted2 && accepted2->PollReadbacks() == S_OK);
        }
        {
            // Implemented in stage 2 (TestEffectGraphNoFallback).
            const EFFECTGRAPH_CONTEXT_DESC desc{ EFFECTGRAPH_CONTEXT_FLAGS_NO_FALLBACK, nullptr };
            winrt::com_ptr<IEffectGraphContext> accepted;
            TEST("V2_NoFallbackFlagAccepted",
                 SUCCEEDED(EffectGraphCreateContext(g_d3dDevice.get(), &desc, IID_PPV_ARGS(accepted.put()))) && accepted);
        }

        // InvalidateRects: nothing, or nothing inside the texture, is S_FALSE.
        const RECT outside{ 100, 100, 120, 110 }, empty{ 5, 5, 5, 9 }, inside{ 4, 4, 8, 8 };
        const RECT nothingInside[] = { outside, empty };
        TEST("V2_InvalidateNoRectsIsFalse", input2 && input2->InvalidateRects(nullptr, 0) == S_FALSE);
        TEST("V2_InvalidateOutsideIsFalse", input2 && input2->InvalidateRects(nothingInside, 2) == S_FALSE);
        TEST("V2_InvalidateNullRectsRefused", input2 && input2->InvalidateRects(nullptr, 1) == E_INVALIDARG);
        TEST("V2_InvalidateInsideIsOk", input2 && input2->InvalidateRects(&inside, 1) == S_OK);

        // RealizeToIncremental's arguments.
        auto target = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, D3D11_BIND_RENDER_TARGET, nullptr, 0);
        shader->SetInput(0, input.get());
        RECT drawn[4]{};
        UINT32 count = 0;
        TEST("V2_DrawnCountRequired", context2->RealizeToIncremental(shader.get(), target.get(), 0, drawn, 4, nullptr,
                                                                     nullptr, 0, nullptr) == E_POINTER);
        TEST("V2_DrawnCapacityRequired", context2->RealizeToIncremental(shader.get(), target.get(), 0, drawn, 0, &count,
                                                                        nullptr, 0, nullptr) == E_INVALIDARG);
        TEST("V2_UnknownFlagRefused", context2->RealizeToIncremental(shader.get(), target.get(), 0x80, drawn, 4, &count,
                                                                     nullptr, 0, nullptr) == E_INVALIDARG);
        TEST("V2_MovesMayBeNull", SUCCEEDED(context2->RealizeToIncremental(shader.get(), target.get(), 0, drawn, 4, &count,
                                                                           nullptr, 0, nullptr)) && count == 1);
    }

    SL_TEST_CASE(TestEffectGraphIncrementalRealize, Tag::Gpu)
    {
        printf("\n=== EffectGraph 1.1: incremental realize ===\n");
        auto context = MakeContext();
        auto context2 = context.as<IEffectGraphContext2>();
        auto internal = context.as<IEffectGraphContextInternal>();
        auto texels = RampTexels(1.0f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);
        winrt::com_ptr<IEffectGraphNode> input, root;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Nit Map", root.put()));
        root->SetInput(0, input.get());
        auto input2 = input.as<IEffectGraphInput2>();
        const UINT bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        auto target = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, bind, nullptr, 0);
        auto reference = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, bind, nullptr, 0);
        const auto& stats = internal->Evaluator().RegionStats();
        // A whole render with nothing cached, as an incremental root is drawn:
        // a cached root reaches the target through an intermediate and rounds
        // differently.
        const auto wholeRender = [&]
        {
            const auto saved = internal->Evaluator().Options();
            auto uncached = saved;
            uncached.effectOutputCaching = false;
            internal->Evaluator().SetOptions(uncached);
            const HRESULT hr = context->RealizeTo(root.get(), reference.get());
            internal->Evaluator().SetOptions(saved);
            return SUCCEEDED(hr) ? ReadBack(reference.get(), 4) : std::vector<uint8_t>{};
        };

        // The first realize into a target is whole, reported as one rect.
        const auto first = RealizeIncremental(context2.get(), root.get(), target.get());
        TEST("Incr_FirstIsWhole", first.hr == S_OK && first.drawn.size() == 1 && RectIs(first.drawn[0], 0, 0, cWidth, cHeight) &&
                                  first.moved == 0 && stats.wholeDraws == 1);
        // Nothing changed: nothing drawn. It counts as a realize, not a draw.
        const auto realizesBeforeIdle = stats.realizes, drawsBeforeIdle = stats.draws;
        const auto idle = RealizeIncremental(context2.get(), root.get(), target.get());
        TEST("Incr_NothingChangedDrawsNothing", idle.hr == S_OK && idle.drawn.empty());
        TEST("Incr_IdleRealizeIsNoDraw", stats.realizes == realizesBeforeIdle + 1 && stats.draws == drawsBeforeIdle);

        // New texels in one rect, reported: GetInvalidRects sees it on the
        // input and the root before any realize, and changes nothing.
        const RECT rect{ 8, 4, 24, 12 };
        Paint(texture.get(), rect, 600.0f);
        TEST("Incr_InvalidateRects", input2->InvalidateRects(&rect, 1) == S_OK);
        const auto onRoot = InvalidOf(root.get());
        const auto again = InvalidOf(root.get());
        const auto onInput = InvalidOf(input.get());
        TEST("Incr_GetInvalidRectsOnRoot", onRoot.hr == S_OK && !onRoot.whole && onRoot.rects.size() == 1 &&
                                           RectIs(onRoot.rects[0], 8, 4, 24, 12));
        TEST("Incr_GetInvalidRectsRepeats", again.hr == S_OK && again.rects.size() == 1 && RectIs(again.rects[0], 8, 4, 24, 12));
        TEST("Incr_GetInvalidRectsOnInput", onInput.hr == S_OK && onInput.rects.size() == 1 &&
                                            RectIs(onInput.rects[0], 8, 4, 24, 12));
        const auto* rootNode = internal->Model().FindNode(internal->NodeId(root.get()));
        const auto* inputNode = internal->Model().FindNode(internal->NodeId(input.get()));
        TEST("Incr_GetInvalidRectsChangesNothing", rootNode && !rootNode->IsDirty() && inputNode && inputNode->IsDirty() &&
                                                   inputNode->Region().IsRects() &&
                                                   internal->Evaluator().PendingRegion(internal->NodeId(root.get())).IsNone());
        {
            UINT32 count = 0;
            BOOL whole = TRUE;
            const HRESULT tooSmall = root.as<IEffectGraphNode2>()->GetInvalidRects(nullptr, 0, &count, &whole);
            TEST("Incr_GetInvalidRectsSmallBuffer", tooSmall == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && count == 1 &&
                                                    !whole);
        }

        // The realize draws that rect only; nothing outside it changes, and
        // the target equals a whole render.
        const auto before = ReadBack(target.get(), 4);
        const auto changed = RealizeIncremental(context2.get(), root.get(), target.get());
        const auto after = ReadBack(target.get(), 4);
        const auto [moved, movedOutside] = PixelsDiffering(before, after, { rect });
        const auto [notWhole, notWholeOutside] = PixelsDiffering(after, wholeRender(), {});
        printf("  [info] one rect: drew %zu rect(s); %zu pixel(s) changed, %zu outside it; %zu differ from a whole render\n",
               changed.drawn.size(), moved, movedOutside, notWhole);
        TEST("Incr_DrawsTheRect", changed.hr == S_OK && changed.drawn.size() == 1 && RectIs(changed.drawn[0], 8, 4, 24, 12));
        TEST("Incr_TheChangeShows", moved > 0);
        TEST("Incr_NothingOutsideTheRect", movedOutside == 0);
        TEST("Incr_EqualsWholeRender", notWhole == 0 && notWholeOutside == 0);
        const auto drained = InvalidOf(root.get());
        const auto drainedInput = InvalidOf(input.get());
        TEST("Incr_AcknowledgedAfterDraw", drained.hr == S_OK && drained.rects.empty() && !drained.whole &&
                                           drainedInput.rects.empty() && !drainedInput.whole);

        // Rects from two calls before a realize both draw.
        const RECT a{ 0, 0, 4, 4 }, b{ 40, 20, 48, 30 };
        Paint(texture.get(), a, 50.0f);
        Paint(texture.get(), b, 900.0f);
        input2->InvalidateRects(&a, 1);
        input2->InvalidateRects(&b, 1);
        const auto two = RealizeIncremental(context2.get(), root.get(), target.get());
        TEST("Incr_TwoCallsBothDraw", two.hr == S_OK && two.drawn.size() == 2 &&
                                      PixelsDiffering(ReadBack(target.get(), 4), wholeRender(), {}).first == 0);

        // More rects than the caller has room for: drawn as their bounding box.
        const RECT three[] = { { 0, 0, 2, 2 }, { 10, 10, 12, 12 }, { 30, 0, 32, 4 } };
        input2->InvalidateRects(three, 3);
        const auto boxed = RealizeIncremental(context2.get(), root.get(), target.get(), EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL, 2);
        TEST("Incr_OverCapacityDrawsTheBoundingBox", boxed.hr == S_OK && boxed.drawn.size() == 1 &&
                                                     RectIs(boxed.drawn[0], 0, 0, 32, 12));

        // Rects covering the texture are the whole of it.
        const RECT halves[] = { { 0, 0, 32, 32 }, { 32, 0, 64, 32 } };
        input2->InvalidateRects(halves, 2);
        const auto covering = InvalidOf(root.get());
        const auto coveringDraw = RealizeIncremental(context2.get(), root.get(), target.get());
        TEST("Incr_CoveringIsWhole", covering.whole && coveringDraw.hr == S_OK && coveringDraw.drawn.size() == 1 &&
                                     RectIs(coveringDraw.drawn[0], 0, 0, cWidth, cHeight));

        // A property change on the root: whole.
        root->SetValueByName(L"Opacity", 0.5f);
        TEST("Incr_PropertyChangeIsWhole", InvalidOf(root.get()).whole);
        const auto property = RealizeIncremental(context2.get(), root.get(), target.get());
        TEST("Incr_PropertyChangeDrawsWhole", property.hr == S_OK && property.drawn.size() == 1 &&
                                              RectIs(property.drawn[0], 0, 0, cWidth, cHeight) &&
                                              PixelsDiffering(ReadBack(target.get(), 4), wholeRender(), {}).first == 0);
        // The frames after it draw nothing, and a cached RealizeTo since
        // changes nothing the target holds: the root stays uncached.
        {
            const auto cached = context->RealizeTo(root.get(), reference.get());
            const auto cachedPixels = ReadBack(reference.get(), 4);
            const auto settled = RealizeIncremental(context2.get(), root.get(), target.get());
            const auto held = ReadBack(target.get(), 4);
            printf("  [info] a cached RealizeTo of the same state differs in %zu of %u pixels\n",
                   PixelsDiffering(held, cachedPixels, {}).first, cWidth * cHeight);
            TEST("Incr_SettledEqualsUncachedWhole", SUCCEEDED(cached) && settled.hr == S_OK && settled.drawn.empty() &&
                                                    PixelsDiffering(held, wholeRender(), {}).first == 0);
        }

        // Without the flag, into another target, or after RealizeTo wrote the
        // target: whole.
        input2->InvalidateRects(&rect, 1);
        const auto noFlag = RealizeIncremental(context2.get(), root.get(), target.get(), EFFECTGRAPH_REALIZE_FLAGS_NONE);
        TEST("Incr_NoFlagIsWhole", noFlag.hr == S_OK && noFlag.drawn.size() == 1 && RectIs(noFlag.drawn[0], 0, 0, cWidth, cHeight));
        auto other = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, bind, nullptr, 0);
        input2->InvalidateRects(&rect, 1);
        const auto otherTarget = RealizeIncremental(context2.get(), root.get(), other.get());
        TEST("Incr_NewTargetIsWhole", otherTarget.hr == S_OK && otherTarget.drawn.size() == 1 &&
                                      RectIs(otherTarget.drawn[0], 0, 0, cWidth, cHeight));
        input2->InvalidateRects(&rect, 1);
        const auto back = RealizeIncremental(context2.get(), root.get(), target.get());
        TEST("Incr_BackToTheOldTargetIsWhole", back.hr == S_OK && back.drawn.size() == 1 &&
                                               RectIs(back.drawn[0], 0, 0, cWidth, cHeight));
        context->RealizeTo(root.get(), target.get());
        input2->InvalidateRects(&rect, 1);
        const auto overwritten = RealizeIncremental(context2.get(), root.get(), target.get());
        TEST("Incr_AfterRealizeToIsWhole", overwritten.hr == S_OK && overwritten.drawn.size() == 1 &&
                                           RectIs(overwritten.drawn[0], 0, 0, cWidth, cHeight));

        // A D2D built-in off the allowlist (Gaussian Blur) makes a rect whole
        // after it; an allowlisted one (Color Matrix, NodeContracts'
        // RegionIdentityBuiltIns) keeps it.
        // A two-input allowlisted one (Composite) keeps it on the pin it changed on.
        winrt::com_ptr<IEffectGraphNode> blur, afterBlur, matrix, afterMatrix, backdrop, composite, afterComposite;
        winrt::check_hresult(context->CreateD2DEffect(CLSID_D2D1GaussianBlur, blur.put()));
        winrt::check_hresult(context->CreateEffect(L"Nit Map", afterBlur.put()));
        winrt::check_hresult(context->CreateD2DEffect(CLSID_D2D1ColorMatrix, matrix.put()));
        winrt::check_hresult(context->CreateEffect(L"Nit Map", afterMatrix.put()));
        auto backdropTexels = RampTexels(0.5f);
        auto backdropTexture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, backdropTexels.data(),
                                           cWidth * 8);
        winrt::check_hresult(context->CreateTextureInput(backdropTexture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR,
                                                         backdrop.put()));
        winrt::check_hresult(context->CreateD2DEffect(CLSID_D2D1Composite, composite.put()));
        winrt::check_hresult(context->CreateEffect(L"Nit Map", afterComposite.put()));
        blur->SetInput(0, input.get());
        afterBlur->SetInput(0, blur.get());
        matrix->SetInput(0, input.get());
        afterMatrix->SetInput(0, matrix.get());
        composite->SetInput(0, backdrop.get());
        composite->SetInput(1, input.get());
        afterComposite->SetInput(0, composite.get());
        auto blurTarget = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, bind, nullptr, 0);
        auto matrixTarget = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, bind, nullptr, 0);
        auto compositeTarget = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, bind, nullptr, 0);
        RealizeIncremental(context2.get(), afterBlur.get(), blurTarget.get());
        RealizeIncremental(context2.get(), afterMatrix.get(), matrixTarget.get());
        RealizeIncremental(context2.get(), afterComposite.get(), compositeTarget.get());
        RealizeIncremental(context2.get(), root.get(), target.get());
        const auto matrixBefore = ReadBack(matrixTarget.get(), 4);
        const auto compositeBefore = ReadBack(compositeTarget.get(), 4);
        // New texels in the rect, in another Nit Map band (Orange at 600
        // nits, Red at 2500), so a rect draw that serves stale pixels shows.
        Paint(texture.get(), rect, 2500.0f);
        input2->InvalidateRects(&rect, 1);
        const auto throughBlur = RealizeIncremental(context2.get(), afterBlur.get(), blurTarget.get());
        const auto throughMatrix = RealizeIncremental(context2.get(), afterMatrix.get(), matrixTarget.get());
        const auto throughComposite = RealizeIncremental(context2.get(), afterComposite.get(), compositeTarget.get());
        const auto direct = RealizeIncremental(context2.get(), root.get(), target.get());
        TEST("Incr_WholeThroughD2DBuiltIn", throughBlur.hr == S_OK && throughBlur.drawn.size() == 1 &&
                                            RectIs(throughBlur.drawn[0], 0, 0, cWidth, cHeight));
        TEST("Incr_RectsThroughAllowlistedBuiltIn", throughMatrix.hr == S_OK && throughMatrix.drawn.size() == 1 &&
                                                    RectIs(throughMatrix.drawn[0], 8, 4, 24, 12));
        TEST("Incr_RectsThroughTwoInputBuiltIn", throughComposite.hr == S_OK && throughComposite.drawn.size() == 1 &&
                                                 RectIs(throughComposite.drawn[0], 8, 4, 24, 12));
        {
            // Each allowlisted chain changed inside the rect only, and its
            // rect draw equals its whole render.
            const auto uncachedWhole = [&](IEffectGraphNode* node)
            {
                const auto saved = internal->Evaluator().Options();
                auto uncached = saved;
                uncached.effectOutputCaching = false;
                internal->Evaluator().SetOptions(uncached);
                const HRESULT hr = context->RealizeTo(node, reference.get());
                internal->Evaluator().SetOptions(saved);
                return SUCCEEDED(hr) ? ReadBack(reference.get(), 4) : std::vector<uint8_t>{};
            };
            const auto matrixAfter = ReadBack(matrixTarget.get(), 4);
            const auto compositeAfter = ReadBack(compositeTarget.get(), 4);
            const auto [matrixMoved, matrixOutside] = PixelsDiffering(matrixBefore, matrixAfter, { rect });
            const auto [compositeMoved, compositeOutside] = PixelsDiffering(compositeBefore, compositeAfter, { rect });
            printf("  [info] through Color Matrix: %zu pixel(s) changed, %zu outside the rect; through Composite: %zu, %zu\n",
                   matrixMoved, matrixOutside, compositeMoved, compositeOutside);
            TEST("Incr_AllowlistedBuiltInChangeShows", matrixMoved > 0 && matrixOutside == 0);
            TEST("Incr_AllowlistedBuiltInEqualsWhole",
                 PixelsDiffering(matrixAfter, uncachedWhole(afterMatrix.get()), {}).first == 0);
            TEST("Incr_TwoInputBuiltInChangeShows", compositeMoved > 0 && compositeOutside == 0);
            TEST("Incr_TwoInputBuiltInEqualsWhole",
                 PixelsDiffering(compositeAfter, uncachedWhole(afterComposite.get()), {}).first == 0);
        }
        TEST("Incr_SecondRootKeepsItsRects", direct.hr == S_OK && direct.drawn.size() == 1 && RectIs(direct.drawn[0], 8, 4, 24, 12));

        // The switch off: whole.
        auto options = internal->Evaluator().Options();
        options.dirtyRegions = false;
        internal->Evaluator().SetOptions(options);
        input2->InvalidateRects(&rect, 1);
        const auto switchedOff = RealizeIncremental(context2.get(), root.get(), target.get());
        TEST("Incr_SwitchOffDrawsWhole", switchedOff.hr == S_OK && switchedOff.drawn.size() == 1 &&
                                         RectIs(switchedOff.drawn[0], 0, 0, cWidth, cHeight));

        // Back on: the switch-off draw had the root cached, which rounds
        // differently, so the first realize after it draws whole once.
        options.dirtyRegions = true;
        internal->Evaluator().SetOptions(options);
        input2->InvalidateRects(&rect, 1);
        const auto switchedOn = RealizeIncremental(context2.get(), root.get(), target.get());
        TEST("Incr_SwitchBackOnDrawsWholeOnce", switchedOn.hr == S_OK && switchedOn.drawn.size() == 1 &&
                                                RectIs(switchedOn.drawn[0], 0, 0, cWidth, cHeight) &&
                                                PixelsDiffering(ReadBack(target.get(), 4), wholeRender(), {}).first == 0);

        // verifyDirtyRegions checks each incremental draw against the target
        // before it and a whole render.
        options.verifyDirtyRegions = true;
        internal->Evaluator().SetOptions(options);
        const auto verifiedBefore = stats.verified;
        const auto drawsBefore = stats.draws;
        const auto pixelsBefore = stats.pixelsDrawn;
        const auto targetBefore = stats.pixelsTarget;
        Paint(texture.get(), rect, 120.0f);
        input2->InvalidateRects(&rect, 1);
        const auto verified = RealizeIncremental(context2.get(), root.get(), target.get());
        printf("  [info] verified %llu draw(s): %llu pixel(s) changed outside the rects, %llu differ from a whole render\n",
               static_cast<unsigned long long>(stats.verified - verifiedBefore),
               static_cast<unsigned long long>(stats.verifyOutsideMismatches),
               static_cast<unsigned long long>(stats.verifyWholeMismatches));
        TEST("Incr_VerifyRuns", verified.hr == S_OK && stats.verified == verifiedBefore + 1);
        TEST("Incr_VerifyFindsNothing", stats.verifyOutsideMismatches == 0 && stats.verifyWholeMismatches == 0);
        TEST("Incr_RegionPixelsCounted", stats.draws == drawsBefore + 1 && stats.pixelsDrawn - pixelsBefore == 16 * 8 &&
                                         stats.pixelsTarget - targetBefore == cWidth * cHeight);

        // The clip canary: texels outside the rect change without being
        // reported. A clipped draw leaves the target there as it was, so it
        // differs from a whole render there, and the verifier says so.
        {
            const RECT unreported{ 40, 20, 44, 24 };
            const size_t at = (static_cast<size_t>(unreported.top) * cWidth + unreported.left) * 4;
            const auto targetBefore2 = ReadBack(target.get(), 4);
            const auto outsideBefore = stats.verifyOutsideMismatches, wholeBefore = stats.verifyWholeMismatches;
            Paint(texture.get(), unreported, 5.0f);
            Paint(texture.get(), rect, 300.0f);
            input2->InvalidateRects(&rect, 1);
            const auto clipped = RealizeIncremental(context2.get(), root.get(), target.get());
            const auto targetAfter = ReadBack(target.get(), 4);
            const auto whole = wholeRender();
            TEST("Incr_ClipLeavesAnUnreportedChange", clipped.hr == S_OK && whole.size() == targetAfter.size() &&
                                                      std::memcmp(&targetAfter[at], &targetBefore2[at], 4) == 0 &&
                                                      std::memcmp(&targetAfter[at], &whole[at], 4) != 0);
            TEST("Incr_VerifyFlagsAnUnreportedChange", stats.verifyWholeMismatches > wholeBefore &&
                                                       stats.verifyOutsideMismatches == outsideBefore);

            // A draw past its clip (fault injected): the outside check fires.
            Paint(texture.get(), unreported, 50.0f);
            input2->InvalidateRects(&rect, 1);
            ShaderLab::Api::Testing::g_unclipNextIncrementalDraw = true;
            const auto unclipped = RealizeIncremental(context2.get(), root.get(), target.get());
            const bool injected = !ShaderLab::Api::Testing::g_unclipNextIncrementalDraw;
            ShaderLab::Api::Testing::g_unclipNextIncrementalDraw = false;
            TEST("Incr_VerifyFlagsADrawPastItsClip", unclipped.hr == S_OK && injected &&
                                                     stats.verifyOutsideMismatches > outsideBefore);

            // The target changed behind the engine's back outside the rect:
            // the whole-render check fires.
            const auto wholeBeforeCorrupt = stats.verifyWholeMismatches;
            const uint8_t garbage[4] = { 1, 2, 3, 255 };
            const D3D11_BOX pixel{ 60, 28, 0, 61, 29, 1 };
            g_d3dContext->UpdateSubresource(target.get(), 0, &pixel, garbage, 4, 0);
            input2->InvalidateRects(&rect, 1);
            const auto overCorrupt = RealizeIncremental(context2.get(), root.get(), target.get());
            TEST("Incr_VerifyFlagsACorruptTarget", overCorrupt.hr == S_OK && stats.verifyWholeMismatches > wholeBeforeCorrupt);
            printf("  [info] verifier positive controls: %llu pixel(s) outside the rects, %llu not a whole render\n",
                   static_cast<unsigned long long>(stats.verifyOutsideMismatches - outsideBefore),
                   static_cast<unsigned long long>(stats.verifyWholeMismatches - wholeBefore));
        }
        options.verifyDirtyRegions = false;
        internal->Evaluator().SetOptions(options);

        // An image input clips to the image's bounds.
        winrt::com_ptr<ID2D1Device> d2dDevice;
        context->GetD2DDevice(d2dDevice.put());
        winrt::com_ptr<ID2D1DeviceContext> mine;
        winrt::check_hresult(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, mine.put()));
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        winrt::check_hresult(mine->CreateBitmap(D2D1::SizeU(cWidth, cHeight), texels.data(), cWidth * 8, &props, bitmap.put()));
        winrt::com_ptr<IEffectGraphNode> image, imageRoot;
        winrt::check_hresult(context->CreateImageInput(bitmap.get(), image.put()));
        winrt::check_hresult(context->CreateEffect(L"Nit Map", imageRoot.put()));
        imageRoot->SetInput(0, image.get());
        auto imageTarget = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, bind, nullptr, 0);
        // Drawing a root acknowledges what feeds it, the image included.
        RealizeIncremental(context2.get(), imageRoot.get(), imageTarget.get());
        const RECT past{ 60, 30, 100, 100 };
        image.as<IEffectGraphInput2>()->InvalidateRects(&past, 1);
        const auto imageRects = InvalidOf(image.get());
        TEST("Incr_ImageInputClipsToBounds", imageRects.hr == S_OK && imageRects.rects.size() == 1 &&
                                             RectIs(imageRects.rects[0], 60, 30, cWidth, cHeight));
    }

    // ---- 1.1 stage 6: pre-render targets keep their own pending region ----

    // FP16 pixels (8 bytes each) that differ, and of them how many lie outside `rects`.
    std::pair<size_t, size_t> Fp16PixelsDiffering(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                                                  const std::vector<RECT>& rects)
    {
        size_t differing = 0, outside = 0;
        if (a.size() != b.size() || a.empty()) return { SIZE_MAX, SIZE_MAX };
        for (UINT y = 0; y < cHeight; ++y)
            for (UINT x = 0; x < cWidth; ++x)
            {
                const size_t at = (static_cast<size_t>(y) * cWidth + x) * 8;
                if (std::memcmp(&a[at], &b[at], 8) == 0) continue;
                ++differing;
                if (!Inside(rects, x, y)) ++outside;
            }
        return { differing, outside };
    }

    // Texture -> Nit Map (P, a pixel shader: not readable in place) -> two
    // Luminance Heatmaps (D3D11 compute with an image output), which share
    // P's pre-render target. Each consumer may be unneeded on the frame P
    // changes; its next dispatch must still see every change since the
    // target was last drawn, and draw only that.
    SL_TEST_CASE(TestEffectGraphPreRenderRegions, Tag::Gpu)
    {
        printf("\n=== EffectGraph 1.1: pre-render target regions ===\n");
        const UINT bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        auto texels = RampTexels(1.0f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);

        struct Chain
        {
            winrt::com_ptr<IEffectGraphContext> context;
            winrt::com_ptr<IEffectGraphNode> input, producer, first, second;
        };
        const auto build = [&](ID3D11Texture2D* source)
        {
            Chain chain;
            chain.context = MakeContext();
            winrt::check_hresult(chain.context->CreateTextureInput(source, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR,
                                                                   chain.input.put()));
            winrt::check_hresult(chain.context->CreateEffect(L"Nit Map", chain.producer.put()));
            winrt::check_hresult(chain.context->CreateEffect(L"Luminance Heatmap", chain.first.put()));
            winrt::check_hresult(chain.context->CreateEffect(L"Luminance Heatmap", chain.second.put()));
            chain.producer->SetInput(0, chain.input.get());
            chain.first->SetInput(0, chain.producer.get());
            chain.second->SetInput(0, chain.producer.get());
            return chain;
        };
        Chain chain = build(texture.get());
        auto context2 = chain.context.as<IEffectGraphContext2>();
        auto internal = chain.context.as<IEffectGraphContextInternal>();
        auto& evaluator = internal->Evaluator();
        const auto& pre = evaluator.PreRenders();
        const uint32_t producerId = internal->NodeId(chain.producer.get());
        auto input2 = chain.input.as<IEffectGraphInput2>();

        auto producerTarget = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, bind, nullptr, 0);
        auto firstTarget = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, bind, nullptr, 0);
        auto secondTarget = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, bind, nullptr, 0);
        auto reference = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, bind, nullptr, 0);
        // A forced-whole run: a fresh context over the texture as it is now.
        const auto freshRender = [&](bool firstConsumer, float producerOpacity = 1.0f)
        {
            Chain fresh = build(texture.get());
            if (producerOpacity != 1.0f) fresh.producer->SetValueByName(L"Opacity", producerOpacity);
            const HRESULT hr = fresh.context->RealizeTo((firstConsumer ? fresh.first : fresh.second).get(), reference.get());
            return SUCCEEDED(hr) ? ReadBack(reference.get(), 8) : std::vector<uint8_t>{};
        };

        // First use draws the target whole.
        auto before = pre;
        const auto primed = RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
        RealizeIncremental(context2.get(), chain.second.get(), secondTarget.get());
        RealizeIncremental(context2.get(), chain.producer.get(), producerTarget.get());
        TEST("PreRender_FirstUseWhole", primed.hr == S_OK && pre.wholeDraws > before.wholeDraws &&
                                        evaluator.PreRenderPending(producerId).IsNone());
        TEST("PreRender_FirstEqualsFresh",
             Fp16PixelsDiffering(ReadBack(firstTarget.get(), 8), freshRender(true), {}).first == 0);

        // A rect with both consumers needed: one draw of that rect, shared.
        const RECT rect{ 8, 4, 24, 12 };
        Paint(texture.get(), rect, 600.0f);
        input2->InvalidateRects(&rect, 1);
        before = pre;
        const auto oneRect = RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
        const uint64_t rectPixels = static_cast<uint64_t>(rect.right - rect.left) * (rect.bottom - rect.top);
        printf("  [info] one rect: %llu draw(s), %llu whole, %llu rect(s), %llu of %llu target pixels drawn\n",
               static_cast<unsigned long long>(pre.draws - before.draws),
               static_cast<unsigned long long>(pre.wholeDraws - before.wholeDraws),
               static_cast<unsigned long long>(pre.rectsDrawn - before.rectsDrawn),
               static_cast<unsigned long long>(pre.pixelsDrawn - before.pixelsDrawn),
               static_cast<unsigned long long>(pre.pixelsTarget - before.pixelsTarget));
        TEST("PreRender_RectDrawsTheRect", oneRect.hr == S_OK && pre.draws == before.draws + 1 &&
                                           pre.wholeDraws == before.wholeDraws && pre.rectsDrawn == before.rectsDrawn + 1 &&
                                           pre.pixelsDrawn == before.pixelsDrawn + rectPixels);
        TEST("PreRender_RectEqualsFresh",
             Fp16PixelsDiffering(ReadBack(firstTarget.get(), 8), freshRender(true), {}).first == 0);
        // The other consumer finds the target current.
        before = pre;
        const auto shared = RealizeIncremental(context2.get(), chain.second.get(), secondTarget.get());
        TEST("PreRender_SecondConsumerReuses", shared.hr == S_OK && pre.draws == before.draws &&
                                               pre.reused == before.reused + 1);
        TEST("PreRender_SecondEqualsFresh",
             Fp16PixelsDiffering(ReadBack(secondTarget.get(), 8), freshRender(false), {}).first == 0);

        // Producer rects on frame N with both consumers unneeded (only the
        // producer is realized), a consumer needed on N + 1: its dispatch
        // draws the N rects, from the target's own region.
        const RECT later{ 30, 10, 46, 22 };
        Paint(texture.get(), later, 2500.0f);
        input2->InvalidateRects(&later, 1);
        RealizeIncremental(context2.get(), chain.producer.get(), producerTarget.get());
        const auto parked = evaluator.PreRenderPending(producerId);
        TEST("PreRender_UnneededConsumerParksTheRegion", parked.IsRects() && parked.Rects().size() == 1 &&
             parked.Rects()[0].rect.left == later.left && parked.Rects()[0].rect.top == later.top &&
             parked.Rects()[0].rect.right == later.right && parked.Rects()[0].rect.bottom == later.bottom);
        before = pre;
        const auto resumed = RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
        TEST("PreRender_NextFrameDrawsTheParkedRects", resumed.hr == S_OK && pre.draws == before.draws + 1 &&
             pre.wholeDraws == before.wholeDraws && pre.rectsDrawn == before.rectsDrawn + 1 &&
             evaluator.PreRenderPending(producerId).IsNone());
        TEST("PreRender_NextFrameEqualsFresh",
             Fp16PixelsDiffering(ReadBack(firstTarget.get(), 8), freshRender(true), {}).first == 0);
        TEST("PreRender_OtherConsumerEqualsFresh",
             RealizeIncremental(context2.get(), chain.second.get(), secondTarget.get()).hr == S_OK &&
             Fp16PixelsDiffering(ReadBack(secondTarget.get(), 8), freshRender(false), {}).first == 0);

        // Pixels changed inside `rect` between two readbacks.
        const auto changedInside = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const RECT& rect)
        {
            const auto [differ, outside] = Fp16PixelsDiffering(a, b, { rect });
            return differ == SIZE_MAX ? size_t{ 0 } : differ - outside;
        };

        // Rects on two frames with both consumers unneeded, a consumer
        // needed on the third: the target holds both, and one dispatch draws
        // each of them.
        {
            const RECT a{ 40, 2, 52, 10 }, b{ 2, 20, 10, 28 };
            const auto consumerBefore = ReadBack(firstTarget.get(), 8);
            Paint(texture.get(), a, 2500.0f);
            input2->InvalidateRects(&a, 1);
            RealizeIncremental(context2.get(), chain.producer.get(), producerTarget.get());
            Paint(texture.get(), b, 2500.0f);
            input2->InvalidateRects(&b, 1);
            RealizeIncremental(context2.get(), chain.producer.get(), producerTarget.get());
            const auto parkedTwo = evaluator.PreRenderPending(producerId);
            TEST("PreRender_TwoFramesParkTwoRects", parkedTwo.IsRects() && parkedTwo.Rects().size() == 2);
            before = pre;
            const auto both = RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
            const uint64_t areas = uint64_t{ 12 } * 8 + uint64_t{ 8 } * 8;
            printf("  [info] two parked rects: %llu draw(s), %llu rect(s), %llu pixels drawn\n",
                   static_cast<unsigned long long>(pre.draws - before.draws),
                   static_cast<unsigned long long>(pre.rectsDrawn - before.rectsDrawn),
                   static_cast<unsigned long long>(pre.pixelsDrawn - before.pixelsDrawn));
            TEST("PreRender_ParkedRectsAllDrawn", both.hr == S_OK && pre.draws == before.draws + 1 &&
                 pre.wholeDraws == before.wholeDraws && pre.rectsDrawn == before.rectsDrawn + 2 &&
                 pre.pixelsDrawn == before.pixelsDrawn + areas && evaluator.PreRenderPending(producerId).IsNone());
            const auto consumerAfter = ReadBack(firstTarget.get(), 8);
            TEST("PreRender_BothParkedRectsShow",
                 changedInside(consumerBefore, consumerAfter, a) > 0 && changedInside(consumerBefore, consumerAfter, b) > 0);
            TEST("PreRender_ParkedRectsEqualFresh", Fp16PixelsDiffering(consumerAfter, freshRender(true), {}).first == 0);
            TEST("PreRender_ParkedRectsSecondEqualsFresh",
                 RealizeIncremental(context2.get(), chain.second.get(), secondTarget.get()).hr == S_OK &&
                 Fp16PixelsDiffering(ReadBack(secondTarget.get(), 8), freshRender(false), {}).first == 0);
        }

        // Overlapping rects stay two entries and are drawn as two: the
        // counter counts the overlap twice, as GraphEvaluator.h documents.
        {
            const RECT overlap[2]{ { 0, 0, 16, 16 }, { 8, 8, 24, 24 } };
            Paint(texture.get(), overlap[0], 900.0f);
            Paint(texture.get(), overlap[1], 900.0f);
            input2->InvalidateRects(overlap, 2);
            before = pre;
            const auto overlapped = RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
            TEST("PreRender_OverlapDrawsBothRects", overlapped.hr == S_OK && pre.wholeDraws == before.wholeDraws &&
                 pre.rectsDrawn == before.rectsDrawn + 2 && pre.pixelsDrawn == before.pixelsDrawn + 512);
            TEST("PreRender_OverlapEqualsFresh",
                 Fp16PixelsDiffering(ReadBack(firstTarget.get(), 8), freshRender(true), {}).first == 0);
            RealizeIncremental(context2.get(), chain.second.get(), secondTarget.get());
        }

        // The producer pin serves another image at dispatch, nothing
        // invalidated and the size unchanged: the target is drawn whole, not
        // reused. Staged white-box: Evaluate republishes every visited
        // node's output, so the twin's image goes in after it, before the
        // dispatch resolves its inputs from the graph.
        {
            winrt::com_ptr<IEffectGraphNode> twin;
            winrt::check_hresult(chain.context->CreateEffect(L"Nit Map", twin.put()));
            twin->SetInput(0, chain.input.get());
            twin->SetValueByName(L"Opacity", 0.5f);
            auto twinTarget = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, bind, nullptr, 0);
            RealizeIncremental(context2.get(), twin.get(), twinTarget.get());
            auto& model = internal->Model();
            const uint32_t firstId = internal->NodeId(chain.first.get());
            auto* producerNode = model.FindNode(producerId);
            const auto* twinNode = model.FindNode(internal->NodeId(twin.get()));
            ID2D1Image* const own = producerNode ? producerNode->cachedOutput : nullptr;
            ID2D1Image* const twinImage = twinNode ? twinNode->cachedOutput : nullptr;
            TEST("PreRender_SwapStaged", own && twinImage && own != twinImage &&
                                         evaluator.PreRenderPending(producerId).IsNone());
            if (own && twinImage)
            {
                // The frame's passes as FrameExecutor::Run orders them;
                // RealizeIncremental then draws the consumer's result.
                ShaderLab::Graph::MarkNodeChanged(model, firstId);
                auto* dc = internal->DeviceContext();
                model.PropagateDirtyDownstream(evaluator.Options().dirtyRegions);
                const uint32_t roots[] = { firstId };
                model.ComputeNeeded(roots);
                evaluator.Evaluate(model, dc);
                if (model.HasDirtyNodes()) evaluator.Evaluate(model, dc);
                const bool pendingNone = evaluator.PreRenderPending(producerId).IsNone();
                producerNode = model.FindNode(producerId);
                if (producerNode) producerNode->cachedOutput = twinImage;
                before = pre;
                evaluator.ProcessDeferredCompute(model, dc);
                producerNode = model.FindNode(producerId);
                if (producerNode) producerNode->cachedOutput = own;
                printf("  [info] other image: %llu draw(s), %llu whole, %llu reused\n",
                       static_cast<unsigned long long>(pre.draws - before.draws),
                       static_cast<unsigned long long>(pre.wholeDraws - before.wholeDraws),
                       static_cast<unsigned long long>(pre.reused - before.reused));
                TEST("PreRender_OtherImageDrawsWhole", pendingNone && pre.draws == before.draws + 1 &&
                                                       pre.wholeDraws == before.wholeDraws + 1 && pre.reused == before.reused &&
                                                       pre.failures == before.failures);
                const auto swapped = RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
                TEST("PreRender_OtherImageEqualsFresh", swapped.hr == S_OK &&
                     Fp16PixelsDiffering(ReadBack(firstTarget.get(), 8), freshRender(true, 0.5f), {}).first == 0);
                // Its own image again: whole again.
                ShaderLab::Graph::MarkNodeChanged(model, firstId);
                before = pre;
                const auto restored = RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
                TEST("PreRender_OwnImageAgainDrawsWhole", restored.hr == S_OK && pre.wholeDraws == before.wholeDraws + 1);
                TEST("PreRender_OwnImageEqualsFresh",
                     Fp16PixelsDiffering(ReadBack(firstTarget.get(), 8), freshRender(true), {}).first == 0);
            }
        }

        // The draw is clipped: a texel changed outside the reported rect,
        // unreported, keeps its old pre-rendered value, which a whole render
        // would not.
        {
            const RECT unreported{ 60, 28, 61, 29 };
            const RECT reported{ 2, 20, 10, 28 };
            Paint(texture.get(), unreported, 7000.0f);
            Paint(texture.get(), reported, 90.0f);
            input2->InvalidateRects(&reported, 1);
            RealizeIncremental(context2.get(), chain.producer.get(), producerTarget.get());
            const auto clipped = RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
            const auto [differ, outside] = Fp16PixelsDiffering(ReadBack(firstTarget.get(), 8), freshRender(true),
                                                               { unreported });
            printf("  [info] clip canary: %zu pixel(s) differ from a fresh render, %zu outside the unreported texel\n",
                   differ, outside);
            TEST("PreRender_ClipLeavesAnUnreportedChange", clipped.hr == S_OK && differ == 1 && outside == 0);
            // Report it, and it shows.
            input2->InvalidateRects(&unreported, 1);
            RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
            TEST("PreRender_ReportedEqualsFresh",
                 Fp16PixelsDiffering(ReadBack(firstTarget.get(), 8), freshRender(true), {}).first == 0);
        }

        // A property change on the producer is whole.
        chain.producer->SetValueByName(L"Opacity", 0.5f);
        before = pre;
        RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
        TEST("PreRender_ProducerPropertyIsWhole", pre.wholeDraws == before.wholeDraws + 1);
        chain.producer->SetValueByName(L"Opacity", 1.0f);
        RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());

        // The switch off: whole on every dispatch, as before regions existed.
        auto options = evaluator.Options();
        options.dirtyRegions = false;
        evaluator.SetOptions(options);
        Paint(texture.get(), rect, 300.0f);
        input2->InvalidateRects(&rect, 1);
        before = pre;
        RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
        TEST("PreRender_SwitchOffDrawsWhole", pre.wholeDraws == before.wholeDraws + 1 && pre.rectsDrawn == before.rectsDrawn);
        TEST("PreRender_SwitchOffEqualsFresh",
             Fp16PixelsDiffering(ReadBack(firstTarget.get(), 8), freshRender(true), {}).first == 0);
        options.dirtyRegions = true;
        evaluator.SetOptions(options);
        // Another Nit Map band (Yellow at 300 nits, Red at 2500), so a stale
        // rect draw shows in the consumer.
        const auto switchedOffPixels = ReadBack(firstTarget.get(), 8);
        Paint(texture.get(), rect, 2500.0f);
        input2->InvalidateRects(&rect, 1);
        before = pre;
        RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
        const auto switchedOnPixels = ReadBack(firstTarget.get(), 8);
        TEST("PreRender_SwitchBackOnDrawsTheRect", pre.rectsDrawn == before.rectsDrawn + 1 &&
                                                   pre.wholeDraws == before.wholeDraws);
        TEST("PreRender_SwitchBackOnChangeShows", Fp16PixelsDiffering(switchedOffPixels, switchedOnPixels, {}).first > 0);
        TEST("PreRender_SwitchBackOnEqualsFresh",
             Fp16PixelsDiffering(switchedOnPixels, freshRender(true), {}).first == 0);
        TEST("PreRender_NoFailures", pre.failures == 0);

        // A failed draw leaves the target unknown (the hook fills it with
        // garbage): the next use draws it whole, not the pending rects over it.
        {
            const RECT failedRect{ 40, 2, 52, 10 }, nextRect{ 4, 20, 12, 30 };
            Paint(texture.get(), failedRect, 50.0f);
            input2->InvalidateRects(&failedRect, 1);
            before = pre;
            evaluator.FailNextPreRenderDrawForTest(true);
            const auto failed = RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
            const bool injected = !evaluator.FailNextPreRenderDrawArmed();
            evaluator.FailNextPreRenderDrawForTest(false);
            TEST("PreRender_InjectedFailureFails", injected && pre.failures == before.failures + 1 && failed.hr != S_OK &&
                                                   evaluator.PreRenderPending(producerId).IsWhole());
            Paint(texture.get(), nextRect, 900.0f);
            input2->InvalidateRects(&nextRect, 1);
            before = pre;
            const auto retried = RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
            TEST("PreRender_AfterFailureDrawsWhole", retried.hr == S_OK && pre.wholeDraws == before.wholeDraws + 1 &&
                                                     pre.rectsDrawn == before.rectsDrawn);
            TEST("PreRender_AfterFailureEqualsFresh",
                 Fp16PixelsDiffering(ReadBack(firstTarget.get(), 8), freshRender(true), {}).first == 0);
        }

        // A producer reset (its effect recreated: a new image) draws whole.
        {
            const RECT resetRect{ 16, 16, 24, 24 };
            internal->Evaluator().InvalidateNode(internal->Model(), producerId);
            Paint(texture.get(), resetRect, 150.0f);
            input2->InvalidateRects(&resetRect, 1);
            before = pre;
            const auto reset = RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
            TEST("PreRender_ProducerResetDrawsWhole", reset.hr == S_OK && pre.wholeDraws == before.wholeDraws + 1 &&
                                                      pre.rectsDrawn == before.rectsDrawn);
            TEST("PreRender_ProducerResetEqualsFresh",
                 Fp16PixelsDiffering(ReadBack(firstTarget.get(), 8), freshRender(true), {}).first == 0);
        }

        // The producer's output resized (rewired to a smaller texture): the
        // target is reallocated and drawn whole.
        {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            desc.Width = cWidth / 2;
            desc.Height = cHeight / 2;
            std::vector<uint16_t> smallTexels(static_cast<size_t>(desc.Width) * desc.Height * 4);
            for (UINT y = 0; y < desc.Height; ++y)
                std::memcpy(&smallTexels[static_cast<size_t>(y) * desc.Width * 4], &texels[static_cast<size_t>(y) * cWidth * 4],
                            desc.Width * 8);
            const D3D11_SUBRESOURCE_DATA init{ smallTexels.data(), desc.Width * 8, 0 };
            winrt::com_ptr<ID3D11Texture2D> smallTexture;
            winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, &init, smallTexture.put()));
            winrt::com_ptr<IEffectGraphNode> smallInput;
            winrt::check_hresult(chain.context->CreateTextureInput(smallTexture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR,
                                                                   smallInput.put()));
            chain.producer->SetInput(0, smallInput.get());
            before = pre;
            const auto resized = RealizeIncremental(context2.get(), chain.first.get(), firstTarget.get());
            TEST("PreRender_ResizeDrawsWhole", resized.hr == S_OK && pre.wholeDraws == before.wholeDraws + 1 &&
                                               pre.rectsDrawn == before.rectsDrawn &&
                                               pre.pixelsDrawn - before.pixelsDrawn == uint64_t{ desc.Width } * desc.Height);
            Chain fresh = build(smallTexture.get());
            const HRESULT freshHr = fresh.context->RealizeTo(fresh.first.get(), reference.get());
            // Compared over the smaller output only.
            const RECT smallRect{ 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
            const auto [differ, outside] = Fp16PixelsDiffering(ReadBack(firstTarget.get(), 8), ReadBack(reference.get(), 8),
                                                               { smallRect });
            TEST("PreRender_ResizeEqualsFresh", SUCCEEDED(freshHr) && differ != SIZE_MAX && differ - outside == 0);
        }

        // A producer whose image does not start at (0,0): the texture
        // composited over a blurred backdrop, whose soft border reaches past
        // the top-left edge. The texture's rects reach the Composite as they
        // are; the target's draws are offset by the image's origin.
        {
            auto backdropTexels = RampTexels(0.5f);
            auto backdropTexture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
                                               backdropTexels.data(), cWidth * 8);
            struct OffsetChain
            {
                winrt::com_ptr<IEffectGraphContext> context;
                winrt::com_ptr<IEffectGraphNode> input, backdrop, blur, producer, consumer;
            };
            const auto buildOffset = [&]()
            {
                OffsetChain c;
                c.context = MakeContext();
                winrt::check_hresult(c.context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR,
                                                                   c.input.put()));
                winrt::check_hresult(c.context->CreateTextureInput(backdropTexture.get(),
                                                                   EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, c.backdrop.put()));
                winrt::check_hresult(c.context->CreateD2DEffect(CLSID_D2D1GaussianBlur, c.blur.put()));
                winrt::check_hresult(c.context->CreateD2DEffect(CLSID_D2D1Composite, c.producer.put()));
                winrt::check_hresult(c.context->CreateEffect(L"Luminance Heatmap", c.consumer.put()));
                c.blur->SetInput(0, c.backdrop.get());
                c.producer->SetInput(0, c.blur.get());
                c.producer->SetInput(1, c.input.get());
                c.consumer->SetInput(0, c.producer.get());
                return c;
            };
            OffsetChain offset = buildOffset();
            auto offsetContext2 = offset.context.as<IEffectGraphContext2>();
            auto offsetInternal = offset.context.as<IEffectGraphContextInternal>();
            auto& offsetEvaluator = offsetInternal->Evaluator();
            const auto& offsetPre = offsetEvaluator.PreRenders();
            const uint32_t compositeId = offsetInternal->NodeId(offset.producer.get());
            auto offsetInput2 = offset.input.as<IEffectGraphInput2>();
            auto offsetTarget = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, bind, nullptr, 0);
            const auto freshOffset = [&]()
            {
                OffsetChain fresh = buildOffset();
                const HRESULT hr = fresh.context->RealizeTo(fresh.consumer.get(), reference.get());
                return SUCCEEDED(hr) ? ReadBack(reference.get(), 8) : std::vector<uint8_t>{};
            };

            auto offsetBefore = offsetPre;
            const auto primedOffset = RealizeIncremental(offsetContext2.get(), offset.consumer.get(), offsetTarget.get());
            // The Composite's image bounds, white-box.
            D2D1_RECT_F bounds{};
            {
                winrt::com_ptr<ID2D1Device> device;
                offset.context->GetD2DDevice(device.put());
                winrt::com_ptr<ID2D1DeviceContext> dc;
                const auto* node = offsetInternal->Model().FindNode(compositeId);
                if (device && SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put())) && node &&
                    node->cachedOutput)
                    dc->GetImageLocalBounds(node->cachedOutput, &bounds);
            }
            printf("  [info] offset producer: image bounds (%.1f, %.1f)-(%.1f, %.1f), %llu target pixels\n", bounds.left,
                   bounds.top, bounds.right, bounds.bottom,
                   static_cast<unsigned long long>(offsetPre.pixelsTarget - offsetBefore.pixelsTarget));
            TEST("PreRender_OffsetProducerFirstUseWhole", primedOffset.hr == S_OK && bounds.left < 0.0f && bounds.top < 0.0f &&
                 offsetPre.wholeDraws == offsetBefore.wholeDraws + 1 && offsetEvaluator.PreRenderPending(compositeId).IsNone());
            TEST("PreRender_OffsetFirstEqualsFresh",
                 Fp16PixelsDiffering(ReadBack(offsetTarget.get(), 8), freshOffset(), {}).first == 0);

            const RECT offsetRect{ 20, 6, 36, 18 };
            const auto offsetConsumerBefore = ReadBack(offsetTarget.get(), 8);
            Paint(texture.get(), offsetRect, 2500.0f);
            offsetInput2->InvalidateRects(&offsetRect, 1);
            offsetBefore = offsetPre;
            const auto offsetDrawn = RealizeIncremental(offsetContext2.get(), offset.consumer.get(), offsetTarget.get());
            const auto offsetConsumerAfter = ReadBack(offsetTarget.get(), 8);
            TEST("PreRender_OffsetDrawsTheRect", offsetDrawn.hr == S_OK && offsetPre.draws == offsetBefore.draws + 1 &&
                 offsetPre.wholeDraws == offsetBefore.wholeDraws && offsetPre.rectsDrawn == offsetBefore.rectsDrawn + 1 &&
                 offsetPre.pixelsDrawn == offsetBefore.pixelsDrawn + uint64_t{ 16 } * 12);
            TEST("PreRender_OffsetRectShows", changedInside(offsetConsumerBefore, offsetConsumerAfter, offsetRect) > 0);
            TEST("PreRender_OffsetEqualsFresh", Fp16PixelsDiffering(offsetConsumerAfter, freshOffset(), {}).first == 0);
            TEST("PreRender_OffsetNoFailures", offsetPre.failures == 0);
        }
    }

    // ---- 1.1 stage 2: the exact gate and NO_FALLBACK ----------------------

    // A pixel shader whose output is its input scaled by Level / 1000, Level
    // gpu-bindable: the bound consumer the gate cases watch.
    ShaderLab::Effects::ShaderLabEffectDescriptor GateConsumerDescriptor()
    {
        ShaderLab::Effects::ShaderLabEffectDescriptor d;
        d.name = L"Gate Test Consumer";
        d.effectId = L"Gate Test Consumer";
        d.effectVersion = 1;
        d.shaderType = ShaderLab::Graph::CustomShaderType::PixelShader;
        d.inputNames = { L"Source" };
        d.hlslSource = R"HLSL(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
SHADERLAB_GPU_BUFFER(Level, t1)
SamplerState InputSampler : register(s0);
cbuffer Constants : register(b0)
{
    SHADERLAB_PARAM(float, Level)
};
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    SHADERLAB_LOAD_PARAM(float, Level)
    float4 c = Source.Sample(InputSampler, uv0.xy);
    return float4(c.rgb * (Level / 1000.0), c.a);
}
)HLSL";
        d.parameters = {
            ShaderLab::Graph::ParameterDefinition{ L"Level", L"float", 0.0f, 0.0f, 10000.0f, 1.0f, {}, L"", true },
        };
        return d;
    }

    winrt::com_ptr<IEffectGraphNode> AddGateConsumer(IEffectGraphContextInternal* internal)
    {
        static const auto descriptor = GateConsumerDescriptor();
        const uint32_t id = internal->Model().AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(descriptor));
        winrt::com_ptr<IEffectGraphNode> node;
        winrt::check_hresult(internal->NodeFor(id, node.put()));
        return node;
    }

    bool RegionIsRect(const ShaderLab::Graph::DirtyRegion& region, const RECT& rect)
    {
        if (!region.IsRects() || region.Rects().size() != 1) return false;
        const auto& r = region.Rects()[0].rect;
        return r.left == rect.left && r.top == rect.top && r.right == rect.right && r.bottom == rect.bottom;
    }

    bool SameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

    // A context with the readback skip off, so every dispatch's values reach
    // the CPU in its own frame and the gate can judge them.
    winrt::com_ptr<IEffectGraphContextInternal> MakeGateContext(bool dirtyRegions = true)
    {
        auto context = MakeContext().as<IEffectGraphContextInternal>();
        auto options = context->Evaluator().Options();
        options.skipUnneededCpuReadback = false;
        options.dirtyRegions = dirtyRegions;
        if (!dirtyRegions) options.effectOutputCaching = false;
        context->Evaluator().SetOptions(options);
        return context;
    }

    // ClippedFraction of `texture` from a fresh context that renders whole:
    // with ClipNits set, or bound to a Working Space under `sdrWhite`.
    float ForcedWholeClippedFraction(ID3D11Texture2D* texture, float clipNits, float sdrWhite)
    {
        auto context = MakeGateContext(false);
        auto profile = ShaderLab::Rendering::PresetP3_1000();
        profile.caps.sdrWhiteLevelNits = sdrWhite;
        context->SetDisplayProfile(profile);
        winrt::com_ptr<IEffectGraphNode> input, stats, workingSpace;
        winrt::check_hresult(context->CreateTextureInput(texture, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        stats->SetInput(0, input.get());
        if (clipNits > 0.0f)
        {
            stats->SetValueByName(L"ClipNits", clipNits);
        }
        else
        {
            winrt::check_hresult(context->CreateEffect(L"Working Space", workingSpace.put()));
            winrt::check_hresult(stats->Bind(L"ClipNits", workingSpace.get(), L"SdrWhiteNits"));
        }
        return FieldOf(stats.get(), L"ClippedFraction");
    }

    // Max of `texture` from a fresh context.
    float FreshMax(ID3D11Texture2D* texture)
    {
        auto context = MakeGateContext(false);
        winrt::com_ptr<IEffectGraphNode> input, stats;
        winrt::check_hresult(context->CreateTextureInput(texture, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        stats->SetInput(0, input.get());
        return FieldOf(stats.get(), L"Max");
    }

    bool MatchesFresh(const std::vector<uint8_t>& pixels, const std::vector<uint8_t>& fresh)
    {
        return !fresh.empty() && pixels.size() == fresh.size() && PixelsDiffering(pixels, fresh, {}).first == 0;
    }

    // A gate consumer drawn from `drawn`, Level bound to the Max of Luminance
    // Statistics over `measured`, rendered whole by a fresh context: no state
    // shared with the context under test, so a consumer it left asleep on a
    // stale Level shows up as differing pixels.
    std::vector<uint8_t> FreshGateConsumerRender(ID3D11Texture2D* drawn, ID3D11Texture2D* measured)
    {
        auto context = MakeGateContext(false);
        auto profile = ShaderLab::Rendering::PresetP3_1000();
        profile.caps.sdrWhiteLevelNits = 240.0f;
        context->SetDisplayProfile(profile);
        winrt::com_ptr<IEffectGraphNode> drawnInput, measuredInput, stats;
        winrt::check_hresult(context->CreateTextureInput(drawn, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, drawnInput.put()));
        if (measured == drawn)
            measuredInput = drawnInput;
        else
            winrt::check_hresult(context->CreateTextureInput(measured, EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, measuredInput.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        auto consumer = AddGateConsumer(context.get());
        if (FAILED(stats->SetInput(0, measuredInput.get())) || FAILED(consumer->SetInput(0, drawnInput.get())) ||
            FAILED(consumer->Bind(L"Level", stats.get(), L"Max")))
            return {};
        auto target = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                  nullptr, 0);
        // A new custom effect needs two passes, and lane 3 serves from the
        // frame after the first publish.
        for (int pass = 0; pass < 3; ++pass)
            if (FAILED(context->RealizeTo(consumer.get(), target.get()))) return {};
        return ReadBack(target.get(), 4);
    }

    // Stage 2's gate through the API, readback skip off: texture A feeds
    // Luminance Statistics and a consumer drawn from A; a second consumer is
    // drawn from texture B. Both bind Level <- Max. A rect on A that leaves
    // Max bit-identical draws the A consumer's rect only and leaves the B
    // consumer clean; one that raises Max redraws both whole.
    SL_TEST_CASE(TestEffectGraphAnalysisGate, Tag::Gpu)
    {
        printf("\n=== EffectGraph 1.1 stage 2: the analysis change gate ===\n");
        auto internal = MakeGateContext();
        auto context2 = internal.as<IEffectGraphContext2>();
        auto& evaluator = internal->Evaluator();
        const auto& gate = evaluator.BindingGate();
        const auto& readbacks = evaluator.ReadbackStats();
        auto profile = ShaderLab::Rendering::PresetP3_1000();
        profile.caps.sdrWhiteLevelNits = 240.0f;
        internal->SetDisplayProfile(profile);

        auto texelsA = RampTexels(1.0f), texelsB = RampTexels(0.5f);
        auto textureA = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texelsA.data(), cWidth * 8);
        auto textureB = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texelsB.data(), cWidth * 8);
        winrt::com_ptr<IEffectGraphNode> inputA, inputB, stats;
        winrt::check_hresult(internal->CreateTextureInput(textureA.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, inputA.put()));
        winrt::check_hresult(internal->CreateTextureInput(textureB.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, inputB.put()));
        winrt::check_hresult(internal->CreateEffect(L"Luminance Statistics", stats.put()));
        auto shared = AddGateConsumer(internal.get());
        auto other = AddGateConsumer(internal.get());
        const bool wired = SUCCEEDED(stats->SetInput(0, inputA.get())) && SUCCEEDED(shared->SetInput(0, inputA.get())) &&
                           SUCCEEDED(other->SetInput(0, inputB.get())) &&
                           SUCCEEDED(shared->Bind(L"Level", stats.get(), L"Max")) &&
                           SUCCEEDED(other->Bind(L"Level", stats.get(), L"Max"));
        TEST("Gate_GraphWired", wired);
        if (!wired) return;
        const uint32_t statsId = internal->NodeId(stats.get()), otherId = internal->NodeId(other.get());
        auto inputA2 = inputA.as<IEffectGraphInput2>();
        const UINT bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        auto sharedTarget = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, bind, nullptr, 0);
        auto otherTarget = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, bind, nullptr, 0);
        auto reference = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, bind, nullptr, 0);
        const auto maxNow = [&] { return AnalysisField(internal->Model().FindNode(statsId), L"Max"); };
        // A whole render with nothing cached, as an incremental root is drawn.
        const auto wholeRender = [&](IEffectGraphNode* root)
        {
            const auto saved = evaluator.Options();
            auto uncached = saved;
            uncached.effectOutputCaching = false;
            evaluator.SetOptions(uncached);
            const HRESULT hr = internal->RealizeTo(root, reference.get());
            evaluator.SetOptions(saved);
            return SUCCEEDED(hr) ? ReadBack(reference.get(), 4) : std::vector<uint8_t>{};
        };
        const auto realizeBoth = [&](Incremental& a, Incremental& b)
        {
            a = RealizeIncremental(context2.get(), shared.get(), sharedTarget.get());
            b = RealizeIncremental(context2.get(), other.get(), otherTarget.get());
        };
        Incremental primeA, primeB;
        realizeBoth(primeA, primeB);
        realizeBoth(primeA, primeB);   // lane 3 serves from the frame after the first publish
        const float firstMax = maxNow();
        TEST("Gate_Primed", SUCCEEDED(primeA.hr) && SUCCEEDED(primeB.hr) && firstMax > 100.0f);

        // A rect that leaves Max bit-identical: the shared consumer draws its
        // rect, the other one nothing, and its change count does not move.
        const RECT dim{ 0, 0, 4, 4 };
        Paint(textureA.get(), dim, 50.0f);
        auto gateBefore = gate;
        auto readbacksBefore = readbacks;
        uint64_t otherChanges = evaluator.OutputChangeCount(otherId);
        TEST("Gate_InvalidateRects", inputA2->InvalidateRects(&dim, 1) == S_OK);
        Incremental a, b;
        a = RealizeIncremental(context2.get(), shared.get(), sharedTarget.get());
        const auto otherPending = InvalidOf(other.get());
        b = RealizeIncremental(context2.get(), other.get(), otherTarget.get());
        printf("  [info] unchanged Max %.4f -> %.4f: shared drew %zu rect(s), other %zu; gate +%llu unchanged, +%llu changed, "
               "+%llu whole, +%llu no value\n", firstMax, maxNow(), a.drawn.size(), b.drawn.size(),
               static_cast<unsigned long long>(gate.unchanged - gateBefore.unchanged),
               static_cast<unsigned long long>(gate.changed - gateBefore.changed),
               static_cast<unsigned long long>(gate.whole - gateBefore.whole),
               static_cast<unsigned long long>(gate.noValue - gateBefore.noValue));
        TEST("Gate_MaxBitUnchanged", SameBits(maxNow(), firstMax));
        TEST("DirtyRegion_GateStatsInputRegionIsTheRect", RegionIsRect(evaluator.DispatchInputRegion(statsId), dim));
        TEST("Gate_UnchangedCounted", gate.unchanged == gateBefore.unchanged + 1 && gate.changed == gateBefore.changed &&
                                      gate.whole == gateBefore.whole && gate.noValue == gateBefore.noValue);
        TEST("DirtyRegion_GateSharedConsumerDrawsTheRect", a.hr == S_OK && a.drawn.size() == 1 && RectIs(a.drawn[0], 0, 0, 4, 4));
        TEST("DirtyRegion_GateOtherConsumerRegionEmpty", otherPending.hr == S_OK && otherPending.rects.empty() &&
                                                          !otherPending.whole && b.hr == S_OK && b.drawn.empty());
        TEST("GpuBinding_GateConsumerChangeCountFlat", evaluator.OutputChangeCount(otherId) == otherChanges);
        // Same context: checks the rect draw, not the binding state.
        TEST("Gate_SharedRectDrawEqualsWholeRender",
             PixelsDiffering(ReadBack(sharedTarget.get(), 4), wholeRender(shared.get()), {}).first == 0);
        TEST("Gate_SharedEqualsFreshRenderUnchanged",
             MatchesFresh(ReadBack(sharedTarget.get(), 4), FreshGateConsumerRender(textureA.get(), textureA.get())));
        TEST("Gate_OtherEqualsFreshRenderUnchanged",
             MatchesFresh(ReadBack(otherTarget.get(), 4), FreshGateConsumerRender(textureB.get(), textureA.get())));
        TEST("Gate_NoFallbackReadback", readbacks.fallbackReadbacks == readbacksBefore.fallbackReadbacks &&
                                        readbacks.unskippedReadbacks > readbacksBefore.unskippedReadbacks);

        // A rect that raises Max: both consumers redraw whole and the other's
        // change count moves. Binding resolution at a visit after the dispatch
        // would wake a consumer whatever the gate did, so the gate's wake is
        // checked where only it can act: the shared consumer right after the
        // dispatch, before the frozen pass; the other one, which the shared
        // frame never visits, with its stored Level staged to the new Max
        // (from a fresh context) so its own visit resolves no change either.
        const RECT bright{ 60, 28, 64, 32 };
        Paint(textureA.get(), bright, 2000.0f);
        const float newMax = FreshMax(textureA.get());
        const uint32_t sharedId = internal->NodeId(shared.get());
        internal->Model().FindNode(otherId)->properties[L"Level"] = newMax;   // white-box: a CPU copy, nothing dirtied
        gateBefore = gate;
        otherChanges = evaluator.OutputChangeCount(otherId);
        inputA2->InvalidateRects(&bright, 1);
        bool sharedWoken = false;
        {
            // White-box: the shared frame's passes up to its dispatch, as
            // FrameExecutor::Run orders them; RealizeIncremental finishes it.
            auto& graph = internal->Model();
            auto* dc = internal->DeviceContext();
            graph.PropagateDirtyDownstream(evaluator.Options().dirtyRegions);
            const uint32_t roots[] = { sharedId };
            graph.ComputeNeeded(roots);
            evaluator.Evaluate(graph, dc);
            if (graph.HasDirtyNodes()) evaluator.Evaluate(graph, dc);
            const bool sharedCleanBefore = !graph.FindNode(sharedId)->IsDirty();
            evaluator.ProcessDeferredCompute(graph, dc);
            const auto* node = graph.FindNode(sharedId);
            sharedWoken = sharedCleanBefore && node->IsDirty() && node->Region().IsWhole();
        }
        a = RealizeIncremental(context2.get(), shared.get(), sharedTarget.get());
        // Not visited by the shared consumer's frame: pending only if the gate woke it.
        const auto otherWoken = InvalidOf(other.get());
        b = RealizeIncremental(context2.get(), other.get(), otherTarget.get());
        printf("  [info] changed Max %.4f (fresh context %.4f): shared drew %zu rect(s), other %zu\n", maxNow(), newMax,
               a.drawn.size(), b.drawn.size());
        TEST("Gate_MaxMoved", std::abs(maxNow() - 2000.0f) < 2.0f);
        TEST("Gate_StagedLevelIsTheNewMax", SameBits(maxNow(), newMax));
        TEST("Gate_ChangedCounted", gate.changed == gateBefore.changed + 1 && gate.unchanged == gateBefore.unchanged);
        TEST("Gate_ChangedWakesSharedConsumerAtDispatch", sharedWoken);
        TEST("Gate_ChangedWakesUnvisitedConsumer", otherWoken.hr == S_OK && otherWoken.whole);
        TEST("Gate_ChangedRedrawsSharedWhole", a.hr == S_OK && a.drawn.size() == 1 && RectIs(a.drawn[0], 0, 0, cWidth, cHeight));
        TEST("Gate_ChangedRedrawsOtherWhole", b.hr == S_OK && b.drawn.size() == 1 && RectIs(b.drawn[0], 0, 0, cWidth, cHeight));
        TEST("GpuBinding_GateConsumerChangeCountMoves", evaluator.OutputChangeCount(otherId) > otherChanges);
        TEST("Gate_SharedEqualsFreshRender",
             MatchesFresh(ReadBack(sharedTarget.get(), 4), FreshGateConsumerRender(textureA.get(), textureA.get())));
        TEST("Gate_OtherEqualsFreshRender",
             MatchesFresh(ReadBack(otherTarget.get(), 4), FreshGateConsumerRender(textureB.get(), textureA.get())));

        // The switch off: the same unchanged rect dirties every consumer.
        {
            auto options = evaluator.Options();
            options.analysisChangeGate = false;
            evaluator.SetOptions(options);
            Paint(textureA.get(), dim, 60.0f);
            gateBefore = gate;
            otherChanges = evaluator.OutputChangeCount(otherId);
            const float maxBefore = maxNow();
            inputA2->InvalidateRects(&dim, 1);
            realizeBoth(a, b);
            TEST("Gate_SwitchOffWakesEveryConsumer", SameBits(maxNow(), maxBefore) && b.hr == S_OK && b.drawn.size() == 1 &&
                                                     RectIs(b.drawn[0], 0, 0, cWidth, cHeight) &&
                                                     evaluator.OutputChangeCount(otherId) > otherChanges);
            TEST("Gate_SwitchOffCountsNothing", gate.unchanged == gateBefore.unchanged && gate.changed == gateBefore.changed);
            options.analysisChangeGate = true;
            evaluator.SetOptions(options);
            realizeBoth(a, b);
        }

        // A ClipNits edit on the same frame as a rect: a whole dispatch that
        // wakes every bound consumer. Stage 2's dispatch still reduces the whole
        // image whatever the region, so the forced-whole equality below cannot
        // catch a wrong region yet; it gains meaning with stage 4's cell mask.
        Paint(textureA.get(), dim, 70.0f);
        gateBefore = gate;
        otherChanges = evaluator.OutputChangeCount(otherId);
        stats->SetValueByName(L"ClipNits", 300.0f);
        inputA2->InvalidateRects(&dim, 1);
        realizeBoth(a, b);
        const float editedFraction = AnalysisField(internal->Model().FindNode(statsId), L"ClippedFraction");
        const float editedReference = ForcedWholeClippedFraction(textureA.get(), 300.0f, 240.0f);
        printf("  [info] ClipNits 300 with a rect: ClippedFraction %.6f, forced whole %.6f\n", editedFraction, editedReference);
        TEST("DirtyRegion_GateClipNitsEditDispatchesWhole", evaluator.DispatchInputRegion(statsId).IsWhole() &&
                                                            gate.whole == gateBefore.whole + 1);
        TEST("GpuBinding_GateClipNitsEditWakesConsumers", evaluator.OutputChangeCount(otherId) > otherChanges &&
                                                          b.hr == S_OK && b.drawn.size() == 1 &&
                                                          RectIs(b.drawn[0], 0, 0, cWidth, cHeight));
        TEST("DirtyRegion_GateClipNitsEditMatchesForcedWhole", editedFraction > 0.0f && SameBits(editedFraction, editedReference));

        // The same with ClipNits bound to a Working Space whose display
        // changes on the frame of the rect.
        winrt::com_ptr<IEffectGraphNode> workingSpace;
        winrt::check_hresult(internal->CreateEffect(L"Working Space", workingSpace.put()));
        TEST("Gate_ClipNitsBound", SUCCEEDED(stats->Bind(L"ClipNits", workingSpace.get(), L"SdrWhiteNits")));
        realizeBoth(a, b);
        const float boundBefore = AnalysisField(internal->Model().FindNode(statsId), L"ClippedFraction");
        profile.caps.sdrWhiteLevelNits = 400.0f;
        internal->SetDisplayProfile(profile);
        Paint(textureA.get(), dim, 80.0f);
        gateBefore = gate;
        otherChanges = evaluator.OutputChangeCount(otherId);
        inputA2->InvalidateRects(&dim, 1);
        realizeBoth(a, b);
        const float boundFraction = AnalysisField(internal->Model().FindNode(statsId), L"ClippedFraction");
        const float boundReference = ForcedWholeClippedFraction(textureA.get(), 0.0f, 400.0f);
        printf("  [info] bound ClipNits, SDR white 240 -> 400 with a rect: ClippedFraction %.6f -> %.6f, forced whole %.6f\n",
               boundBefore, boundFraction, boundReference);
        TEST("DirtyRegion_GateBoundClipNitsDispatchesWhole", evaluator.DispatchInputRegion(statsId).IsWhole() &&
                                                             gate.whole == gateBefore.whole + 1);
        TEST("GpuBinding_GateBoundClipNitsWakesConsumers", evaluator.OutputChangeCount(otherId) > otherChanges &&
                                                           b.hr == S_OK && b.drawn.size() == 1 &&
                                                           RectIs(b.drawn[0], 0, 0, cWidth, cHeight));
        TEST("DirtyRegion_GateBoundClipNitsMatchesForcedWhole",
             boundFraction != boundBefore && SameBits(boundFraction, boundReference));
        TEST("Gate_NoFallbackReadbackAnywhere", readbacks.fallbackReadbacks == 0 && readbacks.fallbackBuilds == 0);
    }

    // Stage 2's input region at the edges: a re-dirty between queue and
    // dispatch, a dropped queue, a failed dispatch, and a binding source
    // queued in the same frame. Each must consume Whole. Graph: A -> Stats1;
    // A -> Mid (Level <- Stats1.Max) -> Stats2; B -> Tail (Level <- Stats2.Max).
    SL_TEST_CASE(TestEffectGraphGateRegions, Tag::Gpu)
    {
        printf("\n=== EffectGraph 1.1 stage 2: dispatch input regions at the edges ===\n");
        auto internal = MakeGateContext();
        auto context2 = internal.as<IEffectGraphContext2>();
        auto& evaluator = internal->Evaluator();
        const auto& gate = evaluator.BindingGate();

        auto texelsA = RampTexels(1.0f), texelsB = RampTexels(0.5f);
        auto textureA = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texelsA.data(), cWidth * 8);
        auto textureB = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texelsB.data(), cWidth * 8);
        winrt::com_ptr<IEffectGraphNode> inputA, inputB, stats1, stats2;
        winrt::check_hresult(internal->CreateTextureInput(textureA.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, inputA.put()));
        winrt::check_hresult(internal->CreateTextureInput(textureB.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, inputB.put()));
        winrt::check_hresult(internal->CreateEffect(L"Luminance Statistics", stats1.put()));
        winrt::check_hresult(internal->CreateEffect(L"Luminance Statistics", stats2.put()));
        auto mid = AddGateConsumer(internal.get());
        auto tail = AddGateConsumer(internal.get());
        const bool wired = SUCCEEDED(stats1->SetInput(0, inputA.get())) && SUCCEEDED(mid->SetInput(0, inputA.get())) &&
                           SUCCEEDED(stats2->SetInput(0, mid.get())) && SUCCEEDED(tail->SetInput(0, inputB.get())) &&
                           SUCCEEDED(mid->Bind(L"Level", stats1.get(), L"Max")) &&
                           SUCCEEDED(tail->Bind(L"Level", stats2.get(), L"Max"));
        TEST("GateRegions_GraphWired", wired);
        if (!wired) return;
        const uint32_t stats1Id = internal->NodeId(stats1.get()), stats2Id = internal->NodeId(stats2.get());
        auto inputA2 = inputA.as<IEffectGraphInput2>();
        const UINT bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        auto target = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, bind, nullptr, 0);
        const auto verdicts = [&] { return gate.whole + gate.unchanged + gate.changed + gate.noValue; };
        const auto frame = [&] { return RealizeIncremental(context2.get(), tail.get(), target.get()); };
        frame();
        frame();
        TEST("GateRegions_Primed", frame().hr == S_OK && AnalysisField(internal->Model().FindNode(stats2Id), L"Max") > 0.0f);

        // Stats1's raised Max wakes Mid whole after Stats2 was queued over the
        // rect; the frozen refresh before Stats2's dispatch re-dirties it.
        const RECT bright{ 60, 28, 64, 32 };
        Paint(textureA.get(), bright, 2000.0f);
        auto gateBefore = gate;
        inputA2->InvalidateRects(&bright, 1);
        const auto raised = frame();
        printf("  [info] re-dirtied between queue and dispatch: Stats1 input %s, Stats2 input %s; gate +%llu whole, "
               "+%llu changed\n", RegionIsRect(evaluator.DispatchInputRegion(stats1Id), bright) ? "the rect" : "other",
               evaluator.DispatchInputRegion(stats2Id).IsWhole() ? "Whole" : "not Whole",
               static_cast<unsigned long long>(gate.whole - gateBefore.whole),
               static_cast<unsigned long long>(gate.changed - gateBefore.changed));
        TEST("DirtyRegion_GateRedirtiedStats1KeepsTheRect",
             raised.hr == S_OK && RegionIsRect(evaluator.DispatchInputRegion(stats1Id), bright) &&
             gate.changed == gateBefore.changed + 1);
        TEST("DirtyRegion_GateRedirtiedBeforeDispatchIsWhole",
             evaluator.DispatchInputRegion(stats2Id).IsWhole() && gate.whole == gateBefore.whole + 1);

        // A dropped queue: a Source with no output skips the frame's dispatches.
        // The next dispatch consumes Whole, not the union of the two rects.
        const RECT first{ 0, 0, 4, 4 }, second{ 8, 8, 12, 12 };
        Paint(textureA.get(), first, 50.0f);
        const uint32_t emptyId = internal->Model().AddNode(
            ShaderLab::Effects::SourceNodeFactory::CreateImageSourceNode(L"", L"Empty"));
        uint64_t verdictsBefore = verdicts();
        inputA2->InvalidateRects(&first, 1);
        frame();
        const bool dropped = verdicts() == verdictsBefore;
        internal->Model().RemoveNode(emptyId);
        Paint(textureA.get(), second, 50.0f);
        inputA2->InvalidateRects(&second, 1);
        TEST("GateRegions_QueueDropped", dropped);
        TEST("DirtyRegion_GateAfterDroppedQueueIsWhole", frame().hr == S_OK && evaluator.DispatchInputRegion(stats1Id).IsWhole());

        // A failed dispatch: the retry consumes Whole.
        frame();
        Paint(textureA.get(), first, 55.0f);
        inputA2->InvalidateRects(&first, 1);
        evaluator.FailNextComputeDispatchForTest(true);
        frame();
        const bool failed = !evaluator.FailNextComputeDispatchArmed() &&
                            !internal->Model().FindNode(stats1Id)->runtimeError.empty();
        evaluator.FailNextComputeDispatchForTest(false);
        Paint(textureA.get(), second, 55.0f);
        inputA2->InvalidateRects(&second, 1);
        const auto retried = frame();
        TEST("GateRegions_DispatchFailed", failed);
        TEST("DirtyRegion_GateAfterFailedDispatchIsWhole",
             retried.hr == S_OK && internal->Model().FindNode(stats1Id)->runtimeError.empty() &&
             evaluator.DispatchInputRegion(stats1Id).IsWhole());

        // Stats1's ClipNits bound to a statistics node on B: a rect on B queues
        // that source, so Stats1 dispatches Whole with no rect of its own and
        // no other path dirtying it whole.
        winrt::com_ptr<IEffectGraphNode> stats0;
        winrt::check_hresult(internal->CreateEffect(L"Luminance Statistics", stats0.put()));
        const bool bound = SUCCEEDED(stats0->SetInput(0, inputB.get())) &&
                           SUCCEEDED(stats1->Bind(L"ClipNits", stats0.get(), L"Max"));
        TEST("GateRegions_ClipNitsBoundToStatistics", bound);
        frame();
        frame();
        Paint(textureB.get(), first, 10.0f);
        inputB.as<IEffectGraphInput2>()->InvalidateRects(&first, 1);
        const auto sourceQueued = frame();
        TEST("DirtyRegion_GateBindingSourceQueuedIsWhole",
             sourceQueued.hr == S_OK && evaluator.DispatchInputRegion(stats1Id).IsWhole());
    }
    // A user effect made from the ICtCp tone mapper (SourcePeakNits
    // gpu-bindable), with its own id and source hash.
    std::wstring NoFallbackEffectJson(const wchar_t* name)
    {
        const auto* desc = ShaderLab::Effects::ShaderLabEffects::Instance().FindByName(L"ICtCp Tone Map (HDR -> SDR)");
        auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
        ShaderLab::Graph::RenameNode(node, name);
        node.customEffect->shaderLabEffectId = name;
        node.customEffect->hlslSource += L"\n// " + std::wstring(name) + L"\n";
        ShaderLab::Graph::GraphModel graph;
        graph.AddNode(std::move(node));
        return std::wstring(graph.ToJson());
    }

    bool RegisterBlob(const wchar_t* name, const ShaderLab::Effects::EffectBlobOptions& shapes)
    {
        const auto blob = ShaderLab::Effects::BuildEffectBlob(NoFallbackEffectJson(name), L"no_fallback.json", shapes);
        if (blob.builds.empty()) return false;
        const auto file = ShaderLab::Effects::EffectBlobFile(blob);
        return EffectGraphRegisterEffect(file.data(), static_cast<UINT32>(file.size())) == S_OK;
    }

    struct NoFallbackRun
    {
        HRESULT realized{ E_FAIL }, incremental{ E_FAIL };
        // The same two calls again with nothing changed: the node is clean
        // and still holds the build the refused frames chose.
        HRESULT retryIncremental{ E_FAIL }, retryRealized{ E_FAIL };
        bool targetUntouched{ false }, stillPending{ false };
        ShaderLab::Rendering::AnalysisReadbackStats readbacks;
        uint64_t laterFallbackReadbacks{ 0 };
    };

    // Texture -> Luminance Statistics -> `effect`, SourcePeakNits <- Max, on
    // a context with `flags`: RealizeTo, then RealizeToIncremental.
    NoFallbackRun RunNoFallback(const wchar_t* effect, EFFECTGRAPH_CONTEXT_FLAGS flags)
    {
        NoFallbackRun run;
        const EFFECTGRAPH_CONTEXT_DESC desc{ flags, nullptr };
        winrt::com_ptr<IEffectGraphContextInternal> context;
        winrt::check_hresult(EffectGraphCreateContext(g_d3dDevice.get(), &desc, __uuidof(IEffectGraphContextInternal),
                                                      context.put_void()));
        auto texels = GreyTexels(1.0f, 3.0f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);
        winrt::com_ptr<IEffectGraphNode> input, stats, toneMap;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        if (FAILED(context->CreateEffect(effect, toneMap.put()))) return run;
        stats->SetInput(0, input.get());
        toneMap->SetInput(0, input.get());
        winrt::check_hresult(toneMap->Bind(L"SourcePeakNits", stats.get(), L"Max"));

        // A target holding a known pattern: a refused frame must not draw.
        std::vector<uint8_t> pattern(cWidth * cHeight * 4);
        for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<uint8_t>(i * 7);
        const UINT bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        auto target = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM, bind, pattern.data(), cWidth * 4);
        run.realized = context->RealizeTo(toneMap.get(), target.get());
        auto incrementalTarget = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM, bind, pattern.data(), cWidth * 4);
        run.incremental = RealizeIncremental(context.as<IEffectGraphContext2>().get(), toneMap.get(),
                                             incrementalTarget.get()).hr;
        run.retryIncremental = RealizeIncremental(context.as<IEffectGraphContext2>().get(), toneMap.get(),
                                                  incrementalTarget.get()).hr;
        run.retryRealized = context->RealizeTo(toneMap.get(), target.get());
        run.targetUntouched = ReadBack(target.get(), 4) == pattern && ReadBack(incrementalTarget.get(), 4) == pattern;
        const auto pending = InvalidOf(toneMap.get());
        run.stillPending = pending.hr == S_OK && (pending.whole || !pending.rects.empty());
        run.readbacks = context->Evaluator().ReadbackStats();
        // Three more frames with new input: a compute consumer is judged by
        // the build it last ran, so only its first frame can read back for it.
        for (int frame = 0; frame < 3; ++frame)
        {
            input.as<IEffectGraphInput>()->Invalidate();
            context->RealizeTo(toneMap.get(), target.get());
        }
        run.laterFallbackReadbacks = context->Evaluator().ReadbackStats().fallbackReadbacks - run.readbacks.fallbackReadbacks;
        return run;
    }

    // NO_FALLBACK: a frame that would run a fallback build (the blob lacks
    // the build the GPU routing needs, and runtime compiles are off) fails
    // with EFFECTGRAPH_E_FALLBACK_BUILD and draws nothing; without the flag
    // it renders, reading the statistics back. A blob with the build passes.
    SL_TEST_CASE(TestEffectGraphNoFallback, Tag::Gpu)
    {
        printf("\n=== EffectGraph 1.1 stage 2: NO_FALLBACK ===\n");
        TEST("NoFallback_BlobsRegister", RegisterBlob(L"NoFallback Generic Tone Map", {}) &&
                                         RegisterBlob(L"NoFallback Shaped Tone Map", { { L"SourcePeakNits" } }));

        const auto refused = RunNoFallback(L"NoFallback Generic Tone Map", EFFECTGRAPH_CONTEXT_FLAGS_NO_FALLBACK);
        printf("  [info] generic blob, NO_FALLBACK: RealizeTo 0x%08X, incremental 0x%08X; %llu fallback build(s)\n",
               static_cast<uint32_t>(refused.realized), static_cast<uint32_t>(refused.incremental),
               static_cast<unsigned long long>(refused.readbacks.fallbackBuilds));
        TEST("NoFallback_RealizeToFails", refused.realized == EFFECTGRAPH_E_FALLBACK_BUILD);
        TEST("NoFallback_IncrementalFails", refused.incremental == EFFECTGRAPH_E_FALLBACK_BUILD);
        TEST("NoFallback_RetryWithoutChangeFails", refused.retryIncremental == EFFECTGRAPH_E_FALLBACK_BUILD &&
                                                     refused.retryRealized == EFFECTGRAPH_E_FALLBACK_BUILD);
        TEST("NoFallback_TargetUntouched", refused.targetUntouched);
        TEST("NoFallback_RegionStaysPending", refused.stillPending);

        const auto allowed = RunNoFallback(L"NoFallback Generic Tone Map", EFFECTGRAPH_CONTEXT_FLAGS_NONE);
        printf("  [info] generic blob, no flag: RealizeTo 0x%08X; %llu fallback build(s), %llu fallback readback(s)\n",
               static_cast<uint32_t>(allowed.realized), static_cast<unsigned long long>(allowed.readbacks.fallbackBuilds),
               static_cast<unsigned long long>(allowed.readbacks.fallbackReadbacks));
        TEST("NoFallback_WithoutFlagRenders", allowed.realized == S_OK && allowed.incremental == S_OK &&
                                             allowed.retryIncremental == S_OK && allowed.retryRealized == S_OK);
        TEST("NoFallback_WithoutFlagCountsTheFallback",
             allowed.readbacks.fallbackBuilds > 0 && allowed.readbacks.fallbackReadbacks > 0);

        const auto shaped = RunNoFallback(L"NoFallback Shaped Tone Map", EFFECTGRAPH_CONTEXT_FLAGS_NO_FALLBACK);
        printf("  [info] shaped blob, NO_FALLBACK: RealizeTo 0x%08X, incremental 0x%08X; %llu fallback build(s), "
               "%llu pending; fallback readbacks: %llu on the first frame, %llu on the three after\n",
               static_cast<uint32_t>(shaped.realized), static_cast<uint32_t>(shaped.incremental),
               static_cast<unsigned long long>(shaped.readbacks.fallbackBuilds),
               static_cast<unsigned long long>(shaped.readbacks.fallbackBuildsPending),
               static_cast<unsigned long long>(shaped.readbacks.fallbackReadbacks),
               static_cast<unsigned long long>(shaped.laterFallbackReadbacks));
        TEST("NoFallback_ShapedBlobRenders", shaped.realized == S_OK && shaped.incremental == S_OK &&
                                            shaped.retryIncremental == S_OK && shaped.retryRealized == S_OK);
        // The first frame of a compute consumer reads back: it has run no
        // build yet for the plan to judge (CollectAnalysisDemand).
        TEST("NoFallback_ShapedBlobNoFallback", shaped.readbacks.fallbackBuilds == 0 && shaped.readbacks.fallbackBuildsPending == 0 &&
                                                shaped.readbacks.fallbackReadbacks <= 1 && shaped.laterFallbackReadbacks == 0);
    }
}
