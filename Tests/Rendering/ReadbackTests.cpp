#include "pch_engine.h"
#include "../TestSupport.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/IccProfileParser.h"
#include "Rendering/SourceRuntime.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// Pixel readback and ICC profile parsing.
namespace
{
    using namespace ShaderLab::Tests;

    // Rendering and reading pixels back with no swap chain, as the headless host
    // and the MCP capture routes do.
    SL_TEST_CASE(TestHeadlessReadback, Tag::Gpu)
    {
        printf("\n=== Headless Readback ===\n");

        // Build the smallest finite-extent graph we can. Gamut Source is
        // a ShaderLab effect with a fixed output size (scRGB FP16) and
        // produces a deterministic CIE xy chromaticity diagram. Any
        // bounded source works; we just need "evaluate -> finite cached
        // output -> can read a pixel".
        auto& reg = ShaderLab::Effects::ShaderLabEffects::Instance();

        ShaderLab::Graph::GraphModel g;
        ShaderLab::Effects::SourceNodeFactory sf;

        auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *reg.FindByName(L"Gamut Source"));
        auto srcId = g.AddNode(std::move(srcNode));

        PrepareSourcesAndEvaluate(g, sf);
        auto* node = g.FindNode(srcId);
        if (!node || !node->cachedOutput) {
            TEST("Gamut Source produces cachedOutput", false);
            return;
        }
        TEST("Gamut Source produces cachedOutput", true);

        // Sample the source onto a small but non-trivial target. A 32x32
        // target with no srcRect lets D2D figure out its own tiling.
        // Map and read pixel (16, 16) for the test.
        const UINT kSize = 32;
        winrt::com_ptr<ID2D1Bitmap1> targetBmp;
        D2D1_BITMAP_PROPERTIES1 targetProps{};
        targetProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        targetProps.dpiX = 96.0f;
        targetProps.dpiY = 96.0f;
        HRESULT hr = g_dc->CreateBitmap(D2D1::SizeU(kSize, kSize), nullptr, 0, targetProps, targetBmp.put());
        if (FAILED(hr)) { TEST("CreateBitmap (target)", false); return; }

        winrt::com_ptr<ID2D1Bitmap1> stagingBmp;
        D2D1_BITMAP_PROPERTIES1 stagingProps{};
        stagingProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        stagingProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        stagingProps.dpiX = 96.0f;
        stagingProps.dpiY = 96.0f;
        hr = g_dc->CreateBitmap(D2D1::SizeU(kSize, kSize), nullptr, 0, stagingProps, stagingBmp.put());
        if (FAILED(hr)) { TEST("CreateBitmap (staging)", false); return; }

        float oldDpiX, oldDpiY;
        g_dc->GetDpi(&oldDpiX, &oldDpiY);
        g_dc->SetDpi(96.0f, 96.0f);

        g_dc->SetTarget(targetBmp.get());
        g_dc->BeginDraw();
        g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        // No srcRect, no offset: render whatever's there. D2D figures out
        // its own bounds; for the Gamut Source's fixed-size output this
        // is well-defined.
        g_dc->DrawImage(node->cachedOutput);
        hr = g_dc->EndDraw();
        g_dc->SetTarget(nullptr);
        g_dc->SetDpi(oldDpiX, oldDpiY);
        if (FAILED(hr)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "EndDraw target (hr=0x%08X)", static_cast<uint32_t>(hr));
            TEST(buf, false);
            return;
        }

        D2D1_POINT_2U dstPt = { 0, 0 };
        D2D1_RECT_U srcRectU = { 0, 0, kSize, kSize };
        hr = stagingBmp->CopyFromBitmap(&dstPt, targetBmp.get(), &srcRectU);
        if (FAILED(hr)) { TEST("CopyFromBitmap (staging)", false); return; }

        D2D1_MAPPED_RECT mapped{};
        hr = stagingBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        if (FAILED(hr)) { TEST("Map staging bitmap", false); return; }

        // Read pixel (16, 16) from the mapped buffer.
        const uint8_t* row = mapped.bits + 16 * mapped.pitch;
        const float* px = reinterpret_cast<const float*>(row) + 16 * 4;
        const float r = px[0], green = px[1], b = px[2], a = px[3];
        stagingBmp->Unmap();

        // The exact value depends on Gamut Source's content at (128,128),
        // which is not predicted here. This confirms (a) the readback path
        // runs without hanging or failing, and (b) the bytes that came back
        // are finite floats: what the headless host's capture routes need.
        bool finite = std::isfinite(r) && std::isfinite(green)
            && std::isfinite(b) && std::isfinite(a);
        TEST("Headless pixel readback completes without hang", true);
        TEST("Headless pixel readback returns finite floats", finite);

        std::printf("    pixel = (%.4f, %.4f, %.4f, %.4f)\n", r, green, b, a);
    }

    // ICC profiles are user-chosen files: a desc or mluc tag whose length or
    // offset runs past the tag must be rejected, not read.
    SL_TEST_CASE(TestIccDescriptionTagBounds, Tag::Fast)
    {
        printf("\n=== ICC description tag bounds ===\n");
        auto put32 = [](std::vector<uint8_t>& tag, size_t offset, uint32_t value) {
            if (tag.size() < offset + 4) tag.resize(offset + 4, 0);
            tag[offset] = static_cast<uint8_t>(value >> 24); tag[offset + 1] = static_cast<uint8_t>(value >> 16);
            tag[offset + 2] = static_cast<uint8_t>(value >> 8); tag[offset + 3] = static_cast<uint8_t>(value);
        };
        auto descTag = [&](uint32_t declaredLength) {
            std::vector<uint8_t> tag;
            put32(tag, 0, 0x64657363);   // 'desc'
            put32(tag, 8, declaredLength);
            for (char ch : std::string("Panel")) tag.push_back(static_cast<uint8_t>(ch));
            tag.push_back(0);
            return tag;
        };
        auto mlucTag = [&](uint32_t stringLength, uint32_t stringOffset) {
            std::vector<uint8_t> tag;
            put32(tag, 0, 0x6D6C7563);   // 'mluc'
            put32(tag, 8, 1);            // one record
            put32(tag, 12, 12);          // record size
            put32(tag, 20, stringLength);
            put32(tag, 24, stringOffset);
            for (wchar_t ch : std::wstring(L"Panel")) { tag.push_back(0); tag.push_back(static_cast<uint8_t>(ch)); }
            return tag;
        };
        auto parse = [](const std::vector<uint8_t>& tag) {
            return ShaderLab::Rendering::ParseIccDescriptionTag(tag.data(), tag.size());
        };

        TEST("IccDesc_WellFormedDesc", parse(descTag(6)) == L"Panel");
        TEST("IccDesc_DescLengthPastTagRejected", parse(descTag(7)).empty());
        TEST("IccDesc_DescLengthWrapRejected", parse(descTag(0xFFFFFFF8u)).empty());
        TEST("IccDesc_WellFormedMluc", parse(mlucTag(10, 28)) == L"Panel");
        TEST("IccDesc_MlucLengthPastTagRejected", parse(mlucTag(12, 28)).empty());
        TEST("IccDesc_MlucOffsetWrapRejected", parse(mlucTag(16, 0xFFFFFFF8u)).empty());
        TEST("IccDesc_TruncatedTagRejected", ShaderLab::Rendering::ParseIccDescriptionTag(descTag(6).data(), 8).empty());
    }
}
