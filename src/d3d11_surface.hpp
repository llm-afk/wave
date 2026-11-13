#pragma once
#include <d3d11.h>

namespace wave::win32 {
class D3D11Surface {
public:
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    IDXGISwapChain* swap_chain{};
    ID3D11RenderTargetView* target{};

    D3D11Surface() = default;
    D3D11Surface(const D3D11Surface&) = delete;
    D3D11Surface& operator=(const D3D11Surface&) = delete;
    ~D3D11Surface() { cleanup(); }

    void release_target() {
        if (context) context->OMSetRenderTargets(0, nullptr, nullptr);
        if (target) { target->Release(); target = nullptr; }
    }
    bool make_target() {
        ID3D11Texture2D* buffer{};
        if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return false;
        const auto result = device->CreateRenderTargetView(buffer, nullptr, &target);
        buffer->Release();
        return SUCCEEDED(result);
    }
    void cleanup() {
        release_target();
        if (swap_chain) { swap_chain->Release(); swap_chain = nullptr; }
        if (context) { context->Release(); context = nullptr; }
        if (device) { device->Release(); device = nullptr; }
    }
    bool create(HWND window) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = window;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL level{};
        auto result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION, &desc, &swap_chain, &device, &level, &context);
        if (FAILED(result)) {
            cleanup();
            result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                levels, 2, D3D11_SDK_VERSION, &desc, &swap_chain, &device, &level, &context);
        }
        if (FAILED(result)) return false;
        IDXGIDevice1* dxgi_device{};
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
            dxgi_device->SetMaximumFrameLatency(1);
            dxgi_device->Release();
        }
        IDXGIFactory* factory{};
        if (SUCCEEDED(swap_chain->GetParent(IID_PPV_ARGS(&factory)))) {
            factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
            factory->Release();
        }
        return make_target();
    }
    bool resize(UINT width, UINT height) {
        release_target();
        return SUCCEEDED(swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)) && make_target();
    }
};
}
