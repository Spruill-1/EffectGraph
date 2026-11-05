#include "pch_engine.h"
#include "DdaStageSim.h"

#include <cmath>
#include <cstring>

namespace ShaderLab::Tests::Dda
{
    // ---- Helpers ------------------------------------------------------------

    uint16_t ToHalf(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        const uint32_t sign = (bits >> 16) & 0x8000u;
        const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 112;
        if (exponent <= 0) return static_cast<uint16_t>(sign);   // below the half normals: zero
        if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
        // Round to nearest, so a value and its half differ by under half an ULP.
        uint32_t half = sign | (static_cast<uint32_t>(exponent) << 10) | ((bits >> 13) & 0x3FFu);
        if ((bits & 0x1FFFu) > 0x1000u || ((bits & 0x1FFFu) == 0x1000u && (half & 1u))) ++half;
        return static_cast<uint16_t>(half);
    }

    float FromHalf(uint16_t half)
    {
        const uint32_t exponent = (half >> 10) & 0x1Fu;
        if (exponent == 0) return 0.0f;
        const uint32_t bits = (static_cast<uint32_t>(half & 0x8000u) << 16) | ((exponent + 112u) << 23) |
                              (static_cast<uint32_t>(half & 0x3FFu) << 13);
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    bool Inside(const std::vector<RECT>& rects, uint32_t x, uint32_t y)
    {
        for (const auto& r : rects)
            if (static_cast<LONG>(x) >= r.left && static_cast<LONG>(x) < r.right &&
                static_cast<LONG>(y) >= r.top && static_cast<LONG>(y) < r.bottom)
                return true;
        return false;
    }

    std::vector<uint8_t> ReadBytes(ID3D11Texture2D* texture)
    {
        winrt::com_ptr<ID3D11Device> device;
        texture->GetDevice(device.put());
        winrt::com_ptr<ID3D11DeviceContext> context;
        device->GetImmediateContext(context.put());
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, staging.put()));
        context->CopyResource(staging.get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<uint8_t> bytes(static_cast<size_t>(desc.Width) * desc.Height * 4);
        for (uint32_t y = 0; y < desc.Height; ++y)
            std::memcpy(&bytes[static_cast<size_t>(y) * desc.Width * 4],
                        static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                        static_cast<size_t>(desc.Width) * 4);
        context->Unmap(staging.get(), 0);
        return bytes;
    }

    PixelDiff Compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t width,
                      const std::vector<RECT>& rects)
    {
        PixelDiff diff;
        if (a.size() != b.size() || width == 0) return { a.size() / 4 + 1, a.size() / 4 + 1 };
        const size_t pixels = a.size() / 4;
        for (size_t i = 0; i < pixels; ++i)
        {
            if (std::memcmp(&a[i * 4], &b[i * 4], 4) == 0) continue;
            ++diff.differing;
            if (!Inside(rects, static_cast<uint32_t>(i % width), static_cast<uint32_t>(i / width))) ++diff.outside;
        }
        return diff;
    }

    std::vector<RECT> ChangedRects(const DesktopFrame& frame)
    {
        std::vector<RECT> rects;
        for (const auto& move : frame.moves) rects.push_back(move.DestinationRect);
        rects.insert(rects.end(), frame.dirty.begin(), frame.dirty.end());
        return rects;
    }

    namespace
    {
        RECT Clip(const RECT& r, uint32_t width, uint32_t height)
        {
            RECT c{ (std::max)(r.left, 0L), (std::max)(r.top, 0L),
                    (std::min)(r.right, static_cast<LONG>(width)), (std::min)(r.bottom, static_cast<LONG>(height)) };
            if (c.right < c.left) c.right = c.left;
            if (c.bottom < c.top) c.bottom = c.top;
            return c;
        }

        bool Empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }
    }

    // ---- Desktop ------------------------------------------------------------

    Desktop::Desktop(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width, uint32_t height)
        : m_width(width), m_height(height), m_context(context)
    {
        m_texels.assign(static_cast<size_t>(width) * height * 4, ToHalf(0.0f));
        for (size_t i = 3; i < m_texels.size(); i += 4) m_texels[i] = ToHalf(1.0f);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{ m_texels.data(), width * 8, 0 };
        winrt::check_hresult(device->CreateTexture2D(&desc, &init, m_texture.put()));
    }

    void Desktop::Put(uint32_t x, uint32_t y, float r, float g, float b)
    {
        uint16_t* texel = &m_texels[(static_cast<size_t>(y) * m_width + x) * 4];
        texel[0] = ToHalf(r);
        texel[1] = ToHalf(g);
        texel[2] = ToHalf(b);
        texel[3] = ToHalf(1.0f);
    }

    void Desktop::ApplyMoves(const std::vector<EFFECTGRAPH_MOVE_RECT>& moves)
    {
        for (const auto& move : moves)
        {
            const RECT dest = Clip(move.DestinationRect, m_width, m_height);
            if (Empty(dest)) continue;
            const LONG dx = move.SourcePoint.x - move.DestinationRect.left;
            const LONG dy = move.SourcePoint.y - move.DestinationRect.top;
            const RECT source{ dest.left + dx, dest.top + dy, dest.right + dx, dest.bottom + dy };
            const RECT clipped = Clip(source, m_width, m_height);
            if (clipped.left != source.left || clipped.top != source.top || clipped.right != source.right ||
                clipped.bottom != source.bottom)
                continue;   // a source outside the surface: nothing sensible to copy
            const auto pixels = ReadRect(source);   // through a copy: source and destination may overlap
            WriteRect(dest, pixels.data());
        }
    }

    void Desktop::WriteRect(const RECT& rect, const uint16_t* pixels)
    {
        const uint32_t w = static_cast<uint32_t>(rect.right - rect.left);
        for (LONG y = rect.top; y < rect.bottom; ++y)
            std::memcpy(&m_texels[(static_cast<size_t>(y) * m_width + rect.left) * 4],
                        pixels + static_cast<size_t>(y - rect.top) * w * 4, static_cast<size_t>(w) * 8);
    }

    std::vector<uint16_t> Desktop::ReadRect(const RECT& rect) const
    {
        const uint32_t w = static_cast<uint32_t>(rect.right - rect.left);
        const uint32_t h = static_cast<uint32_t>(rect.bottom - rect.top);
        std::vector<uint16_t> pixels(static_cast<size_t>(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(&pixels[static_cast<size_t>(y) * w * 4],
                        &m_texels[(static_cast<size_t>(rect.top + y) * m_width + rect.left) * 4],
                        static_cast<size_t>(w) * 8);
        return pixels;
    }

    void Desktop::Upload(const std::vector<RECT>& rects)
    {
        for (const auto& r : rects)
        {
            const RECT c = Clip(r, m_width, m_height);
            if (Empty(c)) continue;
            const D3D11_BOX box{ static_cast<UINT>(c.left), static_cast<UINT>(c.top), 0,
                                 static_cast<UINT>(c.right), static_cast<UINT>(c.bottom), 1 };
            const uint16_t* first = &m_texels[(static_cast<size_t>(c.top) * m_width + c.left) * 4];
            m_context->UpdateSubresource(m_texture.get(), 0, &box, first, m_width * 8, 0);
        }
    }

    void Desktop::UploadAll()
    {
        m_context->UpdateSubresource(m_texture.get(), 0, nullptr, m_texels.data(), m_width * 8, 0);
    }

    // ---- Scenarios ------------------------------------------------------------

    namespace
    {
        // The desktop behind every window: SDR colour gradients with a few
        // HDR bars, so the tone map has highlights to compress.
        void Background(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float rgb[3])
        {
            rgb[0] = 0.05f + 0.85f * static_cast<float>(x) / static_cast<float>(w);
            rgb[1] = 0.05f + 0.85f * static_cast<float>(y) / static_cast<float>(h);
            rgb[2] = 0.45f;
            if (x % 97 < 3) rgb[0] = rgb[1] = rgb[2] = 6.0f;   // 480 nits
        }

        // A page of "text": dark glyph runs on SDR white, by document row.
        void Page(uint32_t x, uint32_t docY, float rgb[3])
        {
            const uint32_t line = docY / 12, inLine = docY % 12;
            const bool glyph = inLine >= 2 && inLine < 10 && ((x / 6 + line * 5) % 9) < 6 && (x / 6) % 23 != 0;
            const float v = glyph ? 0.03f : 0.96f;
            rgb[0] = rgb[1] = v;
            rgb[2] = glyph ? 0.03f : 0.98f;
        }

        // HDR video content at time t: up to ~1000 nits, with wide-gamut
        // colour (negative scRGB components) and a moving highlight.
        void Video(uint32_t x, uint32_t y, uint32_t t, float rgb[3])
        {
            const float fx = static_cast<float>(x), fy = static_cast<float>(y), ft = static_cast<float>(t);
            const float wave = 0.5f + 0.5f * std::sin(fx * 0.031f + ft * 0.4f) * std::cos(fy * 0.027f - ft * 0.3f);
            rgb[0] = 0.2f + 3.5f * wave;
            rgb[1] = -0.15f + 1.2f * (1.0f - wave);   // below zero: outside sRGB
            rgb[2] = 0.1f + 0.8f * wave * wave;
            const float bx = 40.0f + std::fmod(ft * 23.0f, 400.0f), by = 30.0f + std::fmod(ft * 11.0f, 200.0f);
            const float d2 = (fx - bx) * (fx - bx) + (fy - by) * (fy - by);
            if (d2 < 400.0f) rgb[0] = rgb[1] = rgb[2] = 12.5f;   // 1000 nits
        }

        void PaintBackground(Desktop& d, const RECT& r)
        {
            float rgb[3];
            for (LONG y = r.top; y < r.bottom; ++y)
                for (LONG x = r.left; x < r.right; ++x)
                {
                    Background(x, y, d.Width(), d.Height(), rgb);
                    d.Put(x, y, rgb[0], rgb[1], rgb[2]);
                }
        }

        void PaintVideo(Desktop& d, const RECT& r, uint32_t t)
        {
            float rgb[3];
            for (LONG y = r.top; y < r.bottom; ++y)
                for (LONG x = r.left; x < r.right; ++x)
                {
                    Video(x - r.left, y - r.top, t, rgb);
                    d.Put(x, y, rgb[0], rgb[1], rgb[2]);
                }
        }

        void PaintPage(Desktop& d, const RECT& r, uint32_t docTop)
        {
            float rgb[3];
            for (LONG y = r.top; y < r.bottom; ++y)
                for (LONG x = r.left; x < r.right; ++x)
                {
                    Page(x - r.left, docTop + (y - r.top), rgb);
                    d.Put(x, y, rgb[0], rgb[1], rgb[2]);
                }
        }

        RECT Scaled(const Desktop& d, float l, float t, float r, float b)
        {
            return { static_cast<LONG>(d.Width() * l), static_cast<LONG>(d.Height() * t),
                     static_cast<LONG>(d.Width() * r), static_cast<LONG>(d.Height() * b) };
        }

        class ScenarioBase : public Scenario
        {
        public:
            ScenarioBase(ScenarioKind kind, uint32_t frames) : m_kind(kind), m_frames(frames) {}
            const char* Name() const override { return ScenarioName(m_kind); }

        protected:
            bool Begin(DesktopFrame& frame)
            {
                if (m_presents >= m_frames) return false;
                frame = DesktopFrame{};
                frame.lastPresentTime = static_cast<int64_t>(m_tick) * 166667;
                return true;
            }

            ScenarioKind m_kind;
            uint32_t m_frames;
            uint32_t m_presents{ 0 };
            uint32_t m_tick{ 1 };
        };

        class IdleCursor : public ScenarioBase
        {
        public:
            using ScenarioBase::ScenarioBase;
            void Start(Desktop& d) override
            {
                PaintBackground(d, { 0, 0, static_cast<LONG>(d.Width()), static_cast<LONG>(d.Height()) });
                m_page = Scaled(d, 0.1f, 0.1f, 0.6f, 0.8f);
                PaintPage(d, m_page, 0);
                m_caret = { m_page.left + 60, m_page.top + 40, m_page.left + 62, m_page.top + 56 };
            }
            bool Next(Desktop& d, DesktopFrame& frame) override
            {
                if (!Begin(frame)) return false;
                ++m_tick;
                // Two pointer-only frames between presents.
                if (m_pointer < 2)
                {
                    ++m_pointer;
                    frame.kind = FrameKind::NoFrame;
                    frame.lastPresentTime = 0;
                    frame.accumulatedFrames = 0;
                    return true;
                }
                m_pointer = 0;
                m_on = !m_on;
                for (LONG y = m_caret.top; y < m_caret.bottom; ++y)
                    for (LONG x = m_caret.left; x < m_caret.right; ++x)
                    {
                        float rgb[3];
                        Page(x - m_page.left, y - m_page.top, rgb);
                        if (m_on) rgb[0] = rgb[1] = rgb[2] = 0.0f;
                        d.Put(x, y, rgb[0], rgb[1], rgb[2]);
                    }
                // Every fifth present arrives with no metadata: the caret
                // changed, but only a Whole realize can know it.
                if (m_presents % 5 == 4) frame.whole = true;
                else frame.dirty = { m_caret };
                ++m_presents;
                return true;
            }

        private:
            RECT m_page{}, m_caret{};
            uint32_t m_pointer{ 0 };
            bool m_on{ false };
        };

        // An HDR window whose highlight climbs 600 to 2600 nits and starts
        // again, and a block over SdrWhiteNits that comes and goes every
        // fourth present; an idle (pointer-only) frame after every third.
        class HdrHighlights : public ScenarioBase
        {
        public:
            using ScenarioBase::ScenarioBase;
            void Start(Desktop& d) override
            {
                PaintBackground(d, { 0, 0, static_cast<LONG>(d.Width()), static_cast<LONG>(d.Height()) });
                const RECT window = Scaled(d, 0.2f, 0.2f, 0.8f, 0.8f);
                m_light = { window.left + 8, window.top + 8, window.left + 16, window.top + 16 };
                m_block = Scaled(d, 0.3f, 0.5f, 0.7f, 0.75f);
                Paint(d, window, 1.0f);
                Paint(d, m_light, 600.0f / 80.0f);
            }
            bool Next(Desktop& d, DesktopFrame& frame) override
            {
                if (!Begin(frame)) return false;
                ++m_tick;
                if (m_idle)
                {
                    m_idle = false;
                    frame.kind = FrameKind::NoFrame;
                    frame.lastPresentTime = 0;
                    frame.accumulatedFrames = 0;
                    return true;
                }
                const float nits = 600.0f + 400.0f * static_cast<float>((m_presents + 1) % 6);
                Paint(d, m_light, nits / 80.0f);
                frame.dirty = { m_light };
                if (m_presents % 4 == 3)
                {
                    m_blockOn = !m_blockOn;
                    Paint(d, m_block, m_blockOn ? 3.5f : 1.0f);
                    frame.dirty.push_back(m_block);
                }
                ++m_presents;
                m_idle = m_presents % 3 == 0;
                return true;
            }

        private:
            static void Paint(Desktop& d, const RECT& r, float grey)
            {
                for (LONG y = r.top; y < r.bottom; ++y)
                    for (LONG x = r.left; x < r.right; ++x)
                        d.Put(static_cast<uint32_t>(x), static_cast<uint32_t>(y), grey, grey, grey);
            }

            RECT m_light{}, m_block{};
            bool m_idle{ false }, m_blockOn{ false };
        };

        class Scroll : public ScenarioBase
        {
        public:
            using ScenarioBase::ScenarioBase;
            void Start(Desktop& d) override
            {
                PaintBackground(d, { 0, 0, static_cast<LONG>(d.Width()), static_cast<LONG>(d.Height()) });
                m_window = Scaled(d, 0.125f, 0.125f, 0.625f, 0.875f);
                PaintPage(d, m_window, 0);
            }
            bool Next(Desktop& d, DesktopFrame& frame) override
            {
                if (!Begin(frame)) return false;
                ++m_tick;
                constexpr LONG cStep = 16;
                m_docTop += cStep;
                frame.moves.push_back({ { m_window.left, m_window.top + cStep },
                                        { m_window.left, m_window.top, m_window.right, m_window.bottom - cStep } });
                d.ApplyMoves(frame.moves);
                const RECT strip{ m_window.left, m_window.bottom - cStep, m_window.right, m_window.bottom };
                float rgb[3];
                for (LONG y = strip.top; y < strip.bottom; ++y)
                    for (LONG x = strip.left; x < strip.right; ++x)
                    {
                        Page(x - m_window.left, m_docTop + (y - m_window.top), rgb);
                        d.Put(x, y, rgb[0], rgb[1], rgb[2]);
                    }
                frame.dirty = { strip };
                ++m_presents;
                return true;
            }

        private:
            RECT m_window{};
            uint32_t m_docTop{ 0 };
        };

        class HdrVideo : public ScenarioBase
        {
        public:
            using ScenarioBase::ScenarioBase;
            void Start(Desktop& d) override
            {
                PaintBackground(d, { 0, 0, static_cast<LONG>(d.Width()), static_cast<LONG>(d.Height()) });
                m_video = Scaled(d, 0.25f, 0.25f, 0.75f, 0.75f);
                PaintVideo(d, m_video, 0);
            }
            bool Next(Desktop& d, DesktopFrame& frame) override
            {
                if (!Begin(frame)) return false;
                ++m_tick;
                PaintVideo(d, m_video, m_presents + 1);
                frame.dirty = { m_video };
                ++m_presents;
                return true;
            }

        private:
            RECT m_video{};
        };

        class WindowDrag : public ScenarioBase
        {
        public:
            using ScenarioBase::ScenarioBase;
            void Start(Desktop& d) override
            {
                PaintBackground(d, { 0, 0, static_cast<LONG>(d.Width()), static_cast<LONG>(d.Height()) });
                const RECT r = Scaled(d, 0.05f, 0.05f, 0.38f, 0.38f);
                m_window = r;
                PaintPage(d, m_window, 0);
            }
            bool Next(Desktop& d, DesktopFrame& frame) override
            {
                if (!Begin(frame)) return false;
                ++m_tick;
                const LONG w = m_window.right - m_window.left, h = m_window.bottom - m_window.top;
                LONG nx = m_window.left + m_dx, ny = m_window.top + m_dy;
                if (nx < 0 || nx + w > static_cast<LONG>(d.Width())) { m_dx = -m_dx; nx = m_window.left + m_dx; }
                if (ny < 0 || ny + h > static_cast<LONG>(d.Height())) { m_dy = -m_dy; ny = m_window.top + m_dy; }
                const RECT old = m_window;
                m_window = { nx, ny, nx + w, ny + h };
                frame.moves.push_back({ { old.left, old.top }, m_window });
                d.ApplyMoves(frame.moves);
                // The background the window uncovered: up to two strips.
                std::vector<RECT> exposed;
                if (m_window.left > old.left) exposed.push_back({ old.left, old.top, m_window.left, old.bottom });
                if (m_window.right < old.right) exposed.push_back({ m_window.right, old.top, old.right, old.bottom });
                const LONG l = (std::max)(old.left, m_window.left), r = (std::min)(old.right, m_window.right);
                if (m_window.top > old.top) exposed.push_back({ l, old.top, r, m_window.top });
                if (m_window.bottom < old.bottom) exposed.push_back({ l, m_window.bottom, r, old.bottom });
                for (const auto& e : exposed) PaintBackground(d, e);
                frame.dirty = exposed;
                ++m_presents;
                return true;
            }

        private:
            RECT m_window{};
            LONG m_dx{ 12 }, m_dy{ 7 };
        };

        class CoverAll : public ScenarioBase
        {
        public:
            using ScenarioBase::ScenarioBase;
            void Start(Desktop& d) override { PaintVideo(d, Whole(d), 0); }
            bool Next(Desktop& d, DesktopFrame& frame) override
            {
                if (!Begin(frame)) return false;
                ++m_tick;
                PaintVideo(d, Whole(d), m_presents + 1);
                const LONG w = static_cast<LONG>(d.Width()), h = static_cast<LONG>(d.Height());
                frame.dirty = { { 0, 0, w / 2, h / 2 }, { w / 2, 0, w, h / 2 },
                                { 0, h / 2, w / 2, h }, { w / 2, h / 2, w, h } };
                ++m_presents;
                return true;
            }

        private:
            static RECT Whole(const Desktop& d)
            {
                return { 0, 0, static_cast<LONG>(d.Width()), static_cast<LONG>(d.Height()) };
            }
        };

        class VideoAndScroll : public ScenarioBase
        {
        public:
            using ScenarioBase::ScenarioBase;
            void Start(Desktop& d) override
            {
                PaintBackground(d, { 0, 0, static_cast<LONG>(d.Width()), static_cast<LONG>(d.Height()) });
                m_page = Scaled(d, 0.02f, 0.1f, 0.45f, 0.9f);
                m_video = Scaled(d, 0.5f, 0.2f, 0.95f, 0.7f);
                PaintPage(d, m_page, 0);
                PaintVideo(d, m_video, 0);
            }
            bool Next(Desktop& d, DesktopFrame& frame) override
            {
                if (!Begin(frame)) return false;
                ++m_tick;
                constexpr LONG cStep = 8;
                m_docTop += cStep;
                frame.moves.push_back({ { m_page.left, m_page.top + cStep },
                                        { m_page.left, m_page.top, m_page.right, m_page.bottom - cStep } });
                d.ApplyMoves(frame.moves);
                const RECT strip{ m_page.left, m_page.bottom - cStep, m_page.right, m_page.bottom };
                float rgb[3];
                for (LONG y = strip.top; y < strip.bottom; ++y)
                    for (LONG x = strip.left; x < strip.right; ++x)
                    {
                        Page(x - m_page.left, m_docTop + (y - m_page.top), rgb);
                        d.Put(x, y, rgb[0], rgb[1], rgb[2]);
                    }
                frame.dirty.push_back(strip);
                // The video, reported as a 5x4 grid of tiles: 20 rects.
                PaintVideo(d, m_video, m_presents + 1);
                const LONG vw = m_video.right - m_video.left, vh = m_video.bottom - m_video.top;
                for (LONG ty = 0; ty < 4; ++ty)
                    for (LONG tx = 0; tx < 5; ++tx)
                        frame.dirty.push_back({ m_video.left + vw * tx / 5, m_video.top + vh * ty / 4,
                                                m_video.left + vw * (tx + 1) / 5, m_video.top + vh * (ty + 1) / 4 });
                ++m_presents;
                return true;
            }

        private:
            RECT m_page{}, m_video{};
            uint32_t m_docTop{ 0 };
        };
    }

    const char* ScenarioName(ScenarioKind kind)
    {
        switch (kind)
        {
        case ScenarioKind::IdleCursor:     return "IdleCursor";
        case ScenarioKind::Scroll:         return "Scroll";
        case ScenarioKind::HdrVideo:       return "HdrVideo";
        case ScenarioKind::WindowDrag:     return "WindowDrag";
        case ScenarioKind::CoverAll:       return "CoverAll";
        case ScenarioKind::VideoAndScroll: return "VideoAndScroll";
        case ScenarioKind::HdrHighlights:  return "HdrHighlights";
        }
        return "?";
    }

    std::unique_ptr<Scenario> MakeScenario(ScenarioKind kind, uint32_t frames)
    {
        switch (kind)
        {
        case ScenarioKind::IdleCursor:     return std::make_unique<IdleCursor>(kind, frames);
        case ScenarioKind::Scroll:         return std::make_unique<Scroll>(kind, frames);
        case ScenarioKind::HdrVideo:       return std::make_unique<HdrVideo>(kind, frames);
        case ScenarioKind::WindowDrag:     return std::make_unique<WindowDrag>(kind, frames);
        case ScenarioKind::CoverAll:       return std::make_unique<CoverAll>(kind, frames);
        case ScenarioKind::VideoAndScroll: return std::make_unique<VideoAndScroll>(kind, frames);
        case ScenarioKind::HdrHighlights:  return std::make_unique<HdrHighlights>(kind, frames);
        }
        return nullptr;
    }

    // ---- The stage ----------------------------------------------------------

    StageSim::StageSim(IEffectGraphContext* context, IEffectGraphNode* input, IEffectGraphNode* root,
                       ID3D11Texture2D* intermediate, ID3D11DeviceContext* immediate)
        : m_immediate(immediate), m_hosted(std::make_unique<HostedDriver>())
    {
        winrt::check_hresult(context->QueryInterface(__uuidof(IEffectGraphContext2), m_context.put_void()));
        winrt::check_hresult(input->QueryInterface(__uuidof(IEffectGraphInput2), m_input.put_void()));
        m_root.copy_from(root);
        m_intermediate.copy_from(intermediate);
        D3D11_TEXTURE2D_DESC desc{};
        intermediate->GetDesc(&desc);
        m_width = desc.Width;
        m_height = desc.Height;
    }

    uint32_t StageSim::AddClient(ID3D11Texture2D* surface, IDXGIKeyedMutex* mutex)
    {
        Client client;
        client.surface.copy_from(surface);
        client.mutex.copy_from(mutex);
        m_clients.push_back(std::move(client));
        return static_cast<uint32_t>(m_clients.size() - 1);
    }

    HRESULT StageSim::Realize()
    {
        RECT drawn[16]{};
        UINT32 count = 0;
        EFFECTGRAPH_MOVE_RECT moved[32]{};
        UINT32 movedCount = 0;
        m_lastRealize = m_context->RealizeToIncremental(m_root.get(), m_intermediate.get(),
                                                        EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL, drawn, 16, &count,
                                                        m_moves ? moved : nullptr, m_moves ? 32 : 0,
                                                        m_moves ? &movedCount : nullptr);
        m_lastDrawn.clear();
        m_lastMoved.clear();
        if (FAILED(m_lastRealize)) return m_lastRealize;
        m_lastDrawn.assign(drawn, drawn + count);
        m_lastMoved.assign(moved, moved + movedCount);
        const bool whole = count == 1 && drawn[0].left == 0 && drawn[0].top == 0 &&
                           drawn[0].right == static_cast<LONG>(m_width) && drawn[0].bottom == static_cast<LONG>(m_height);
        ++m_realizes;
        if (whole) ++m_wholeDraws;
        // Row by row, and only while something is marked: a timed present
        // includes this.
        if (m_trackDrift)
        {
            const size_t pixels = static_cast<size_t>(m_width) * m_height;
            const auto mark = [this](const RECT& r, uint8_t value)
            {
                const LONG left = (std::max)(r.left, 0L), top = (std::max)(r.top, 0L);
                const LONG right = (std::min)(r.right, static_cast<LONG>(m_width));
                const LONG bottom = (std::min)(r.bottom, static_cast<LONG>(m_height));
                if (right <= left) return;
                for (LONG y = top; y < bottom; ++y)
                    std::memset(&m_drift[static_cast<size_t>(y) * m_width + left], value, static_cast<size_t>(right - left));
            };
            if (m_drift.size() != pixels)
            {
                m_drift.assign(pixels, 0);
                m_anyDrift = false;
            }
            if (whole && m_anyDrift)
            {
                std::fill(m_drift.begin(), m_drift.end(), uint8_t{ 0 });
                m_anyDrift = false;
            }
            else if (!whole)
            {
                for (const auto& move : m_lastMoved)
                {
                    mark(move.DestinationRect, 1);
                    m_anyDrift = true;
                }
                if (m_anyDrift)
                    for (const auto& r : m_lastDrawn) mark(r, 0);
            }
        }
        for (auto& client : m_clients)
        {
            ++client.presents;
            if (client.whole) continue;
            if (!m_lastDrawn.empty() || !m_lastMoved.empty()) ++client.contributing;
            if (whole)
            {
                client.whole = true;
                client.rects.clear();
                client.moves.clear();
                client.drawn.clear();
            }
            else
            {
                // Moves first, then the rects drawn; the copy takes both.
                for (const auto& move : m_lastMoved)
                {
                    client.moves.push_back(move);
                    client.rects.push_back(move.DestinationRect);
                }
                client.rects.insert(client.rects.end(), m_lastDrawn.begin(), m_lastDrawn.end());
                client.drawn.insert(client.drawn.end(), m_lastDrawn.begin(), m_lastDrawn.end());
            }
        }
        return m_lastRealize;
    }

    HRESULT StageSim::Prime()
    {
        m_realizes = m_wholeDraws = 0;
        m_hostedCommitted = false;
        return Realize();
    }

    HRESULT StageSim::Decide(bool present)
    {
        m_realizes = m_wholeDraws = 0;
        m_hostedCommitted = false;
        if (m_pollFirst)
        {
            // The stage: what landed since the last present is committed (Exact,
            // Threshold) or decided on (Hosted) before the one realize.
            // PollReadbacks fails only without the deferred commit; the realize
            // reports anything else.
            (void)m_context->PollReadbacks();
            m_hostedCommitted = m_hosted->OnPresent(present);
            return Realize();
        }
        // The drain arm: it sees the landed value only after the realize, and
        // realizes once more to draw what it committed.
        HRESULT hr = Realize();
        if (FAILED(hr)) return hr;
        if (m_hosted->OnPresent(present))
        {
            m_hostedCommitted = true;
            hr = Realize();
        }
        return hr;
    }

    HRESULT StageSim::Present(const DesktopFrame& frame)
    {
        if (frame.whole)
        {
            m_input->Invalidate();
        }
        else if (m_moves)
        {
            if (!frame.moves.empty()) m_input->MoveRects(frame.moves.data(), static_cast<UINT32>(frame.moves.size()));
            if (!frame.dirty.empty()) m_input->InvalidateRects(frame.dirty.data(), static_cast<UINT32>(frame.dirty.size()));
        }
        else
        {
            const auto changed = ChangedRects(frame);
            if (!changed.empty()) m_input->InvalidateRects(changed.data(), static_cast<UINT32>(changed.size()));
        }
        return Decide(true);
    }

    bool StageSim::TickDue() const
    {
        return FAILED(m_lastRealize) || m_lastRealize == S_FALSE || m_hosted->WouldRatchet();
    }

    HRESULT StageSim::Tick() { return Decide(false); }

    // ---- The damped Hosted driver ---------------------------------------------

    DampedHostedDriver::DampedHostedDriver(IEffectGraphNode* producer, const wchar_t* field, uint32_t decayPresents)
        : m_field(field), m_decayPresents(decayPresents)
    {
        winrt::check_hresult(producer->QueryInterface(__uuidof(IEffectGraphNode2), m_producer.put_void()));
    }

    bool DampedHostedDriver::Values(float& landed, float& committed) const
    {
        winrt::com_ptr<IEffectGraphValues> landedValues, committedValues;
        // A pending commit (S_FALSE with values) still holds the Hosted field.
        if (m_producer->TryGetValues(landedValues.put()) != S_OK ||
            FAILED(m_producer->GetCommittedValues(committedValues.put())) || !committedValues)
            return false;
        return SUCCEEDED(landedValues->GetFloat(m_field.c_str(), &landed)) &&
               SUCCEEDED(committedValues->GetFloat(m_field.c_str(), &committed));
    }

    bool DampedHostedDriver::WouldRatchet() const
    {
        float landed = 0, committed = 0;
        return Values(landed, committed) && landed > committed;
    }

    bool DampedHostedDriver::OnPresent(bool present)
    {
        float landed = 0, committed = 0;
        if (!Values(landed, committed)) return false;
        bool commit = false;
        if (landed > committed)
        {
            commit = true;
            ++m_ratchets;
        }
        else if (landed < committed)
        {
            if (present && ++m_below >= m_decayPresents)
            {
                commit = true;
                ++m_decays;
            }
        }
        else
        {
            m_below = 0;
        }
        if (!commit) return false;
        m_below = 0;
        winrt::com_ptr<IEffectGraphValues> values;
        winrt::check_hresult(m_producer->TryGetValues(values.put()));
        const UINT32 index = values->GetFieldIndex(m_field.c_str());
        return SUCCEEDED(m_producer->CommitFields(&index, &landed, 1));
    }

    Delivery StageSim::Acquire(uint32_t index)
    {
        Delivery delivery;
        if (FAILED(m_lastRealize))
        {
            delivery.hr = DXGI_ERROR_WAIT_TIMEOUT;
            return delivery;
        }
        auto& client = m_clients[index];
        if (client.mutex) winrt::check_hresult(client.mutex->AcquireSync(0, INFINITE));
        if (client.whole)
        {
            m_immediate->CopyResource(client.surface.get(), m_intermediate.get());
        }
        else
        {
            for (const auto& r : client.rects)
            {
                const RECT c = Clip(r, m_width, m_height);
                if (Empty(c)) continue;
                const D3D11_BOX box{ static_cast<UINT>(c.left), static_cast<UINT>(c.top), 0,
                                     static_cast<UINT>(c.right), static_cast<UINT>(c.bottom), 1 };
                m_immediate->CopySubresourceRegion(client.surface.get(), 0, box.left, box.top, 0,
                                                   m_intermediate.get(), 0, &box);
            }
        }
        if (client.mutex) winrt::check_hresult(client.mutex->ReleaseSync(1));
        delivery.whole = client.whole;
        delivery.rects = client.whole ? std::vector<RECT>{ { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) } }
                                      : std::move(client.rects);
        delivery.presents = client.presents;
        // One contributing realize: its moves, then its rects. Several: every
        // move is a dirty rect at its destination (design section 0.1).
        if (!delivery.whole && client.contributing <= 1)
        {
            delivery.moves = std::move(client.moves);
            delivery.dirty = std::move(client.drawn);
        }
        else
        {
            delivery.dirty = delivery.rects;
        }
        client.moves.clear();
        client.drawn.clear();
        client.rects.clear();
        client.whole = false;
        client.presents = 0;
        client.contributing = 0;
        return delivery;
    }

    // ---- JPEG XR --------------------------------------------------------------

    namespace
    {
        winrt::com_ptr<IWICImagingFactory> Wic()
        {
            winrt::com_ptr<IWICImagingFactory> factory;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(factory.put()))))
                return nullptr;
            return factory;
        }
    }

    bool WriteHalfJxr(const std::filesystem::path& file, uint32_t width, uint32_t height, const uint16_t* texels)
    {
        auto factory = Wic();
        if (!factory) return false;
        winrt::com_ptr<IWICStream> stream;
        winrt::com_ptr<IWICBitmapEncoder> encoder;
        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        winrt::com_ptr<IPropertyBag2> bag;
        if (FAILED(factory->CreateStream(stream.put())) ||
            FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) ||
            FAILED(factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, encoder.put())) ||
            FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(frame.put(), bag.put())))
            return false;
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"Lossless");
        VARIANT value{};
        value.vt = VT_BOOL;
        value.boolVal = VARIANT_TRUE;
        if (FAILED(bag->Write(1, &option, &value)) || FAILED(frame->Initialize(bag.get())) ||
            FAILED(frame->SetSize(width, height)))
            return false;
        WICPixelFormatGUID format = GUID_WICPixelFormat64bppRGBAHalf;
        if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat64bppRGBAHalf)
            return false;
        const UINT stride = width * 8;
        return SUCCEEDED(frame->WritePixels(height, stride, stride * height,
                                            reinterpret_cast<BYTE*>(const_cast<uint16_t*>(texels)))) &&
               SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }

    bool ReadHalfJxr(const std::filesystem::path& file, uint32_t width, uint32_t height, std::vector<uint16_t>& texels)
    {
        auto factory = Wic();
        if (!factory) return false;
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        winrt::com_ptr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, decoder.put())) ||
            FAILED(decoder->GetFrame(0, frame.put())))
            return false;
        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || w != width || h != height) return false;
        if (FAILED(factory->CreateFormatConverter(converter.put())) ||
            FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat64bppRGBAHalf, WICBitmapDitherTypeNone,
                                         nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            return false;
        texels.resize(static_cast<size_t>(width) * height * 4);
        const UINT stride = width * 8;
        return SUCCEEDED(converter->CopyPixels(nullptr, stride, stride * height, reinterpret_cast<BYTE*>(texels.data())));
    }

    // ---- Recordings -----------------------------------------------------------

    namespace
    {
        std::filesystem::path KeyframePath(const std::filesystem::path& folder, uint32_t index)
        {
            wchar_t name[32];
            swprintf_s(name, L"key_%06u.jxr", index);
            return folder / name;
        }
    }

    RecordingWriter::RecordingWriter(const std::filesystem::path& folder, uint32_t width, uint32_t height,
                                     uint32_t keyframeInterval)
        : m_folder(folder)
    {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        std::memcpy(m_header.magic, "DDAREC1", 8);
        m_header.version = cRecordingVersion;
        m_header.width = width;
        m_header.height = height;
        m_header.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        m_header.rotation = DXGI_MODE_ROTATION_IDENTITY;
        m_header.keyframeInterval = keyframeInterval;
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        m_header.qpcFrequency = frequency.QuadPart;
        if (_wfopen_s(&m_file, (folder / L"frames.bin").c_str(), L"wb") != 0) m_file = nullptr;
        if (m_file) fwrite(&m_header, sizeof(m_header), 1, m_file);
    }

    RecordingWriter::~RecordingWriter() { Close(); }

    bool RecordingWriter::Write(const DesktopFrame& frame, const Desktop& after)
    {
        if (!m_file) return false;
        RecordingFrame record{};
        record.kind = static_cast<uint32_t>(frame.kind);
        record.accumulatedFrames = frame.accumulatedFrames;
        record.rectsCoalesced = frame.rectsCoalesced ? 1 : 0;
        record.lastPresentTime = frame.lastPresentTime;
        record.holdMs = frame.holdMs;
        const bool present = frame.kind == FrameKind::Present;
        record.moveCount = present ? static_cast<uint32_t>(frame.moves.size()) : 0;
        record.dirtyCount = present ? static_cast<uint32_t>(frame.dirty.size()) : 0;
        record.keyframe = present && (frame.whole || (m_index % m_header.keyframeInterval) == 0) ? 1 : 0;
        record.pixelSets = present && !record.keyframe ? 1 : 0;
        fwrite(&record, sizeof(record), 1, m_file);
        if (present)
        {
            for (const auto& move : frame.moves)
            {
                DXGI_OUTDUPL_MOVE_RECT m{ move.SourcePoint, move.DestinationRect };
                fwrite(&m, sizeof(m), 1, m_file);
            }
            if (!frame.dirty.empty()) fwrite(frame.dirty.data(), sizeof(RECT), frame.dirty.size(), m_file);
            if (record.keyframe)
            {
                if (!WriteHalfJxr(KeyframePath(m_folder, m_index), after.Width(), after.Height(), after.Texels().data()))
                    return false;
            }
            else
            {
                for (const auto& r : ChangedRects(frame))
                {
                    const RECT c = Clip(r, after.Width(), after.Height());
                    if (Empty(c)) continue;
                    const auto pixels = after.ReadRect(c);
                    fwrite(pixels.data(), sizeof(uint16_t), pixels.size(), m_file);
                }
            }
        }
        ++m_index;
        return ferror(m_file) == 0;
    }

    bool RecordingWriter::Close()
    {
        if (!m_file) return true;
        m_header.frameCount = m_index;
        fseek(m_file, 0, SEEK_SET);
        fwrite(&m_header, sizeof(m_header), 1, m_file);
        const bool ok = ferror(m_file) == 0;
        fclose(m_file);
        m_file = nullptr;
        return ok;
    }

    RecordingReader::RecordingReader(const std::filesystem::path& folder) : m_folder(folder)
    {
        if (_wfopen_s(&m_file, (folder / L"frames.bin").c_str(), L"rb") != 0 || !m_file)
        {
            m_file = nullptr;
            m_error = "no frames.bin";
            return;
        }
        if (fread(&m_header, sizeof(m_header), 1, m_file) != 1 || std::memcmp(m_header.magic, "DDAREC1", 8) != 0)
        {
            m_error = "not a DDAREC1 recording";
            return;
        }
        if (m_header.version != cRecordingVersion || m_header.format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            m_header.width == 0 || m_header.height == 0)
        {
            m_error = "unsupported version, format or size";
            return;
        }
        m_ok = true;
    }

    RecordingReader::~RecordingReader()
    {
        if (m_file) fclose(m_file);
    }

    bool RecordingReader::Next(Desktop& desktop, DesktopFrame& frame, size_t& keyframeMismatches)
    {
        keyframeMismatches = 0;
        if (!m_ok || m_index >= m_header.frameCount) return false;
        RecordingFrame record{};
        if (fread(&record, sizeof(record), 1, m_file) != 1)
        {
            m_error = "truncated frame record";
            m_ok = false;
            return false;
        }
        frame = DesktopFrame{};
        frame.kind = static_cast<FrameKind>(record.kind);
        frame.accumulatedFrames = record.accumulatedFrames;
        frame.rectsCoalesced = record.rectsCoalesced != 0;
        frame.lastPresentTime = record.lastPresentTime;
        frame.holdMs = record.holdMs;
        for (uint32_t i = 0; i < record.moveCount; ++i)
        {
            DXGI_OUTDUPL_MOVE_RECT m{};
            if (fread(&m, sizeof(m), 1, m_file) != 1) { m_ok = false; m_error = "truncated moves"; return false; }
            frame.moves.push_back({ m.SourcePoint, m.DestinationRect });
        }
        frame.dirty.resize(record.dirtyCount);
        if (record.dirtyCount && fread(frame.dirty.data(), sizeof(RECT), record.dirtyCount, m_file) != record.dirtyCount)
        {
            m_ok = false;
            m_error = "truncated dirty rects";
            return false;
        }
        const auto changed = ChangedRects(frame);
        frame.whole = frame.kind == FrameKind::Present && changed.empty();
        if (frame.whole && !record.keyframe)
        {
            m_ok = false;
            m_error = "a present with no metadata that is not a keyframe";
            return false;
        }
        if (frame.kind == FrameKind::Present)
        {
            desktop.ApplyMoves(frame.moves);
            if (record.keyframe)
            {
                std::vector<uint16_t> key;
                if (!ReadHalfJxr(KeyframePath(m_folder, m_index), m_header.width, m_header.height, key))
                {
                    m_ok = false;
                    m_error = "unreadable keyframe";
                    return false;
                }
                // The self-check: the previous image plus this frame's moves,
                // with its dirty rects' pixels from the keyframe, must be the
                // keyframe; a change the rects missed shows as a difference.
                auto& texels = desktop.Texels();
                for (const auto& r : frame.dirty)
                {
                    const RECT c = Clip(r, m_header.width, m_header.height);
                    for (LONG y = c.top; y < c.bottom; ++y)
                        std::memcpy(&texels[(static_cast<size_t>(y) * m_header.width + c.left) * 4],
                                    &key[(static_cast<size_t>(y) * m_header.width + c.left) * 4],
                                    static_cast<size_t>(c.right - c.left) * 8);
                }
                // A frame with no metadata has no rects to rebuild from.
                if (m_index > 0 && !frame.whole)
                    for (size_t i = 0; i < key.size(); i += 4)
                        if (std::memcmp(&texels[i], &key[i], 8) != 0) ++keyframeMismatches;
                texels = std::move(key);
                desktop.UploadAll();
            }
            else
            {
                for (const auto& r : changed)
                {
                    const RECT c = Clip(r, m_header.width, m_header.height);
                    if (Empty(c)) continue;
                    std::vector<uint16_t> pixels(static_cast<size_t>(c.right - c.left) * (c.bottom - c.top) * 4);
                    if (fread(pixels.data(), sizeof(uint16_t), pixels.size(), m_file) != pixels.size())
                    {
                        m_ok = false;
                        m_error = "truncated rect pixels";
                        return false;
                    }
                    desktop.WriteRect(c, pixels.data());
                }
                // Set 1 (a rotated output): the rects rotated into texel space,
                // each block led by its RECT. Kept for the rect-space check,
                // not replayed.
                for (uint32_t set = 1; set < record.pixelSets; ++set)
                    for (size_t i = 0; i < changed.size(); ++i)
                    {
                        RECT c{};
                        if (fread(&c, sizeof(c), 1, m_file) != 1) { m_ok = false; m_error = "truncated rotated rect"; return false; }
                        if (Empty(c)) continue;
                        std::vector<uint16_t> pixels(static_cast<size_t>(c.right - c.left) * (c.bottom - c.top) * 4);
                        if (fread(pixels.data(), sizeof(uint16_t), pixels.size(), m_file) != pixels.size())
                        {
                            m_ok = false;
                            m_error = "truncated rect pixels";
                            return false;
                        }
                    }
                desktop.Upload(changed);
            }
        }
        ++m_index;
        return true;
    }
}
