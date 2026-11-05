#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/CustomComputeBridgeEffect.h"
#include "Effects/CustomComputeShaderEffect.h"
#include "Effects/CustomPixelShaderEffect.h"
#include "Effects/EffectRegistry.h"
#include "Effects/ShaderCompiler.h"
#include "Graph/EffectEdge.h"
#include "Graph/NodeContracts.h"
#include "Rendering/D2DHelpers.h"

#include <cmath>
#include <cstdio>
#include <optional>
#include <vector>

// Dirty-rect parity with Direct2D (EffectGraph 1.1, stage 6).
//
// The table oracle: Direct2D's own invalidation (InvalidateEffectInputRectangle
// on an input, GetEffectInvalidRectangles on the effect) is the reference for
// which built-ins MapRegion may pass rects through. A built-in is on the
// allowlist (Graph::RegionIdentityBuiltIns) only if Direct2D maps a small
// interior rect to itself on every input at every point of a property sweep;
// every other built-in maps Whole.
//
// The engine's own custom effects answer MapInvalidRect as MapRegion does, so
// a host driving them through Direct2D directly gets the same rects.
namespace
{
    using namespace ShaderLab;
    using namespace ShaderLab::Tests;
    namespace Numerics = winrt::Windows::Foundation::Numerics;

    constexpr UINT32 cInputSize = 512;
    // Interior, and farther from every edge than a 20-sigma blur reaches.
    constexpr D2D1_RECT_L cProbe{ 200, 180, 232, 204 };

    // An FP16 gradient around `value` at alpha `alpha`, so every pixel
    // differs from its neighbours; `bump` adds to RGB inside cProbe and
    // lowers alpha there.
    winrt::com_ptr<ID2D1Bitmap1> InputBitmap(UINT32 width, UINT32 height, float value, float bump = 0.0f,
                                             float alpha = 1.0f)
    {
        std::vector<uint16_t> texels(static_cast<size_t>(width) * height * 4);
        auto half = [](float f)
        {
            uint32_t b; std::memcpy(&b, &f, 4);
            if ((b & 0x7FFFFFFF) == 0) return static_cast<uint16_t>((b >> 16) & 0x8000);
            return static_cast<uint16_t>(((b >> 16) & 0x8000) | ((((b >> 23) & 0xFF) - 112) << 10) | ((b >> 13) & 0x3FF));
        };
        for (UINT32 y = 0; y < height; ++y)
            for (UINT32 x = 0; x < width; ++x)
            {
                uint16_t* p = &texels[(static_cast<size_t>(y) * width + x) * 4];
                const bool inProbe = static_cast<LONG>(x) >= cProbe.left && static_cast<LONG>(x) < cProbe.right &&
                                     static_cast<LONG>(y) >= cProbe.top && static_cast<LONG>(y) < cProbe.bottom;
                const float a = inProbe && bump != 0.0f ? 0.6f * alpha : alpha;
                const float base = value + 0.4f * x / width + 0.2f * y / height + (inProbe ? bump : 0.0f);
                p[0] = half(base * a);
                p[1] = half((base * 0.7f + 0.1f) * a);
                p[2] = half((1.0f - 0.5f * base) * a);
                p[3] = half(a);
            }
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        winrt::check_hresult(g_dc->CreateBitmap(D2D1::SizeU(width, height), texels.data(), width * 8, &props, bitmap.put()));
        return bitmap;
    }

    // Draws `image` once, so the effects in it start with no invalid rects.
    HRESULT DrawOnce(ID2D1Image* image)
    {
        static winrt::com_ptr<ID2D1Bitmap1> target;
        static ID2D1DeviceContext5* targetDc = nullptr;
        if (!target || targetDc != g_dc.get())
        {
            target = nullptr;
            const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(g_dc->CreateBitmap(D2D1::SizeU(1024, 1024), nullptr, 0, &props, target.put()));
            targetDc = g_dc.get();
        }
        Rendering::ScopedTarget scoped(g_dc.get(), target.get());
        g_dc->BeginDraw();
        g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        g_dc->DrawImage(image);
        return g_dc->EndDraw();
    }

    // `image` drawn at the origin into an FP32 1024 x 1024 target, read back.
    std::vector<float> RenderPixels(ID2D1Image* image)
    {
        static winrt::com_ptr<ID2D1Bitmap1> target, staging;
        static ID2D1DeviceContext5* targetDc = nullptr;
        if (!target || targetDc != g_dc.get())
        {
            target = nullptr;
            staging = nullptr;
            const auto format = D2D1::PixelFormat(DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED);
            const auto targetProps = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format);
            const auto stagingProps = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                              format);
            winrt::check_hresult(g_dc->CreateBitmap(D2D1::SizeU(1024, 1024), nullptr, 0, &targetProps, target.put()));
            winrt::check_hresult(g_dc->CreateBitmap(D2D1::SizeU(1024, 1024), nullptr, 0, &stagingProps, staging.put()));
            targetDc = g_dc.get();
        }
        {
            Rendering::ScopedTarget scoped(g_dc.get(), target.get());
            g_dc->BeginDraw();
            g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            g_dc->DrawImage(image, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
            if (FAILED(g_dc->EndDraw())) return {};
        }
        if (FAILED(staging->CopyFromBitmap(nullptr, target.get(), nullptr))) return {};
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return {};
        std::vector<float> pixels(1024 * 1024 * 4);
        for (UINT32 y = 0; y < 1024; ++y)
            std::memcpy(&pixels[static_cast<size_t>(y) * 1024 * 4], mapped.bits + static_cast<size_t>(y) * mapped.pitch,
                        1024 * 16);
        staging->Unmap();
        return pixels;
    }

    // The effect's invalid rects since it was last drawn, as their bounding
    // box; `count` is how many there were.
    D2D1_RECT_F InvalidBounds(ID2D1Effect* effect, UINT32& count)
    {
        count = 0;
        D2D1_RECT_F box{ FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
        if (FAILED(g_dc->GetEffectInvalidRectangleCount(effect, &count)) || count == 0) return {};
        std::vector<D2D1_RECT_F> rects(count);
        if (FAILED(g_dc->GetEffectInvalidRectangles(effect, rects.data(), count))) { count = 0; return {}; }
        for (const auto& r : rects)
        {
            box.left = (std::min)(box.left, r.left);
            box.top = (std::min)(box.top, r.top);
            box.right = (std::max)(box.right, r.right);
            box.bottom = (std::max)(box.bottom, r.bottom);
        }
        return box;
    }

    bool SameRect(const D2D1_RECT_F& a, const D2D1_RECT_L& b)
    {
        return a.left == static_cast<float>(b.left) && a.top == static_cast<float>(b.top) &&
               a.right == static_cast<float>(b.right) && a.bottom == static_cast<float>(b.bottom);
    }

    bool Covers(const D2D1_RECT_F& outer, const D2D1_RECT_L& inner)
    {
        return outer.left <= static_cast<float>(inner.left) && outer.top <= static_cast<float>(inner.top) &&
               outer.right >= static_cast<float>(inner.right) && outer.bottom >= static_cast<float>(inner.bottom);
    }

    bool InsideRect(const D2D1_RECT_F& inner, const D2D1_RECT_L& outer)
    {
        return inner.left >= static_cast<float>(outer.left) && inner.top >= static_cast<float>(outer.top) &&
               inner.right <= static_cast<float>(outer.right) && inner.bottom <= static_cast<float>(outer.bottom);
    }

    // A property by name, as the evaluator sets one (EvaluatorCore::ApplyProperties).
    // E_INVALIDARG for a key Direct2D does not know; otherwise SetValue's answer.
    HRESULT SetProperty(ID2D1Effect* effect, const std::wstring& key, const Graph::PropertyValue& value)
    {
        const UINT32 index = effect->GetPropertyIndex(key.c_str());
        if (index == UINT32_MAX) return E_INVALIDARG;
        return std::visit([effect, index](auto&& v) -> HRESULT
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, float> || std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t>)
                return effect->SetValue(index, v);
            else if constexpr (std::is_same_v<T, bool>)
                return effect->SetValue(index, static_cast<BOOL>(v));
            else if constexpr (std::is_same_v<T, Numerics::float2>)
                return effect->SetValue(index, D2D1_VECTOR_2F{ v.x, v.y });
            else if constexpr (std::is_same_v<T, Numerics::float3>)
                return effect->SetValue(index, D2D1_VECTOR_3F{ v.x, v.y, v.z });
            else if constexpr (std::is_same_v<T, Numerics::float4>)
                return effect->SetValue(index, D2D1_VECTOR_4F{ v.x, v.y, v.z, v.w });
            else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                return effect->SetValue(index, D2D1_PROPERTY_TYPE_MATRIX_5X4, reinterpret_cast<const BYTE*>(&v), sizeof(v));
            else if constexpr (std::is_same_v<T, std::vector<float>>)
                return effect->SetValue(index, reinterpret_cast<const BYTE*>(v.data()),
                                        static_cast<UINT32>(v.size() * sizeof(float)));
            else
                return E_NOTIMPL;
        }, value);
    }

    // `from` moved a fraction `t` of the way to `to`, in its own type; nullopt
    // for a type with no in-between (bool, matrix, curve).
    std::optional<Graph::PropertyValue> Lerp(const Graph::PropertyValue& from, const Graph::PropertyValue& to, float t)
    {
        if (from.index() != to.index()) return std::nullopt;
        const auto f = [t](float a, float b) { return a + (b - a) * t; };
        return std::visit([&](auto&& a) -> std::optional<Graph::PropertyValue>
        {
            using T = std::decay_t<decltype(a)>;
            const T& b = std::get<T>(to);
            if constexpr (std::is_same_v<T, float>) return f(a, b);
            else if constexpr (std::is_same_v<T, int32_t>)
                return static_cast<int32_t>(std::lround(f(static_cast<float>(a), static_cast<float>(b))));
            else if constexpr (std::is_same_v<T, uint32_t>)
                return static_cast<uint32_t>(std::lround(f(static_cast<float>(a), static_cast<float>(b))));
            else if constexpr (std::is_same_v<T, Numerics::float2>) return Numerics::float2{ f(a.x, b.x), f(a.y, b.y) };
            else if constexpr (std::is_same_v<T, Numerics::float3>)
                return Numerics::float3{ f(a.x, b.x), f(a.y, b.y), f(a.z, b.z) };
            else if constexpr (std::is_same_v<T, Numerics::float4>)
                return Numerics::float4{ f(a.x, b.x), f(a.y, b.y), f(a.z, b.z), f(a.w, b.w) };
            else return std::nullopt;
        }, from);
    }

    // One point of a sweep: properties over the defaults, and anything a
    // property map cannot say (2D Affine Transform's matrix).
    struct SweepPoint
    {
        std::wstring label;
        std::map<std::wstring, Graph::PropertyValue> overrides;
        std::function<void(ID2D1Effect*)> extra;
    };

    // The defaults, then each property moved alone to the ends of its range
    // and its middle (integers too), every enum value, the other bool and a
    // non-identity matrix.
    std::vector<SweepPoint> FullSweep(const Effects::EffectDescriptor& desc)
    {
        std::vector<SweepPoint> points{ { L"defaults", {}, nullptr } };
        for (const auto& [key, value] : desc.defaultProperties)
        {
            const auto meta = desc.propertyMetadata.find(key);
            const bool hasMeta = meta != desc.propertyMetadata.end();
            const float lo = hasMeta ? meta->second.minValue : 0.0f;
            const float hi = hasMeta ? meta->second.maxValue : 1.0f;
            // Per-component ranges where the registry gives them.
            const auto component = [&](size_t i, bool max)
            {
                if (!hasMeta) return max ? hi : lo;
                const auto& list = max ? meta->second.componentMax : meta->second.componentMin;
                return i < list.size() ? list[i] : (max ? hi : lo);
            };
            const auto add = [&](const wchar_t* tag, Graph::PropertyValue v)
            {
                points.push_back({ key + L"=" + tag, { { key, std::move(v) } }, nullptr });
            };
            const size_t options = hasMeta ? meta->second.enumLabels.size() : 0;
            if (std::holds_alternative<float>(value))
            {
                add(L"min", lo);
                add(L"max", hi);
                add(L"mid", 0.5f * (lo + hi));
            }
            else if (std::holds_alternative<uint32_t>(value) && options > 0)
            {
                for (uint32_t i = 0; i < options; ++i)
                    points.push_back({ key + L"=" + std::to_wstring(i), { { key, i } }, nullptr });
            }
            else if (std::holds_alternative<int32_t>(value) && options > 0)
            {
                for (int32_t i = 0; i < static_cast<int32_t>(options); ++i)
                    points.push_back({ key + L"=" + std::to_wstring(i), { { key, i } }, nullptr });
            }
            else if (std::holds_alternative<uint32_t>(value))
            {
                add(L"min", static_cast<uint32_t>(std::lround((std::max)(lo, 0.0f))));
                add(L"max", static_cast<uint32_t>(std::lround((std::max)(hi, 0.0f))));
                add(L"mid", static_cast<uint32_t>(std::lround((std::max)(0.5f * (lo + hi), 0.0f))));
            }
            else if (std::holds_alternative<int32_t>(value))
            {
                add(L"min", static_cast<int32_t>(std::lround(lo)));
                add(L"max", static_cast<int32_t>(std::lround(hi)));
                add(L"mid", static_cast<int32_t>(std::lround(0.5f * (lo + hi))));
            }
            else if (std::holds_alternative<bool>(value))
                add(L"toggled", !std::get<bool>(value));
            else if (std::holds_alternative<Numerics::float2>(value))
            {
                add(L"min", Numerics::float2{ component(0, false), component(1, false) });
                add(L"max", Numerics::float2{ component(0, true), component(1, true) });
            }
            else if (std::holds_alternative<Numerics::float3>(value))
            {
                add(L"min", Numerics::float3{ component(0, false), component(1, false), component(2, false) });
                add(L"max", Numerics::float3{ component(0, true), component(1, true), component(2, true) });
            }
            else if (std::holds_alternative<Numerics::float4>(value))
            {
                add(L"min", Numerics::float4{ component(0, false), component(1, false), component(2, false),
                                              component(3, false) });
                add(L"max", Numerics::float4{ component(0, true), component(1, true), component(2, true),
                                              component(3, true) });
            }
            else if (std::holds_alternative<D2D1_MATRIX_5X4_F>(value))
            {
                D2D1_MATRIX_5X4_F m{};
                m._11 = 0.5f; m._12 = 0.2f; m._22 = 1.5f; m._33 = 0.25f; m._44 = 0.75f;
                m._51 = 0.1f; m._52 = -0.1f; m._53 = 0.3f; m._54 = 0.05f;
                add(L"scrambled", m);
            }
            else
                add(L"unsweepable", value);   // a type the sweep cannot move: rejected in FitSweepToD2D
        }
        return points;
    }

    // Whether two numeric values of the same type are equal.
    bool SameNumber(const Graph::PropertyValue& a, const Graph::PropertyValue& b)
    {
        if (a.index() != b.index()) return false;
        return std::visit([&](auto&& x) -> bool
        {
            using T = std::decay_t<decltype(x)>;
            const T& y = std::get<T>(b);
            if constexpr (std::is_same_v<T, float> || std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t>)
                return x == y;
            else if constexpr (std::is_same_v<T, Numerics::float2>) return x.x == y.x && x.y == y.y;
            else if constexpr (std::is_same_v<T, Numerics::float3>) return x.x == y.x && x.y == y.y && x.z == y.z;
            else if constexpr (std::is_same_v<T, Numerics::float4>)
                return x.x == y.x && x.y == y.y && x.z == y.z && x.w == y.w;
            else return false;
        }, a);
    }

    // Fits each point to what Direct2D accepts. An override it rejects moves
    // toward the default, by bisection, to the farthest value it takes (the
    // end of Direct2D's own range). One never taken short of the default
    // stays as it was, so MakeEffect fails it and the point counts as not
    // probed. `clamped` counts the moved overrides, `rejected` the ones left
    // failing.
    void FitSweepToD2D(const Effects::EffectDescriptor& desc, std::vector<SweepPoint>& points,
                       size_t& clamped, size_t& rejected)
    {
        winrt::com_ptr<ID2D1Effect> scratch;
        if (FAILED(g_dc->CreateEffect(desc.clsid, scratch.put()))) return;
        for (auto& point : points)
            for (auto& [key, value] : point.overrides)
            {
                if (point.label.ends_with(L"=unsweepable")) { point.label += L" (rejected)"; ++rejected; continue; }
                if (SUCCEEDED(SetProperty(scratch.get(), key, value))) continue;
                const auto def = desc.defaultProperties.find(key);
                bool taken = false;
                if (def != desc.defaultProperties.end() && Lerp(value, def->second, 0.5f))
                {
                    // t = 0 is the refused value, t = 1 the default.
                    float refused = 0.0f, accepted = 1.0f;
                    for (int step = 0; step < 24; ++step)
                    {
                        const float t = 0.5f * (refused + accepted);
                        const auto moved = Lerp(value, def->second, t);
                        (moved && SUCCEEDED(SetProperty(scratch.get(), key, *moved)) ? accepted : refused) = t;
                    }
                    const auto moved = Lerp(value, def->second, accepted);
                    if (accepted < 1.0f && moved && !SameNumber(*moved, def->second) &&
                        SUCCEEDED(SetProperty(scratch.get(), key, *moved)))
                    {
                        value = *moved;
                        point.label += L" (Direct2D's end)";
                        taken = true;
                    }
                }
                if (!taken) point.label += L" (rejected)";
                ++(taken ? clamped : rejected);
            }
    }

    // The points the measurement plan names, for any effect they apply to.
    std::vector<SweepPoint> SpecialPoints(const GUID& clsid)
    {
        if (IsEqualGUID(clsid, CLSID_D2D12DAffineTransform))
            return { { L"translated", {}, [](ID2D1Effect* e)
            {
                e->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, D2D1::Matrix3x2F::Translation(13.0f, 7.0f));
            } } };
        if (IsEqualGUID(clsid, CLSID_D2D1Scale))
            return { { L"scale 2x", { { L"Scale", Numerics::float2{ 2.0f, 2.0f } } }, nullptr } };
        if (IsEqualGUID(clsid, CLSID_D2D1GaussianBlur))
            return { { L"sigma 20", { { L"StandardDeviation", 20.0f } }, nullptr } };
        return {};
    }

    // A built-in with the point's properties over its defaults, on `inputs`.
    winrt::com_ptr<ID2D1Effect> MakeEffect(const Effects::EffectDescriptor& desc, const SweepPoint& point,
                                           const std::vector<winrt::com_ptr<ID2D1Bitmap1>>& inputs)
    {
        winrt::com_ptr<ID2D1Effect> effect;
        if (FAILED(g_dc->CreateEffect(desc.clsid, effect.put()))) return nullptr;
        const UINT32 pins = static_cast<UINT32>(desc.inputPins.size());
        if (effect->GetInputCount() != pins && FAILED(effect->SetInputCount(pins))) return nullptr;
        for (UINT32 i = 0; i < pins; ++i)
            effect->SetInput(i, inputs[i].get());
        // Every property must take, or the point is not the one it says.
        for (const auto& [key, value] : desc.defaultProperties)
            if (FAILED(SetProperty(effect.get(), key, value))) return nullptr;
        for (const auto& [key, value] : point.overrides)
            if (FAILED(SetProperty(effect.get(), key, value))) return nullptr;
        if (point.label.ends_with(L"(rejected)")) return nullptr;
        if (point.extra) point.extra(effect.get());
        return effect;
    }

    // The pixels of the output that move when input `pin` changes inside
    // cProbe only: an instrument independent of Direct2D's own map. False
    // when either render failed.
    bool Footprint(const Effects::EffectDescriptor& desc, const SweepPoint& point, UINT32 pin,
                   const std::vector<winrt::com_ptr<ID2D1Bitmap1>>& inputs, const std::vector<winrt::com_ptr<ID2D1Bitmap1>>& bumped,
                   size_t& inside, size_t& outside)
    {
        inside = outside = 0;
        auto effect = MakeEffect(desc, point, inputs);
        if (!effect) return false;
        winrt::com_ptr<ID2D1Image> output;
        effect->GetOutput(output.put());
        const auto before = RenderPixels(output.get());
        effect->SetInput(pin, bumped[pin].get());
        const auto after = RenderPixels(output.get());
        if (before.empty() || after.empty()) return false;
        for (UINT32 y = 0; y < 1024; ++y)
            for (UINT32 x = 0; x < 1024; ++x)
            {
                const size_t at = (static_cast<size_t>(y) * 1024 + x) * 4;
                if (std::memcmp(&before[at], &after[at], 16) == 0) continue;
                const bool inProbe = static_cast<LONG>(x) >= cProbe.left && static_cast<LONG>(x) < cProbe.right &&
                                     static_cast<LONG>(y) >= cProbe.top && static_cast<LONG>(y) < cProbe.bottom;
                ++(inProbe ? inside : outside);
            }
        return true;
    }

    // Direct2D's invalid rect on `effect` for the probe on input `pin`, with
    // the point's properties. False when the effect could not be drawn.
    bool Probe(const Effects::EffectDescriptor& desc, const SweepPoint& point, UINT32 pin,
               const std::vector<winrt::com_ptr<ID2D1Bitmap1>>& inputs, D2D1_RECT_F& invalid, UINT32& count)
    {
        auto effect = MakeEffect(desc, point, inputs);
        if (!effect) return false;

        winrt::com_ptr<ID2D1Image> output;
        effect->GetOutput(output.put());
        if (FAILED(DrawOnce(output.get()))) return false;
        UINT32 before = 0;
        InvalidBounds(effect.get(), before);
        if (before != 0) return false;   // a draw clears them
        const D2D1_RECT_F probe{ static_cast<float>(cProbe.left), static_cast<float>(cProbe.top),
                                 static_cast<float>(cProbe.right), static_cast<float>(cProbe.bottom) };
        if (FAILED(g_dc->InvalidateEffectInputRectangle(effect.get(), pin, &probe)))
            return false;
        invalid = InvalidBounds(effect.get(), count);
        return true;
    }

    SL_TEST_CASE(TestD2DTableOracle, Tag::Gpu)
    {
        printf("\n=== Stage 6: Direct2D table oracle ===\n");
        Rendering::ScopedDpi96 dpi(g_dc.get());

        // Wider than any expansion the sweep can make reach the probe from an edge.
        std::vector<winrt::com_ptr<ID2D1Bitmap1>> inputs, bumped, halfInputs, halfBumped;
        for (int i = 0; i < 4; ++i)
        {
            inputs.push_back(InputBitmap(cInputSize, cInputSize, 0.25f + 0.2f * i));
            bumped.push_back(InputBitmap(cInputSize, cInputSize, 0.25f + 0.2f * i, 0.3f));
            // Translucent, so no input of a multi-input effect hides another.
            halfInputs.push_back(InputBitmap(cInputSize, cInputSize, 0.25f + 0.2f * i, 0.0f, 0.5f));
            halfBumped.push_back(InputBitmap(cInputSize, cInputSize, 0.25f + 0.2f * i, 0.3f, 0.5f));
        }

        Graph::EffectNode src;
        src.type = Graph::NodeType::Source;
        Graph::DirtyRegion rect = Graph::DirtyRegion::Rect(cProbe, 1);

        const auto& registry = Effects::EffectRegistry::Instance();
        size_t allowlisted = 0, allowlistedPoints = 0, narrower = 0, notIdentity = 0, unprobed = 0;
        size_t othersWhole = 0, others = 0, othersIdentityEverywhere = 0;
        // Pixel footprints of the listed effects: outside the probe must be 0.
        size_t footprints = 0, footprintOutside = 0, footprintInside = 0, footprintFailed = 0;
        // Override values Direct2D refused: moved to its end, or left rejected.
        size_t sweepClamped = 0, sweepRejected = 0;
        for (const auto& desc : registry.All())
        {
            const bool listed = Graph::IsRegionIdentityBuiltIn(desc.clsid);
            const UINT32 pins = static_cast<UINT32>(desc.inputPins.size());

            // MapRegion on every input pin: the same rects for a listed
            // effect, Whole for every other.
            Graph::EffectNode dst = Effects::EffectRegistry::CreateNode(desc);
            bool mapsAsListed = true;
            for (UINT32 pin = 0; pin < (std::max)(pins, 1u); ++pin)
            {
                Graph::EffectEdge edge{};
                edge.destPin = pin;
                const auto mapped = Graph::MapRegion(src, dst, edge, rect);
                mapsAsListed = mapsAsListed && (listed ? mapped == rect : mapped.IsWhole());
            }

            // Every effect sweeps fully: a listed one must hold at every
            // point, and the others are reported when they would qualify.
            std::vector<SweepPoint> points = SpecialPoints(desc.clsid);
            for (auto& p : FullSweep(desc)) points.push_back(std::move(p));
            size_t clamped = 0, rejected = 0;
            FitSweepToD2D(desc, points, clamped, rejected);
            sweepClamped += clamped;
            sweepRejected += rejected;

            bool identityEverywhere = pins > 0;
            bool listedFailed = false;
            std::wstring firstFailure;
            // A pin whose bump never changed a pixel is one the footprint cannot see.
            std::vector<bool> pinSeen(pins, false);
            for (const auto& point : points)
                for (UINT32 pin = 0; pin < pins; ++pin)
                {
                    D2D1_RECT_F invalid{};
                    UINT32 count = 0;
                    if (!Probe(desc, point, pin, inputs, invalid, count))
                    {
                        ++unprobed;
                        identityEverywhere = false;
                        if (listed) { listedFailed = true; if (firstFailure.empty()) firstFailure = point.label + L" (not probed)"; }
                        continue;
                    }
                    const bool identity = count > 0 && SameRect(invalid, cProbe);
                    identityEverywhere = identityEverywhere && identity;
                    if (listed)
                    {
                        ++allowlistedPoints;
                        // The allowlist says the probe itself: never narrower than Direct2D.
                        if (count > 0 && !InsideRect(invalid, cProbe)) ++narrower;
                        if (!identity)
                        {
                            ++notIdentity;
                            listedFailed = true;
                            if (firstFailure.empty())
                                firstFailure = std::format(L"{} pin {}: {} rect(s), ({:g},{:g})-({:g},{:g})", point.label, pin,
                                                           count, invalid.left, invalid.top, invalid.right, invalid.bottom);
                        }
                        // What Direct2D says, checked against what the pixels do.
                        size_t inside = 0, outside = 0;
                        if (!Footprint(desc, point, pin, inputs, bumped, inside, outside))
                            ++footprintFailed;
                        ++footprints;
                        if (pins > 1)
                        {
                            // Again with translucent inputs: an opaque one can hide the pin.
                            size_t halfInside = 0, halfOutside = 0;
                            if (!Footprint(desc, point, pin, halfInputs, halfBumped, halfInside, halfOutside))
                                ++footprintFailed;
                            ++footprints;
                            inside += halfInside;
                            outside += halfOutside;
                        }
                        if (inside > 0) pinSeen[pin] = true;
                        footprintInside += inside;
                        footprintOutside += outside;
                        if (outside > 0)
                        {
                            listedFailed = true;
                            if (firstFailure.empty())
                                firstFailure = std::format(L"{} pin {}: {} pixel(s) outside the probe changed", point.label,
                                                           pin, outside);
                        }
                    }
                }

            if (listed)
            {
                for (UINT32 pin = 0; pin < pins; ++pin)
                    if (!pinSeen[pin])
                    {
                        listedFailed = true;
                        if (firstFailure.empty())
                            firstFailure = std::format(L"pin {}: no point changed a pixel (footprint blind)", pin);
                    }
                // An effect with properties is proven away from its defaults too.
                if (!desc.defaultProperties.empty() && points.size() - SpecialPoints(desc.clsid).size() <= 1)
                {
                    listedFailed = true;
                    if (firstFailure.empty()) firstFailure = L"swept at its defaults only";
                }
                ++allowlisted;
                printf("  [%s] %ls: identity at %zu point(s) x %u input(s), %zu value(s) moved to Direct2D's end, "
                       "%zu rejected%s%ls\n", listedFailed ? "FAIL" : "ok", desc.name.c_str(), points.size(), pins,
                       clamped, rejected, firstFailure.empty() ? "" : "; first miss ", firstFailure.c_str());
            }
            else
            {
                ++others;
                if (mapsAsListed) ++othersWhole;
                if (identityEverywhere)
                {
                    // Direct2D's map is not the whole story: a kernel effect
                    // it maps as identity still changes pixels around the rect.
                    ++othersIdentityEverywhere;
                    size_t inside = 0, outside = 0;
                    const bool rendered = Footprint(desc, SweepPoint{ L"defaults", {}, nullptr }, 0, inputs, bumped,
                                                    inside, outside);
                    printf("  [info] %ls: Direct2D maps identity across its sweep; at its defaults %s%zu pixel(s) outside "
                           "the rect change. Not allowlisted (Whole)\n",
                           desc.name.c_str(), rendered ? "" : "(render failed) ", outside);
                }
            }
            TEST((std::string("Oracle_MapRegion_") + winrt::to_string(desc.name)).c_str(), mapsAsListed);
            if (listed)
                TEST((std::string("Oracle_Identity_") + winrt::to_string(desc.name)).c_str(), !listedFailed);
        }
        printf("  [info] %zu allowlisted effect(s), %zu probe(s): %zu narrower than Direct2D, %zu not identity; "
               "%zu other effect(s) map Whole of %zu; %zu probe(s) could not run\n",
               allowlisted, allowlistedPoints, narrower, notIdentity, othersWhole, others, unprobed);
        printf("  [info] pixel footprints of the allowlisted: %zu render pair(s), %zu changed pixel(s) inside the rect, "
               "%zu outside; %zu render(s) failed\n", footprints, footprintInside, footprintOutside, footprintFailed);
        printf("  [info] sweep values Direct2D refused, all effects: %zu moved to its end, %zu rejected\n", sweepClamped,
               sweepRejected);
        TEST("Oracle_AllowlistInRegistry", allowlisted == Graph::RegionIdentityBuiltIns().size());
        TEST("Oracle_NoneNarrowerThanD2D", narrower == 0);
        TEST("Oracle_EveryOtherWhole", othersWhole == others);
        TEST("Oracle_FootprintsInsideTheRect", footprintFailed == 0 && footprintOutside == 0 && footprintInside > 0);

        // The instrument sees a non-identity map: a translated affine moves
        // the probe, a 20-sigma blur widens it.
        {
            const auto* affine = registry.FindByClsid(CLSID_D2D12DAffineTransform);
            const auto* blur = registry.FindByClsid(CLSID_D2D1GaussianBlur);
            D2D1_RECT_F moved{}, widened{};
            UINT32 movedCount = 0, widenedCount = 0;
            const bool probed = affine && blur &&
                Probe(*affine, SpecialPoints(affine->clsid)[0], 0, inputs, moved, movedCount) &&
                Probe(*blur, SpecialPoints(blur->clsid)[0], 0, inputs, widened, widenedCount);
            printf("  [info] control: translated affine -> (%g,%g)-(%g,%g); sigma-20 blur -> (%g,%g)-(%g,%g)\n",
                   moved.left, moved.top, moved.right, moved.bottom, widened.left, widened.top, widened.right, widened.bottom);
            TEST("Oracle_ControlSeesATranslation", probed && movedCount > 0 && moved.left == cProbe.left + 13.0f &&
                                                   moved.top == cProbe.top + 7.0f);
            TEST("Oracle_ControlSeesABlur", probed && widenedCount > 0 && widened.left < cProbe.left - 20.0f &&
                                            widened.right > cProbe.right + 20.0f);
            // The pixel footprint sees past the rect where pixels do move.
            size_t inside = 0, outside = 0;
            const bool rendered = blur && Footprint(*blur, SweepPoint{ L"defaults", {}, nullptr }, 0, inputs, bumped,
                                                    inside, outside);
            printf("  [info] control: a default blur's footprint changes %zu pixel(s) inside the rect, %zu outside\n",
                   inside, outside);
            TEST("Oracle_FootprintSeesABlur", rendered && inside > 0 && outside > 0);
        }
    }

    // ---- The engine's custom effects through Direct2D ----------------------

    constexpr GUID cParityShaderClsid = { 0x5b0c7d11, 0x8e2f, 0x4a63, { 0x9d, 0x41, 0x2c, 0x6e, 0x10, 0x7a, 0x33, 0x01 } };
    constexpr GUID cParityGeneratorClsid = { 0x5b0c7d11, 0x8e2f, 0x4a63, { 0x9d, 0x41, 0x2c, 0x6e, 0x10, 0x7a, 0x33, 0x02 } };
    constexpr GUID cParityComputeClsid = { 0x5b0c7d11, 0x8e2f, 0x4a63, { 0x9d, 0x41, 0x2c, 0x6e, 0x10, 0x7a, 0x33, 0x03 } };

    // Input 0 is an image pin, input 1 a lookup read at a fixed coordinate.
    constexpr const char* cParityShaderHlsl = R"HLSL(
Texture2D Image : register(t0);
Texture2D Table : register(t1);
SamplerState ImageSampler : register(s0);
SamplerState TableSampler : register(s1);
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION,
            float4 uv0 : TEXCOORD0, float4 uv1 : TEXCOORD1) : SV_TARGET
{
    return Image.Sample(ImageSampler, uv0.xy) * 0.5 + Table.Sample(TableSampler, float2(0.5, 0.5)) * 0.25;
}
)HLSL";

    constexpr const char* cParityGeneratorHlsl = R"HLSL(
Texture2D Image : register(t0);
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    return float4(frac(floor(scenePos.xy) / 64.0), 0.0, 1.0);
}
)HLSL";

    winrt::com_ptr<ID2D1Effect> ParityShader(const GUID& clsid, const char* hlsl, UINT32 inputs, UINT32 imagePins,
                                             UINT32 fixedSize, Effects::CustomPixelShaderEffect** implOut)
    {
        winrt::com_ptr<ID2D1Factory> factory;
        g_dc->GetFactory(factory.put());
        auto factory1 = factory.as<ID2D1Factory1>();
        winrt::check_hresult(Effects::CustomPixelShaderEffect::RegisterWithInputCount(factory1.get(), clsid, inputs));
        const auto compiled = Effects::ShaderCompiler::CompileFromString(hlsl, "parity", "main", "ps_5_0");
        if (!compiled.succeeded) return nullptr;
        winrt::com_ptr<ID2D1Effect> effect;
        Effects::CustomPixelShaderEffect* impl = nullptr;
        winrt::check_hresult(Effects::CustomPixelShaderEffect::Create(g_dc.get(), clsid, inputs, effect, &impl));
        impl->SetImageInputCount(imagePins);
        if (fixedSize) impl->SetFixedOutputSize(fixedSize, fixedSize);
        GUID shaderGuid{};
        CoCreateGuid(&shaderGuid);
        impl->SetShaderGuid(shaderGuid);
        impl->LoadShaderBytecode(static_cast<const BYTE*>(compiled.bytecode->GetBufferPointer()),
                                 static_cast<UINT32>(compiled.bytecode->GetBufferSize()));
        if (implOut) *implOut = impl;
        return effect;
    }

    // Invalidates `rect` on `pin` of `first` after a draw of `last`, and
    // returns what Direct2D says changed in `last`.
    D2D1_RECT_F ThroughD2D(ID2D1Effect* first, UINT32 pin, const D2D1_RECT_L& rect, ID2D1Effect* last, UINT32& count)
    {
        winrt::com_ptr<ID2D1Image> output;
        last->GetOutput(output.put());
        count = 0;
        if (const HRESULT hr = DrawOnce(output.get()); FAILED(hr))
        {
            printf("  [info] ThroughD2D: the draw failed (0x%08X)\n", static_cast<unsigned>(hr));
            return {};
        }
        UINT32 leftover = 0;
        InvalidBounds(last, leftover);
        if (leftover != 0)
        {
            printf("  [info] ThroughD2D: %u invalid rect(s) left after the draw\n", leftover);
            return {};
        }
        const D2D1_RECT_F rectF{ static_cast<float>(rect.left), static_cast<float>(rect.top),
                                 static_cast<float>(rect.right), static_cast<float>(rect.bottom) };
        if (const HRESULT hr = g_dc->InvalidateEffectInputRectangle(first, pin, &rectF); FAILED(hr))
        {
            printf("  [info] ThroughD2D: InvalidateEffectInputRectangle failed (0x%08X)\n", static_cast<unsigned>(hr));
            return {};
        }
        return InvalidBounds(last, count);
    }

    SL_TEST_CASE(TestD2DEngineEffectInvalidation, Tag::Gpu)
    {
        printf("\n=== Stage 6: engine effects' MapInvalidRect through Direct2D ===\n");
        Rendering::ScopedDpi96 dpi(g_dc.get());
        auto image = InputBitmap(cInputSize, cInputSize, 0.5f);
        auto other = InputBitmap(cInputSize, cInputSize, 0.25f);
        auto table = InputBitmap(16, 16, 0.75f);
        const D2D1_RECT_L whole{ 0, 0, static_cast<LONG>(cInputSize), static_cast<LONG>(cInputSize) };

        // A pixel shader: the rect on its image pin, everything on its lookup pin.
        auto shader = ParityShader(cParityShaderClsid, cParityShaderHlsl, 2, 1, 0, nullptr);
        TEST("Engine_ShaderBuilt", shader != nullptr);
        if (!shader) return;
        shader->SetInput(0, image.get());
        shader->SetInput(1, table.get());
        UINT32 count = 0;
        const auto onImage = ThroughD2D(shader.get(), 0, cProbe, shader.get(), count);
        TEST("Engine_ImagePinIsIdentity", count > 0 && SameRect(onImage, cProbe));
        const auto onLookup = ThroughD2D(shader.get(), 1, { 2, 2, 3, 3 }, shader.get(), count);
        printf("  [info] lookup pin -> %u rect(s), (%g,%g)-(%g,%g)\n", count, onLookup.left, onLookup.top, onLookup.right,
               onLookup.bottom);
        // Direct2D reports the infinite rect as it is, unclamped: whole.
        TEST("Engine_LookupPinIsWhole", count > 0 && Covers(onLookup, whole));
        // Not clamped to the output either (CustomEffectBase.h says so).
        TEST("Engine_InfiniteReportedUnclamped", count > 0 && onLookup.left < -1.0e9f && onLookup.right > 1.0e9f);

        // A fixed-size output (a generator, a diagram): all of it.
        auto generator = ParityShader(cParityGeneratorClsid, cParityGeneratorHlsl, 1, 1, 64, nullptr);
        TEST("Engine_GeneratorBuilt", generator != nullptr);
        if (generator)
        {
            generator->SetInput(0, image.get());
            const auto onGenerator = ThroughD2D(generator.get(), 0, cProbe, generator.get(), count);
            printf("  [info] fixed-size output -> %u rect(s), (%g,%g)-(%g,%g)\n", count, onGenerator.left, onGenerator.top,
                   onGenerator.right, onGenerator.bottom);
            TEST("Engine_FixedOutputIsWhole", count > 0 && Covers(onGenerator, { 0, 0, 64, 64 }));
        }

        // A mixed chain: engine shader -> allowlisted built-ins (Color Matrix,
        // Composite over another image, Saturation) -> engine shader. The
        // rect comes out of the far end unchanged.
        {
            winrt::com_ptr<ID2D1Effect> matrix, composite, saturation;
            winrt::check_hresult(g_dc->CreateEffect(CLSID_D2D1ColorMatrix, matrix.put()));
            winrt::check_hresult(g_dc->CreateEffect(CLSID_D2D1Composite, composite.put()));
            winrt::check_hresult(g_dc->CreateEffect(CLSID_D2D1Saturation, saturation.put()));
            auto tail = ParityShader(cParityShaderClsid, cParityShaderHlsl, 2, 1, 0, nullptr);
            matrix->SetInputEffect(0, shader.get());
            composite->SetInput(0, other.get());
            composite->SetInputEffect(1, matrix.get());
            saturation->SetInputEffect(0, composite.get());
            tail->SetInputEffect(0, saturation.get());
            tail->SetInput(1, table.get());
            const auto throughChain = ThroughD2D(shader.get(), 0, cProbe, tail.get(), count);
            printf("  [info] mixed chain -> %u rect(s), (%g,%g)-(%g,%g)\n", count, throughChain.left, throughChain.top,
                   throughChain.right, throughChain.bottom);
            TEST("Engine_MixedChainKeepsTheRect", count > 0 && SameRect(throughChain, cProbe));
            // The same graph through MapRegion: rects all the way.
            Graph::EffectNode src, pixel, builtIn;
            src.type = Graph::NodeType::Source;
            pixel.type = Graph::NodeType::PixelShader;
            pixel.customEffect.emplace();
            pixel.customEffect->shaderType = Graph::CustomShaderType::PixelShader;
            pixel.customEffect->inputNames = { L"Image", L"Table" };
            pixel.customEffect->lookupInputCount = 1;
            Graph::EffectEdge edge{};
            Graph::DirtyRegion region = Graph::DirtyRegion::Rect(cProbe, 1);
            region = Graph::MapRegion(src, pixel, edge, region);
            for (const GUID* clsid : { &CLSID_D2D1ColorMatrix, &CLSID_D2D1Composite, &CLSID_D2D1Saturation })
            {
                builtIn = Effects::EffectRegistry::CreateNode(*Effects::EffectRegistry::Instance().FindByClsid(*clsid));
                edge.destPin = IsEqualGUID(*clsid, CLSID_D2D1Composite) ? 1u : 0u;
                region = Graph::MapRegion(pixel, builtIn, edge, region);
                edge.destPin = 0;
            }
            region = Graph::MapRegion(builtIn, pixel, edge, region);
            TEST("Engine_MixedChainMapRegionAgrees", region == Graph::DirtyRegion::Rect(cProbe, 1));
            // A built-in off the list in the same place: Direct2D widens it,
            // MapRegion makes it Whole.
            winrt::com_ptr<ID2D1Effect> blur;
            winrt::check_hresult(g_dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.put()));
            blur->SetInputEffect(0, shader.get());
            tail->SetInputEffect(0, blur.get());
            const auto throughBlur = ThroughD2D(shader.get(), 0, cProbe, tail.get(), count);
            TEST("Engine_BlurInTheChainWidens", count > 0 && !SameRect(throughBlur, cProbe) && InsideRect(
                D2D1_RECT_F{ static_cast<float>(cProbe.left), static_cast<float>(cProbe.top),
                             static_cast<float>(cProbe.right), static_cast<float>(cProbe.bottom) },
                { static_cast<LONG>(throughBlur.left), static_cast<LONG>(throughBlur.top),
                  static_cast<LONG>(throughBlur.right), static_cast<LONG>(throughBlur.bottom) }));
        }

        // The compute bridge: its output is not laid out in its input's pixels.
        {
            winrt::com_ptr<ID2D1Factory> factory;
            g_dc->GetFactory(factory.put());
            Effects::CustomComputeBridgeEffect::RegisterEffect(factory.as<ID2D1Factory1>().get());
            winrt::com_ptr<ID2D1Effect> bridge;
            Effects::CustomComputeBridgeEffect* bridgeImpl = nullptr;
            const HRESULT created = Effects::CustomComputeBridgeEffect::Create(g_dc.get(), bridge, &bridgeImpl);
            TEST("Engine_BridgeCreated", SUCCEEDED(created) && bridge);
            if (bridge)
            {
                bridge->SetInput(0, image.get());
                const auto onBridge = ThroughD2D(bridge.get(), 0, cProbe, bridge.get(), count);
                printf("  [info] bridge -> %u rect(s), (%g,%g)-(%g,%g)\n", count, onBridge.left, onBridge.top,
                       onBridge.right, onBridge.bottom);
                TEST("Engine_BridgeIsWhole", count > 0 && Covers(onBridge, whole));
            }
            D2D1_RECT_L mapped{};
            TEST("Engine_BridgeMapsInfinite", bridgeImpl && SUCCEEDED(bridgeImpl->MapInvalidRect(0, cProbe, &mapped)) &&
                 mapped.left == Effects::cInfiniteRectL.left && mapped.right == Effects::cInfiniteRectL.right);
        }

        // A D2D compute shader may read any texel for any output texel.
        {
            winrt::com_ptr<ID2D1Factory> factory;
            g_dc->GetFactory(factory.put());
            Effects::CustomComputeShaderEffect::RegisterWithInputCount(factory.as<ID2D1Factory1>().get(), cParityComputeClsid, 1);
            winrt::com_ptr<ID2D1Effect> compute;
            Effects::CustomComputeShaderEffect* computeImpl = nullptr;
            const HRESULT created = Effects::CustomComputeShaderEffect::Create(g_dc.get(), cParityComputeClsid, 1, compute,
                                                                               &computeImpl);
            D2D1_RECT_L mapped{};
            TEST("Engine_D2DComputeMapsInfinite", SUCCEEDED(created) && computeImpl &&
                 SUCCEEDED(computeImpl->MapInvalidRect(0, cProbe, &mapped)) &&
                 mapped.left == Effects::cInfiniteRectL.left && mapped.bottom == Effects::cInfiniteRectL.bottom);
        }
    }
}
