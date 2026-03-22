// File: Dx12RendererSetup.cpp
// Purpose: Builds DX12 device objects, pipeline states, root signatures, and shader programs.

#ifdef _WIN32

#include "engine/render/dx12/Dx12RendererPrivate.h"

namespace engine::render {

void Dx12Renderer::Impl::updateCamera(float deltaTime) {
        // Keyboard-driven debug camera; also caches basis vectors for culling/HUD math.
        if (deltaTime <= 0.0F) {
            return;
        }

        const float speedMultiplier = isKeyDown(VK_SHIFT) ? 2.5F : 1.0F;
        const float translationStep = cameraMoveSpeed * speedMultiplier * deltaTime;
        const float rotationStep = cameraLookSpeed * deltaTime;

        if (isKeyDown(VK_LEFT)) {
            cameraYawRadians -= rotationStep;
        }
        if (isKeyDown(VK_RIGHT)) {
            cameraYawRadians += rotationStep;
        }
        if (isKeyDown(VK_UP)) {
            cameraPitchRadians += rotationStep;
        }
        if (isKeyDown(VK_DOWN)) {
            cameraPitchRadians -= rotationStep;
        }

        cameraPitchRadians = std::clamp(cameraPitchRadians, -1.40F, 1.40F);

        const math::Vec3 forward = math::normalize({
            std::sin(cameraYawRadians) * std::cos(cameraPitchRadians),
            std::sin(cameraPitchRadians),
            std::cos(cameraYawRadians) * std::cos(cameraPitchRadians),
        });
        const math::Vec3 worldUp{0.0F, 1.0F, 0.0F};
        const math::Vec3 right = math::normalize(math::cross(worldUp, forward));
        const math::Vec3 up = math::normalize(math::cross(forward, right));
        const float fovYRadians = DirectX::XMConvertToRadians(60.0F);
        cameraAspectRatio = (height > 0) ? (static_cast<float>(width) / static_cast<float>(height)) : 1.0F;
        cameraTanHalfFovY = std::tan(fovYRadians * 0.5F);
        cameraForwardCached = forward;
        cameraRightCached = right;
        cameraUpCached = up;

        if (isKeyDown('W')) {
            cameraPosition += forward * translationStep;
        }
        if (isKeyDown('S')) {
            cameraPosition -= forward * translationStep;
        }
        if (isKeyDown('A')) {
            cameraPosition -= right * translationStep;
        }
        if (isKeyDown('D')) {
            cameraPosition += right * translationStep;
        }
        if (isKeyDown('Q')) {
            cameraPosition.y += translationStep;
        }
        if (isKeyDown('E')) {
            cameraPosition.y -= translationStep;
        }
    }

bool Dx12Renderer::Impl::isSphereVisible(const math::Vec3& center,
                                         float radius,
                                         const math::Vec3& cameraForward,
                                         const math::Vec3& cameraRight,
                                         const math::Vec3& cameraUp,
                                         float aspect,
                                         float tanHalfFovY) const {
        // CPU frustum test used as fallback and reference for GPU culling.
        const float nearPlane = 0.1F;
        const float farPlane = 100.0F;
        const math::Vec3 toCenter = center - cameraPosition;

        const float z = math::dot(toCenter, cameraForward);
        if (z < (nearPlane - radius) || z > (farPlane + radius)) {
            return false;
        }

        const float frustumHalfY = std::max(nearPlane, z) * tanHalfFovY;
        const float frustumHalfX = frustumHalfY * aspect;
        const float x = math::dot(toCenter, cameraRight);
        const float y = math::dot(toCenter, cameraUp);
        if (std::abs(x) > (frustumHalfX + radius)) {
            return false;
        }
        if (std::abs(y) > (frustumHalfY + radius)) {
            return false;
        }
        return true;
    }

void Dx12Renderer::Impl::queueResize(std::uint32_t newWidth, std::uint32_t newHeight) {
        // Defer expensive resize work until safe point in the frame loop.
        if (newWidth == 0 || newHeight == 0) {
            return;
        }
        pendingWidth = newWidth;
        pendingHeight = newHeight;
        resizePending = true;
    }

LRESULT CALLBACK Dx12Renderer::Impl::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        // Route Win32 messages to renderer instance state.
        if (message == WM_NCCREATE) {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* self = static_cast<Impl*>(createStruct->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }

        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self != nullptr) {
            switch (message) {
                case WM_CLOSE:
                    self->closeRequested = true;
                    DestroyWindow(hwnd);
                    return 0;
                case WM_DESTROY:
                    self->closeRequested = true;
                    PostQuitMessage(0);
                    return 0;
                case WM_SIZE:
                    if (wParam != SIZE_MINIMIZED) {
                        const auto resizedWidth = static_cast<std::uint32_t>(LOWORD(lParam));
                        const auto resizedHeight = static_cast<std::uint32_t>(HIWORD(lParam));
                        self->queueResize(resizedWidth, resizedHeight);
                    }
                    return 0;
                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                    self->handleKeyEvent(wParam, lParam, true);
                    return 0;
                case WM_KEYUP:
                case WM_SYSKEYUP:
                    self->handleKeyEvent(wParam, lParam, false);
                    return 0;
                default:
                    break;
            }
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

void Dx12Renderer::Impl::createWindow() {
        // Create and show the main render window.
        const HINSTANCE instance = GetModuleHandleW(nullptr);

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(WNDCLASSEXW);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &Impl::windowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kWindowClassName;

        const ATOM classAtom = RegisterClassExW(&windowClass);
        if (classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("Failed to register window class");
        }

        RECT bounds{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);

        window = CreateWindowExW(0,
                                 kWindowClassName,
                                 title.c_str(),
                                 WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT,
                                 CW_USEDEFAULT,
                                 bounds.right - bounds.left,
                                 bounds.bottom - bounds.top,
                                 nullptr,
                                 nullptr,
                                 instance,
                                 this);
        if (window == nullptr) {
            throw std::runtime_error("Failed to create window");
        }

        ShowWindow(window, SW_SHOWDEFAULT);
        UpdateWindow(window);
    }

void Dx12Renderer::Impl::createDevice() {
        // Pick hardware adapter first, then fallback to WARP if needed.
        std::uint32_t factoryFlags = 0;

        if (enableValidation) {
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
                debugController->EnableDebugLayer();
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            }
        }

        throwIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)), "Failed to create DXGI factory");

        ComPtr<IDXGIAdapter1> adapter;
        for (std::uint32_t index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U) {
                continue;
            }

            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
                break;
            }

            adapter.Reset();
        }

        if (!device) {
            throwIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "Failed to get WARP adapter");
            throwIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
                          "Failed to create D3D12 device");
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        throwIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)),
                      "Failed to create command queue");
    }

void Dx12Renderer::Impl::updateViewportAndScissorFromSize() {
        // Keep scene and shadow viewport/scissor in sync with current dimensions.
        viewport.TopLeftX = 0.0F;
        viewport.TopLeftY = 0.0F;
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0F;
        viewport.MaxDepth = 1.0F;

        scissorRect.left = 0;
        scissorRect.top = 0;
        scissorRect.right = static_cast<LONG>(width);
        scissorRect.bottom = static_cast<LONG>(height);

        shadowViewport.TopLeftX = 0.0F;
        shadowViewport.TopLeftY = 0.0F;
        shadowViewport.Width = static_cast<float>(kShadowMapResolution);
        shadowViewport.Height = static_cast<float>(kShadowMapResolution);
        shadowViewport.MinDepth = 0.0F;
        shadowViewport.MaxDepth = 1.0F;

        shadowScissorRect.left = 0;
        shadowScissorRect.top = 0;
        shadowScissorRect.right = static_cast<LONG>(kShadowMapResolution);
        shadowScissorRect.bottom = static_cast<LONG>(kShadowMapResolution);
    }

void Dx12Renderer::Impl::createSwapChainAndTargets() {
        // Create swapchain, RTV/DSV/SRV descriptor heaps, and per-frame render targets.
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
        swapChainDesc.Width = width;
        swapChainDesc.Height = height;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = kFrameCount;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.Scaling = DXGI_SCALING_STRETCH;

        ComPtr<IDXGISwapChain1> swapChainV1;
        throwIfFailed(factory->CreateSwapChainForHwnd(
                          commandQueue.Get(), window, &swapChainDesc, nullptr, nullptr, &swapChainV1),
                      "Failed to create swap chain");
        throwIfFailed(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "Failed to set window association");
        throwIfFailed(swapChainV1.As(&swapChain), "Failed to query IDXGISwapChain3");

        frameIndex = swapChain->GetCurrentBackBufferIndex();

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.NumDescriptors = kFrameCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        throwIfFailed(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)),
                      "Failed to create RTV descriptor heap");
        rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.NumDescriptors = 2;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        throwIfFailed(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap)),
                      "Failed to create DSV descriptor heap");
        dsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
        srvHeapDesc.NumDescriptors = static_cast<UINT>(kMaxTextures);
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        throwIfFailed(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)),
                      "Failed to create SRV descriptor heap");
        srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t i = 0; i < kFrameCount; ++i) {
            throwIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i])), "Failed to get swap chain buffer");
            device->CreateRenderTargetView(renderTargets[i].Get(), nullptr, rtvHandle);
            rtvHandle.ptr += rtvDescriptorSize;
        }
        updateViewportAndScissorFromSize();
    }

void Dx12Renderer::Impl::recreateSceneDepthStencil() {
        // Recreate scene depth buffer after init/resize.
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC sceneDepthDesc{};
        sceneDepthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        sceneDepthDesc.Width = width;
        sceneDepthDesc.Height = height;
        sceneDepthDesc.DepthOrArraySize = 1;
        sceneDepthDesc.MipLevels = 1;
        sceneDepthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        sceneDepthDesc.SampleDesc.Count = 1;
        sceneDepthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        sceneDepthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE sceneClearValue{};
        sceneClearValue.Format = DXGI_FORMAT_D32_FLOAT;
        sceneClearValue.DepthStencil.Depth = 1.0F;
        sceneClearValue.DepthStencil.Stencil = 0;

        throwIfFailed(device->CreateCommittedResource(&heapProps,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &sceneDepthDesc,
                                                      D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                      &sceneClearValue,
                                                      IID_PPV_ARGS(&depthStencil)),
                      "Failed to create depth stencil resource");

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

        D3D12_CPU_DESCRIPTOR_HANDLE sceneDsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateDepthStencilView(depthStencil.Get(), &dsvDesc, sceneDsv);
    }

void Dx12Renderer::Impl::createDepthStencil() {
        // Create scene depth and persistent shadow-map depth resources.
        recreateSceneDepthStencil();
        if (shadowMap) {
            return;
        }

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC shadowDesc{};
        shadowDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        shadowDesc.Width = kShadowMapResolution;
        shadowDesc.Height = kShadowMapResolution;
        shadowDesc.DepthOrArraySize = 1;
        shadowDesc.MipLevels = 1;
        shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        shadowDesc.SampleDesc.Count = 1;
        shadowDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        shadowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE shadowClearValue{};
        shadowClearValue.Format = DXGI_FORMAT_D32_FLOAT;
        shadowClearValue.DepthStencil.Depth = 1.0F;
        shadowClearValue.DepthStencil.Stencil = 0;

        throwIfFailed(device->CreateCommittedResource(&heapProps,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &shadowDesc,
                                                      D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                      &shadowClearValue,
                                                      IID_PPV_ARGS(&shadowMap)),
                      "Failed to create shadow map resource");

        D3D12_DEPTH_STENCIL_VIEW_DESC shadowDsvDesc{};
        shadowDsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        shadowDsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        shadowDsv.ptr += dsvDescriptorSize;
        device->CreateDepthStencilView(shadowMap.Get(), &shadowDsvDesc, shadowDsv);

        if (nextSrvIndex >= kMaxTextures) {
            throw std::runtime_error("Shadow map SRV reservation failed: heap capacity reached");
        }

        shadowMapSrvIndex = nextSrvIndex++;
        D3D12_CPU_DESCRIPTOR_HANDLE shadowSrv = srvHeap->GetCPUDescriptorHandleForHeapStart();
        shadowSrv.ptr += static_cast<SIZE_T>(shadowMapSrvIndex) * srvDescriptorSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc{};
        shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shadowSrvDesc.Texture2D.MostDetailedMip = 0;
        shadowSrvDesc.Texture2D.MipLevels = 1;
        shadowSrvDesc.Texture2D.ResourceMinLODClamp = 0.0F;
        device->CreateShaderResourceView(shadowMap.Get(), &shadowSrvDesc, shadowSrv);

        shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

void Dx12Renderer::Impl::applyPendingResize() {
        // Rebuild back buffers/depth resources only when a valid resize was requested.
        if (!resizePending || !initialized || !swapChain || !rtvHeap || !dsvHeap || !device) {
            return;
        }
        if (pendingWidth == 0 || pendingHeight == 0) {
            resizePending = false;
            return;
        }

        const std::uint32_t newWidth = pendingWidth;
        const std::uint32_t newHeight = pendingHeight;
        resizePending = false;
        if (newWidth == width && newHeight == height) {
            return;
        }

        waitForGpu();

        for (auto& target : renderTargets) {
            target.Reset();
        }
        depthStencil.Reset();

        throwIfFailed(swapChain->ResizeBuffers(kFrameCount, newWidth, newHeight, DXGI_FORMAT_R8G8B8A8_UNORM, 0),
                      "Failed to resize swap chain buffers");
        frameIndex = swapChain->GetCurrentBackBufferIndex();
        width = newWidth;
        height = newHeight;

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t i = 0; i < kFrameCount; ++i) {
            throwIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i])),
                          "Failed to reacquire swap-chain buffer after resize");
            device->CreateRenderTargetView(renderTargets[i].Get(), nullptr, rtvHandle);
            rtvHandle.ptr += rtvDescriptorSize;
        }

        updateViewportAndScissorFromSize();
        recreateSceneDepthStencil();
        rebuildGpuMemoryStats();
    }

void Dx12Renderer::Impl::createPipeline() {
        // Compile shaders and build graphics/compute pipeline state objects.
        D3D12_DESCRIPTOR_RANGE diffuseRange{};
        diffuseRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        diffuseRange.NumDescriptors = static_cast<UINT>(kMaxTextures);
        diffuseRange.BaseShaderRegister = 0;
        diffuseRange.RegisterSpace = 0;
        diffuseRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE shadowRange{};
        shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        shadowRange.NumDescriptors = 1;
        shadowRange.BaseShaderRegister = 128;
        shadowRange.RegisterSpace = 0;
        shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParameters[4]{};
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].Descriptor.ShaderRegister = 0;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
        rootParameters[1].DescriptorTable.pDescriptorRanges = &diffuseRange;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;
        rootParameters[2].DescriptorTable.pDescriptorRanges = &shadowRange;
        rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[3].Constants.ShaderRegister = 1;
        rootParameters[3].Constants.RegisterSpace = 0;
        rootParameters[3].Constants.Num32BitValues = 1;
        rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC staticSamplers[2]{};
        staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[0].MipLODBias = 0.0F;
        staticSamplers[0].MaxAnisotropy = 1;
        staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        staticSamplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        staticSamplers[0].MinLOD = 0.0F;
        staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        staticSamplers[0].ShaderRegister = 0;
        staticSamplers[0].RegisterSpace = 0;
        staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        staticSamplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[1].MipLODBias = 0.0F;
        staticSamplers[1].MaxAnisotropy = 1;
        staticSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        staticSamplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        staticSamplers[1].MinLOD = 0.0F;
        staticSamplers[1].MaxLOD = D3D12_FLOAT32_MAX;
        staticSamplers[1].ShaderRegister = 1;
        staticSamplers[1].RegisterSpace = 0;
        staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
        rootSignatureDesc.NumParameters = static_cast<UINT>(std::size(rootParameters));
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = static_cast<UINT>(std::size(staticSamplers));
        rootSignatureDesc.pStaticSamplers = staticSamplers;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> serializedRootSignature;
        ComPtr<ID3DBlob> rootSignatureErrors;
        throwIfFailed(D3D12SerializeRootSignature(
                          &rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSignature, &rootSignatureErrors),
                      "Failed to serialize root signature");
        throwIfFailed(device->CreateRootSignature(0,
                                                  serializedRootSignature->GetBufferPointer(),
                                                  serializedRootSignature->GetBufferSize(),
                                                  IID_PPV_ARGS(&rootSignature)),
                      "Failed to create root signature");

        const char* mainShaderSource = R"(
cbuffer ObjectCB : register(b0)
{
    float4x4 mvp;
    float4x4 model;
    float4x4 lightViewProjection;
    float4 baseColor;
    float4 lightDirectionAndShadowBias;
    float4 debugOptions;
}
Texture2D diffuseMaps[128] : register(t0);
Texture2D shadowMap : register(t128);
SamplerState linearSampler : register(s0);
SamplerComparisonState shadowSampler : register(s1);
cbuffer DrawRootConstants : register(b1)
{
    uint diffuseDescriptorIndex;
}

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 color : COLOR;
    float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL0;
    float3 color : COLOR0;
    float2 uv : TEXCOORD0;
    float4 shadowClip : TEXCOORD1;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float4 worldPosition = mul(float4(input.position, 1.0f), model);
    output.position = mul(float4(input.position, 1.0f), mvp);
    output.worldNormal = normalize(mul(float4(input.normal, 0.0f), model).xyz);
    output.color = input.color;
    output.uv = input.uv;
    output.shadowClip = mul(worldPosition, lightViewProjection);
    return output;
}

float computeShadowFactor(float4 shadowClip, float bias)
{
    float zDivisor = max(shadowClip.w, 1e-5f);
    float3 shadowNdc = shadowClip.xyz / zDivisor;
    float2 shadowUv = float2(shadowNdc.x * 0.5f + 0.5f, -shadowNdc.y * 0.5f + 0.5f);
    float shadowDepth = shadowNdc.z - bias;
    if (shadowUv.x < 0.0f || shadowUv.x > 1.0f || shadowUv.y < 0.0f || shadowUv.y > 1.0f)
    {
        return 1.0f;
    }
    if (shadowDepth < 0.0f || shadowDepth > 1.0f)
    {
        return 1.0f;
    }
    return shadowMap.SampleCmpLevelZero(shadowSampler, shadowUv, shadowDepth);
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float3 L = normalize(-lightDirectionAndShadowBias.xyz);
    float ndotl = saturate(dot(normalize(input.worldNormal), L));
    float shadowFactor = computeShadowFactor(input.shadowClip, lightDirectionAndShadowBias.w);
    if (debugOptions.x > 0.5f)
    {
        return float4(shadowFactor.xxx, 1.0f);
    }
    uint diffuseIndex = min(diffuseDescriptorIndex, 127u);
    float4 albedo = diffuseMaps[diffuseIndex].Sample(linearSampler, input.uv);
    float lighting = 0.25f + shadowFactor * ndotl * 0.75f;
    float3 shaded = input.color * albedo.rgb * baseColor.rgb * lighting;
    return float4(shaded, albedo.a * baseColor.a);
}
)";

        const char* shadowShaderSource = R"(
cbuffer ObjectCB : register(b0)
{
    float4x4 mvp;
    float4x4 model;
    float4x4 lightViewProjection;
    float4 baseColor;
    float4 lightDirectionAndShadowBias;
    float4 debugOptions;
}

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 color : COLOR;
    float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_POSITION;
};

VSOutput ShadowVSMain(VSInput input)
{
    VSOutput output;
    float4 worldPosition = mul(float4(input.position, 1.0f), model);
    output.position = mul(worldPosition, lightViewProjection);
    return output;
}
)";

        const char* debugShaderSource = R"(
cbuffer ObjectCB : register(b0)
{
    float4x4 mvp;
    float4x4 model;
    float4x4 lightViewProjection;
    float4 baseColor;
    float4 lightDirectionAndShadowBias;
    float4 debugOptions;
}

struct VSInput
{
    float3 position : POSITION;
    float3 color : COLOR;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 color : COLOR0;
};

VSOutput DebugVSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0f), mvp);
    output.color = input.color;
    return output;
}

float4 DebugPSMain(VSOutput input) : SV_TARGET
{
    return float4(input.color, 1.0f);
}
)";

        const char* computeCullShaderSource = R"(
cbuffer CullParams : register(b0)
{
    float4 cameraPositionNear;
    float4 cameraForwardFar;
    float4 cameraRightAspect;
    float4 cameraUpTanHalfFovY;
    uint4 drawAndPadding;
}

StructuredBuffer<float4> sphereBuffer : register(t0);
RWStructuredBuffer<uint> visibleBuffer : register(u0);
RWStructuredBuffer<uint> compactIndexBuffer : register(u1);
RWByteAddressBuffer compactCounter : register(u2);

[numthreads(64, 1, 1)]
void FrustumCullCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint drawIndex = dispatchThreadId.x;
    if (drawIndex >= drawAndPadding.x)
    {
        return;
    }

    float4 sphere = sphereBuffer[drawIndex];
    if (sphere.w <= 0.0f)
    {
        visibleBuffer[drawIndex] = 0;
        return;
    }

    float3 toCenter = sphere.xyz - cameraPositionNear.xyz;
    float z = dot(toCenter, cameraForwardFar.xyz);
    float nearPlane = cameraPositionNear.w;
    float farPlane = cameraForwardFar.w;

    bool visible = (z >= (nearPlane - sphere.w) && z <= (farPlane + sphere.w));
    if (visible)
    {
        float frustumHalfY = max(nearPlane, z) * cameraUpTanHalfFovY.w;
        float frustumHalfX = frustumHalfY * cameraRightAspect.w;
        float x = dot(toCenter, cameraRightAspect.xyz);
        float y = dot(toCenter, cameraUpTanHalfFovY.xyz);
        visible = (abs(x) <= (frustumHalfX + sphere.w)) && (abs(y) <= (frustumHalfY + sphere.w));
    }

    if (!visible)
    {
        visibleBuffer[drawIndex] = 0u;
        return;
    }

    visibleBuffer[drawIndex] = 1u;
    uint compactSlot = 0u;
    compactCounter.InterlockedAdd(0, 1u, compactSlot);
    compactIndexBuffer[compactSlot] = drawIndex;
}
)";

        const char* computeOcclusionShaderSource = R"(
cbuffer OcclusionParams : register(b0)
{
    float aspect;
    float tanHalfFovY;
    float maxVisibleRatio;
    float occlusionCoverageThreshold;
    float depthSlack;
    uint drawCount;
    uint candidateCount;
    uint visibilityBudgetEnabled;
    uint hzbEnabled;
    uint occlusionDispatchGroupSize;
    uint pad0;
    uint pad1;
}

StructuredBuffer<float4> camSpaceBuffer : register(t0);
StructuredBuffer<uint> candidateIndices : register(t1);
ByteAddressBuffer candidateCountBuffer : register(t2);
RWStructuredBuffer<uint> visibleIndices : register(u0);
RWStructuredBuffer<uint> outStats : register(u1);
RWByteAddressBuffer indirectCountBuffer : register(u2);

[numthreads(64, 1, 1)]
void OcclusionCullCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint candidateCountResolved = min(candidateCountBuffer.Load(0), drawCount);
    const uint candidatePos = dispatchThreadId.x;
    if (candidatePos >= candidateCountResolved)
    {
        return;
    }

    if (candidatePos == 0u)
    {
        outStats[4] = (hzbEnabled != 0u) ? 1u : 0u;
        outStats[6] = candidateCountResolved;
    }

    const uint drawIndex = candidateIndices[candidatePos];
    const float4 cam = camSpaceBuffer[drawIndex];
    if (cam.w <= 0.0f)
    {
        return;
    }

    const float clampedMaxVisibleRatio = clamp(maxVisibleRatio, 0.05f, 1.0f);
    const uint unclampedMaxVisibleCount = (uint)ceil(clampedMaxVisibleRatio * (float)drawCount);
    const uint maxVisibleCount = (visibilityBudgetEnabled != 0u)
        ? min(drawCount, max(1u, unclampedMaxVisibleCount))
        : drawCount;

    bool accepted = false;
    uint visibleSlot = 0u;
    [loop]
    for (uint spin = 0u; spin < 64u; ++spin)
    {
        const uint currentVisible = outStats[0];
        if (currentVisible >= maxVisibleCount)
        {
            break;
        }

        uint originalVisible = 0u;
        InterlockedCompareExchange(outStats[0], currentVisible, currentVisible + 1u, originalVisible);
        if (originalVisible == currentVisible)
        {
            visibleSlot = currentVisible;
            accepted = true;
            break;
        }
    }

    if (!accepted)
    {
        uint _;
        InterlockedAdd(outStats[1], 1u, _);
        return;
    }

    visibleIndices[visibleSlot] = drawIndex;
}
)";

        const char* computeMainBuildShaderSource = R"(
cbuffer BuildParams : register(b0)
{
    uint drawCount;
    uint pad0;
    uint pad1;
    uint pad2;
}

struct DrawMetadata
{
    uint2 vertexBufferAddress;
    uint vertexBufferSize;
    uint vertexBufferStride;
    uint2 indexBufferAddress;
    uint indexBufferSize;
    uint indexBufferFormat;
    uint2 constantBufferAddress;
    uint indexCount;
    uint textureDescriptorIndex;
};

struct MainIndirectCommand
{
    uint2 vertexBufferAddress;
    uint vertexBufferSize;
    uint vertexBufferStride;
    uint2 indexBufferAddress;
    uint indexBufferSize;
    uint indexBufferFormat;
    uint2 constantBufferAddress;
    uint textureDescriptorIndex;
    uint indexCountPerInstance;
    uint instanceCount;
    uint startIndexLocation;
    int baseVertexLocation;
    uint startInstanceLocation;
};

StructuredBuffer<uint> visibleIndices : register(t0);
StructuredBuffer<DrawMetadata> drawMetadataBuffer : register(t1);
RWStructuredBuffer<MainIndirectCommand> outCommands : register(u0);
RWByteAddressBuffer indirectCountBuffer : register(u1);

[numthreads(64, 1, 1)]
void BuildMainIndirectCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint commandIndex = dispatchThreadId.x;
    uint visibleCount = min(indirectCountBuffer.Load(0), drawCount);
    if (commandIndex >= visibleCount)
    {
        return;
    }

    uint drawIndex = visibleIndices[commandIndex];
    if (drawIndex >= drawCount)
    {
        return;
    }

    DrawMetadata metadata = drawMetadataBuffer[drawIndex];
    MainIndirectCommand command;
    command.vertexBufferAddress = uint2(0u, 0u);
    command.vertexBufferSize = 0u;
    command.vertexBufferStride = 0u;
    command.indexBufferAddress = uint2(0u, 0u);
    command.indexBufferSize = 0u;
    command.indexBufferFormat = 0u;
    command.constantBufferAddress = uint2(0u, 0u);
    command.textureDescriptorIndex = 0u;
    command.indexCountPerInstance = 0u;
    command.instanceCount = 0u;
    command.startIndexLocation = 0u;
    command.baseVertexLocation = 0;
    command.startInstanceLocation = 0u;
    if (metadata.indexCount == 0u)
    {
        outCommands[commandIndex] = command;
        return;
    }

    command.vertexBufferAddress = metadata.vertexBufferAddress;
    command.vertexBufferSize = metadata.vertexBufferSize;
    command.vertexBufferStride = metadata.vertexBufferStride;
    command.indexBufferAddress = metadata.indexBufferAddress;
    command.indexBufferSize = metadata.indexBufferSize;
    command.indexBufferFormat = metadata.indexBufferFormat;
    command.constantBufferAddress = metadata.constantBufferAddress;
    command.textureDescriptorIndex = metadata.textureDescriptorIndex;
    command.indexCountPerInstance = metadata.indexCount;
    command.instanceCount = 1u;
    command.startIndexLocation = 0u;
    command.baseVertexLocation = 0;
    command.startInstanceLocation = 0u;
    outCommands[commandIndex] = command;
}
)";

        std::uint32_t compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> pixelShader;
        ComPtr<ID3DBlob> shadowVertexShader;
        ComPtr<ID3DBlob> debugVertexShader;
        ComPtr<ID3DBlob> debugPixelShader;
        ComPtr<ID3DBlob> computeCullShader;
        ComPtr<ID3DBlob> computeOcclusionShader;
        ComPtr<ID3DBlob> computeMainBuildShader;
        ComPtr<ID3DBlob> shaderErrors;

        throwIfFailed(D3DCompile(mainShaderSource,
                                 std::strlen(mainShaderSource),
                                 "InlineMainShader",
                                 nullptr,
                                 nullptr,
                                 "VSMain",
                                 "vs_5_0",
                                 compileFlags,
                                 0,
                                 &vertexShader,
                                 &shaderErrors),
                      "Failed to compile main vertex shader");
        throwIfFailed(D3DCompile(mainShaderSource,
                                 std::strlen(mainShaderSource),
                                 "InlineMainShader",
                                 nullptr,
                                 nullptr,
                                 "PSMain",
                                 "ps_5_0",
                                 compileFlags,
                                 0,
                                 &pixelShader,
                                 &shaderErrors),
                      "Failed to compile main pixel shader");
        throwIfFailed(D3DCompile(shadowShaderSource,
                                 std::strlen(shadowShaderSource),
                                 "InlineShadowShader",
                                 nullptr,
                                 nullptr,
                                 "ShadowVSMain",
                                 "vs_5_0",
                                 compileFlags,
                                 0,
                                 &shadowVertexShader,
                                 &shaderErrors),
                      "Failed to compile shadow vertex shader");
        throwIfFailed(D3DCompile(debugShaderSource,
                                 std::strlen(debugShaderSource),
                                 "InlineDebugShader",
                                 nullptr,
                                 nullptr,
                                 "DebugVSMain",
                                 "vs_5_0",
                                 compileFlags,
                                 0,
                                 &debugVertexShader,
                                 &shaderErrors),
                      "Failed to compile debug vertex shader");
        throwIfFailed(D3DCompile(debugShaderSource,
                                 std::strlen(debugShaderSource),
                                 "InlineDebugShader",
                                 nullptr,
                                 nullptr,
                                 "DebugPSMain",
                                 "ps_5_0",
                                 compileFlags,
                                 0,
                                 &debugPixelShader,
                                 &shaderErrors),
                      "Failed to compile debug pixel shader");
        throwIfFailed(D3DCompile(computeCullShaderSource,
                                 std::strlen(computeCullShaderSource),
                                 "InlineComputeCullShader",
                                 nullptr,
                                 nullptr,
                                 "FrustumCullCS",
                                 "cs_5_0",
                                 compileFlags,
                                 0,
                                 &computeCullShader,
                                 &shaderErrors),
                      "Failed to compile frustum compute shader");
        throwIfFailed(D3DCompile(computeOcclusionShaderSource,
                                 std::strlen(computeOcclusionShaderSource),
                                 "InlineComputeOcclusionShader",
                                 nullptr,
                                 nullptr,
                                 "OcclusionCullCS",
                                 "cs_5_0",
                                 compileFlags,
                                 0,
                                 &computeOcclusionShader,
                                 &shaderErrors),
                      "Failed to compile occlusion compute shader");
        throwIfFailed(D3DCompile(computeMainBuildShaderSource,
                                 std::strlen(computeMainBuildShaderSource),
                                 "InlineComputeMainBuildShader",
                                 nullptr,
                                 nullptr,
                                 "BuildMainIndirectCS",
                                 "cs_5_0",
                                 compileFlags,
                                 0,
                                 &computeMainBuildShader,
                                 &shaderErrors),
                      "Failed to compile indirect-main build compute shader");

        const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
        pipelineDesc.pRootSignature = rootSignature.Get();
        pipelineDesc.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
        pipelineDesc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
        pipelineDesc.BlendState = makeDefaultBlendDesc();
        pipelineDesc.SampleMask = UINT_MAX;
        pipelineDesc.RasterizerState = makeDefaultRasterizerDesc();
        pipelineDesc.DepthStencilState = makeDefaultDepthStencilDesc();
        pipelineDesc.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
        pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipelineDesc.NumRenderTargets = 1;
        pipelineDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipelineDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pipelineDesc.SampleDesc.Count = 1;

        throwIfFailed(device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&pipelineState)),
                      "Failed to create graphics pipeline state");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC wireframeDesc = pipelineDesc;
        wireframeDesc.RasterizerState = makeDefaultRasterizerDesc();
        wireframeDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        wireframeDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        throwIfFailed(device->CreateGraphicsPipelineState(&wireframeDesc, IID_PPV_ARGS(&wireframePipelineState)),
                      "Failed to create wireframe pipeline state");

        const D3D12_INPUT_ELEMENT_DESC debugInputLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC debugDesc = pipelineDesc;
        debugDesc.VS = {debugVertexShader->GetBufferPointer(), debugVertexShader->GetBufferSize()};
        debugDesc.PS = {debugPixelShader->GetBufferPointer(), debugPixelShader->GetBufferSize()};
        debugDesc.InputLayout = {debugInputLayout, static_cast<UINT>(std::size(debugInputLayout))};
        debugDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        debugDesc.RasterizerState = makeDefaultRasterizerDesc();
        debugDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        debugDesc.DepthStencilState = makeDefaultDepthStencilDesc();
        debugDesc.DepthStencilState.DepthEnable = FALSE;
        debugDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        throwIfFailed(device->CreateGraphicsPipelineState(&debugDesc, IID_PPV_ARGS(&debugLinePipelineState)),
                      "Failed to create debug line pipeline state");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowDesc = pipelineDesc;
        shadowDesc.VS = {shadowVertexShader->GetBufferPointer(), shadowVertexShader->GetBufferSize()};
        shadowDesc.PS = {nullptr, 0};
        shadowDesc.BlendState = makeDefaultBlendDesc();
        shadowDesc.NumRenderTargets = 0;
        shadowDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        shadowDesc.DepthStencilState = makeDefaultDepthStencilDesc();
        shadowDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        shadowDesc.RasterizerState = makeDefaultRasterizerDesc();
        shadowDesc.RasterizerState.DepthBias = 1500;
        shadowDesc.RasterizerState.SlopeScaledDepthBias = 2.0F;
        shadowDesc.RasterizerState.DepthBiasClamp = 0.0F;

        throwIfFailed(device->CreateGraphicsPipelineState(&shadowDesc, IID_PPV_ARGS(&shadowPipelineState)),
                      "Failed to create shadow pipeline state");

        D3D12_INDIRECT_ARGUMENT_DESC mainDrawArgumentDescs[5]{};
        mainDrawArgumentDescs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
        mainDrawArgumentDescs[0].VertexBuffer.Slot = 0;
        mainDrawArgumentDescs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
        mainDrawArgumentDescs[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
        mainDrawArgumentDescs[2].ConstantBufferView.RootParameterIndex = 0;
        mainDrawArgumentDescs[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        mainDrawArgumentDescs[3].Constant.RootParameterIndex = 3;
        mainDrawArgumentDescs[3].Constant.DestOffsetIn32BitValues = 0;
        mainDrawArgumentDescs[3].Constant.Num32BitValuesToSet = 1;
        mainDrawArgumentDescs[4].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

        D3D12_COMMAND_SIGNATURE_DESC mainDrawSignatureDesc{};
        mainDrawSignatureDesc.ByteStride = sizeof(GpuMainIndirectCommand);
        mainDrawSignatureDesc.NumArgumentDescs = static_cast<UINT>(std::size(mainDrawArgumentDescs));
        mainDrawSignatureDesc.pArgumentDescs = mainDrawArgumentDescs;
        mainDrawSignatureDesc.NodeMask = 0;
        throwIfFailed(device->CreateCommandSignature(&mainDrawSignatureDesc,
                                                     rootSignature.Get(),
                                                     IID_PPV_ARGS(&mainDrawCommandSignature)),
                      "Failed to create main draw command signature");

        D3D12_ROOT_PARAMETER computeRootParameters[5]{};
        computeRootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        computeRootParameters[0].Constants.ShaderRegister = 0;
        computeRootParameters[0].Constants.RegisterSpace = 0;
        computeRootParameters[0].Constants.Num32BitValues = sizeof(GpuFrustumCullParams) / 4;
        computeRootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeRootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        computeRootParameters[1].Descriptor.ShaderRegister = 0;
        computeRootParameters[1].Descriptor.RegisterSpace = 0;
        computeRootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeRootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        computeRootParameters[2].Descriptor.ShaderRegister = 0;
        computeRootParameters[2].Descriptor.RegisterSpace = 0;
        computeRootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeRootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        computeRootParameters[3].Descriptor.ShaderRegister = 1;
        computeRootParameters[3].Descriptor.RegisterSpace = 0;
        computeRootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeRootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        computeRootParameters[4].Descriptor.ShaderRegister = 2;
        computeRootParameters[4].Descriptor.RegisterSpace = 0;
        computeRootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC computeRootSignatureDesc{};
        computeRootSignatureDesc.NumParameters = static_cast<UINT>(std::size(computeRootParameters));
        computeRootSignatureDesc.pParameters = computeRootParameters;
        computeRootSignatureDesc.NumStaticSamplers = 0;
        computeRootSignatureDesc.pStaticSamplers = nullptr;
        computeRootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> serializedComputeRootSignature;
        ComPtr<ID3DBlob> computeRootSignatureErrors;
        throwIfFailed(D3D12SerializeRootSignature(&computeRootSignatureDesc,
                                                  D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &serializedComputeRootSignature,
                                                  &computeRootSignatureErrors),
                      "Failed to serialize compute root signature");
        throwIfFailed(device->CreateRootSignature(0,
                                                  serializedComputeRootSignature->GetBufferPointer(),
                                                  serializedComputeRootSignature->GetBufferSize(),
                                                  IID_PPV_ARGS(&computeCullRootSignature)),
                      "Failed to create compute root signature");

        D3D12_COMPUTE_PIPELINE_STATE_DESC computeCullDesc{};
        computeCullDesc.pRootSignature = computeCullRootSignature.Get();
        computeCullDesc.CS = {computeCullShader->GetBufferPointer(), computeCullShader->GetBufferSize()};
        throwIfFailed(device->CreateComputePipelineState(&computeCullDesc, IID_PPV_ARGS(&computeCullPipelineState)),
                      "Failed to create frustum compute pipeline state");

        D3D12_ROOT_PARAMETER computeOcclusionRootParameters[7]{};
        computeOcclusionRootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        computeOcclusionRootParameters[0].Constants.ShaderRegister = 0;
        computeOcclusionRootParameters[0].Constants.RegisterSpace = 0;
        computeOcclusionRootParameters[0].Constants.Num32BitValues = sizeof(GpuOcclusionCullParams) / 4;
        computeOcclusionRootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeOcclusionRootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        computeOcclusionRootParameters[1].Descriptor.ShaderRegister = 0;
        computeOcclusionRootParameters[1].Descriptor.RegisterSpace = 0;
        computeOcclusionRootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeOcclusionRootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        computeOcclusionRootParameters[2].Descriptor.ShaderRegister = 1;
        computeOcclusionRootParameters[2].Descriptor.RegisterSpace = 0;
        computeOcclusionRootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeOcclusionRootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        computeOcclusionRootParameters[3].Descriptor.ShaderRegister = 2;
        computeOcclusionRootParameters[3].Descriptor.RegisterSpace = 0;
        computeOcclusionRootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeOcclusionRootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        computeOcclusionRootParameters[4].Descriptor.ShaderRegister = 0;
        computeOcclusionRootParameters[4].Descriptor.RegisterSpace = 0;
        computeOcclusionRootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeOcclusionRootParameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        computeOcclusionRootParameters[5].Descriptor.ShaderRegister = 1;
        computeOcclusionRootParameters[5].Descriptor.RegisterSpace = 0;
        computeOcclusionRootParameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeOcclusionRootParameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        computeOcclusionRootParameters[6].Descriptor.ShaderRegister = 2;
        computeOcclusionRootParameters[6].Descriptor.RegisterSpace = 0;
        computeOcclusionRootParameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC computeOcclusionRootSignatureDesc{};
        computeOcclusionRootSignatureDesc.NumParameters = static_cast<UINT>(std::size(computeOcclusionRootParameters));
        computeOcclusionRootSignatureDesc.pParameters = computeOcclusionRootParameters;
        computeOcclusionRootSignatureDesc.NumStaticSamplers = 0;
        computeOcclusionRootSignatureDesc.pStaticSamplers = nullptr;
        computeOcclusionRootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> serializedComputeOcclusionRootSignature;
        ComPtr<ID3DBlob> computeOcclusionRootSignatureErrors;
        throwIfFailed(D3D12SerializeRootSignature(&computeOcclusionRootSignatureDesc,
                                                  D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &serializedComputeOcclusionRootSignature,
                                                  &computeOcclusionRootSignatureErrors),
                      "Failed to serialize occlusion compute root signature");
        throwIfFailed(device->CreateRootSignature(0,
                                                  serializedComputeOcclusionRootSignature->GetBufferPointer(),
                                                  serializedComputeOcclusionRootSignature->GetBufferSize(),
                                                  IID_PPV_ARGS(&computeOcclusionRootSignature)),
                      "Failed to create occlusion compute root signature");

        D3D12_COMPUTE_PIPELINE_STATE_DESC computeOcclusionDesc{};
        computeOcclusionDesc.pRootSignature = computeOcclusionRootSignature.Get();
        computeOcclusionDesc.CS = {computeOcclusionShader->GetBufferPointer(), computeOcclusionShader->GetBufferSize()};
        throwIfFailed(device->CreateComputePipelineState(&computeOcclusionDesc, IID_PPV_ARGS(&computeOcclusionPipelineState)),
                      "Failed to create occlusion compute pipeline state");

        D3D12_ROOT_PARAMETER computeMainBuildRootParameters[5]{};
        computeMainBuildRootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        computeMainBuildRootParameters[0].Constants.ShaderRegister = 0;
        computeMainBuildRootParameters[0].Constants.RegisterSpace = 0;
        computeMainBuildRootParameters[0].Constants.Num32BitValues = 4;
        computeMainBuildRootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeMainBuildRootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        computeMainBuildRootParameters[1].Descriptor.ShaderRegister = 0;
        computeMainBuildRootParameters[1].Descriptor.RegisterSpace = 0;
        computeMainBuildRootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeMainBuildRootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        computeMainBuildRootParameters[2].Descriptor.ShaderRegister = 1;
        computeMainBuildRootParameters[2].Descriptor.RegisterSpace = 0;
        computeMainBuildRootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeMainBuildRootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        computeMainBuildRootParameters[3].Descriptor.ShaderRegister = 0;
        computeMainBuildRootParameters[3].Descriptor.RegisterSpace = 0;
        computeMainBuildRootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeMainBuildRootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        computeMainBuildRootParameters[4].Descriptor.ShaderRegister = 1;
        computeMainBuildRootParameters[4].Descriptor.RegisterSpace = 0;
        computeMainBuildRootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC computeMainBuildRootSignatureDesc{};
        computeMainBuildRootSignatureDesc.NumParameters = static_cast<UINT>(std::size(computeMainBuildRootParameters));
        computeMainBuildRootSignatureDesc.pParameters = computeMainBuildRootParameters;
        computeMainBuildRootSignatureDesc.NumStaticSamplers = 0;
        computeMainBuildRootSignatureDesc.pStaticSamplers = nullptr;
        computeMainBuildRootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> serializedComputeMainBuildRootSignature;
        ComPtr<ID3DBlob> computeMainBuildRootSignatureErrors;
        throwIfFailed(D3D12SerializeRootSignature(&computeMainBuildRootSignatureDesc,
                                                  D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &serializedComputeMainBuildRootSignature,
                                                  &computeMainBuildRootSignatureErrors),
                      "Failed to serialize indirect-main build compute root signature");
        throwIfFailed(device->CreateRootSignature(0,
                                                  serializedComputeMainBuildRootSignature->GetBufferPointer(),
                                                  serializedComputeMainBuildRootSignature->GetBufferSize(),
                                                  IID_PPV_ARGS(&computeMainBuildRootSignature)),
                      "Failed to create indirect-main build compute root signature");

        D3D12_COMPUTE_PIPELINE_STATE_DESC computeMainBuildDesc{};
        computeMainBuildDesc.pRootSignature = computeMainBuildRootSignature.Get();
        computeMainBuildDesc.CS = {computeMainBuildShader->GetBufferPointer(), computeMainBuildShader->GetBufferSize()};
        throwIfFailed(device->CreateComputePipelineState(&computeMainBuildDesc, IID_PPV_ARGS(&computeMainBuildPipelineState)),
                      "Failed to create indirect-main build compute pipeline state");

        for (std::uint32_t i = 0; i < kFrameCount; ++i) {
            throwIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators[i])),
                          "Failed to create command allocator");
            throwIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&computeCommandAllocators[i])),
                          "Failed to create compute command allocator");
        }

        throwIfFailed(device->CreateCommandList(0,
                                                D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                commandAllocators[frameIndex].Get(),
                                                pipelineState.Get(),
                                                IID_PPV_ARGS(&commandList)),
                      "Failed to create command list");
        throwIfFailed(commandList->Close(), "Failed to close initial command list");

        throwIfFailed(device->CreateCommandList(0,
                                                D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                computeCommandAllocators[frameIndex].Get(),
                                                computeCullPipelineState.Get(),
                                                IID_PPV_ARGS(&computeCommandList)),
                      "Failed to create compute command list");
        throwIfFailed(computeCommandList->Close(), "Failed to close initial compute command list");
    }



}  // namespace engine::render

#endif  // _WIN32

