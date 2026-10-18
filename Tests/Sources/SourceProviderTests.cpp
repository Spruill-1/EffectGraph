#include "pch_engine.h"
#include "../TestSupport.h"

#include "../../Graph/GraphModel.h"
#include "../../Effects/ImageLoader.h"
#include "../../Effects/SourceNodeFactory.h"
#include "../../Effects/SourceTextureUtil.h"
#include "../../Effects/VideoFrameConverter.h"
#include "../../Effects/VideoFrameReader.h"
#include "../../Effects/VideoSeekModel.h"
#include "../../Rendering/ImageCapture.h"
#include "Graph/GraphEdits.h"

#include <cmath>
#include <cstdio>
#include <filesystem>

// The source side: the colour contract every provider meets (linear scRGB),
// the image loader's error channel, the image cache's file stamp, the video
// field schema, the video colour description and conversion shader, and the
// seek model.
namespace ShaderLab::Tests
{
    namespace
    {
        using namespace ShaderLab::Effects;
        using ShaderLab::Rendering::PixelBuffer;

        // The first pixel of `image` rendered into an FP32 target.
        bool ReadFirstPixel(ID2D1DeviceContext5* dc, ID2D1Image* image, float rgba[4])
        {
            PixelBuffer pixels;
            const HRESULT hr = ShaderLab::Rendering::RenderToPixels(dc, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT,
                D2D1::ColorF(0, 0, 0, 0), [&](ID2D1DeviceContext* ctx) { ctx->DrawImage(image); }, pixels);
            if (FAILED(hr) || pixels.bytes.size() < 16) return false;
            std::memcpy(rgba, pixels.bytes.data(), 16);
            return true;
        }

        // Writes a width x height PNG of one sRGB-encoded grey level.
        bool WriteGreyPng(ID2D1DeviceContext5* dc, const std::wstring& path, uint32_t width, uint32_t height, float grey)
        {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(grey, grey, grey, 1.0f), brush.put()))) return false;
            PixelBuffer pixels;
            const HRESULT hr = ShaderLab::Rendering::RenderToPixels(dc, width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1::ColorF(0, 0, 0, 1), [&](ID2D1DeviceContext* ctx)
                { ctx->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)), brush.get()); },
                pixels);
            return SUCCEEDED(hr) && SUCCEEDED(ShaderLab::Rendering::EncodeToFile(pixels, path));
        }

        double SrgbToLinear(double s)
        {
            return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        }

        float HalfToFloat(uint16_t half)
        {
            const uint32_t sign = (half & 0x8000u) << 16;
            const uint32_t exponent = (half >> 10) & 0x1Fu;
            const uint32_t mantissa = half & 0x3FFu;
            uint32_t bits;
            if (exponent == 0)
            {
                if (mantissa == 0) bits = sign;
                else
                {
                    float value = std::ldexp(static_cast<float>(mantissa), -24);
                    return (half & 0x8000u) ? -value : value;
                }
            }
            else if (exponent == 31) bits = sign | 0x7F800000u | (mantissa << 13);
            else bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
            float result;
            std::memcpy(&result, &bits, 4);
            return result;
        }

        // The converter's output pixel (0,0) as floats.
        bool ReadConverterPixel(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, float rgba[4])
        {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            winrt::com_ptr<ID3D11Texture2D> staging;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put()))) return false;
            context->CopyResource(staging.get(), texture);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
            const auto* half = static_cast<const uint16_t*>(mapped.pData);
            for (int c = 0; c < 4; ++c) rgba[c] = HalfToFloat(half[c]);
            context->Unmap(staging.get(), 0);
            return true;
        }

        void TestColorContract(ID2D1DeviceContext5* dc, ID3D11Device* device, ID3D11DeviceContext* context)
        {
            std::printf("\n=== Source colour contract ===\n");

            auto fp16 = CreateSharedOutput(dc, device, 4, 4, DXGI_FORMAT_R16G16B16A16_FLOAT, L"test");
            winrt::com_ptr<ID2D1Image> bitmapAsImage;
            if (fp16.bitmap) fp16.bitmap.as(bitmapAsImage);
            TEST("SourceColor_FP16OutputIsTheBitmap",
                 fp16.error.empty() && !fp16.decode && fp16.image && fp16.image.get() == bitmapAsImage.get());

            // An 8-bit surface holds sRGB-encoded values; the image decodes them.
            auto unorm = CreateSharedOutput(dc, device, 4, 4, DXGI_FORMAT_B8G8R8A8_UNORM, L"test");
            bool decoded = false;
            float rgba[4]{};
            if (unorm.error.empty() && unorm.texture && unorm.decode)
            {
                std::vector<uint32_t> texels(16, 0xFFBCBCBCu);   // 188 in every colour channel
                context->UpdateSubresource(unorm.texture.get(), 0, nullptr, texels.data(), 16, 0);
                decoded = ReadFirstPixel(dc, unorm.image.get(), rgba);
            }
            const double expected = SrgbToLinear(188.0 / 255.0);
            std::printf("  [info] 8-bit 188 reads %.4f %.4f %.4f (sRGB-decoded %.4f, encoded %.4f)\n",
                        rgba[0], rgba[1], rgba[2], expected, 188.0 / 255.0);
            // 3e-4 separates the decode (0.50289) from an 8-bit store of it (128/255 = 0.50196).
            TEST("SourceColor_8BitOutputIsLinear", decoded &&
                 std::abs(rgba[0] - expected) < 3e-4 && std::abs(rgba[1] - expected) < 3e-4 &&
                 std::abs(rgba[2] - expected) < 3e-4);

            // Through a downstream effect the decoded image keeps float precision:
            // in an 8-bit intermediate a dark code would land on 0 or 1/255.
            bool chained = false;
            float dark[4]{};
            winrt::com_ptr<ID2D1Effect> identity;
            if (unorm.error.empty() && unorm.texture && unorm.decode &&
                SUCCEEDED(dc->CreateEffect(CLSID_D2D1ColorMatrix, identity.put())))
            {
                std::vector<uint32_t> texels(16, 0xFF0A0A0Au);   // 10 in every colour channel
                context->UpdateSubresource(unorm.texture.get(), 0, nullptr, texels.data(), 16, 0);
                identity->SetInput(0, unorm.image.get());
                winrt::com_ptr<ID2D1Image> out;
                identity->GetOutput(out.put());
                chained = ReadFirstPixel(dc, out.get(), dark);
            }
            const double darkExpected = SrgbToLinear(10.0 / 255.0);
            std::printf("  [info] 8-bit 10 through an effect reads %.6f (sRGB-decoded %.6f)\n", dark[0], darkExpected);
            TEST("SourceColor_8BitDecodeKeepsPrecisionDownstream", chained &&
                 std::abs(dark[0] - darkExpected) < 1e-5 && std::abs(dark[1] - darkExpected) < 1e-5 &&
                 std::abs(dark[2] - darkExpected) < 1e-5);
        }
        SL_REGISTER_TEST(TestColorContract, Tag::Gpu, [] { TestColorContract(g_dc.get(), g_d3dDevice.get(), g_d3dContext.get()); });

        void TestImageLoader(ID2D1DeviceContext5* dc)
        {
            std::printf("\n=== Image loader errors and context state ===\n");
            const ScopedTempDir tempDir(L"ShaderLabSourceTests");
            const auto dir = tempDir.Path();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const std::wstring png = (dir / L"grey.png").wstring();
            const bool wrote = WriteGreyPng(dc, png, 8, 8, 0.5f);

            ImageLoader loader;
            auto missing = loader.LoadFromFile((dir / L"absent.png").wstring(), dc);
            std::printf("  [info] missing file: '%ls'\n", loader.LastError().c_str());
            TEST("ImageLoader_MissingFileNamesStage", !missing &&
                 loader.LastError().find(L"Opening a decoder") != std::wstring::npos &&
                 loader.LastError().find(L"0x") != std::wstring::npos);

            // The caller's transform survives the flatten.
            const D2D1_MATRIX_3X2_F scaled = D2D1::Matrix3x2F::Scale(2.0f, 3.0f);
            dc->SetTransform(scaled);
            auto loaded = wrote ? loader.LoadFromFile(png, dc) : nullptr;
            D2D1_MATRIX_3X2_F after{};
            dc->GetTransform(&after);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            TEST("ImageLoader_KeepsCallerTransform", loaded && loader.LastError().empty() &&
                 after._11 == 2.0f && after._22 == 3.0f);

            // Inside a draw session the flatten cannot run: an error, never the encoded bitmap.
            winrt::com_ptr<ID2D1Bitmap1> target;
            const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::com_ptr<ID2D1Bitmap1> nested;
            if (wrote && SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(4, 4), nullptr, 0, props, target.put())))
            {
                winrt::com_ptr<ID2D1Image> oldTarget;
                dc->GetTarget(oldTarget.put());
                dc->SetTarget(target.get());
                dc->BeginDraw();
                nested = loader.LoadFromFile(png, dc);
                dc->EndDraw();
                dc->SetTarget(oldTarget.get());
            }
            std::printf("  [info] nested draw session: '%ls'\n", loader.LastError().c_str());
            TEST("ImageLoader_NestedDrawSessionIsAnError", wrote && !nested &&
                 loader.LastError().find(L"draw session") != std::wstring::npos);

            std::filesystem::remove_all(dir, ec);   // best effort: temp dir
        }
        SL_REGISTER_TEST(TestImageLoader, Tag::Gpu, [] { TestImageLoader(g_dc.get()); });

        void TestImageReload(ID2D1DeviceContext5* dc, ID3D11Device* device, ID3D11DeviceContext* context)
        {
            std::printf("\n=== Image source reloads a changed file ===\n");
            const ScopedTempDir tempDir(L"ShaderLabSourceReload");
            const auto dir = tempDir.Path();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const std::wstring png = (dir / L"image.png").wstring();

            SourceNodeFactory factory;
            auto node = SourceNodeFactory::CreateImageSourceNode(png);
            node.id = 1;
            float first[4]{}, second[4]{}, third[4]{};
            bool ok = WriteGreyPng(dc, png, 8, 8, 0.25f);
            factory.PrepareSourceNode(node, dc, device, context);
            ok = ok && node.cachedOutput && ReadFirstPixel(dc, node.cachedOutput, first);
            ID2D1Image* const decoded = node.cachedOutput;

            // Unchanged file: the decode is reused.
            ShaderLab::Graph::MarkNodeChanged(node);
            factory.PrepareSourceNode(node, dc, device, context);
            const bool reused = node.cachedOutput == decoded;

            // Re-saved file: decoded again.
            ok = ok && WriteGreyPng(dc, png, 8, 8, 0.75f);
            std::filesystem::last_write_time(png, std::filesystem::last_write_time(png, ec) + std::chrono::seconds(2), ec);
            ShaderLab::Graph::MarkNodeChanged(node);
            factory.PrepareSourceNode(node, dc, device, context);
            ok = ok && node.cachedOutput && ReadFirstPixel(dc, node.cachedOutput, second);

            // A file that no longer decodes reports why.
            std::filesystem::remove(png, ec);
            ShaderLab::Graph::MarkNodeChanged(node);
            factory.PrepareSourceNode(node, dc, device, context);
            (void)third;
            std::printf("  [info] grey 0.25 -> %.4f, re-saved 0.75 -> %.4f; removed: '%ls'\n",
                        first[0], second[0], node.runtimeError.c_str());
            TEST("Source_ImageReusedWhileFileUnchanged", ok && reused);
            TEST("Source_ImageReloadsWhenFileChanges", ok && second[0] > first[0] + 0.2f);
            TEST("Source_ImageDecodeErrorSaysWhy",
                 node.runtimeError.starts_with(L"Failed to decode image: ") &&
                 node.runtimeError.find(L"0x") != std::wstring::npos);
            std::filesystem::remove_all(dir, ec);   // best effort: temp dir
        }
        SL_REGISTER_TEST(TestImageReload, Tag::Gpu, [] { TestImageReload(g_dc.get(), g_d3dDevice.get(), g_d3dContext.get()); });

        bool FieldNamesAre(const Graph::EffectNode& node, std::span<const wchar_t* const> names)
        {
            if (node.analysisOutput.fields.size() != names.size()) return false;
            for (size_t i = 0; i < names.size(); ++i)
                if (node.analysisOutput.fields[i].name != names[i]) return false;
            return node.analysisOutput.type == Graph::AnalysisOutputType::Typed;
        }

        SL_TEST_CASE(TestFieldSchemas, Tag::Fast)
        {
            std::printf("\n=== Source analysis field schemas ===\n");
            TEST("SourceFields_VideoSeededAtCreation",
                 FieldNamesAre(SourceNodeFactory::CreateVideoSourceNode(L"C:\\clip.mp4"), SourceNodeFactory::scVideoFields));

            SourceNodeFactory factory;
            factory.RecordVideoSettleTimeout(3, L"C:\\clip.mp4", 1.5);
            const bool remembered = factory.VideoSettleTimedOut(3, L"C:\\clip.mp4", 1.5);
            factory.ForgetVideoSettleTimeouts();
            const bool forgotten = !factory.VideoSettleTimedOut(3, L"C:\\clip.mp4", 1.5);
            factory.RecordVideoSettleTimeout(3, L"C:\\clip.mp4", 1.5);
            factory.ReleaseCache();
            TEST("SourceSettle_TimeoutsBelongToTheFactory",
                 remembered && forgotten && !factory.VideoSettleTimedOut(3, L"C:\\clip.mp4", 1.5));
        }

        SL_TEST_CASE(TestVideoColor, Tag::Fast | Tag::Video)
        {
            std::printf("\n=== Video colour description ===\n");
            using D = VideoColorDescription;
            auto describe = [](UINT32 transfer, UINT32 primaries, UINT32 matrix, UINT32 range, bool main10 = false)
            {
                VideoColorTags tags;
                tags.transferFunction = transfer;
                tags.primaries = primaries;
                tags.yuvMatrix = matrix;
                tags.nominalRange = range;
                tags.hevcMain10 = main10;
                return DescribeVideoColor(tags);
            };

            const D untagged = describe(0, 0, 0, 0);
            TEST("VideoColor_UntaggedIsSdr709Limited", untagged.transfer == D::Transfer::Sdr &&
                 untagged.primaries == D::Primaries::Bt709 && untagged.matrix == D::Matrix::Bt709 &&
                 untagged.range == D::Range::Limited);
            const D hdr10 = describe(MFVideoTransFunc_2084, MFVideoPrimaries_BT2020, MFVideoTransferMatrix_BT2020_10, MFNominalRange_16_235);
            TEST("VideoColor_Hdr10", hdr10.transfer == D::Transfer::Pq && hdr10.primaries == D::Primaries::Bt2020 &&
                 hdr10.matrix == D::Matrix::Bt2020 && hdr10.range == D::Range::Limited);
            TEST("VideoColor_Untagged2020IsPq", describe(0, MFVideoPrimaries_BT2020, 0, 0).transfer == D::Transfer::Pq &&
                 describe(0, MFVideoPrimaries_BT2020, 0, 0).matrix == D::Matrix::Bt2020);
            TEST("VideoColor_UntaggedMain10IsPq", describe(0, 0, 0, 0, true).transfer == D::Transfer::Pq &&
                 describe(0, 0, 0, 0, true).primaries == D::Primaries::Bt2020);
            const D hlg = describe(MFVideoTransFunc_HLG, MFVideoPrimaries_BT2020, 0, 0);
            TEST("VideoColor_HlgIsNotPq", hlg.transfer == D::Transfer::Hlg && hlg.IsHdr());
            const D sdr2020 = describe(MFVideoTransFunc_709, MFVideoPrimaries_BT2020, 0, 0, true);
            TEST("VideoColor_TaggedSdrStaysSdr", sdr2020.transfer == D::Transfer::Sdr && !sdr2020.IsHdr() &&
                 sdr2020.primaries == D::Primaries::Bt2020);
            TEST("VideoColor_TagsHonoured", describe(0, 0, MFVideoTransferMatrix_BT601, MFNominalRange_0_255).matrix == D::Matrix::Bt601 &&
                 describe(0, 0, MFVideoTransferMatrix_BT601, MFNominalRange_0_255).range == D::Range::Full);

            // Every variant of the one conversion template compiles.
            int variants = 0, compiled = 0;
            std::string firstError;
            for (auto format : { VideoFrameFormat::RGB32, VideoFrameFormat::NV12, VideoFrameFormat::P010 })
                for (auto transfer : { D::Transfer::Sdr, D::Transfer::Pq, D::Transfer::Hlg })
                    for (auto primaries : { D::Primaries::Bt709, D::Primaries::Bt2020 })
                        for (auto matrix : { D::Matrix::Bt601, D::Matrix::Bt709, D::Matrix::Bt2020 })
                            for (auto range : { D::Range::Limited, D::Range::Full })
                            {
                                const D color{ transfer, primaries, matrix, range };
                                const std::string source = VideoFrameConverter::ShaderSource(format, color);
                                winrt::com_ptr<ID3DBlob> blob, errors;
                                ++variants;
                                if (SUCCEEDED(D3DCompile(source.data(), source.size(), "VideoConvert", nullptr, nullptr,
                                                         "main", "cs_5_0", 0, 0, blob.put(), errors.put())))
                                    ++compiled;
                                else if (firstError.empty() && errors)
                                    firstError.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
                            }
            std::printf("  [info] %d of %d conversion variants compile%s%s\n", compiled, variants,
                        firstError.empty() ? "" : ": ", firstError.c_str());
            TEST("VideoConvert_EveryVariantCompiles", compiled == variants);
        }

        // Converts one grey P010 / NV12 pixel and compares it with the same
        // formula evaluated on the CPU.
        void TestVideoConversion(ID3D11Device* device, ID3D11DeviceContext* context)
        {
            std::printf("\n=== Video conversion shader ===\n");
            using D = VideoColorDescription;
            constexpr uint32_t cSize = 16;

            auto convertGrey = [&](VideoFrameFormat format, const D& color, uint32_t lumaCode, float rgba[4])
            {
                VideoFrameConverter converter;
                if (!converter.Create(device, context, format, color, cSize, cSize)) return false;
                const bool tenBit = format == VideoFrameFormat::P010;
                const UINT pitch = tenBit ? cSize * 2 : cSize;
                std::vector<BYTE> bytes(static_cast<size_t>(pitch) * cSize * 3 / 2);
                const uint32_t chromaMid = tenBit ? 512 : 128;
                for (uint32_t y = 0; y < cSize; ++y)
                    for (uint32_t x = 0; x < cSize; ++x)
                    {
                        if (tenBit) reinterpret_cast<uint16_t*>(bytes.data() + y * pitch)[x] = static_cast<uint16_t>(lumaCode << 6);
                        else bytes[y * pitch + x] = static_cast<BYTE>(lumaCode);
                    }
                BYTE* chroma = bytes.data() + static_cast<size_t>(pitch) * cSize;
                for (uint32_t y = 0; y < cSize / 2; ++y)
                    for (uint32_t x = 0; x < cSize; ++x)   // interleaved Cb Cr
                    {
                        if (tenBit) reinterpret_cast<uint16_t*>(chroma + y * pitch)[x] = static_cast<uint16_t>(chromaMid << 6);
                        else chroma[y * pitch + x] = static_cast<BYTE>(chromaMid);
                    }
                if (!converter.ConvertBytes(bytes.data(), pitch)) return false;
                return ReadConverterPixel(device, context, converter.Output(), rgba);
            };

            // The shader's arithmetic in double, from the same code values.
            auto inversePq = [](double n)
            {
                const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
                const double np = std::pow((std::max)(n, 0.0), 1.0 / m2);
                const double den = c2 - c3 * np;
                return den <= 0.0 ? 0.0 : 10000.0 * std::pow((std::max)(np - c1, 0.0) / den, 1.0 / m1);
            };
            auto inverseHlg = [](double e)
            {
                const double a = 0.17883277, b = 0.28466892, c = 0.55991073;
                return e <= 0.5 ? e * e / 3.0 : (std::exp((e - c) / a) + b) / 12.0;
            };
            auto toBt709 = [](const double n[3], double out[3])
            {
                out[0] =  1.6605 * n[0] - 0.5877 * n[1] - 0.0728 * n[2];
                out[1] = -0.1246 * n[0] + 1.1330 * n[1] - 0.0084 * n[2];
                out[2] = -0.0182 * n[0] - 0.1006 * n[1] + 1.1187 * n[2];
            };
            auto reference = [&](bool tenBit, D::Transfer transfer, uint32_t lumaCode, double out[3])
            {
                const double codeMax = tenBit ? 1023.0 : 255.0;
                const double yUnorm = tenBit ? static_cast<double>(lumaCode << 6) / 65535.0 : lumaCode / 255.0;
                const double cUnorm = tenBit ? static_cast<double>(512u << 6) / 65535.0 : 128.0 / 255.0;
                const double yScaled = (std::max)(tenBit ? (yUnorm * codeMax - 64.0) / 876.0 : (yUnorm * codeMax - 16.0) / 219.0, 0.0);
                const double c = tenBit ? (cUnorm * codeMax - 512.0) / 896.0 : (cUnorm * codeMax - 128.0) / 224.0;
                const double rCr = tenBit ? 1.47460 : 1.5748, gCb = tenBit ? 0.16455 : 0.1873;
                const double gCr = tenBit ? 0.57135 : 0.4681, bCb = tenBit ? 1.88100 : 1.8556;
                const double e[3] = { std::clamp(yScaled + rCr * c, 0.0, 1.0),
                                      std::clamp(yScaled - gCb * c - gCr * c, 0.0, 1.0),
                                      std::clamp(yScaled + bCb * c, 0.0, 1.0) };
                double n[3];
                if (transfer == D::Transfer::Pq)
                {
                    for (int i = 0; i < 3; ++i) n[i] = inversePq(e[i]);
                    toBt709(n, out);
                    for (int i = 0; i < 3; ++i) out[i] /= 80.0;
                }
                else if (transfer == D::Transfer::Hlg)
                {
                    double scene[3];
                    for (int i = 0; i < 3; ++i) scene[i] = inverseHlg(e[i]);
                    const double ys = 0.2627 * scene[0] + 0.6780 * scene[1] + 0.0593 * scene[2];
                    for (int i = 0; i < 3; ++i) n[i] = 1000.0 * std::pow((std::max)(ys, 1e-6), 0.2) * scene[i];
                    toBt709(n, out);
                    for (int i = 0; i < 3; ++i) out[i] /= 80.0;
                }
                else
                {
                    for (int i = 0; i < 3; ++i) out[i] = SrgbToLinear(e[i]);
                }
            };

            float pq[4]{}, hlg[4]{}, sdr[4]{};
            const uint32_t pqCode = 520;    // about 100 nits
            const uint32_t hlgCode = 721;   // 75%: HLG reference white
            const uint32_t sdrCode = 180;
            const bool pqOk = convertGrey(VideoFrameFormat::P010, { D::Transfer::Pq, D::Primaries::Bt2020, D::Matrix::Bt2020, D::Range::Limited }, pqCode, pq);
            const bool hlgOk = convertGrey(VideoFrameFormat::P010, { D::Transfer::Hlg, D::Primaries::Bt2020, D::Matrix::Bt2020, D::Range::Limited }, hlgCode, hlg);
            const bool sdrOk = convertGrey(VideoFrameFormat::NV12, { D::Transfer::Sdr, D::Primaries::Bt709, D::Matrix::Bt709, D::Range::Limited }, sdrCode, sdr);

            double pqWant[3], hlgWant[3], sdrWant[3];
            reference(true, D::Transfer::Pq, pqCode, pqWant);
            reference(true, D::Transfer::Hlg, hlgCode, hlgWant);
            reference(false, D::Transfer::Sdr, sdrCode, sdrWant);
            std::printf("  [info] PQ code %u -> %.4f (want %.4f, %.1f nits); HLG code %u -> %.4f (want %.4f, %.1f nits); "
                        "SDR code %u -> %.4f (want %.4f)\n",
                        pqCode, pq[1], pqWant[1], pqWant[1] * 80.0, hlgCode, hlg[1], hlgWant[1], hlgWant[1] * 80.0,
                        sdrCode, sdr[1], sdrWant[1]);
            auto matches = [](const float value[4], const double want[3])
            {
                for (int i = 0; i < 3; ++i)
                    if (std::abs(value[i] - want[i]) > 2e-3 * (std::max)(1.0, std::abs(want[i]))) return false;
                return true;
            };
            TEST("VideoConvert_PqGrey", pqOk && matches(pq, pqWant));
            TEST("VideoConvert_HlgReferenceWhite", hlgOk && matches(hlg, hlgWant) && std::abs(hlgWant[1] * 80.0 - 203.0) < 3.0);
            TEST("VideoConvert_SdrGrey", sdrOk && matches(sdr, sdrWant));
        }
        SL_REGISTER_TEST(TestVideoConversion, Tag::Gpu | Tag::Video, [] { TestVideoConversion(g_d3dDevice.get(), g_d3dContext.get()); });

        SL_TEST_CASE(TestSeekModel, Tag::Fast | Tag::Video)
        {
            std::printf("\n=== Video seek model ===\n");
            VideoSeekModel model;
            model.Reset(60.0);
            const double frame = 1.0 / 60.0;
            // Unmeasured: a quarter second of frames, never under the decode-ahead depth.
            TEST("SeekModel_DefaultThreshold", model.SeekThresholdFrames() == 15.0);
            model.Reset(10.0);
            TEST("SeekModel_ThresholdFloor", model.SeekThresholdFrames() == static_cast<double>(cVideoDecodeAheadFrames));
            model.Reset(60.0);
            TEST("SeekModel_NearTargetDecodesOn", !model.ForwardSeekPays(0.2, 0.0, frame, true));

            // A keyframe at 10 s known to cover 10.5 s: a seek from far behind
            // pays, one from just before the keyframe does not.
            model.RecordKeyframe(10.0, 10.5);
            TEST("SeekModel_KnownKeyframeAheadPays", model.ForwardSeekPays(10.4, 0.0, frame, true));
            TEST("SeekModel_KnownKeyframeBehindHeadDoesNot", !model.ForwardSeekPays(10.4, 9.9, frame, true));

            // Measured costs move the threshold.
            model.RecordDecodeRate(0.002);
            model.RecordSeekOverhead(0.1);
            TEST("SeekModel_MeasuredThreshold", std::abs(model.SeekThresholdFrames() - 50.0) < 1e-9);
            TEST("SeekModel_KeyframesCounted", model.KnownKeyframeCount() == 1);
        }
    }
}
