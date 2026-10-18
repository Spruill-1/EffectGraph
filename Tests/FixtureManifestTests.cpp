#include "pch_engine.h"
#include "TestSupport.h"
#include "Engine/Utf8.h"

#include <bcrypt.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Tests\fixtures\manifest.json lists every fixture the suite reads: the
// committed ones must be present (and binary clips match their recorded
// SHA-256), the generated ones count a skip when absent.
namespace
{
    using namespace ShaderLab::Tests;

    // Lower-case hex SHA-256 of a file, or empty when it cannot be read.
    std::string FileSha256(const std::wstring& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return {};
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        UCHAR digest[32]{};
        if (!BCRYPT_SUCCESS(::BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                         reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                         static_cast<ULONG>(bytes.size()), digest, sizeof(digest))))
            return {};
        std::string hex;
        char pair[3]{};
        for (const UCHAR byte : digest)
        {
            std::snprintf(pair, sizeof(pair), "%02x", byte);
            hex += pair;
        }
        return hex;
    }

    SL_TEST_CASE(TestFixtureManifest, Tag::Fast)
    {
        printf("\n=== Fixture manifest ===\n");
        using namespace winrt::Windows::Data::Json;
        const std::wstring manifestPath = FindFixture(L"manifest.json");
        TEST("Fixtures_ManifestFound", !manifestPath.empty());
        if (manifestPath.empty()) return;

        std::ifstream file(manifestPath, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        JsonObject manifest{ nullptr };
        const bool parsed = JsonObject::TryParse(winrt::to_hstring(text), manifest) && manifest.HasKey(L"fixtures");
        TEST("Fixtures_ManifestParses", parsed);
        if (!parsed) return;

        for (const auto& value : manifest.GetNamedArray(L"fixtures"))
        {
            const JsonObject entry = value.GetObject();
            const std::wstring name(entry.GetNamedString(L"name"));
            const std::string label = "Fixtures_" + ShaderLab::WideToUtf8(name);
            const std::wstring path = FindFixture(name.c_str());
            if (entry.GetNamedString(L"kind") == L"generated")
            {
                if (path.empty()) SkipMissingFixture(name.c_str());
                continue;
            }
            TEST((label + "_Present").c_str(), !path.empty());
            if (path.empty() || !entry.HasKey(L"sha256")) continue;
            const std::string expected = winrt::to_string(entry.GetNamedString(L"sha256"));
            const std::string actual = FileSha256(path);
            TEST((label + "_MatchesManifest").c_str(), actual == expected);
            if (actual != expected)
                printf("         sha256 %s, manifest %s\n", actual.c_str(), expected.c_str());
        }
    }
}
