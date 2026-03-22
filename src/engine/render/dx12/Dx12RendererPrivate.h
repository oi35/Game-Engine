// File: Dx12RendererPrivate.h
// Purpose: Defines DX12 renderer internals, GPU resource state, and shared helper routines.

#pragma once

#ifdef _WIN32

#include "engine/render/Dx12Renderer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <variant>

namespace engine::render {

using Microsoft::WRL::ComPtr;
using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;
using DirectX::XMFLOAT4X4;
using DirectX::XMMATRIX;
using DirectX::XMStoreFloat4x4;
using DirectX::XMVectorSet;

// Global renderer capacity and tuning constants.
constexpr std::uint32_t kFrameCount = 2;
constexpr std::size_t kMaxDrawsPerFrame = 1024;
constexpr std::size_t kMaxConstantEntries = kMaxDrawsPerFrame + 1;
constexpr std::size_t kConstantBufferStride = 256;
constexpr std::size_t kMaxTextures = 128;
constexpr std::uint32_t kShadowMapResolution = 2048;
constexpr std::size_t kMaxDebugLineVertices = 65536;
constexpr std::uint32_t kGpuTimestampSlotsPerFrame = 8;
constexpr std::size_t kGpuTimingHistoryWindow = 120;
constexpr std::uint32_t kComputeCullThreadGroupSize = 64;
constexpr int kOcclusionTilesX = 48;
constexpr int kOcclusionTilesY = 27;
constexpr std::size_t kHzbMaxLevels = 8;
constexpr int kBudgetJsonSchemaVersionCurrent = 1;
constexpr std::uint32_t kBudgetAutoSwitchHoldFrames = 45;
constexpr wchar_t kWindowClassName[] = L"GameMvpDx12WindowClass";

// Main mesh vertex layout used by the primary graphics pipeline.
struct Vertex {
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT3 color;
    XMFLOAT2 uv;
};

// Debug line vertex layout used by collision/HUD line rendering.
struct DebugVertex {
    XMFLOAT3 position;
    XMFLOAT3 color;
};

// Per-draw constant payload shared by graphics shaders.
struct ObjectConstants {
    XMFLOAT4X4 mvp;
    XMFLOAT4X4 model;
    XMFLOAT4X4 lightViewProjection;
    XMFLOAT4 baseColor;
    XMFLOAT4 lightDirectionAndShadowBias;
    XMFLOAT4 debugOptions;
};

// Timestamp slots resolved each frame for pass-level GPU profiling.
enum GpuTimestampSlot : std::uint32_t {
    kTsFrameStart = 0,
    kTsShadowStart,
    kTsShadowEnd,
    kTsMainStart,
    kTsMainEnd,
    kTsDebugStart,
    kTsDebugEnd,
    kTsFrameEnd,
};

// Aggregated GPU timings in milliseconds.
struct GpuTimingStats {
    double shadowMs = 0.0;
    double mainMs = 0.0;
    double debugMs = 0.0;
    double totalMs = 0.0;
    bool valid = false;
};

// Approximate GPU memory usage grouped by heap type.
struct GpuMemoryStats {
    std::uint64_t defaultBytes = 0;
    std::uint64_t uploadBytes = 0;
    std::uint64_t readbackBytes = 0;
    std::uint64_t otherBytes = 0;
    std::uint32_t defaultResourceCount = 0;
    std::uint32_t uploadResourceCount = 0;
    std::uint32_t readbackResourceCount = 0;
    std::uint32_t otherResourceCount = 0;
};

// Per-frame draw/culling counters for telemetry and budget control.
struct PassDrawStats {
    std::uint32_t shadowDrawCalls = 0;
    std::uint64_t shadowTriangles = 0;
    std::uint32_t mainDrawCalls = 0;
    std::uint64_t mainTriangles = 0;
    std::uint32_t debugDrawCalls = 0;
    std::uint64_t debugLines = 0;
    std::uint32_t cullInputCount = 0;
    std::uint32_t cullVisibleCount = 0;
    std::uint32_t cullRejectedCount = 0;
    std::uint32_t cullFrustumRejectedCount = 0;
    std::uint32_t cullOcclusionRejectedCount = 0;
    std::uint32_t cullFrustumDispatchGroups = 0;
    std::uint32_t cullOcclusionDispatchGroups = 0;
    std::uint32_t hzbBuildDispatchGroups = 0;
    std::uint32_t hzbLevelsBuilt = 0;
    std::uint32_t hzbCellsTested = 0;
    std::uint32_t hzbRejectedCount = 0;
};

// GPU-friendly culling sphere payload.
struct GpuCullSphere {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float radius = -1.0F;
};

// Constant parameters consumed by frustum culling compute shader.
struct GpuFrustumCullParams {
    XMFLOAT4 cameraPositionNear{0.0F, 0.0F, 0.0F, 0.1F};
    XMFLOAT4 cameraForwardFar{0.0F, 0.0F, 1.0F, 100.0F};
    XMFLOAT4 cameraRightAspect{1.0F, 0.0F, 0.0F, 1.0F};
    XMFLOAT4 cameraUpTanHalfFovY{0.0F, 1.0F, 0.0F, 0.57735026F};
    std::uint32_t drawCount = 0;
    std::uint32_t padding0 = 0;
    std::uint32_t padding1 = 0;
    std::uint32_t padding2 = 0;
};

// Constant parameters consumed by occlusion culling compute shader.
struct GpuOcclusionCullParams {
    float aspect = 1.0F;
    float tanHalfFovY = 0.57735026F;
    float maxVisibleRatio = 1.0F;
    float occlusionCoverageThreshold = 1.0F;
    float depthSlack = 0.15F;
    std::uint32_t drawCount = 0;
    std::uint32_t candidateCount = 0;
    std::uint32_t visibilityBudgetEnabled = 1;
    std::uint32_t hzbEnabled = 1;
    std::uint32_t occlusionDispatchGroupSize = 64;
    std::uint32_t padding0 = 0;
    std::uint32_t padding1 = 0;
};

// Packed indirect draw command built by compute pipeline.
struct GpuMainIndirectCommand {
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW indexBufferView{};
    D3D12_GPU_VIRTUAL_ADDRESS constantBufferAddress = 0;
    std::uint32_t textureDescriptorIndex = 0;
    D3D12_DRAW_INDEXED_ARGUMENTS drawIndexed{};
};

// Static metadata used when building indirect draw commands.
struct GpuMainDrawMetadata {
    D3D12_GPU_VIRTUAL_ADDRESS vertexBufferAddress = 0;
    std::uint32_t vertexBufferSize = 0;
    std::uint32_t vertexBufferStride = 0;
    D3D12_GPU_VIRTUAL_ADDRESS indexBufferAddress = 0;
    std::uint32_t indexBufferSize = 0;
    std::uint32_t indexBufferFormat = DXGI_FORMAT_R32_UINT;
    D3D12_GPU_VIRTUAL_ADDRESS constantBufferAddress = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t textureDescriptorIndex = 0;
};

static_assert(sizeof(GpuMainIndirectCommand) == 64, "GpuMainIndirectCommand must match HLSL layout");
static_assert(sizeof(GpuMainDrawMetadata) == 48, "GpuMainDrawMetadata must match HLSL layout");

// Which pass budget is currently selected for runtime adjustment.
enum class BudgetTarget : std::uint32_t {
    Total = 0,
    Shadow = 1,
    Main = 2,
    Debug = 3,
};

// Millisecond budgets for each major render pass.
struct PassBudgetConfig {
    double totalMs = 16.67;
    double shadowMs = 4.00;
    double mainMs = 10.00;
    double debugMs = 2.00;
};

// Named profile keyed by scene and quality labels.
struct BudgetProfile {
    std::string scene;
    std::string quality;
    PassBudgetConfig budgets{};
};

// Runtime complexity tiers used by auto budget mode.
enum class ComplexityTier : std::uint32_t {
    Low = 0,
    Medium = 1,
    High = 2,
};

// Maps a complexity tier to a (scene, quality) profile.
struct ComplexityTierBinding {
    std::string scene;
    std::string quality;
};

// Visibility budget knobs consumed by occlusion/culling logic.
struct VisibilityTierBudget {
    double maxVisibleRatio = 1.0;
    double occlusionCoverageThreshold = 1.0;
    double depthSlack = 0.15;
};

inline D3D12_RESOURCE_DESC makeBufferDesc(std::uint64_t sizeInBytes) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = sizeInBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

inline D3D12_RESOURCE_BARRIER makeTransitionBarrier(ID3D12Resource* resource,
                                             D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

inline D3D12_BLEND_DESC makeDefaultBlendDesc() {
    D3D12_BLEND_DESC desc{};
    desc.AlphaToCoverageEnable = FALSE;
    desc.IndependentBlendEnable = FALSE;

    D3D12_RENDER_TARGET_BLEND_DESC rt{};
    rt.BlendEnable = FALSE;
    rt.LogicOpEnable = FALSE;
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_ZERO;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    for (auto& item : desc.RenderTarget) {
        item = rt;
    }
    return desc;
}

inline D3D12_RASTERIZER_DESC makeDefaultRasterizerDesc() {
    D3D12_RASTERIZER_DESC desc{};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    desc.CullMode = D3D12_CULL_MODE_BACK;
    desc.FrontCounterClockwise = FALSE;
    desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.DepthClipEnable = TRUE;
    desc.MultisampleEnable = FALSE;
    desc.AntialiasedLineEnable = FALSE;
    desc.ForcedSampleCount = 0;
    desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return desc;
}

inline D3D12_DEPTH_STENCIL_DESC makeDefaultDepthStencilDesc() {
    D3D12_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = TRUE;
    desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.StencilEnable = FALSE;
    desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    desc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    desc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    desc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    desc.BackFace = desc.FrontFace;
    return desc;
}

inline void throwIfFailed(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        throw std::runtime_error(message);
    }
}


// Internal DX12 renderer state and helper methods.
struct Dx12Renderer::Impl {
    // CPU-side draw submission item captured from ECS.
    struct DrawItem {
        physics::Transform transform;
        RenderMesh renderMesh;
    };

    // GPU mesh resources and local bounds for culling.
    struct GpuMesh {
        ComPtr<ID3D12Resource> vertexBuffer;
        ComPtr<ID3D12Resource> indexBuffer;
        D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW indexBufferView{};
        UINT indexCount = 0;
        math::Vec3 localBoundsCenter{0.0F, 0.0F, 0.0F};
        float localBoundsRadius = 0.5F;
    };

    // GPU texture resource and descriptor table index.
    struct GpuTexture {
        ComPtr<ID3D12Resource> texture;
        std::uint32_t descriptorIndex = 0;
    };

    // Material state resolved at draw time.
    struct Material {
        std::uint32_t textureHandle = 0;
        math::Vec3 baseColor{1.0F, 1.0F, 1.0F};
    };

    // Debug collision primitives emitted by gameplay/physics.
    struct DebugAabbItem {
        physics::Transform transform;
        physics::BoxCollider collider;
    };

    struct DebugContactItem {
        math::Vec3 position{};
        math::Vec3 normal{};
    };

    // Upload resources retired after upload fence completion.
    struct UploadRetireBatch {
        std::uint64_t fenceValue = 0;
        std::vector<ComPtr<ID3D12Resource>> resources;
    };

    // Streaming request/ready payload types.
    struct StreamMeshRequest {
        std::uint32_t handle = 0;
        assets::MeshData mesh{};
    };

    struct StreamTextureRequest {
        std::uint32_t handle = 0;
        std::uint32_t descriptorIndex = 0;
        assets::TextureData texture{};
    };

    struct PreparedMeshUpload {
        std::uint32_t handle = 0;
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;
        math::Vec3 boundsCenter{0.0F, 0.0F, 0.0F};
        float boundsRadius = 0.5F;
    };

    struct PreparedTextureUpload {
        std::uint32_t handle = 0;
        std::uint32_t descriptorIndex = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba;
    };

    struct StreamUploadError {
        bool isMesh = true;
        std::uint32_t handle = 0;
        std::string message;
    };

    using StreamUploadRequest = std::variant<StreamMeshRequest, StreamTextureRequest>;
    using StreamUploadReadyItem = std::variant<PreparedMeshUpload, PreparedTextureUpload, StreamUploadError>;

    // Window/runtime flags.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::wstring title;
    bool vsync = true;
    bool enableValidation = false;
    bool initialized = false;
    bool closeRequested = false;
    bool resizePending = false;
    std::uint32_t pendingWidth = 0;
    std::uint32_t pendingHeight = 0;
    std::uint32_t simulationFrame = 0;
    float simulationTimeSeconds = 0.0F;

    // CPU-side frame scratch buffers for culling and HZB build.
    HWND window = nullptr;
    std::vector<DrawItem> drawQueue;
    std::vector<float> cullCenterX;
    std::vector<float> cullCenterY;
    std::vector<float> cullCenterZ;
    std::vector<float> cullRadius;
    std::vector<std::size_t> visibleDrawIndices;
    std::vector<std::size_t> frustumCandidatesScratch;
    std::vector<float> camSpaceXScratch;
    std::vector<float> camSpaceYScratch;
    std::vector<float> camSpaceZScratch;
    std::vector<GpuCullSphere> frustumSpheresScratch;
    std::vector<std::size_t> occlusionVisibleCandidatesScratch;
    std::array<std::vector<float>, kHzbMaxLevels> hzbLevelsScratch{};

    // Core DX12 objects.
    ComPtr<ID3D12Debug> debugController;
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<IDXGISwapChain3> swapChain;

    // Graphics/compute resources and descriptor heaps.
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    std::array<ComPtr<ID3D12Resource>, kFrameCount> renderTargets;
    ComPtr<ID3D12Resource> depthStencil;
    ComPtr<ID3D12Resource> shadowMap;

    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> commandAllocators;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    // Upload batching and async streaming state.
    ComPtr<ID3D12CommandAllocator> uploadCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> uploadCommandList;
    ComPtr<ID3D12Fence> uploadFence;
    std::uint64_t nextUploadFenceValue = 1;
    bool uploadRecording = false;
    std::vector<ComPtr<ID3D12Resource>> pendingUploadBuffers;
    std::vector<UploadRetireBatch> uploadRetireBatches;

    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12PipelineState> wireframePipelineState;
    ComPtr<ID3D12PipelineState> debugLinePipelineState;
    ComPtr<ID3D12PipelineState> shadowPipelineState;
    ComPtr<ID3D12CommandSignature> mainDrawCommandSignature;
    ComPtr<ID3D12RootSignature> computeCullRootSignature;
    ComPtr<ID3D12PipelineState> computeCullPipelineState;
    ComPtr<ID3D12RootSignature> computeOcclusionRootSignature;
    ComPtr<ID3D12PipelineState> computeOcclusionPipelineState;
    ComPtr<ID3D12RootSignature> computeMainBuildRootSignature;
    ComPtr<ID3D12PipelineState> computeMainBuildPipelineState;
    ComPtr<ID3D12Resource> constantBuffer;
    ComPtr<ID3D12Resource> debugLineVertexBuffer;
    ComPtr<ID3D12Resource> mainDrawIndirectArgumentBuffer;
    ComPtr<ID3D12Resource> mainDrawIndirectCountBuffer;
    ComPtr<ID3D12Resource> mainDrawMetadataBuffer;
    ComPtr<ID3D12Resource> computeCullSphereBuffer;
    ComPtr<ID3D12Resource> computeCullVisibleBuffer;
    ComPtr<ID3D12Resource> computeCullCompactIndexBuffer;
    ComPtr<ID3D12Resource> computeCullCompactCounterBuffer;
    ComPtr<ID3D12Resource> computeCullCounterResetUploadBuffer;
    ComPtr<ID3D12Resource> computeOcclusionCamSpaceUploadBuffer;
    ComPtr<ID3D12Resource> computeOcclusionCandidateUploadBuffer;
    ComPtr<ID3D12Resource> computeOcclusionVisibleIndexBuffer;
    ComPtr<ID3D12Resource> computeOcclusionStatsBuffer;
    ComPtr<ID3D12Resource> computeOcclusionStatsResetUploadBuffer;
    std::array<ComPtr<ID3D12Resource>, kFrameCount> computeOcclusionStatsReadbackBuffers;
    ComPtr<ID3D12QueryHeap> gpuTimestampQueryHeap;
    ComPtr<ID3D12Resource> gpuTimestampReadback;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> computeCommandAllocators;
    ComPtr<ID3D12GraphicsCommandList> computeCommandList;
    std::unordered_map<std::uint32_t, GpuMesh> gpuMeshes;
    std::unordered_map<std::uint32_t, GpuTexture> gpuTextures;
    std::unordered_map<std::uint32_t, Material> materials;
    std::unordered_set<std::uint32_t> pendingMeshHandles;
    std::unordered_set<std::uint32_t> pendingTextureHandles;
    std::mutex streamUploadMutex;
    std::condition_variable streamUploadCv;
    std::thread streamUploadWorker;
    std::deque<StreamUploadRequest> streamUploadRequests;
    std::deque<StreamUploadReadyItem> streamUploadReadyItems;
    bool streamUploadStopRequested = false;
    bool streamUploadWorkerRunning = false;
    std::size_t streamUploadBatchBudgetPerFrame = 3;
    std::uint32_t defaultMeshHandle = 0;
    std::uint32_t defaultTextureHandle = 0;
    std::uint32_t defaultMaterialHandle = 0;
    std::uint32_t nextMeshHandle = 1;
    std::uint32_t nextTextureHandle = 1;
    std::uint32_t nextMaterialHandle = 1;
    std::uint32_t nextSrvIndex = 0;
    std::uint32_t shadowMapSrvIndex = 0;
    D3D12_RESOURCE_STATES shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    math::Vec3 lightDirection{0.45F, -1.0F, 0.35F};
    float shadowBias = 0.0012F;
    // Runtime render toggles and camera state.
    std::array<bool, 256> keyStates{};
    bool debugShowShadowFactor = false;
    bool debugWireframe = false;
    bool debugShowCollision = false;
    bool debugShowHud = true;
    math::Vec3 cameraPosition{0.0F, 4.0F, -10.0F};
    float cameraYawRadians = 0.0F;
    float cameraPitchRadians = 0.0F;
    float cameraAspectRatio = 1.0F;
    float cameraTanHalfFovY = 0.57735026F;
    math::Vec3 cameraForwardCached{0.0F, 0.0F, 1.0F};
    math::Vec3 cameraRightCached{1.0F, 0.0F, 0.0F};
    math::Vec3 cameraUpCached{0.0F, 1.0F, 0.0F};
    float cameraMoveSpeed = 5.5F;
    float cameraLookSpeed = 1.5F;
    float lastSimulationTimeSeconds = 0.0F;
    bool hasPreviousSimulationTime = false;
    std::chrono::steady_clock::time_point cpuFrameStart{};
    double cpuFrameAccumulatedMs = 0.0;
    std::uint32_t cpuFrameAccumulatedCount = 0;
    double cpuFrameLastMs = 0.0;
    double cpuFrameLastFps = 0.0;
    std::uint64_t gpuTimestampFrequency = 0;
    std::array<bool, kFrameCount> gpuTimestampPending{};
    GpuTimingStats gpuTimingLast{};
    GpuTimingStats gpuTimingAverage{};
    std::array<GpuTimingStats, kGpuTimingHistoryWindow> gpuTimingHistory{};
    std::size_t gpuTimingHistoryCount = 0;
    std::size_t gpuTimingHistoryCursor = 0;
    double gpuTimingShadowSum = 0.0;
    double gpuTimingMainSum = 0.0;
    double gpuTimingDebugSum = 0.0;
    double gpuTimingTotalSum = 0.0;
    GpuMemoryStats gpuMemoryStats{};
    PassDrawStats passDrawStatsLast{};
    PassBudgetConfig passBudgetConfig{};
    BudgetTarget selectedBudgetTarget = BudgetTarget::Total;
    std::vector<BudgetProfile> budgetProfiles;
    std::size_t activeBudgetProfileIndex = 0;
    bool budgetAutoModeEnabled = false;
    ComplexityTier budgetTierCurrent = ComplexityTier::Medium;
    ComplexityTier budgetTierEvaluated = ComplexityTier::Medium;
    ComplexityTier budgetTierPending = ComplexityTier::Medium;
    std::uint32_t budgetTierPendingFrames = 0;
    double budgetComplexityScore = 0.0;
    std::string budgetComplexityReason = "warming-up";
    std::size_t lastEntityCount = 0;
    std::array<ComplexityTierBinding, 3> budgetTierBindings = {{
        {"sample", "quality"},
        {"sample", "balanced"},
        {"stress", "performance"},
    }};
    bool visibilityBudgetEnabled = true;
    bool hzbOcclusionEnabled = true;
    bool computeDispatchPrototypeEnabled = true;
    std::uint32_t computeFrustumDispatchGroupSize = 64;
    std::uint32_t computeOcclusionDispatchGroupSize = 64;
    std::array<VisibilityTierBudget, 3> visibilityTierBudgets = {{
        {0.92, 0.95, 0.25},
        {0.74, 0.82, 0.12},
        {0.58, 0.68, 0.06},
    }};
    double visibilityVisibleRatio = 0.0;
    double visibilityFrustumRejectedRatio = 0.0;
    double visibilityOcclusionRejectedRatio = 0.0;
    std::string visibilityBudgetState = "warmup";
    std::string visibilityBudgetReason = "none";
    std::filesystem::path passBudgetConfigPath = std::filesystem::path("assets") / "config" / "pass_budgets.cfg";
    std::filesystem::path passBudgetJsonPath = std::filesystem::path("assets") / "config" / "pass_budgets.json";
    std::vector<DebugAabbItem> debugAabbs;
    std::vector<DebugContactItem> debugContacts;
    std::uint8_t* mappedDebugLineVertices = nullptr;
    GpuMainDrawMetadata* mappedMainDrawMetadata = nullptr;
    bool indirectMainDrawReady = false;
    GpuCullSphere* mappedComputeCullSpheres = nullptr;
    std::uint32_t* mappedComputeCullCounterReset = nullptr;
    XMFLOAT4* mappedComputeOcclusionCamSpace = nullptr;
    std::uint32_t* mappedComputeOcclusionCandidates = nullptr;
    bool gpuFrustumCullingReady = false;
    bool gpuFrustumCullFailureLogged = false;
    std::array<bool, kFrameCount> gpuFrustumResultPending{};
    std::array<std::uint32_t, kFrameCount> gpuFrustumValidSphereCount{};
    std::array<std::uint32_t, kFrameCount> gpuFrustumDispatchGroups{};
    bool gpuOcclusionCullingReady = false;
    bool gpuOcclusionCullFailureLogged = false;
    std::array<bool, kFrameCount> gpuOcclusionResultPending{};
    std::array<std::uint32_t, kFrameCount> gpuOcclusionCandidateCount{};
    std::array<std::uint32_t, kFrameCount> gpuOcclusionDispatchGroups{};
    HFONT hudFont = nullptr;

    ComPtr<ID3D12Fence> fence;
    std::array<std::uint64_t, kFrameCount> fenceValues{};
    HANDLE fenceEvent = nullptr;

    std::uint32_t frameIndex = 0;
    std::uint32_t rtvDescriptorSize = 0;
    std::uint32_t dsvDescriptorSize = 0;
    std::uint32_t srvDescriptorSize = 0;
    std::uint8_t* mappedConstantData = nullptr;
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissorRect{};
    D3D12_VIEWPORT shadowViewport{};
    D3D12_RECT shadowScissorRect{};

    bool isKeyDown(int virtualKey) const {
        if (virtualKey < 0 || virtualKey >= static_cast<int>(keyStates.size())) {
            return false;
        }
        return keyStates[static_cast<std::size_t>(virtualKey)];
    }

    static std::string trimText(const std::string& text) {
        const std::size_t begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) {
            return {};
        }
        const std::size_t end = text.find_last_not_of(" \t\r\n");
        return text.substr(begin, end - begin + 1);
    }

    static std::string toLowerAscii(std::string text) {
        for (char& ch : text) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        return text;
    }

    static bool parseBoolText(const std::string& text, bool& outValue) {
        const std::string normalized = toLowerAscii(trimText(text));
        if (normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes") {
            outValue = true;
            return true;
        }
        if (normalized == "0" || normalized == "false" || normalized == "off" || normalized == "no") {
            outValue = false;
            return true;
        }
        return false;
    }

    const char* budgetTargetName(BudgetTarget target) const {
        switch (target) {
            case BudgetTarget::Total:
                return "Total";
            case BudgetTarget::Shadow:
                return "Shadow";
            case BudgetTarget::Main:
                return "Main";
            case BudgetTarget::Debug:
                return "Debug";
            default:
                return "Unknown";
        }
    }

    static constexpr std::size_t complexityTierCount() {
        return 3;
    }

    static std::size_t complexityTierIndex(ComplexityTier tier) {
        return static_cast<std::size_t>(tier);
    }

    const char* complexityTierName(ComplexityTier tier) const {
        switch (tier) {
            case ComplexityTier::Low:
                return "LOW";
            case ComplexityTier::Medium:
                return "MEDIUM";
            case ComplexityTier::High:
                return "HIGH";
            default:
                return "UNKNOWN";
        }
    }

    ComplexityTier effectiveVisibilityTier() const {
        return budgetAutoModeEnabled ? budgetTierCurrent : budgetTierEvaluated;
    }

    const VisibilityTierBudget& visibilityBudgetForTier(ComplexityTier tier) const {
        const std::size_t tierIdx = std::min(complexityTierIndex(tier), visibilityTierBudgets.size() - 1);
        return visibilityTierBudgets[tierIdx];
    }

    void refreshVisibilityBudgetState(const PassDrawStats& drawStats, bool verbose) {
        const double drawCount = static_cast<double>(drawStats.cullInputCount);
        visibilityVisibleRatio = drawCount > 0.0 ? (static_cast<double>(drawStats.cullVisibleCount) / drawCount) : 0.0;
        visibilityFrustumRejectedRatio =
            drawCount > 0.0 ? (static_cast<double>(drawStats.cullFrustumRejectedCount) / drawCount) : 0.0;
        visibilityOcclusionRejectedRatio =
            drawCount > 0.0 ? (static_cast<double>(drawStats.cullOcclusionRejectedCount) / drawCount) : 0.0;

        if (!visibilityBudgetEnabled || drawStats.cullInputCount == 0) {
            visibilityBudgetState = visibilityBudgetEnabled ? "WARMUP" : "OFF";
            visibilityBudgetReason = visibilityBudgetEnabled ? "waiting for draws" : "disabled";
            return;
        }
        if (drawStats.cullInputCount < 12) {
            visibilityBudgetState = "WARMUP";
            visibilityBudgetReason = "insufficient visibility workload";
            return;
        }

        const ComplexityTier tier = effectiveVisibilityTier();
        const VisibilityTierBudget& budget = visibilityBudgetForTier(tier);
        const double target = std::clamp(budget.maxVisibleRatio, 0.05, 1.0);
        const double ratio = visibilityVisibleRatio;

        if (ratio <= target * 0.80) {
            visibilityBudgetState = "OK";
        } else if (ratio <= target) {
            visibilityBudgetState = "WATCH";
        } else if (ratio <= target * 1.12) {
            visibilityBudgetState = "HIGH";
        } else {
            visibilityBudgetState = "OVER";
        }

        std::ostringstream reason;
        reason << std::fixed << std::setprecision(2)
               << "tier=" << complexityTierName(tier)
               << " vis=" << ratio
               << " target<=" << target
               << " frustumReject=" << visibilityFrustumRejectedRatio
               << " occlusionReject=" << visibilityOcclusionRejectedRatio;
        visibilityBudgetReason = reason.str();

        if (verbose) {
            std::cout << "[VisibilityBudget] state=" << visibilityBudgetState
                      << " " << visibilityBudgetReason << '\n';
        }
    }

    void toggleVisibilityBudget(bool verbose) {
        visibilityBudgetEnabled = !visibilityBudgetEnabled;
        if (verbose) {
            std::cout << "[VisibilityBudget] "
                      << (visibilityBudgetEnabled ? "ENABLED" : "DISABLED")
                      << " key=V tier=" << complexityTierName(effectiveVisibilityTier())
                      << '\n';
        }
        savePassBudgetConfig(false);
    }

    void toggleHzbOcclusion(bool verbose) {
        hzbOcclusionEnabled = !hzbOcclusionEnabled;
        if (verbose) {
            std::cout << "[HZB] "
                      << (hzbOcclusionEnabled ? "ENABLED" : "DISABLED")
                      << " key=H tiles=" << kOcclusionTilesX << "x" << kOcclusionTilesY
                      << " levels<=" << kHzbMaxLevels
                      << '\n';
        }
        savePassBudgetConfig(false);
    }

    void toggleComputeDispatchPrototype(bool verbose) {
        computeDispatchPrototypeEnabled = !computeDispatchPrototypeEnabled;
        if (verbose) {
            std::cout << "[DispatchProto] "
                      << (computeDispatchPrototypeEnabled ? "ENABLED" : "DISABLED")
                      << " key=C frustumGroup=" << computeFrustumDispatchGroupSize
                      << " occlusionGroup=" << computeOcclusionDispatchGroupSize
                      << " indirectMain=" << (indirectMainDrawReady ? "yes" : "no")
                      << " frustumGpu=" << (gpuFrustumCullingReady ? "yes" : "no")
                      << " occlusionGpu=" << (gpuOcclusionCullingReady ? "yes" : "no")
                      << '\n';
        }
        savePassBudgetConfig(false);
    }

    void ensureBudgetProfiles() {
        if (!budgetProfiles.empty()) {
            return;
        }

        budgetProfiles = {
            {"sample", "performance", {14.00, 3.20, 8.40, 1.40}},
            {"sample", "balanced", {16.67, 4.00, 10.00, 2.00}},
            {"sample", "quality", {20.00, 5.00, 12.50, 2.50}},
            {"stress", "performance", {13.00, 2.80, 7.80, 1.20}},
            {"stress", "balanced", {15.50, 3.60, 9.20, 1.80}},
            {"stress", "quality", {18.50, 4.60, 11.20, 2.30}},
        };
        activeBudgetProfileIndex = 1;
        passBudgetConfig = budgetProfiles[activeBudgetProfileIndex].budgets;
    }

    static std::vector<std::string> splitByDelimiter(const std::string& value, char delimiter) {
        std::vector<std::string> parts;
        std::string current;
        for (char ch : value) {
            if (ch == delimiter) {
                parts.push_back(current);
                current.clear();
            } else {
                current.push_back(ch);
            }
        }
        parts.push_back(current);
        return parts;
    }

    static std::string escapeJsonString(const std::string& value) {
        std::string escaped;
        escaped.reserve(value.size() + 8);
        for (char ch : value) {
            switch (ch) {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '"':
                    escaped += "\\\"";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    escaped.push_back(ch);
                    break;
            }
        }
        return escaped;
    }

    static bool extractJsonStringField(const std::string& jsonObject,
                                       const std::string& fieldName,
                                       std::string& outValue) {
        const std::regex fieldRegex("\"" + fieldName + "\"\\s*:\\s*\"([^\"]*)\"");
        std::smatch match;
        if (!std::regex_search(jsonObject, match, fieldRegex) || match.size() < 2) {
            return false;
        }
        outValue = match[1].str();
        return true;
    }

    static bool extractJsonNumberField(const std::string& jsonObject,
                                       const std::string& fieldName,
                                       double& outValue) {
        const std::regex fieldRegex(
            "\"" + fieldName + "\"\\s*:\\s*([-+]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][-+]?\\d+)?)");
        std::smatch match;
        if (!std::regex_search(jsonObject, match, fieldRegex) || match.size() < 2) {
            return false;
        }
        try {
            outValue = std::stod(match[1].str());
        } catch (...) {
            return false;
        }
        return true;
    }

    static bool extractJsonIntegerField(const std::string& jsonObject,
                                        const std::string& fieldName,
                                        int& outValue) {
        double value = 0.0;
        if (!extractJsonNumberField(jsonObject, fieldName, value)) {
            return false;
        }
        const double rounded = std::round(value);
        if (std::abs(value - rounded) > 1e-6) {
            return false;
        }
        outValue = static_cast<int>(rounded);
        return true;
    }

    static bool extractJsonBoolField(const std::string& jsonObject,
                                     const std::string& fieldName,
                                     bool& outValue) {
        int integerValue = 0;
        if (extractJsonIntegerField(jsonObject, fieldName, integerValue)) {
            outValue = (integerValue != 0);
            return true;
        }

        const std::regex fieldRegex(
            "\"" + fieldName + "\"\\s*:\\s*(true|false)",
            std::regex_constants::ECMAScript | std::regex_constants::icase);
        std::smatch match;
        if (!std::regex_search(jsonObject, match, fieldRegex) || match.size() < 2) {
            return false;
        }
        const std::string normalized = toLowerAscii(match[1].str());
        outValue = (normalized == "true");
        return true;
    }

    static std::string extractJsonProfilesArray(const std::string& jsonText) {
        const std::string marker = "\"profiles\"";
        const std::size_t markerPos = jsonText.find(marker);
        if (markerPos == std::string::npos) {
            return {};
        }

        const std::size_t arrayStart = jsonText.find('[', markerPos);
        if (arrayStart == std::string::npos) {
            return {};
        }

        int depth = 0;
        for (std::size_t i = arrayStart; i < jsonText.size(); ++i) {
            if (jsonText[i] == '[') {
                depth += 1;
            } else if (jsonText[i] == ']') {
                depth -= 1;
                if (depth == 0) {
                    const std::size_t contentStart = arrayStart + 1;
                    return jsonText.substr(contentStart, i - contentStart);
                }
            }
        }
        return {};
    }

    std::size_t findBudgetProfileIndex(const std::string& scene, const std::string& quality) const {
        for (std::size_t i = 0; i < budgetProfiles.size(); ++i) {
            if (budgetProfiles[i].scene == scene && budgetProfiles[i].quality == quality) {
                return i;
            }
        }
        return static_cast<std::size_t>(-1);
    }

    std::size_t upsertBudgetProfile(const std::string& scene, const std::string& quality) {
        const std::size_t existing = findBudgetProfileIndex(scene, quality);
        if (existing != static_cast<std::size_t>(-1)) {
            return existing;
        }

        BudgetProfile profile{};
        profile.scene = scene;
        profile.quality = quality;
        profile.budgets = passBudgetConfig;
        budgetProfiles.push_back(profile);
        return budgetProfiles.size() - 1;
    }

    const BudgetProfile* activeBudgetProfile() const {
        if (budgetProfiles.empty() || activeBudgetProfileIndex >= budgetProfiles.size()) {
            return nullptr;
        }
        return &budgetProfiles[activeBudgetProfileIndex];
    }

    std::string activeBudgetProfileLabel() const {
        const BudgetProfile* profile = activeBudgetProfile();
        if (profile == nullptr) {
            return "none/none";
        }
        return profile->scene + "/" + profile->quality;
    }

    std::size_t resolveBudgetProfileIndex(const std::string& scene, const std::string& quality) const {
        const std::size_t exact = findBudgetProfileIndex(scene, quality);
        if (exact != static_cast<std::size_t>(-1)) {
            return exact;
        }

        for (std::size_t i = 0; i < budgetProfiles.size(); ++i) {
            if (budgetProfiles[i].scene == scene) {
                return i;
            }
        }
        for (std::size_t i = 0; i < budgetProfiles.size(); ++i) {
            if (budgetProfiles[i].quality == quality) {
                return i;
            }
        }
        if (!budgetProfiles.empty()) {
            return std::min(activeBudgetProfileIndex, budgetProfiles.size() - 1);
        }
        return static_cast<std::size_t>(-1);
    }

    ComplexityTierBinding defaultTierBinding(ComplexityTier tier) const {
        switch (tier) {
            case ComplexityTier::Low:
                return {"sample", "quality"};
            case ComplexityTier::Medium:
                return {"sample", "balanced"};
            case ComplexityTier::High:
                return {"stress", "performance"};
            default:
                return {"sample", "balanced"};
        }
    }

    void sanitizeTierBindings(bool verbose) {
        ensureBudgetProfiles();
        if (budgetProfiles.empty()) {
            return;
        }

        for (std::size_t tierIdx = 0; tierIdx < complexityTierCount(); ++tierIdx) {
            const ComplexityTier tier = static_cast<ComplexityTier>(tierIdx);
            ComplexityTierBinding& binding = budgetTierBindings[tierIdx];
            if (trimText(binding.scene).empty() || trimText(binding.quality).empty()) {
                binding = defaultTierBinding(tier);
            }

            std::size_t resolvedIndex = resolveBudgetProfileIndex(binding.scene, binding.quality);
            if (resolvedIndex == static_cast<std::size_t>(-1)) {
                resolvedIndex = 0;
            }

            const BudgetProfile& resolved = budgetProfiles[resolvedIndex];
            if (binding.scene != resolved.scene || binding.quality != resolved.quality) {
                if (verbose) {
                    std::cout << "[BudgetAuto] Binding " << complexityTierName(tier)
                              << " remapped from " << binding.scene << "/" << binding.quality
                              << " to " << resolved.scene << "/" << resolved.quality << '\n';
                }
                binding.scene = resolved.scene;
                binding.quality = resolved.quality;
            }
        }
    }

    std::size_t budgetProfileIndexForTier(ComplexityTier tier) const {
        if (budgetProfiles.empty()) {
            return static_cast<std::size_t>(-1);
        }
        const std::size_t tierIdx = complexityTierIndex(tier);
        if (tierIdx >= budgetTierBindings.size()) {
            return static_cast<std::size_t>(-1);
        }
        const ComplexityTierBinding& binding = budgetTierBindings[tierIdx];
        return resolveBudgetProfileIndex(binding.scene, binding.quality);
    }

    void evaluateComplexityTier(std::size_t entityCount,
                                const PassDrawStats& drawStats,
                                ComplexityTier& outTier,
                                double& outScore,
                                std::string& outReason) const {
        const auto normalize = [](double value, double low, double high) -> double {
            if (high <= low) {
                return 0.0;
            }
            const double t = (value - low) / (high - low);
            return std::clamp(t, 0.0, 1.0);
        };

        const double entityScore = normalize(static_cast<double>(entityCount), 16.0, 96.0);
        const double drawScore = normalize(static_cast<double>(drawStats.mainDrawCalls), 18.0, 180.0);
        const double triScore = normalize(static_cast<double>(drawStats.mainTriangles), 30000.0, 450000.0);
        const double visibleScore = normalize(static_cast<double>(drawStats.cullVisibleCount), 12.0, 180.0);
        const double visibleRatio =
            drawStats.cullInputCount > 0
                ? (static_cast<double>(drawStats.cullVisibleCount) / static_cast<double>(drawStats.cullInputCount))
                : 1.0;
        const double occlusionRelief =
            drawStats.cullInputCount > 0
                ? (static_cast<double>(drawStats.cullOcclusionRejectedCount) /
                   static_cast<double>(drawStats.cullInputCount))
                : 0.0;
        const double visibilityPressure = std::clamp(visibleRatio - occlusionRelief * 0.5, 0.0, 1.0);

        double weightedScore =
            entityScore * 0.23 + drawScore * 0.25 + triScore * 0.25 + visibleScore * 0.17 + visibilityPressure * 0.10;
        double weightTotal = 1.0;
        double gpuScore = 0.0;
        if (gpuTimingAverage.valid && gpuTimingHistoryCount > 0) {
            gpuScore = normalize(gpuTimingAverage.totalMs, 8.50, 20.00);
            weightedScore += gpuScore * 0.10;
            weightTotal += 0.10;
        }

        outScore = weightTotal > 0.0 ? (weightedScore / weightTotal) : 0.0;
        if (outScore >= 0.67) {
            outTier = ComplexityTier::High;
        } else if (outScore >= 0.34) {
            outTier = ComplexityTier::Medium;
        } else {
            outTier = ComplexityTier::Low;
        }

        std::ostringstream reason;
        reason << std::fixed << std::setprecision(2)
               << "entities=" << entityScore
               << " draw=" << drawScore
               << " tri=" << triScore
               << " visible=" << visibleScore
               << " visPressure=" << visibilityPressure
               << " occRelief=" << occlusionRelief
               << " gpu=" << gpuScore;
        outReason = reason.str();
    }

    bool applyTierBoundProfile(ComplexityTier tier, const char* reason, bool verbose) {
        ensureBudgetProfiles();
        sanitizeTierBindings(false);
        const std::size_t targetIndex = budgetProfileIndexForTier(tier);
        if (targetIndex == static_cast<std::size_t>(-1)) {
            return false;
        }

        const std::string previous = activeBudgetProfileLabel();
        if (targetIndex == activeBudgetProfileIndex) {
            return false;
        }

        applyBudgetProfile(targetIndex, false);
        if (verbose) {
            std::cout << "[BudgetAuto] Tier=" << complexityTierName(tier)
                      << " switched " << previous << " -> " << activeBudgetProfileLabel()
                      << " reason=" << reason << '\n';
        }
        savePassBudgetConfig(false);
        return true;
    }

    void updateAutoBudgetSelection(std::size_t entityCount,
                                   const PassDrawStats& drawStats,
                                   bool forceSwitch,
                                   bool verbose) {
        ComplexityTier evaluatedTier = ComplexityTier::Medium;
        double evaluatedScore = 0.0;
        std::string evaluatedReason;
        evaluateComplexityTier(entityCount, drawStats, evaluatedTier, evaluatedScore, evaluatedReason);
        budgetTierEvaluated = evaluatedTier;
        budgetComplexityScore = evaluatedScore;
        budgetComplexityReason = std::move(evaluatedReason);

        if (!budgetAutoModeEnabled) {
            budgetTierPending = evaluatedTier;
            budgetTierPendingFrames = 0;
            return;
        }

        if (forceSwitch) {
            budgetTierCurrent = evaluatedTier;
            budgetTierPending = evaluatedTier;
            budgetTierPendingFrames = 0;
            (void)applyTierBoundProfile(evaluatedTier, "force", verbose);
            return;
        }

        if (evaluatedTier == budgetTierCurrent) {
            budgetTierPending = evaluatedTier;
            budgetTierPendingFrames = 0;
            return;
        }

        if (budgetTierPending != evaluatedTier) {
            budgetTierPending = evaluatedTier;
            budgetTierPendingFrames = 1;
            return;
        }

        budgetTierPendingFrames += 1;
        if (budgetTierPendingFrames >= kBudgetAutoSwitchHoldFrames) {
            budgetTierCurrent = evaluatedTier;
            budgetTierPending = evaluatedTier;
            budgetTierPendingFrames = 0;
            (void)applyTierBoundProfile(evaluatedTier, "stable-threshold", verbose);
        }
    }

    void setBudgetAutoMode(bool enabled, bool verbose) {
        if (budgetAutoModeEnabled == enabled) {
            if (verbose) {
                std::cout << "[BudgetAuto] Mode unchanged: " << (budgetAutoModeEnabled ? "AUTO" : "MANUAL") << '\n';
            }
            return;
        }

        budgetAutoModeEnabled = enabled;
        budgetTierPendingFrames = 0;
        if (budgetAutoModeEnabled) {
            updateAutoBudgetSelection(lastEntityCount, passDrawStatsLast, true, verbose);
        }
        savePassBudgetConfig(false);
        if (verbose) {
            std::cout << "[BudgetAuto] Mode=" << (budgetAutoModeEnabled ? "AUTO" : "MANUAL")
                      << " tier=" << complexityTierName(budgetTierCurrent)
                      << " profile=" << activeBudgetProfileLabel()
                      << " toggleKey=B"
                      << '\n';
        }
    }

    bool validateBudgetProfiles(bool verbose) {
        ensureBudgetProfiles();

        bool hadIssues = false;
        std::unordered_map<std::string, std::size_t> seenProfiles;
        std::vector<BudgetProfile> sanitizedProfiles;
        sanitizedProfiles.reserve(budgetProfiles.size());

        for (std::size_t i = 0; i < budgetProfiles.size(); ++i) {
            BudgetProfile profile = budgetProfiles[i];
            profile.scene = trimText(profile.scene);
            profile.quality = trimText(profile.quality);
            if (profile.scene.empty() || profile.quality.empty()) {
                hadIssues = true;
                if (verbose) {
                    std::cout << "[Budget] Dropping profile with empty scene/quality at index=" << i << '\n';
                }
                continue;
            }

            auto clampMetric = [&](double& value, const char* metricName) {
                const double before = value;
                value = std::clamp(value, 0.05, 100.0);
                if (before != value) {
                    hadIssues = true;
                    if (verbose) {
                        std::cout << "[Budget] Clamped profile " << profile.scene << "/" << profile.quality
                                  << " " << metricName << " from " << before << " to " << value << '\n';
                    }
                }
            };
            clampMetric(profile.budgets.totalMs, "total_ms");
            clampMetric(profile.budgets.shadowMs, "shadow_ms");
            clampMetric(profile.budgets.mainMs, "main_ms");
            clampMetric(profile.budgets.debugMs, "debug_ms");

            const std::string uniqueKey = profile.scene + "|" + profile.quality;
            if (seenProfiles.contains(uniqueKey)) {
                hadIssues = true;
                if (verbose) {
                    std::cout << "[Budget] Dropping duplicate profile " << profile.scene << "/" << profile.quality << '\n';
                }
                continue;
            }
            seenProfiles.emplace(uniqueKey, sanitizedProfiles.size());
            sanitizedProfiles.push_back(profile);
        }

        if (sanitizedProfiles.empty()) {
            hadIssues = true;
            budgetProfiles.clear();
            ensureBudgetProfiles();
        } else {
            budgetProfiles = std::move(sanitizedProfiles);
        }

        if (activeBudgetProfileIndex >= budgetProfiles.size()) {
            activeBudgetProfileIndex = 0;
            hadIssues = true;
        }

        passBudgetConfig = budgetProfiles[activeBudgetProfileIndex].budgets;
        clampPassBudgets();
        sanitizeTierBindings(verbose);

        const std::size_t currentTierIndex = complexityTierIndex(budgetTierCurrent);
        if (currentTierIndex >= budgetTierBindings.size()) {
            budgetTierCurrent = ComplexityTier::Medium;
            hadIssues = true;
        }

        if (verbose) {
            std::cout << "[Budget] Profile validation complete count=" << budgetProfiles.size()
                      << " status=" << (hadIssues ? "adjusted" : "ok")
                      << '\n';
        }
        return !hadIssues;
    }

    void applyBudgetProfile(std::size_t index, bool verbose) {
        if (budgetProfiles.empty()) {
            ensureBudgetProfiles();
        }
        if (budgetProfiles.empty()) {
            return;
        }

        activeBudgetProfileIndex = std::min(index, budgetProfiles.size() - 1);
        passBudgetConfig = budgetProfiles[activeBudgetProfileIndex].budgets;
        clampPassBudgets();
        budgetProfiles[activeBudgetProfileIndex].budgets = passBudgetConfig;
        if (verbose) {
            logPassBudgets("Applied budget profile");
        }
    }

    void clampPassBudgets() {
        passBudgetConfig.totalMs = std::clamp(passBudgetConfig.totalMs, 0.05, 100.0);
        passBudgetConfig.shadowMs = std::clamp(passBudgetConfig.shadowMs, 0.05, 100.0);
        passBudgetConfig.mainMs = std::clamp(passBudgetConfig.mainMs, 0.05, 100.0);
        passBudgetConfig.debugMs = std::clamp(passBudgetConfig.debugMs, 0.05, 100.0);
        if (!budgetProfiles.empty() && activeBudgetProfileIndex < budgetProfiles.size()) {
            budgetProfiles[activeBudgetProfileIndex].budgets = passBudgetConfig;
        }
    }

    double& budgetRef(BudgetTarget target) {
        switch (target) {
            case BudgetTarget::Total:
                return passBudgetConfig.totalMs;
            case BudgetTarget::Shadow:
                return passBudgetConfig.shadowMs;
            case BudgetTarget::Main:
                return passBudgetConfig.mainMs;
            case BudgetTarget::Debug:
                return passBudgetConfig.debugMs;
            default:
                return passBudgetConfig.totalMs;
        }
    }

    double budgetStep(BudgetTarget target) const {
        switch (target) {
            case BudgetTarget::Total:
                return 0.25;
            case BudgetTarget::Shadow:
                return 0.10;
            case BudgetTarget::Main:
                return 0.10;
            case BudgetTarget::Debug:
                return 0.05;
            default:
                return 0.10;
        }
    }

    void logPassBudgets(const char* reason) const {
        std::cout << "[Budget] " << reason
                  << " profile=" << activeBudgetProfileLabel()
                  << " target=" << budgetTargetName(selectedBudgetTarget)
                  << " total=" << passBudgetConfig.totalMs << "ms"
                  << " shadow=" << passBudgetConfig.shadowMs << "ms"
                  << " main=" << passBudgetConfig.mainMs << "ms"
                  << " debug=" << passBudgetConfig.debugMs << "ms"
                  << " autoMode=" << (budgetAutoModeEnabled ? "on" : "off")
                  << " visibilityBudget=" << (visibilityBudgetEnabled ? "on" : "off")
                  << " hzb=" << (hzbOcclusionEnabled ? "on" : "off")
                  << " dispatchProto=" << (computeDispatchPrototypeEnabled ? "on" : "off")
                  << " indirectMainReady=" << (indirectMainDrawReady ? "on" : "off")
                  << " frustumGpuReady=" << (gpuFrustumCullingReady ? "on" : "off")
                  << " occlusionGpuReady=" << (gpuOcclusionCullingReady ? "on" : "off")
                  << " config=" << passBudgetConfigPath.string()
                  << '\n';
    }

    bool loadPassBudgetConfig(bool verbose);
    bool savePassBudgetConfig(bool verbose);
    bool exportBudgetProfilesJson(bool verbose);
    bool importBudgetProfilesJson(bool verbose);
    void cycleBudgetTarget();
    void cycleBudgetQualityProfile();
    void cycleBudgetSceneProfile();
    void adjustSelectedBudget(bool increase);

    void handleKeyEvent(WPARAM wParam, LPARAM lParam, bool isDown) {
        const std::size_t keyCode = static_cast<std::size_t>(wParam);
        if (keyCode < keyStates.size()) {
            keyStates[keyCode] = isDown;
        }

        if (!isDown) {
            return;
        }

        const bool wasAlreadyDown = (lParam & (1LL << 30)) != 0;
        if (wasAlreadyDown) {
            return;
        }

        if (wParam == VK_ESCAPE) {
            closeRequested = true;
            if (window != nullptr) {
                DestroyWindow(window);
            }
            return;
        }

        if (wParam == VK_F1) {
            debugShowShadowFactor = !debugShowShadowFactor;
            std::cout << "[Debug] Shadow factor view: " << (debugShowShadowFactor ? "ON" : "OFF") << '\n';
            return;
        }

        if (wParam == VK_F2) {
            debugWireframe = !debugWireframe;
            std::cout << "[Debug] Wireframe mode: " << (debugWireframe ? "ON" : "OFF") << '\n';
            return;
        }

        if (wParam == VK_F3) {
            debugShowCollision = !debugShowCollision;
            std::cout << "[Debug] Collision overlay: " << (debugShowCollision ? "ON" : "OFF") << '\n';
            return;
        }

        if (wParam == VK_F4) {
            debugShowHud = !debugShowHud;
            std::cout << "[Debug] HUD overlay: " << (debugShowHud ? "ON" : "OFF") << '\n';
            return;
        }

        if (wParam == 'V') {
            toggleVisibilityBudget(true);
            return;
        }

        if (wParam == 'H') {
            toggleHzbOcclusion(true);
            return;
        }

        if (wParam == 'C') {
            toggleComputeDispatchPrototype(true);
            return;
        }

        if (wParam == 'B') {
            setBudgetAutoMode(!budgetAutoModeEnabled, true);
            return;
        }

        if (wParam == VK_F5) {
            cycleBudgetTarget();
            return;
        }

        if (wParam == VK_F6) {
            adjustSelectedBudget(false);
            return;
        }

        if (wParam == VK_F7) {
            adjustSelectedBudget(true);
            return;
        }

        if (wParam == VK_F8) {
            loadPassBudgetConfig(true);
            return;
        }

        if (wParam == VK_F9) {
            if (budgetAutoModeEnabled) {
                setBudgetAutoMode(false, true);
            }
            cycleBudgetQualityProfile();
            return;
        }

        if (wParam == VK_F10) {
            if (budgetAutoModeEnabled) {
                setBudgetAutoMode(false, true);
            }
            cycleBudgetSceneProfile();
            return;
        }

        if (wParam == VK_F11) {
            exportBudgetProfilesJson(true);
            return;
        }

        if (wParam == VK_F12) {
            importBudgetProfilesJson(true);
        }
    }

    // Platform/device/pipeline setup.
    void updateCamera(float deltaTime);
    bool isSphereVisible(const math::Vec3& center,
                         float radius,
                         const math::Vec3& cameraForward,
                         const math::Vec3& cameraRight,
                         const math::Vec3& cameraUp,
                         float aspect,
                         float tanHalfFovY) const;
    void queueResize(std::uint32_t newWidth, std::uint32_t newHeight);
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void createWindow();
    void createDevice();
    void updateViewportAndScissorFromSize();
    void createSwapChainAndTargets();
    void recreateSceneDepthStencil();
    void createDepthStencil();
    void applyPendingResize();
    void createPipeline();

    // Resource registration/upload paths.
    std::uint32_t registerMeshInternal(const assets::MeshData& mesh);
    PreparedMeshUpload prepareMeshUpload(std::uint32_t handle, const assets::MeshData& mesh) const;
    bool uploadPreparedMesh(const PreparedMeshUpload& prepared);
    void createDefaultMesh();
    std::uint32_t registerTextureInternal(const assets::TextureData& textureData);
    PreparedTextureUpload prepareTextureUpload(std::uint32_t handle,
                                               std::uint32_t descriptorIndex,
                                               const assets::TextureData& textureData) const;
    bool uploadPreparedTexture(const PreparedTextureUpload& prepared);
    void createDefaultTexture();
    std::uint32_t registerMaterialInternal(const MaterialDesc& materialDesc);
    void createDefaultMaterial();
    void createConstantBuffer();
    void createDebugResources();
    void startStreamUploadWorker();
    void stopStreamUploadWorker();
    void enqueueStreamUploadRequest(StreamUploadRequest request);
    void processReadyStreamUploads(std::size_t maxUploads);
    void createIndirectMainDrawResources();
    void createComputeCullingResources();
    void createComputeOcclusionResources();

    // GPU culling dispatch paths.
    bool runGpuFrustumCull(const std::vector<GpuCullSphere>& spheres,
                           const math::Vec3& cameraForward,
                           const math::Vec3& cameraRight,
                           const math::Vec3& cameraUp,
                           std::vector<std::size_t>& outCandidates,
                           PassDrawStats& outStats);
    bool runGpuOcclusionCull(const std::vector<float>& camSpaceX,
                             const std::vector<float>& camSpaceY,
                             const std::vector<float>& camSpaceZ,
                             const std::vector<float>& worldRadius,
                             const std::vector<std::size_t>& sortedFrustumCandidates,
                             std::size_t drawCount,
                             const VisibilityTierBudget& visBudget,
                             std::vector<std::size_t>& outVisibleDrawIndices,
                             PassDrawStats& outStats);

    void createGpuProfilerResources() {
        // Allocate timestamp query + readback resources for per-pass GPU timings.
        D3D12_QUERY_HEAP_DESC queryHeapDesc{};
        queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryHeapDesc.Count = kFrameCount * kGpuTimestampSlotsPerFrame;
        queryHeapDesc.NodeMask = 0;
        throwIfFailed(device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&gpuTimestampQueryHeap)),
                      "Failed to create GPU timestamp query heap");

        const std::size_t readbackSizeBytes =
            static_cast<std::size_t>(kFrameCount) * kGpuTimestampSlotsPerFrame * sizeof(std::uint64_t);
        D3D12_HEAP_PROPERTIES readbackHeapProps{};
        readbackHeapProps.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC readbackDesc = makeBufferDesc(readbackSizeBytes);
        throwIfFailed(device->CreateCommittedResource(&readbackHeapProps,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &readbackDesc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr,
                                                      IID_PPV_ARGS(&gpuTimestampReadback)),
                      "Failed to create GPU timestamp readback buffer");

        throwIfFailed(commandQueue->GetTimestampFrequency(&gpuTimestampFrequency),
                      "Failed to get command queue timestamp frequency");
        gpuTimestampPending.fill(false);
        gpuTimingLast = {};
        resetGpuTimingHistory();
    }

    std::uint32_t timestampIndex(std::uint32_t frame, GpuTimestampSlot slot) const {
        return frame * kGpuTimestampSlotsPerFrame + static_cast<std::uint32_t>(slot);
    }

    void resetGpuTimingHistory() {
        // Reset rolling timing accumulators and history buffer.
        gpuTimingHistoryCount = 0;
        gpuTimingHistoryCursor = 0;
        gpuTimingShadowSum = 0.0;
        gpuTimingMainSum = 0.0;
        gpuTimingDebugSum = 0.0;
        gpuTimingTotalSum = 0.0;
        gpuTimingAverage = {};
        gpuTimingHistory.fill({});
    }

    void pushGpuTimingSample(const GpuTimingStats& sample) {
        // Push into fixed-size rolling window and update averages incrementally.
        if (!sample.valid) {
            return;
        }

        if (gpuTimingHistoryCount == kGpuTimingHistoryWindow) {
            const GpuTimingStats& replaced = gpuTimingHistory[gpuTimingHistoryCursor];
            gpuTimingShadowSum -= replaced.shadowMs;
            gpuTimingMainSum -= replaced.mainMs;
            gpuTimingDebugSum -= replaced.debugMs;
            gpuTimingTotalSum -= replaced.totalMs;
        } else {
            gpuTimingHistoryCount += 1;
        }

        gpuTimingHistory[gpuTimingHistoryCursor] = sample;
        gpuTimingHistoryCursor = (gpuTimingHistoryCursor + 1) % kGpuTimingHistoryWindow;

        gpuTimingShadowSum += sample.shadowMs;
        gpuTimingMainSum += sample.mainMs;
        gpuTimingDebugSum += sample.debugMs;
        gpuTimingTotalSum += sample.totalMs;

        const double invSampleCount = 1.0 / static_cast<double>(gpuTimingHistoryCount);
        gpuTimingAverage.shadowMs = gpuTimingShadowSum * invSampleCount;
        gpuTimingAverage.mainMs = gpuTimingMainSum * invSampleCount;
        gpuTimingAverage.debugMs = gpuTimingDebugSum * invSampleCount;
        gpuTimingAverage.totalMs = gpuTimingTotalSum * invSampleCount;
        gpuTimingAverage.valid = true;
    }

    void consumeGpuProfilingResults(std::uint32_t frame) {
        // Resolve one frame's timestamp query data into millisecond stats.
        if (!gpuTimestampPending[frame] || !gpuTimestampReadback || gpuTimestampFrequency == 0) {
            return;
        }

        const std::size_t readbackOffsetBytes =
            static_cast<std::size_t>(frame) * kGpuTimestampSlotsPerFrame * sizeof(std::uint64_t);
        D3D12_RANGE readRange{readbackOffsetBytes, readbackOffsetBytes + (kGpuTimestampSlotsPerFrame * sizeof(std::uint64_t))};
        std::uint64_t* mappedData = nullptr;
        throwIfFailed(gpuTimestampReadback->Map(0, &readRange, reinterpret_cast<void**>(&mappedData)),
                      "Failed to map GPU timestamp readback buffer");

        const std::uint64_t* timestamps = mappedData + (frame * kGpuTimestampSlotsPerFrame);
        const auto diffMs = [this](std::uint64_t start, std::uint64_t end) -> double {
            if (end <= start || gpuTimestampFrequency == 0) {
                return 0.0;
            }
            return (static_cast<double>(end - start) * 1000.0) / static_cast<double>(gpuTimestampFrequency);
        };

        gpuTimingLast.shadowMs = diffMs(timestamps[kTsShadowStart], timestamps[kTsShadowEnd]);
        gpuTimingLast.mainMs = diffMs(timestamps[kTsMainStart], timestamps[kTsMainEnd]);
        gpuTimingLast.debugMs = diffMs(timestamps[kTsDebugStart], timestamps[kTsDebugEnd]);
        gpuTimingLast.totalMs = diffMs(timestamps[kTsFrameStart], timestamps[kTsFrameEnd]);
        gpuTimingLast.valid = true;
        pushGpuTimingSample(gpuTimingLast);

        D3D12_RANGE writeRange{0, 0};
        gpuTimestampReadback->Unmap(0, &writeRange);
        gpuTimestampPending[frame] = false;
    }

    std::uint64_t estimateResourceAllocationSize(const D3D12_RESOURCE_DESC& resourceDesc) const {
        if (!device) {
            return 0;
        }
        const D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = device->GetResourceAllocationInfo(0, 1, &resourceDesc);
        return allocationInfo.SizeInBytes;
    }

    void accumulateResourceMemory(ID3D12Resource* resource, GpuMemoryStats& stats) const {
        if (resource == nullptr) {
            return;
        }

        D3D12_HEAP_PROPERTIES heapProps{};
        D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
        if (FAILED(resource->GetHeapProperties(&heapProps, &heapFlags))) {
            return;
        }

        const std::uint64_t sizeBytes = estimateResourceAllocationSize(resource->GetDesc());
        switch (heapProps.Type) {
            case D3D12_HEAP_TYPE_DEFAULT:
                stats.defaultBytes += sizeBytes;
                stats.defaultResourceCount += 1;
                break;
            case D3D12_HEAP_TYPE_UPLOAD:
                stats.uploadBytes += sizeBytes;
                stats.uploadResourceCount += 1;
                break;
            case D3D12_HEAP_TYPE_READBACK:
                stats.readbackBytes += sizeBytes;
                stats.readbackResourceCount += 1;
                break;
            default:
                stats.otherBytes += sizeBytes;
                stats.otherResourceCount += 1;
                break;
        }
    }

    void rebuildGpuMemoryStats() {
        // Re-scan tracked resources and refresh aggregate memory counters.
        GpuMemoryStats nextStats{};

        for (const auto& rt : renderTargets) {
            accumulateResourceMemory(rt.Get(), nextStats);
        }
        accumulateResourceMemory(depthStencil.Get(), nextStats);
        accumulateResourceMemory(shadowMap.Get(), nextStats);
        accumulateResourceMemory(constantBuffer.Get(), nextStats);
        accumulateResourceMemory(debugLineVertexBuffer.Get(), nextStats);
        accumulateResourceMemory(mainDrawIndirectArgumentBuffer.Get(), nextStats);
        accumulateResourceMemory(mainDrawIndirectCountBuffer.Get(), nextStats);
        accumulateResourceMemory(mainDrawMetadataBuffer.Get(), nextStats);
        accumulateResourceMemory(computeCullSphereBuffer.Get(), nextStats);
        accumulateResourceMemory(computeCullVisibleBuffer.Get(), nextStats);
        accumulateResourceMemory(computeCullCompactIndexBuffer.Get(), nextStats);
        accumulateResourceMemory(computeCullCompactCounterBuffer.Get(), nextStats);
        accumulateResourceMemory(computeCullCounterResetUploadBuffer.Get(), nextStats);
        accumulateResourceMemory(computeOcclusionCamSpaceUploadBuffer.Get(), nextStats);
        accumulateResourceMemory(computeOcclusionCandidateUploadBuffer.Get(), nextStats);
        accumulateResourceMemory(computeOcclusionVisibleIndexBuffer.Get(), nextStats);
        accumulateResourceMemory(computeOcclusionStatsBuffer.Get(), nextStats);
        accumulateResourceMemory(computeOcclusionStatsResetUploadBuffer.Get(), nextStats);
        for (const auto& readback : computeOcclusionStatsReadbackBuffers) {
            accumulateResourceMemory(readback.Get(), nextStats);
        }
        accumulateResourceMemory(gpuTimestampReadback.Get(), nextStats);

        for (const auto& [handle, mesh] : gpuMeshes) {
            (void)handle;
            accumulateResourceMemory(mesh.vertexBuffer.Get(), nextStats);
            accumulateResourceMemory(mesh.indexBuffer.Get(), nextStats);
        }

        for (const auto& [handle, texture] : gpuTextures) {
            (void)handle;
            accumulateResourceMemory(texture.texture.Get(), nextStats);
        }

        gpuMemoryStats = nextStats;
    }

    void createHudResources() {
        hudFont = CreateFontW(-18,
                              0,
                              0,
                              0,
                              FW_MEDIUM,
                              FALSE,
                              FALSE,
                              FALSE,
                              DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY,
                              FIXED_PITCH | FF_DONTCARE,
                              L"Consolas");
    }

    void releaseHudResources() {
        if (hudFont != nullptr) {
            DeleteObject(hudFont);
            hudFont = nullptr;
        }
    }

    void createUploadCommandResources() {
        // Dedicated upload command list/fence path to decouple uploads from frame rendering.
        throwIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&uploadCommandAllocator)),
                      "Failed to create upload command allocator");
        throwIfFailed(device->CreateCommandList(0,
                                                D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                uploadCommandAllocator.Get(),
                                                nullptr,
                                                IID_PPV_ARGS(&uploadCommandList)),
                      "Failed to create upload command list");
        throwIfFailed(uploadCommandList->Close(), "Failed to close initial upload command list");
        throwIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploadFence)),
                      "Failed to create upload synchronization fence");
        nextUploadFenceValue = 1;
        uploadRecording = false;
        pendingUploadBuffers.clear();
        uploadRetireBatches.clear();
    }

    void collectCompletedUploadBatches() {
        if (!uploadFence) {
            return;
        }

        const std::uint64_t completedValue = uploadFence->GetCompletedValue();
        for (auto it = uploadRetireBatches.begin(); it != uploadRetireBatches.end();) {
            if (it->fenceValue <= completedValue) {
                it = uploadRetireBatches.erase(it);
            } else {
                ++it;
            }
        }
    }

    void beginUploadRecording() {
        // Lazily begin a new upload command batch.
        if (!uploadCommandAllocator || !uploadCommandList) {
            throw std::runtime_error("Upload command resources not initialized");
        }
        if (uploadRecording) {
            return;
        }

        collectCompletedUploadBatches();
        throwIfFailed(uploadCommandAllocator->Reset(), "Failed to reset upload command allocator");
        throwIfFailed(uploadCommandList->Reset(uploadCommandAllocator.Get(), nullptr),
                      "Failed to reset upload command list");
        uploadRecording = true;
    }

    void enqueueUploadResource(ComPtr<ID3D12Resource> resource) {
        if (resource) {
            pendingUploadBuffers.push_back(std::move(resource));
        }
    }

    void flushUploadCommands() {
        // Submit upload batch and retire temporary upload buffers by fence value.
        if (!uploadRecording || !uploadCommandList || !commandQueue || !uploadFence) {
            return;
        }

        throwIfFailed(uploadCommandList->Close(), "Failed to close upload command list");
        ID3D12CommandList* uploadLists[] = {uploadCommandList.Get()};
        commandQueue->ExecuteCommandLists(1, uploadLists);

        const std::uint64_t fenceValue = nextUploadFenceValue++;
        throwIfFailed(commandQueue->Signal(uploadFence.Get(), fenceValue), "Failed to signal upload fence");

        UploadRetireBatch batch{};
        batch.fenceValue = fenceValue;
        batch.resources = std::move(pendingUploadBuffers);
        uploadRetireBatches.push_back(std::move(batch));
        pendingUploadBuffers.clear();
        uploadRecording = false;
        collectCompletedUploadBatches();
    }

    void waitForPendingUploads() {
        // Block until all outstanding upload batches are complete.
        flushUploadCommands();
        collectCompletedUploadBatches();
        if (!uploadFence || !fenceEvent || uploadRetireBatches.empty()) {
            return;
        }

        const std::uint64_t lastFenceValue = uploadRetireBatches.back().fenceValue;
        if (uploadFence->GetCompletedValue() < lastFenceValue) {
            throwIfFailed(uploadFence->SetEventOnCompletion(lastFenceValue, fenceEvent),
                          "Failed to set upload fence completion event");
            WaitForSingleObject(fenceEvent, INFINITE);
        }

        uploadRetireBatches.clear();
        pendingUploadBuffers.clear();
    }

    void drawHudOverlay(std::size_t entityCount);

    D3D12_CPU_DESCRIPTOR_HANDLE currentRtv() const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(frameIndex) * rtvDescriptorSize;
        return handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE sceneDsvHandle() const {
        return dsvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE shadowDsvHandle() const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += dsvDescriptorSize;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandle(std::uint32_t descriptorIndex) const {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = srvHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(descriptorIndex) * srvDescriptorSize;
        return handle;
    }

    void moveToNextFrame() {
        // Advance swapchain frame and synchronize frame-latency fence.
        const std::uint64_t currentFenceValue = fenceValues[frameIndex];
        throwIfFailed(commandQueue->Signal(fence.Get(), currentFenceValue), "Failed to signal fence");

        frameIndex = swapChain->GetCurrentBackBufferIndex();

        if (fence->GetCompletedValue() < fenceValues[frameIndex]) {
            throwIfFailed(fence->SetEventOnCompletion(fenceValues[frameIndex], fenceEvent),
                          "Failed to set fence event");
            WaitForSingleObject(fenceEvent, INFINITE);
        }

        fenceValues[frameIndex] = currentFenceValue + 1;
    }

    void waitForGpu() {
        // Global GPU idle wait used during resize/shutdown paths.
        if (!commandQueue || !fence || !fenceEvent) {
            return;
        }

        const std::uint64_t fenceValue = fenceValues[frameIndex];
        throwIfFailed(commandQueue->Signal(fence.Get(), fenceValue), "Failed to signal fence for GPU wait");
        throwIfFailed(fence->SetEventOnCompletion(fenceValue, fenceEvent), "Failed to set completion event");
        WaitForSingleObject(fenceEvent, INFINITE);
        fenceValues[frameIndex] = fenceValue + 1;
    }

    XMMATRIX computeLightViewProjection() const {
        // Directional light view-projection matrix for shadow mapping.
        const math::Vec3 normalizedLight = math::normalize(lightDirection);
        const XMMATRIX lightView = DirectX::XMMatrixLookAtLH(
            XMVectorSet(-normalizedLight.x * 30.0F, -normalizedLight.y * 30.0F, -normalizedLight.z * 30.0F, 1.0F),
            XMVectorSet(0.0F, 0.0F, 0.0F, 1.0F),
            XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F));
        const XMMATRIX lightProjection = DirectX::XMMatrixOrthographicLH(30.0F, 30.0F, 1.0F, 80.0F);
        return lightView * lightProjection;
    }

    XMMATRIX computeCameraViewMatrix() const {
        // View matrix from camera position and yaw/pitch orientation.
        const math::Vec3 forward = math::normalize({
            std::sin(cameraYawRadians) * std::cos(cameraPitchRadians),
            std::sin(cameraPitchRadians),
            std::cos(cameraYawRadians) * std::cos(cameraPitchRadians),
        });
        return DirectX::XMMatrixLookAtLH(XMVectorSet(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0F),
                                         XMVectorSet(cameraPosition.x + forward.x,
                                                     cameraPosition.y + forward.y,
                                                     cameraPosition.z + forward.z,
                                                     1.0F),
                                         XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F));
    }

    void updateDebugConstant() {
        // Fill constant slot consumed by debug line rendering.
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        const XMMATRIX view = computeCameraViewMatrix();
        const XMMATRIX projection = DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(60.0F), aspect, 0.1F, 100.0F);
        const XMMATRIX mvp = DirectX::XMMatrixTranspose(view * projection);
        const XMMATRIX identity = DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity());
        const XMMATRIX lightViewProjection = DirectX::XMMatrixTranspose(computeLightViewProjection());
        const math::Vec3 normalizedLight = math::normalize(lightDirection);

        ObjectConstants objectConstants{};
        XMStoreFloat4x4(&objectConstants.mvp, mvp);
        XMStoreFloat4x4(&objectConstants.model, identity);
        XMStoreFloat4x4(&objectConstants.lightViewProjection, lightViewProjection);
        objectConstants.baseColor = {1.0F, 1.0F, 1.0F, 1.0F};
        objectConstants.lightDirectionAndShadowBias = {
            normalizedLight.x,
            normalizedLight.y,
            normalizedLight.z,
            shadowBias,
        };
        objectConstants.debugOptions = {0.0F, 0.0F, 0.0F, 0.0F};

        const std::size_t frameOffset = static_cast<std::size_t>(frameIndex) * kMaxConstantEntries * kConstantBufferStride;
        const std::size_t drawOffset = kMaxDrawsPerFrame * kConstantBufferStride;
        const std::size_t totalOffset = frameOffset + drawOffset;

        std::memcpy(mappedConstantData + totalOffset, &objectConstants, sizeof(ObjectConstants));
        commandList->SetGraphicsRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress() + totalOffset);
    }

    void updateObjectConstant(std::size_t drawIndex, const physics::Transform& transform, const math::Vec3& baseColor) {
        // Fill per-object constants for main/shadow rendering.
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        const float rotation = simulationTimeSeconds * 0.8F + static_cast<float>(drawIndex) * 0.2F;

        const XMMATRIX model =
            DirectX::XMMatrixScaling(transform.scale.x, transform.scale.y, transform.scale.z) *
            DirectX::XMMatrixRotationY(rotation) *
            DirectX::XMMatrixTranslation(transform.position.x, transform.position.y, transform.position.z);

        const XMMATRIX view = computeCameraViewMatrix();
        const XMMATRIX projection = DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(60.0F), aspect, 0.1F, 100.0F);
        const XMMATRIX mvp = DirectX::XMMatrixTranspose(model * view * projection);
        const XMMATRIX modelTranspose = DirectX::XMMatrixTranspose(model);
        const XMMATRIX lightViewProjection = DirectX::XMMatrixTranspose(computeLightViewProjection());
        const math::Vec3 normalizedLight = math::normalize(lightDirection);

        ObjectConstants objectConstants{};
        XMStoreFloat4x4(&objectConstants.mvp, mvp);
        XMStoreFloat4x4(&objectConstants.model, modelTranspose);
        XMStoreFloat4x4(&objectConstants.lightViewProjection, lightViewProjection);
        objectConstants.baseColor = {baseColor.x, baseColor.y, baseColor.z, 1.0F};
        objectConstants.lightDirectionAndShadowBias = {
            normalizedLight.x,
            normalizedLight.y,
            normalizedLight.z,
            shadowBias,
        };
        objectConstants.debugOptions = {debugShowShadowFactor ? 1.0F : 0.0F, 0.0F, 0.0F, 0.0F};

        const std::size_t frameOffset = static_cast<std::size_t>(frameIndex) * kMaxConstantEntries * kConstantBufferStride;
        const std::size_t drawOffset = drawIndex * kConstantBufferStride;
        const std::size_t totalOffset = frameOffset + drawOffset;

        std::memcpy(mappedConstantData + totalOffset, &objectConstants, sizeof(ObjectConstants));
        commandList->SetGraphicsRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress() + totalOffset);
    }
};

}  // namespace engine::render

#endif  // _WIN32

