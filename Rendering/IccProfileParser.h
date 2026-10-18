#pragma once

#include "pch_engine.h"
#include "DisplayProfile.h"

namespace ShaderLab::Rendering
{
    // Data extracted from an ICC profile's monitor tags.
    struct IccProfileData
    {
        std::wstring description;
        ChromaticityXY primaryRed, primaryGreen, primaryBlue;
        ChromaticityXY whitePoint;
        float luminanceNits{ 0.0f };  // 0 means not present in profile
        bool valid{ false };
    };

    // Parses ICC profile binary files to extract display colorimetry data.
    class IccProfileParser
    {
    public:
        static std::optional<IccProfileData> LoadFromFile(const std::wstring& filePath);
    };

    // The text of a 'desc' (ICC v2) or 'mluc' (ICC v4, first record) tag
    // body, or empty when the tag is malformed. The bytes are untrusted.
    std::wstring ParseIccDescriptionTag(const uint8_t* data, size_t size);

    // Converts parsed ICC data into a full DisplayProfile.
    DisplayProfile DisplayProfileFromIcc(const IccProfileData& icc);
}
