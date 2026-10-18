#include "pch_engine.h"
#include "BuiltInEffects.h"
#include "ColorMathCpu.h"

// The C++ a built-in carries beyond its HLSL. The generated headers hold
// description and bytecode only, so the engine attaches these by effect id
// when a built-in registers.
namespace ShaderLab::Effects
{
    // ---- Derived constants (see ShaderLabEffectDescriptor::deriveConstants) --
    namespace
    {
        using PropMap = std::map<std::wstring, Graph::PropertyValue>;
        namespace CM = ColorMathCpu;

        double PropNumber(const PropMap& p, const wchar_t* name, double fallback)
        {
            auto it = p.find(name);
            if (it == p.end()) return fallback;
            if (auto* f = std::get_if<float>(&it->second)) return *f;
            if (auto* u = std::get_if<uint32_t>(&it->second)) return *u;
            if (auto* i = std::get_if<int32_t>(&it->second)) return *i;
            return fallback;
        }
        CM::V2 PropV2(const PropMap& p, const wchar_t* name, CM::V2 fallback)
        {
            auto it = p.find(name);
            if (it == p.end()) return fallback;
            if (auto* v = std::get_if<winrt::Windows::Foundation::Numerics::float2>(&it->second))
                return { v->x, v->y };
            return fallback;
        }

        // A handful of recently used tables, keyed by every input that shapes
        // them. The evaluator re-applies a node on every frame its upstream
        // changes, so without this a video under the effect would rebuild the
        // tables per frame for nothing.
        // Shared by every evaluator in the process, so it locks; entries are
        // handed out shared so a clear cannot free a table still in use.
        // The build runs outside the lock.
        struct DerivedMemo
        {
            using Table = std::shared_ptr<const std::vector<float>>;
            std::mutex mutex;
            std::map<std::vector<double>, Table> entries;
            Table Get(const std::vector<double>& key,
                      const std::function<std::vector<float>()>& build)
            {
                {
                    std::lock_guard lock(mutex);
                    auto it = entries.find(key);
                    if (it != entries.end()) return it->second;
                }
                auto table = std::make_shared<const std::vector<float>>(build());
                std::lock_guard lock(mutex);
                if (entries.size() >= 16) entries.clear();
                return entries.try_emplace(key, std::move(table)).first->second;
            }
        };

        // ComputeICtCpFitScale from the HLSL: the uniform Ct/Cp scale that
        // brings every source-boundary vertex inside the target boundary.
        template <size_t N>
        double FitScale(const std::array<CM::V2, N>& src, const std::array<CM::V2, N>& tgt)
        {
            double maxRatio = 1.0;
            for (size_t i = 0; i < N; ++i)
            {
                const CM::V2 dir = src[i];
                if (std::hypot(dir.x, dir.y) < 1e-8) continue;
                double bestT = 1e10;
                for (size_t j = 0; j < N; ++j)
                {
                    const CM::V2 a = tgt[j];
                    const CM::V2 b = tgt[(j + 1) % N];
                    const CM::V2 ab{ b.x - a.x, b.y - a.y };
                    const double denom = dir.x * ab.y - dir.y * ab.x;
                    if (std::abs(denom) < 1e-10) continue;
                    const double t = (a.x * ab.y - a.y * ab.x) / denom;
                    const double u = (a.x * dir.y - a.y * dir.x) / denom;
                    if (t > 0.0 && u >= 0.0 && u <= 1.0 && t < bestT) bestT = t;
                }
                if (bestT > 0.0 && bestT < 1e9) maxRatio = (std::max)(maxRatio, 1.0 / bestT);
            }
            return (maxRatio > 1.0) ? 1.0 / maxRatio : 1.0;
        }

        ShaderLabEffectDescriptor::DerivedConstantsFn MakeIctcpGamutMapDerived()
        {
            auto memo = std::make_shared<DerivedMemo>();
            return [memo](const PropMap& p, const ShaderLabEffectDescriptor::DerivedConstantWriter& write)
            {
                const int mode = static_cast<int>(PropNumber(p, L"Mode", 0.0));
                const int tg = static_cast<int>(PropNumber(p, L"TargetGamut", 0.0));
                const auto tgt = CM::GamutPrimaries(tg,
                    PropV2(p, L"TargetRedPrimary",   { 0.64, 0.33 }),
                    PropV2(p, L"TargetGreenPrimary", { 0.30, 0.60 }),
                    PropV2(p, L"TargetBluePrimary",  { 0.15, 0.06 }));
                if (mode == 2)
                {
                    const int sg = static_cast<int>(PropNumber(p, L"SourceGamut", 2.0));
                    const auto src = CM::GamutPrimaries(sg,
                        PropV2(p, L"SourceRedPrimary",   { 0.708, 0.292 }),
                        PropV2(p, L"SourceGreenPrimary", { 0.170, 0.797 }),
                        PropV2(p, L"SourceBluePrimary",  { 0.131, 0.046 }));
                    std::vector<double> key{ 2.0 };
                    for (const auto& v : tgt) { key.push_back(v.x); key.push_back(v.y); }
                    for (const auto& v : src) { key.push_back(v.x); key.push_back(v.y); }
                    const auto lut = memo->Get(key, [&]
                    {
                        std::vector<float> out(256);
                        std::array<CM::V2, 48> sb{}, tb{};
                        for (size_t k = 0; k < out.size(); ++k)
                        {
                            const double I = static_cast<double>(k) / 255.0;
                            CM::SampleBoundary(src, I, sb);
                            CM::SampleBoundary(tgt, I, tb);
                            out[k] = static_cast<float>(FitScale(sb, tb));
                        }
                        return out;
                    });
                    write(L"FitScaleLut", lut->data(), lut->size());
                }
                else
                {
                    std::vector<double> key{ 0.0 };
                    for (const auto& v : tgt) { key.push_back(v.x); key.push_back(v.y); }
                    const auto lut = memo->Get(key, [&]
                    {
                        constexpr size_t kLevels = 64;   // POLY_LEVELS in the HLSL
                        std::vector<float> out;
                        out.reserve(kLevels * 48 * 2);
                        std::array<CM::V2, 48> b{};
                        for (size_t lv = 0; lv < kLevels; ++lv)
                        {
                            CM::SampleBoundary(tgt, static_cast<double>(lv) / (kLevels - 1), b);
                            for (const auto& v : b)
                            {
                                out.push_back(static_cast<float>(v.x));
                                out.push_back(static_cast<float>(v.y));
                            }
                        }
                        return out;
                    });
                    write(L"TgtPolyLut", lut->data(), lut->size());
                }
            };
        }

        ShaderLabEffectDescriptor::DerivedConstantsFn MakeIctcpBoundaryDerived()
        {
            auto memo = std::make_shared<DerivedMemo>();
            return [memo](const PropMap& p, const ShaderLabEffectDescriptor::DerivedConstantWriter& write)
            {
                const int tg = static_cast<int>(PropNumber(p, L"TargetGamut", 0.0));
                const auto g = CM::GamutPrimaries(tg,
                    PropV2(p, L"RedPrimary",   { 0.64, 0.33 }),
                    PropV2(p, L"GreenPrimary", { 0.30, 0.60 }),
                    PropV2(p, L"BluePrimary",  { 0.15, 0.06 }));
                const double intensity = PropNumber(p, L"Intensity", 0.5);
                std::vector<double> key{ intensity };
                for (const auto& v : g) { key.push_back(v.x); key.push_back(v.y); }
                const auto pts = memo->Get(key, [&]
                {
                    // Must match the level order the HLSL colours them in.
                    const double levels[6] = { 0.15, 0.3, 0.45, 0.6, 0.75, intensity };
                    std::vector<float> out;
                    out.reserve(6 * 24 * 2);
                    std::array<CM::V2, 24> b{};
                    for (double lv : levels)
                    {
                        CM::SampleBoundary(g, lv, b);
                        for (const auto& v : b)
                        {
                            out.push_back(static_cast<float>(v.x));
                            out.push_back(static_cast<float>(v.y));
                        }
                    }
                    return out;
                });
                write(L"BndPts", pts->data(), pts->size());
            };
        }

        // Side by Side: columns and rows for `count` tiles. Must match Shape()
        // in its HLSL.
        void SideBySideShape(uint32_t layout, uint32_t count, uint32_t& columns, uint32_t& rows)
        {
            columns = count;
            rows = 1;
            if (layout == 1)
            {
                columns = 1;
                rows = count;
            }
            else if (layout == 2)
            {
                columns = 1;
                while (columns * columns < count) ++columns;
                rows = (count + columns - 1) / columns;
            }
        }

        // Side by Side output size. Native: the arrangement at 1:1, tiles the
        // size of the first connected input. Fit: OutputWidth x OutputHeight.
        D2D1_SIZE_U SideBySideOutputSize(const PropMap& props, const std::vector<D2D1_SIZE_U>& inputSizes)
        {
            if (PropNumber(props, L"SizeMode", 0.0) >= 0.5)
            {
                const double width  = std::round(PropNumber(props, L"OutputWidth", 1920.0));
                const double height = std::round(PropNumber(props, L"OutputHeight", 1080.0));
                return D2D1::SizeU(static_cast<UINT32>(std::clamp(width, 1.0, 1.0e6)),
                                   static_cast<UINT32>(std::clamp(height, 1.0, 1.0e6)));
            }

            uint32_t count = 0;
            D2D1_SIZE_U tile{ 0, 0 };
            for (const auto& size : inputSizes)
            {
                if (size.width == 0 || size.height == 0) continue;
                if (count == 0) tile = size;
                ++count;
            }
            if (count == 0)
                return D2D1::SizeU(64, 64);

            uint32_t columns = 0, rows = 0;
            SideBySideShape(static_cast<uint32_t>(PropNumber(props, L"Layout", 0.0) + 0.5), count, columns, rows);
            const uint64_t gap = static_cast<uint64_t>(std::floor((std::max)(PropNumber(props, L"Gap", 0.0), 0.0) + 0.5));
            const uint64_t width  = uint64_t{ columns } * (tile.width + gap) - gap;
            const uint64_t height = uint64_t{ rows } * (tile.height + gap) - gap;
            constexpr uint64_t cMaxReported = 1u << 30;
            return D2D1::SizeU(static_cast<UINT32>((std::min)(width, cMaxReported)),
                               static_cast<UINT32>((std::min)(height, cMaxReported)));
        }
    }

    void AttachBuiltInHooks(ShaderLabEffectDescriptor& descriptor)
    {
        if (descriptor.effectId == L"ICtCp Gamut Map")
            descriptor.deriveConstants = MakeIctcpGamutMapDerived();
        else if (descriptor.effectId == L"ICtCp Boundary")
            descriptor.deriveConstants = MakeIctcpBoundaryDerived();
        else if (descriptor.effectId == L"Side by Side")
            descriptor.deriveImageOutputSize = SideBySideOutputSize;
    }
}
