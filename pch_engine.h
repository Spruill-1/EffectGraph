#pragma once

// Windows SDK
#include <windows.h>
#include <unknwn.h>
#include <restrictederrorinfo.h>
#include <hstring.h>

// WinRT base
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Graphics.Display.h>

// Direct3D / Direct2D / DXGI
#include <d3d11_4.h>
#include <d2d1_3.h>
#include <d2d1_3helper.h>
#include <d2d1effects_2.h>
#include <d2d1effectauthor_1.h>
#include <d2d1effecthelpers.h>
#include <dxgi1_6.h>
#include <dwrite_3.h>

// Shader compilation
#include <d3dcompiler.h>

// WIC (image loading)
#include <wincodec.h>

// Media Foundation (video decoding)
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

// What the engine library links against, so every program that links it does.
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mscms.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "Cabinet.lib")
#pragma comment(lib, "bcrypt.lib")

// STL
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
