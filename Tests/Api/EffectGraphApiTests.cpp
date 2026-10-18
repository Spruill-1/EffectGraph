#include "pch_engine.h"
#include "../TestSupport.h"
#include "Engine/Api/EffectGraph.h"
#include "Engine/Api/EffectGraphInternal.h"
#include "Engine/Utf8.h"

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
}
