#include "pch_engine.h"
#include "DeviceStack.h"

namespace ShaderLab::Rendering
{
    namespace
    {
        constexpr UINT cD3DFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;   // required for D2D interop
        constexpr D3D_FEATURE_LEVEL cFeatureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

        HRESULT CreateD3DDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType,
                                winrt::com_ptr<ID3D11Device>& device, winrt::com_ptr<ID3D11DeviceContext>& context)
        {
            device = nullptr;
            context = nullptr;
            return D3D11CreateDevice(adapter, driverType, nullptr, cD3DFlags,
                cFeatureLevels, ARRAYSIZE(cFeatureLevels), D3D11_SDK_VERSION,
                device.put(), nullptr, context.put());
        }
    }

    HRESULT CreateDeviceStack(const DeviceStackOptions& options, DeviceStack& stack, std::wstring* failedStep)
    {
        stack = {};
        auto fail = [&](HRESULT hr, const wchar_t* step)
        {
            if (failedStep) *failedStep = step;
            stack = {};
            return hr;
        };

        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(stack.dxgiFactory.put()));
        if (FAILED(hr)) return fail(hr, L"CreateDXGIFactory2");

        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        hr = E_FAIL;
        if (options.choice == AdapterChoice::Adapter)
        {
            winrt::com_ptr<IDXGIAdapter1> adapter;
            for (UINT i = 0; stack.dxgiFactory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i)
            {
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                if (desc.AdapterLuid.LowPart == options.adapterLuid.LowPart &&
                    desc.AdapterLuid.HighPart == options.adapterLuid.HighPart)
                {
                    hr = CreateD3DDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, device, context);
                    break;
                }
                adapter = nullptr;
            }
        }
        if (FAILED(hr))
        {
            const D3D_DRIVER_TYPE driverType = options.choice == AdapterChoice::Warp
                ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
            hr = CreateD3DDevice(nullptr, driverType, device, context);
            if (FAILED(hr) && (options.choice == AdapterChoice::Default || options.choice == AdapterChoice::Adapter))
                hr = CreateD3DDevice(nullptr, D3D_DRIVER_TYPE_WARP, device, context);
        }
        if (FAILED(hr)) return fail(hr, L"D3D11CreateDevice");
        return CreateDeviceStackOn(device.get(), stack, failedStep);
    }

    HRESULT CreateDeviceStackOn(ID3D11Device* device, DeviceStack& stack, std::wstring* failedStep)
    {
        stack = {};
        auto fail = [&](HRESULT hr, const wchar_t* step)
        {
            if (failedStep) *failedStep = step;
            stack = {};
            return hr;
        };
        if (!device) return fail(E_INVALIDARG, L"No device");
        if (!(device->GetCreationFlags() & D3D11_CREATE_DEVICE_BGRA_SUPPORT))
            return fail(E_INVALIDARG, L"D3D11_CREATE_DEVICE_BGRA_SUPPORT");

        HRESULT hr = device->QueryInterface(IID_PPV_ARGS(stack.d3dDevice.put()));
        if (SUCCEEDED(hr))
        {
            winrt::com_ptr<ID3D11DeviceContext> context;
            device->GetImmediateContext(context.put());
            hr = context->QueryInterface(IID_PPV_ARGS(stack.d3dContext.put()));
        }
        if (FAILED(hr)) return fail(hr, L"ID3D11Device5");

        // Video decode and the compute runner use the device from other threads.
        if (auto multithread = stack.d3dDevice.try_as<ID3D10Multithread>())
            multithread->SetMultithreadProtected(TRUE);

        winrt::com_ptr<IDXGIDevice> dxgiDevice = stack.d3dDevice.try_as<IDXGIDevice>();
        if (!dxgiDevice) return fail(E_NOINTERFACE, L"IDXGIDevice");
        winrt::com_ptr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.put())) && adapter)
        {
            if (!stack.dxgiFactory)
                adapter->GetParent(IID_PPV_ARGS(stack.dxgiFactory.put()));
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc)))
            {
                stack.adapterName = desc.Description;
                // WARP: VendorId 0x1414 (Microsoft), DeviceId 0x8C.
                stack.isWarp = desc.VendorId == 0x1414 && desc.DeviceId == 0x008C;
            }
        }

        // Multi-threaded: capture providers deliver frames on background
        // threads, and device teardown runs on the UI thread.
        D2D1_FACTORY_OPTIONS d2dOptions{};
        d2dOptions.debugLevel = D2D1_DEBUG_LEVEL_NONE;
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory7), &d2dOptions,
                               stack.d2dFactory.put_void());
        if (FAILED(hr)) return fail(hr, L"D2D1CreateFactory");

        winrt::com_ptr<ID2D1Device> d2dDevice;
        hr = stack.d2dFactory->CreateDevice(dxgiDevice.get(), d2dDevice.put());
        if (SUCCEEDED(hr)) hr = d2dDevice->QueryInterface(IID_PPV_ARGS(stack.d2dDevice.put()));
        if (FAILED(hr)) return fail(hr, L"ID2D1Factory::CreateDevice");
        return S_OK;
    }
}
