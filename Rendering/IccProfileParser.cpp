#include "pch_engine.h"
#include "IccProfileParser.h"

// Use Windows' Image Color Management library (mscms.dll) to read ICC profile
// tags instead of parsing the binary container ourselves. mscms knows the
// profile layout, big-endian byte order, ICC v2/v4 differences, and tag
// addressing -- we only have to interpret each tag body, which is a small
// well-defined record (XYZ tags = 12 bytes of s15Fixed16 after an 8-byte
// type header; 'desc' and 'mluc' carry localized text).
//
// API used:
//   OpenColorProfileW       - open profile from disk path (PROFILE_FILENAME).
//   GetColorProfileElement  - fetch the raw bytes of a tag by 4cc signature.
//   CloseColorProfile       - via std::unique_ptr custom deleter.
//
// See https://github.com/Spruill-1/IccWriter for a similar mscms-based
// approach (writing tags via SetColorProfileElement); we use the read path.

#include <icm.h>

#pragma comment(lib, "mscms.lib")

namespace
{
	using ShaderLab::Rendering::ChromaticityXY;
	using ShaderLab::Rendering::GamutId;

	// ICC integers and 4cc signatures are big-endian on disk and in mscms
	// tag-element output; convert with byteswap.
	inline uint32_t SwapBE32(uint32_t v) noexcept { return _byteswap_ulong(v); }

	// Parse a signed-15.16 fixed-point number stored big-endian.
	float ReadS15Fixed16(const uint8_t* p) noexcept
	{
		uint32_t be = 0;
		std::memcpy(&be, p, 4);
		const int32_t v = static_cast<int32_t>(SwapBE32(be));
		return static_cast<float>(v) / 65536.0f;
	}

	ChromaticityXY XyzToXy(float X, float Y, float Z) noexcept
	{
		const float sum = X + Y + Z;
		if (sum < 1e-8f) return { 0.0f, 0.0f };
		return { X / sum, Y / sum };
	}

	// ICC tag signatures (host-endian; mscms takes them as 32-bit ints).
	constexpr uint32_t MakeTag(char a, char b, char c, char d) noexcept
	{
		return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24)
			 | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16)
			 | (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8)
			 |  static_cast<uint32_t>(static_cast<uint8_t>(d));
	}

	constexpr uint32_t kTagDesc = MakeTag('d','e','s','c');
	constexpr uint32_t kTagRXYZ = MakeTag('r','X','Y','Z');
	constexpr uint32_t kTagGXYZ = MakeTag('g','X','Y','Z');
	constexpr uint32_t kTagBXYZ = MakeTag('b','X','Y','Z');
	constexpr uint32_t kTagWtpt = MakeTag('w','t','p','t');
	constexpr uint32_t kTagLumi = MakeTag('l','u','m','i');

	constexpr uint32_t kTypeDesc = MakeTag('d','e','s','c'); // ICC v2 textDescriptionType
	constexpr uint32_t kTypeMluc = MakeTag('m','l','u','c'); // ICC v4 multiLocalizedUnicodeType

	struct ProfileHandleDeleter
	{
		void operator()(HPROFILE h) const noexcept
		{
			if (h) ::CloseColorProfile(h);
		}
	};
	using UniqueProfile = std::unique_ptr<std::remove_pointer_t<HPROFILE>, ProfileHandleDeleter>;

	// Returns true on success; fills X/Y/Z from a 20-byte XYZType tag body.
	// Body layout: 4 bytes type sig ('XYZ '), 4 bytes reserved, 3 * s15Fixed16.
	bool ReadXYZElement(HPROFILE prof, uint32_t tag, float& X, float& Y, float& Z)
	{
		DWORD cb = 0;
		BOOL ref = FALSE;
		::GetColorProfileElement(prof, tag, 0, &cb, nullptr, &ref);
		if (cb < 20) return false;

		std::vector<uint8_t> buf(cb);
		if (!::GetColorProfileElement(prof, tag, 0, &cb, buf.data(), &ref))
			return false;

		X = ReadS15Fixed16(buf.data() + 8);
		Y = ReadS15Fixed16(buf.data() + 12);
		Z = ReadS15Fixed16(buf.data() + 16);
		return true;
	}

	// Returns the profile description string. Handles both ICC v2
	// textDescriptionType ('desc') and ICC v4 multiLocalizedUnicodeType
	// ('mluc'); selects the first record from mluc.
	std::wstring ReadDescription(HPROFILE prof)
	{
		DWORD cb = 0;
		BOOL ref = FALSE;
		::GetColorProfileElement(prof, kTagDesc, 0, &cb, nullptr, &ref);
		if (cb < 12) return {};

		std::vector<uint8_t> buf(cb);
		if (!::GetColorProfileElement(prof, kTagDesc, 0, &cb, buf.data(), &ref))
			return {};
		return ShaderLab::Rendering::ParseIccDescriptionTag(buf.data(), buf.size());
	}
}

namespace ShaderLab::Rendering
{
	std::wstring ParseIccDescriptionTag(const uint8_t* data, size_t size)
	{
		if (!data || size < 12) return {};
		const std::span<const uint8_t> buf(data, size);

		uint32_t typeSigBe = 0;
		std::memcpy(&typeSigBe, buf.data(), 4);
		const uint32_t typeSig = SwapBE32(typeSigBe);

		if (typeSig == kTypeDesc)
		{
			uint32_t lenBe = 0;
			std::memcpy(&lenBe, buf.data() + 8, 4);
			const uint32_t len = SwapBE32(lenBe);
			// Subtraction form: the lengths come from the file and may be huge.
			if (len == 0 || len > buf.size() - 12) return {};
			std::string ascii(reinterpret_cast<const char*>(buf.data() + 12), len);
			if (!ascii.empty() && ascii.back() == '\0') ascii.pop_back();
			return std::wstring(ascii.begin(), ascii.end());
		}

		if (typeSig == kTypeMluc)
		{
			if (buf.size() < 28) return {};
			// Records start at offset 16; each record header is 12 bytes:
			// lang(2) country(2) strLength(4) strOffset(4). Take record 0.
			uint32_t strLenBe = 0, strOffBe = 0;
			std::memcpy(&strLenBe, buf.data() + 20, 4);
			std::memcpy(&strOffBe, buf.data() + 24, 4);
			const uint32_t strLen = SwapBE32(strLenBe);
			const uint32_t strOff = SwapBE32(strOffBe);
			if (strLen < 2 || strOff > buf.size() || strLen > buf.size() - strOff) return {};

			const uint32_t chars = strLen / 2;
			std::wstring out;
			out.reserve(chars);
			for (uint32_t i = 0; i < chars; ++i)
			{
				const uint16_t ch =
					(static_cast<uint16_t>(buf[strOff + i * 2]) << 8) |
					 static_cast<uint16_t>(buf[strOff + i * 2 + 1]);
				if (ch == 0) break;
				out.push_back(static_cast<wchar_t>(ch));
			}
			return out;
		}

		return {};
	}
}

namespace ShaderLab::Rendering
{
	std::optional<IccProfileData> IccProfileParser::LoadFromFile(const std::wstring& filePath)
	{
		// Open the profile through mscms. PROFILE_FILENAME tells the API to
		// read the bytes from disk; PROFILE_READ + FILE_SHARE_READ is the
		// standard combination for read-only inspection.
		PROFILE prof{};
		prof.dwType = PROFILE_FILENAME;
		prof.pProfileData = const_cast<wchar_t*>(filePath.c_str());
		prof.cbDataSize = static_cast<DWORD>((filePath.size() + 1) * sizeof(wchar_t));

		UniqueProfile handle{ ::OpenColorProfileW(&prof, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING) };
		if (!handle)
			return std::nullopt;

		IccProfileData result{};
		bool hasR = false, hasG = false, hasB = false, hasW = false;

		float X, Y, Z;
		if (ReadXYZElement(handle.get(), kTagRXYZ, X, Y, Z))
		{
			result.primaryRed = XyzToXy(X, Y, Z);
			hasR = true;
		}
		if (ReadXYZElement(handle.get(), kTagGXYZ, X, Y, Z))
		{
			result.primaryGreen = XyzToXy(X, Y, Z);
			hasG = true;
		}
		if (ReadXYZElement(handle.get(), kTagBXYZ, X, Y, Z))
		{
			result.primaryBlue = XyzToXy(X, Y, Z);
			hasB = true;
		}
		if (ReadXYZElement(handle.get(), kTagWtpt, X, Y, Z))
		{
			result.whitePoint = XyzToXy(X, Y, Z);
			hasW = true;
		}
		if (ReadXYZElement(handle.get(), kTagLumi, X, Y, Z))
		{
			// ICC encodes display luminance as XYZType where Y carries cd/m^2.
			result.luminanceNits = Y;
		}

		result.description = ReadDescription(handle.get());
		result.valid = hasR && hasG && hasB && hasW;
		if (!result.valid)
			return std::nullopt;

		return result;
	}

	DisplayProfile DisplayProfileFromIcc(const IccProfileData& icc)
	{
		DisplayProfile p{};

		const float lum = (icc.luminanceNits > 0.0f) ? icc.luminanceNits : 270.0f;

		p.caps.maxLuminanceNits = lum;
		p.caps.hdrEnabled = (lum > 400.0f);
		p.caps.sdrWhiteLevelNits = 80.0f;
		p.caps.minLuminanceNits = p.caps.hdrEnabled ? 0.05f : 0.5f;
		p.caps.maxFullFrameLuminanceNits = (std::min)(lum, lum * 0.8f + 100.0f);

		p.primaryRed   = icc.primaryRed;
		p.primaryGreen = icc.primaryGreen;
		p.primaryBlue  = icc.primaryBlue;
		p.whitePoint   = icc.whitePoint;

		p.gamut = DetectGamut(icc.primaryRed, icc.primaryGreen, icc.primaryBlue);

		// Reuse the preset helper to stamp coherent ACM/WCG/activeColorMode
		// flags + bits-per-channel, derived from hdrEnabled and the gamut.
		StampSimulatedColorMode(p);
		p.profileName = icc.description.empty() ? L"ICC Profile" : icc.description;
		p.isSimulated = true;

		return p;
	}
}
