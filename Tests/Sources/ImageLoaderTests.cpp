#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/ImageLoader.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"

#include <cstdio>
#include <string>
#include <vector>

// Image loader file types.
namespace
{
    using namespace ShaderLab::Tests;

    // ImageLoader::SupportedExtensions comes from the WIC decoders registered on
    // this machine, not from a fixed list.
    SL_TEST_CASE(TestImageLoaderExtensions, Tag::Fast)
    {
        printf("\n=== ImageLoader supported extensions ===\n");

        auto exts = ShaderLab::Effects::ImageLoader::SupportedExtensions();

        printf("  %zu extension(s): ", exts.size());
        for (size_t i = 0; i < exts.size(); ++i)
            printf("%ls%s", exts[i].c_str(), (i + 1 < exts.size()) ? " " : "\n");

        TEST("ImageExt_NotEmpty", !exts.empty());

        // Every entry must be a filter FileOpenPicker will accept: a dot, at least
        // one character, lowercase, and no path/wildcard characters. One malformed
        // entry from a badly-registered third-party codec would throw E_INVALIDARG
        // and take out the entire dialog.
        bool wellFormed = true;
        for (const auto& e : exts)
        {
            if (e.size() < 2 || e.front() != L'.') { wellFormed = false; break; }
            if (e.find_first_of(L" \t\\/:*?\"<>|") != std::wstring::npos) { wellFormed = false; break; }
            for (wchar_t c : e)
                if (c != towlower(c)) { wellFormed = false; break; }
            if (!wellFormed) break;
        }
        TEST("ImageExt_AllWellFormed", wellFormed);

        // Sorted and de-duplicated: WIC reports the same extension from more than
        // one decoder (.tif from TIFF and from some RAW codecs).
        TEST("ImageExt_SortedUnique",
            std::is_sorted(exts.begin(), exts.end()) &&
            std::adjacent_find(exts.begin(), exts.end()) == exts.end());

        // The formats WIC has always shipped must be present; if these are missing
        // the enumeration is broken and the fallback list would be in play.
        auto has = [&](const wchar_t* e) {
            return std::find(exts.begin(), exts.end(), std::wstring(e)) != exts.end();
        };
        TEST("ImageExt_HasBaselineFormats", has(L".png") && has(L".jpg") && has(L".bmp") && has(L".tif"));

        // Informational, NOT a gate: HEIF/AVIF ship as installable OS extensions,
        // so their presence is a property of the machine. The point of the change
        // is that whatever this machine can decode is what the picker offers.
        printf("  [info] .heic %ls  .heif %ls  .avif %ls  .jxr %ls\n",
            has(L".heic") ? L"present" : L"absent",
            has(L".heif") ? L"present" : L"absent",
            has(L".avif") ? L"present" : L"absent",
            has(L".jxr")  ? L"present" : L"absent");
    }
}
