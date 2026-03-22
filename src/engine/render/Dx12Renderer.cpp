// File: Dx12Renderer.cpp
// Purpose: Implements frame orchestration, draw submission, and high-level DX12 render passes.

#ifdef _WIN32

#include "engine/render/dx12/Dx12RendererPrivate.h"

namespace engine::render {

Dx12Renderer::Dx12Renderer(std::uint32_t width,
                           std::uint32_t height,
                           std::wstring windowTitle,
                           bool vsync,
                           bool enableValidation)
    : impl_(std::make_unique<Impl>()) {
    impl_->width = width;
    impl_->height = height;
    impl_->title = std::move(windowTitle);
    impl_->vsync = vsync;
    impl_->enableValidation = enableValidation;
}

Dx12Renderer::~Dx12Renderer() {
    shutdown();
}

void Dx12Renderer::initialize() {
    if (impl_->initialized) {
        return;
    }

    // Create device/swapchain/pipelines and allocate persistent GPU resources.
    impl_->createWindow();
    impl_->createDevice();
    impl_->createSwapChainAndTargets();
    impl_->createDepthStencil();
    impl_->createPipeline();
    impl_->createConstantBuffer();
    impl_->createDebugResources();
    impl_->createIndirectMainDrawResources();
    impl_->createGpuProfilerResources();
    impl_->createComputeCullingResources();
    impl_->createComputeOcclusionResources();
    impl_->loadPassBudgetConfig(true);

    throwIfFailed(impl_->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl_->fence)),
                  "Failed to create synchronization fence");
    impl_->fenceValues.fill(0);
    impl_->fenceValues[impl_->frameIndex] = 1;
    impl_->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl_->fenceEvent == nullptr) {
        throw std::runtime_error("Failed to create fence event");
    }
    impl_->createUploadCommandResources();
    impl_->startStreamUploadWorker();

    // Register fallback assets so rendering remains valid while streamed content is pending.
    impl_->createDefaultMesh();
    impl_->createDefaultTexture();
    impl_->createDefaultMaterial();
    impl_->drawQueue.reserve(kMaxDrawsPerFrame);
    impl_->debugAabbs.reserve(kMaxDrawsPerFrame);
    impl_->debugContacts.reserve(kMaxDrawsPerFrame);
    impl_->cullCenterX.reserve(kMaxDrawsPerFrame);
    impl_->cullCenterY.reserve(kMaxDrawsPerFrame);
    impl_->cullCenterZ.reserve(kMaxDrawsPerFrame);
    impl_->cullRadius.reserve(kMaxDrawsPerFrame);
    impl_->visibleDrawIndices.reserve(kMaxDrawsPerFrame);
    impl_->frustumCandidatesScratch.reserve(kMaxDrawsPerFrame);
    impl_->camSpaceXScratch.reserve(kMaxDrawsPerFrame);
    impl_->camSpaceYScratch.reserve(kMaxDrawsPerFrame);
    impl_->camSpaceZScratch.reserve(kMaxDrawsPerFrame);
    impl_->frustumSpheresScratch.reserve(kMaxDrawsPerFrame);
    impl_->occlusionVisibleCandidatesScratch.reserve(kMaxDrawsPerFrame);
    for (std::size_t level = 0; level < kHzbMaxLevels; ++level) {
        const std::size_t levelWidth = static_cast<std::size_t>(std::max(1, kOcclusionTilesX >> level));
        const std::size_t levelHeight = static_cast<std::size_t>(std::max(1, kOcclusionTilesY >> level));
        impl_->hzbLevelsScratch[level].reserve(levelWidth * levelHeight);
    }
    impl_->rebuildGpuMemoryStats();
    impl_->hasPreviousSimulationTime = false;
    impl_->cpuFrameAccumulatedMs = 0.0;
    impl_->cpuFrameAccumulatedCount = 0;

    impl_->initialized = true;
    std::cout << "[Dx12Renderer] Initialized (" << impl_->width << "x" << impl_->height << ")\n";
}

void Dx12Renderer::shutdown() {
    if (!impl_ || !impl_->initialized) {
        return;
    }

    // Stop background upload first, then drain/idle GPU to safely release resources.
    impl_->stopStreamUploadWorker();
    impl_->waitForPendingUploads();
    impl_->waitForGpu();

    if (impl_->constantBuffer && impl_->mappedConstantData != nullptr) {
        impl_->constantBuffer->Unmap(0, nullptr);
        impl_->mappedConstantData = nullptr;
    }

    if (impl_->debugLineVertexBuffer && impl_->mappedDebugLineVertices != nullptr) {
        impl_->debugLineVertexBuffer->Unmap(0, nullptr);
        impl_->mappedDebugLineVertices = nullptr;
    }

    if (impl_->mainDrawMetadataBuffer && impl_->mappedMainDrawMetadata != nullptr) {
        impl_->mainDrawMetadataBuffer->Unmap(0, nullptr);
        impl_->mappedMainDrawMetadata = nullptr;
    }
    impl_->indirectMainDrawReady = false;

    if (impl_->computeCullSphereBuffer && impl_->mappedComputeCullSpheres != nullptr) {
        impl_->computeCullSphereBuffer->Unmap(0, nullptr);
        impl_->mappedComputeCullSpheres = nullptr;
    }
    if (impl_->computeCullCounterResetUploadBuffer && impl_->mappedComputeCullCounterReset != nullptr) {
        impl_->computeCullCounterResetUploadBuffer->Unmap(0, nullptr);
        impl_->mappedComputeCullCounterReset = nullptr;
    }
    impl_->gpuFrustumCullingReady = false;
    if (impl_->computeOcclusionCamSpaceUploadBuffer && impl_->mappedComputeOcclusionCamSpace != nullptr) {
        impl_->computeOcclusionCamSpaceUploadBuffer->Unmap(0, nullptr);
        impl_->mappedComputeOcclusionCamSpace = nullptr;
    }
    if (impl_->computeOcclusionCandidateUploadBuffer && impl_->mappedComputeOcclusionCandidates != nullptr) {
        impl_->computeOcclusionCandidateUploadBuffer->Unmap(0, nullptr);
        impl_->mappedComputeOcclusionCandidates = nullptr;
    }
    impl_->gpuOcclusionCullingReady = false;


    if (impl_->fenceEvent != nullptr) {
        CloseHandle(impl_->fenceEvent);
        impl_->fenceEvent = nullptr;
    }

    if (impl_->window != nullptr) {
        DestroyWindow(impl_->window);
        impl_->window = nullptr;
    }

    impl_->gpuTimingAverage = {};
    impl_->resetGpuTimingHistory();
    impl_->gpuMemoryStats = {};
    impl_->passDrawStatsLast = {};
    impl_->pendingMeshHandles.clear();
    impl_->pendingTextureHandles.clear();
    impl_->uploadRetireBatches.clear();
    impl_->pendingUploadBuffers.clear();
    impl_->uploadCommandList.Reset();
    impl_->uploadCommandAllocator.Reset();
    impl_->uploadFence.Reset();
    impl_->uploadRecording = false;
    impl_->initialized = false;
    std::cout << "[Dx12Renderer] Shutdown\n";
}

void Dx12Renderer::pumpEvents() {
    // Pump Win32 messages and apply deferred resize requests.
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (message.message == WM_QUIT) {
            impl_->closeRequested = true;
        }
    }
    impl_->applyPendingResize();
}

bool Dx12Renderer::shouldClose() const {
    return impl_->closeRequested;
}

std::uint32_t Dx12Renderer::registerMesh(const assets::MeshData& mesh) {
    if (!impl_->initialized) {
        throw std::runtime_error("Dx12Renderer::registerMesh called before initialize");
    }
    return impl_->registerMeshInternal(mesh);
}

std::uint32_t Dx12Renderer::registerTexture(const assets::TextureData& texture) {
    if (!impl_->initialized) {
        throw std::runtime_error("Dx12Renderer::registerTexture called before initialize");
    }
    return impl_->registerTextureInternal(texture);
}

std::uint32_t Dx12Renderer::registerMaterial(const MaterialDesc& material) {
    if (!impl_->initialized) {
        throw std::runtime_error("Dx12Renderer::registerMaterial called before initialize");
    }
    return impl_->registerMaterialInternal(material);
}

void Dx12Renderer::beginFrame(std::uint32_t frameIndex, float simulationTimeSeconds) {
    if (!impl_->initialized || impl_->closeRequested) {
        return;
    }

    // Begin-frame CPU prep: camera update, streamed upload commit, profiling readback.
    impl_->simulationFrame = frameIndex;
    impl_->simulationTimeSeconds = simulationTimeSeconds;
    impl_->drawQueue.clear();
    impl_->debugAabbs.clear();
    impl_->debugContacts.clear();
    impl_->cpuFrameStart = std::chrono::steady_clock::now();

    float simulationDeltaTime = 1.0F / 60.0F;
    if (impl_->hasPreviousSimulationTime) {
        simulationDeltaTime = simulationTimeSeconds - impl_->lastSimulationTimeSeconds;
    } else {
        impl_->hasPreviousSimulationTime = true;
    }
    impl_->lastSimulationTimeSeconds = simulationTimeSeconds;
    simulationDeltaTime = std::clamp(simulationDeltaTime, 0.0F, 0.1F);
    impl_->updateCamera(simulationDeltaTime);
    impl_->processReadyStreamUploads(impl_->streamUploadBatchBudgetPerFrame);
    impl_->consumeGpuProfilingResults(impl_->frameIndex);
    impl_->collectCompletedUploadBatches();
    impl_->flushUploadCommands();

    throwIfFailed(impl_->commandAllocators[impl_->frameIndex]->Reset(), "Failed to reset command allocator");
    throwIfFailed(impl_->commandList->Reset(impl_->commandAllocators[impl_->frameIndex].Get(), impl_->pipelineState.Get()),
                  "Failed to reset command list");

    // Transition back buffer into render-target state and clear attachments.
    const auto toRenderTarget = makeTransitionBarrier(impl_->renderTargets[impl_->frameIndex].Get(),
                                                      D3D12_RESOURCE_STATE_PRESENT,
                                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
    impl_->commandList->ResourceBarrier(1, &toRenderTarget);

    const auto rtvHandle = impl_->currentRtv();
    const auto dsvHandle = impl_->sceneDsvHandle();

    const float clearColor[] = {
        0.07F + 0.05F * std::sin(simulationTimeSeconds * 0.5F),
        0.10F,
        0.15F + 0.03F * std::cos(simulationTimeSeconds * 0.25F),
        1.0F,
    };

    impl_->commandList->RSSetViewports(1, &impl_->viewport);
    impl_->commandList->RSSetScissorRects(1, &impl_->scissorRect);
    impl_->commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
    impl_->commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    impl_->commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0, 0, nullptr);
}

void Dx12Renderer::drawEntity(ecs::Entity, const physics::Transform& transform, const RenderMesh& renderMesh) {
    if (!impl_->initialized || impl_->closeRequested) {
        return;
    }
    // Defer draw submission until endFrame batching/culling.
    impl_->drawQueue.push_back({transform, renderMesh});
}

void Dx12Renderer::drawDebugAabb(ecs::Entity, const physics::Transform& transform, const physics::BoxCollider& collider) {
    if (!impl_->initialized || impl_->closeRequested) {
        return;
    }
    impl_->debugAabbs.push_back({transform, collider});
}

void Dx12Renderer::drawDebugContact(const math::Vec3& position, const math::Vec3& normal) {
    if (!impl_->initialized || impl_->closeRequested) {
        return;
    }
    impl_->debugContacts.push_back({position, normal});
}

void Dx12Renderer::endFrame(std::size_t entityCount) {
    if (!impl_->initialized || impl_->closeRequested) {
        return;
    }

    PassDrawStats framePassDrawStats{};
    // Timestamp helper for GPU pass timing.
    const bool gpuProfilingEnabled = (impl_->gpuTimestampQueryHeap != nullptr && impl_->gpuTimestampReadback != nullptr);
    const auto writeTimestamp = [&](GpuTimestampSlot slot) {
        if (!gpuProfilingEnabled) {
            return;
        }
        impl_->commandList->EndQuery(
            impl_->gpuTimestampQueryHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            impl_->timestampIndex(impl_->frameIndex, slot));
    };
    auto resolveMesh = [&](std::uint32_t meshHandle) -> const Impl::GpuMesh* {
        auto meshIt = impl_->gpuMeshes.find(meshHandle);
        if (meshIt == impl_->gpuMeshes.end()) {
            meshIt = impl_->gpuMeshes.find(impl_->defaultMeshHandle);
        }
        if (meshIt == impl_->gpuMeshes.end()) {
            return nullptr;
        }
        return &meshIt->second;
    };

    auto resolveMaterial = [&](std::uint32_t materialHandle) -> const Impl::Material* {
        auto materialIt = impl_->materials.find(materialHandle);
        if (materialIt == impl_->materials.end()) {
            materialIt = impl_->materials.find(impl_->defaultMaterialHandle);
        }
        if (materialIt == impl_->materials.end()) {
            return nullptr;
        }
        return &materialIt->second;
    };

    auto resolveTexture = [&](std::uint32_t textureHandle) -> const Impl::GpuTexture* {
        auto textureIt = impl_->gpuTextures.find(textureHandle);
        if (textureIt == impl_->gpuTextures.end()) {
            textureIt = impl_->gpuTextures.find(impl_->defaultTextureHandle);
        }
        if (textureIt == impl_->gpuTextures.end()) {
            return nullptr;
        }
        return &textureIt->second;
    };

    const Impl::GpuTexture* defaultTexture = resolveTexture(impl_->defaultTextureHandle);
    if (defaultTexture == nullptr) {
        return;
    }

    // Build per-draw culling inputs.
    const std::size_t drawCount = std::min<std::size_t>(impl_->drawQueue.size(), kMaxDrawsPerFrame);
    framePassDrawStats.cullInputCount = static_cast<std::uint32_t>(drawCount);
    impl_->cullCenterX.resize(drawCount);
    impl_->cullCenterY.resize(drawCount);
    impl_->cullCenterZ.resize(drawCount);
    impl_->cullRadius.resize(drawCount);
    impl_->visibleDrawIndices.clear();
    impl_->visibleDrawIndices.reserve(drawCount);
    std::vector<std::size_t>& frustumCandidates = impl_->frustumCandidatesScratch;
    frustumCandidates.clear();
    frustumCandidates.reserve(drawCount);
    std::vector<float>& camSpaceX = impl_->camSpaceXScratch;
    std::vector<float>& camSpaceY = impl_->camSpaceYScratch;
    std::vector<float>& camSpaceZ = impl_->camSpaceZScratch;
    camSpaceX.assign(drawCount, 0.0F);
    camSpaceY.assign(drawCount, 0.0F);
    camSpaceZ.assign(drawCount, 0.0F);

    const math::Vec3& cameraForward = impl_->cameraForwardCached;
    const math::Vec3& cameraRight = impl_->cameraRightCached;
    const math::Vec3& cameraUp = impl_->cameraUpCached;
    const float cameraAspect = impl_->cameraAspectRatio;
    const float cameraTanHalfFovY = impl_->cameraTanHalfFovY;
    std::vector<GpuCullSphere>& frustumSpheres = impl_->frustumSpheresScratch;
    frustumSpheres.assign(drawCount, GpuCullSphere{});
    const bool gpuDrivenMainEligible =
        impl_->computeDispatchPrototypeEnabled &&
        impl_->indirectMainDrawReady &&
        impl_->mainDrawCommandSignature &&
        impl_->mainDrawIndirectArgumentBuffer &&
        impl_->mainDrawIndirectCountBuffer &&
        impl_->mainDrawMetadataBuffer &&
        impl_->mappedMainDrawMetadata != nullptr;
    std::uint64_t mainTrianglesAllDraws = 0;
    for (std::size_t i = 0; i < drawCount; ++i) {
        frustumSpheres[i].radius = -1.0F;
    }

    for (std::size_t i = 0; i < drawCount; ++i) {
        if (gpuDrivenMainEligible) {
            impl_->mappedMainDrawMetadata[i] = {};
        }
        const Impl::DrawItem& item = impl_->drawQueue[i];
        const Impl::GpuMesh* mesh = resolveMesh(item.renderMesh.meshHandle);
        if (mesh == nullptr) {
            continue;
        }

        const math::Vec3 scaledBoundsCenter{
            mesh->localBoundsCenter.x * item.transform.scale.x,
            mesh->localBoundsCenter.y * item.transform.scale.y,
            mesh->localBoundsCenter.z * item.transform.scale.z,
        };
        const math::Vec3 worldBoundsCenter = item.transform.position + scaledBoundsCenter;
        const float maxScale = std::max(
            std::abs(item.transform.scale.x),
            std::max(std::abs(item.transform.scale.y), std::abs(item.transform.scale.z)));
        const float worldRadius = std::max(0.05F, mesh->localBoundsRadius * maxScale);

        impl_->cullCenterX[i] = worldBoundsCenter.x;
        impl_->cullCenterY[i] = worldBoundsCenter.y;
        impl_->cullCenterZ[i] = worldBoundsCenter.z;
        impl_->cullRadius[i] = worldRadius;

        const math::Vec3 toCenter = worldBoundsCenter - impl_->cameraPosition;
        camSpaceX[i] = math::dot(toCenter, cameraRight);
        camSpaceY[i] = math::dot(toCenter, cameraUp);
        camSpaceZ[i] = math::dot(toCenter, cameraForward);

        frustumSpheres[i].x = worldBoundsCenter.x;
        frustumSpheres[i].y = worldBoundsCenter.y;
        frustumSpheres[i].z = worldBoundsCenter.z;
        frustumSpheres[i].radius = worldRadius;

        if (gpuDrivenMainEligible) {
            const Impl::Material* material = resolveMaterial(item.renderMesh.materialHandle);
            if (material == nullptr) {
                continue;
            }
            const Impl::GpuTexture* texture = resolveTexture(material->textureHandle);
            if (texture == nullptr) {
                continue;
            }

            GpuMainDrawMetadata& metadata = impl_->mappedMainDrawMetadata[i];
            metadata.vertexBufferAddress = mesh->vertexBufferView.BufferLocation;
            metadata.vertexBufferSize = mesh->vertexBufferView.SizeInBytes;
            metadata.vertexBufferStride = mesh->vertexBufferView.StrideInBytes;
            metadata.indexBufferAddress = mesh->indexBufferView.BufferLocation;
            metadata.indexBufferSize = mesh->indexBufferView.SizeInBytes;
            metadata.indexBufferFormat = static_cast<std::uint32_t>(mesh->indexBufferView.Format);
            const std::size_t frameOffset =
                static_cast<std::size_t>(impl_->frameIndex) * kMaxConstantEntries * kConstantBufferStride;
            const std::size_t drawOffset = i * kConstantBufferStride;
            metadata.constantBufferAddress = impl_->constantBuffer->GetGPUVirtualAddress() + frameOffset + drawOffset;
            metadata.indexCount = mesh->indexCount;
            metadata.textureDescriptorIndex = texture->descriptorIndex;
            mainTrianglesAllDraws += static_cast<std::uint64_t>(mesh->indexCount / 3);
        }
    }

    bool usedGpuFrustumPath = false;
    if (gpuDrivenMainEligible && drawCount > 0) {
        usedGpuFrustumPath = impl_->runGpuFrustumCull(
            frustumSpheres,
            cameraForward,
            cameraRight,
            cameraUp,
            frustumCandidates,
            framePassDrawStats);
    }

    bool cpuFrustumComputed = false;
    if (!usedGpuFrustumPath) {
        cpuFrustumComputed = true;
        const std::size_t frustumGroupSize = impl_->computeDispatchPrototypeEnabled
                                                 ? std::max<std::size_t>(1, impl_->computeFrustumDispatchGroupSize)
                                                 : std::max<std::size_t>(1, drawCount);
        for (std::size_t groupBase = 0; groupBase < drawCount; groupBase += frustumGroupSize) {
            const std::size_t groupEnd = std::min(groupBase + frustumGroupSize, drawCount);
            framePassDrawStats.cullFrustumDispatchGroups += 1;
            for (std::size_t i = groupBase; i < groupEnd; ++i) {
                if (frustumSpheres[i].radius <= 0.0F) {
                    continue;
                }
                const math::Vec3 center{
                    frustumSpheres[i].x,
                    frustumSpheres[i].y,
                    frustumSpheres[i].z,
                };
                if (impl_->isSphereVisible(center,
                                           frustumSpheres[i].radius,
                                           cameraForward,
                                           cameraRight,
                                           cameraUp,
                                           cameraAspect,
                                           cameraTanHalfFovY)) {
                    frustumCandidates.push_back(i);
                } else {
                    framePassDrawStats.cullFrustumRejectedCount += 1;
                }
            }
        }
    }

    const ComplexityTier visibilityTier = impl_->effectiveVisibilityTier();
    const VisibilityTierBudget& visBudget = impl_->visibilityBudgetForTier(visibilityTier);
    std::sort(frustumCandidates.begin(),
              frustumCandidates.end(),
              [&camSpaceZ](std::size_t lhs, std::size_t rhs) { return camSpaceZ[lhs] < camSpaceZ[rhs]; });
    bool usedGpuOcclusionPath = false;
    if (gpuDrivenMainEligible && usedGpuFrustumPath) {
        std::vector<std::size_t>& occlusionVisibleCandidates = impl_->occlusionVisibleCandidatesScratch;
        occlusionVisibleCandidates.clear();
        occlusionVisibleCandidates.reserve(drawCount);
        usedGpuOcclusionPath = impl_->runGpuOcclusionCull(camSpaceX,
                                                          camSpaceY,
                                                          camSpaceZ,
                                                          impl_->cullRadius,
                                                          frustumCandidates,
                                                          drawCount,
                                                          visBudget,
                                                          occlusionVisibleCandidates,
                                                          framePassDrawStats);
    }

    if (!usedGpuOcclusionPath && !cpuFrustumComputed) {
        cpuFrustumComputed = true;
        const std::size_t frustumGroupSize = impl_->computeDispatchPrototypeEnabled
                                                 ? std::max<std::size_t>(1, impl_->computeFrustumDispatchGroupSize)
                                                 : std::max<std::size_t>(1, drawCount);
        for (std::size_t groupBase = 0; groupBase < drawCount; groupBase += frustumGroupSize) {
            const std::size_t groupEnd = std::min(groupBase + frustumGroupSize, drawCount);
            framePassDrawStats.cullFrustumDispatchGroups += 1;
            for (std::size_t i = groupBase; i < groupEnd; ++i) {
                if (frustumSpheres[i].radius <= 0.0F) {
                    continue;
                }
                const math::Vec3 center{
                    frustumSpheres[i].x,
                    frustumSpheres[i].y,
                    frustumSpheres[i].z,
                };
                if (impl_->isSphereVisible(center,
                                           frustumSpheres[i].radius,
                                           cameraForward,
                                           cameraRight,
                                           cameraUp,
                                           cameraAspect,
                                           cameraTanHalfFovY)) {
                    frustumCandidates.push_back(i);
                } else {
                    framePassDrawStats.cullFrustumRejectedCount += 1;
                }
            }
        }
        std::sort(frustumCandidates.begin(),
                  frustumCandidates.end(),
                  [&camSpaceZ](std::size_t lhs, std::size_t rhs) { return camSpaceZ[lhs] < camSpaceZ[rhs]; });
    }

    if (!usedGpuOcclusionPath) {
        std::array<float, kOcclusionTilesX * kOcclusionTilesY> coarseDepth{};
        coarseDepth.fill(std::numeric_limits<float>::infinity());
        auto& hzbLevels = impl_->hzbLevelsScratch;
        for (auto& levelData : hzbLevels) {
            levelData.clear();
        }
        std::array<int, kHzbMaxLevels> hzbWidths{};
        std::array<int, kHzbMaxLevels> hzbHeights{};
        std::size_t hzbLevelCount = 1;
        std::uint32_t pendingHzbUpdates = 0;
        constexpr std::uint32_t kHzbRebuildInterval = 8;

        const auto rebuildHzb = [&]() {
            if (!impl_->hzbOcclusionEnabled) {
                return;
            }

            hzbWidths.fill(1);
            hzbHeights.fill(1);
            hzbWidths[0] = kOcclusionTilesX;
            hzbHeights[0] = kOcclusionTilesY;
            hzbLevels[0].assign(coarseDepth.begin(), coarseDepth.end());
            hzbLevelCount = 1;
            framePassDrawStats.hzbLevelsBuilt =
                std::max<std::uint32_t>(framePassDrawStats.hzbLevelsBuilt, static_cast<std::uint32_t>(hzbLevelCount));

            for (std::size_t level = 1; level < kHzbMaxLevels; ++level) {
                const int sourceWidth = hzbWidths[level - 1];
                const int sourceHeight = hzbHeights[level - 1];
                if (sourceWidth <= 1 && sourceHeight <= 1) {
                    break;
                }

                const int destWidth = std::max(1, (sourceWidth + 1) / 2);
                const int destHeight = std::max(1, (sourceHeight + 1) / 2);
                hzbWidths[level] = destWidth;
                hzbHeights[level] = destHeight;
                hzbLevels[level].assign(static_cast<std::size_t>(destWidth) * static_cast<std::size_t>(destHeight),
                                        std::numeric_limits<float>::infinity());

                const std::vector<float>& sourceLevel = hzbLevels[level - 1];
                std::vector<float>& destLevel = hzbLevels[level];
                for (int y = 0; y < destHeight; ++y) {
                    for (int x = 0; x < destWidth; ++x) {
                        const int sx = x * 2;
                        const int sy = y * 2;
                        float minDepth = std::numeric_limits<float>::infinity();
                        for (int oy = 0; oy < 2; ++oy) {
                            const int sampleY = std::min(sourceHeight - 1, sy + oy);
                            for (int ox = 0; ox < 2; ++ox) {
                                const int sampleX = std::min(sourceWidth - 1, sx + ox);
                                const std::size_t sampleIdx =
                                    static_cast<std::size_t>(sampleY * sourceWidth + sampleX);
                                minDepth = std::min(minDepth, sourceLevel[sampleIdx]);
                            }
                        }
                        const std::size_t destIdx = static_cast<std::size_t>(y * destWidth + x);
                        destLevel[destIdx] = minDepth;
                    }
                }

                const std::size_t cellCount = static_cast<std::size_t>(destWidth) * static_cast<std::size_t>(destHeight);
                if (impl_->computeDispatchPrototypeEnabled) {
                    const std::size_t groupSize = std::max<std::size_t>(1, impl_->computeOcclusionDispatchGroupSize);
                    framePassDrawStats.hzbBuildDispatchGroups +=
                        static_cast<std::uint32_t>((cellCount + groupSize - 1) / groupSize);
                } else {
                    framePassDrawStats.hzbBuildDispatchGroups += 1;
                }

                hzbLevelCount = level + 1;
                framePassDrawStats.hzbLevelsBuilt =
                    std::max<std::uint32_t>(framePassDrawStats.hzbLevelsBuilt, static_cast<std::uint32_t>(hzbLevelCount));
            }
            pendingHzbUpdates = 0;
        };
        rebuildHzb();

        const float aspect = cameraAspect;
        const float tanHalfFovY = cameraTanHalfFovY;
        const float depthSlack = static_cast<float>(visBudget.depthSlack);
        const std::size_t occlusionGroupSize = impl_->computeDispatchPrototypeEnabled
                                                   ? std::max<std::size_t>(1, impl_->computeOcclusionDispatchGroupSize)
                                                   : std::max<std::size_t>(1, frustumCandidates.size());
        for (std::size_t groupBase = 0; groupBase < frustumCandidates.size(); groupBase += occlusionGroupSize) {
            const std::size_t groupEnd = std::min(groupBase + occlusionGroupSize, frustumCandidates.size());
            framePassDrawStats.cullOcclusionDispatchGroups += 1;
            for (std::size_t sortedPos = groupBase; sortedPos < groupEnd; ++sortedPos) {
                if (impl_->hzbOcclusionEnabled && pendingHzbUpdates >= kHzbRebuildInterval) {
                    rebuildHzb();
                }

                const std::size_t candidate = frustumCandidates[sortedPos];
                const float z = std::max(0.1F, camSpaceZ[candidate]);
                const float invDepth = 1.0F / std::max(z, 0.1F);
                const float ndcX = camSpaceX[candidate] * (invDepth / (tanHalfFovY * aspect));
                const float ndcY = camSpaceY[candidate] * (invDepth / tanHalfFovY);
                const float radius = impl_->cullRadius[candidate];
                const float ndcRadiusY = radius * (invDepth / tanHalfFovY);
                const float ndcRadiusX = radius * (invDepth / (tanHalfFovY * aspect));
                const float minX = std::clamp(ndcX - ndcRadiusX, -1.0F, 1.0F);
                const float maxX = std::clamp(ndcX + ndcRadiusX, -1.0F, 1.0F);
                const float minY = std::clamp(ndcY - ndcRadiusY, -1.0F, 1.0F);
                const float maxY = std::clamp(ndcY + ndcRadiusY, -1.0F, 1.0F);

                const auto toTileX = [](float ndc) {
                    const float u = (ndc * 0.5F) + 0.5F;
                    return static_cast<int>(std::floor(u * static_cast<float>(kOcclusionTilesX)));
                };
                const auto toTileY = [](float ndc) {
                    const float v = ((-ndc) * 0.5F) + 0.5F;
                    return static_cast<int>(std::floor(v * static_cast<float>(kOcclusionTilesY)));
                };

                int tileMinX = std::clamp(toTileX(minX), 0, kOcclusionTilesX - 1);
                int tileMaxX = std::clamp(toTileX(maxX), 0, kOcclusionTilesX - 1);
                int tileMinY = std::clamp(toTileY(maxY), 0, kOcclusionTilesY - 1);
                int tileMaxY = std::clamp(toTileY(minY), 0, kOcclusionTilesY - 1);
                if (tileMinX > tileMaxX || tileMinY > tileMaxY) {
                    impl_->visibleDrawIndices.push_back(candidate);
                    continue;
                }

                int coveredCells = 0;
                int occludedCells = 0;
                if (impl_->hzbOcclusionEnabled && hzbLevelCount > 0) {
                    std::size_t level = 0;
                    int projectedWidth = tileMaxX - tileMinX + 1;
                    int projectedHeight = tileMaxY - tileMinY + 1;
                    while ((projectedWidth > 2 || projectedHeight > 2) && (level + 1 < hzbLevelCount)) {
                        projectedWidth = std::max(1, (projectedWidth + 1) / 2);
                        projectedHeight = std::max(1, (projectedHeight + 1) / 2);
                        level += 1;
                    }

                    const int levelWidth = hzbWidths[level];
                    const int levelHeight = hzbHeights[level];
                    const int cellMinX = std::clamp(tileMinX >> level, 0, levelWidth - 1);
                    const int cellMaxX = std::clamp(tileMaxX >> level, 0, levelWidth - 1);
                    const int cellMinY = std::clamp(tileMinY >> level, 0, levelHeight - 1);
                    const int cellMaxY = std::clamp(tileMaxY >> level, 0, levelHeight - 1);

                    const std::vector<float>& levelData = hzbLevels[level];
                    for (int cy = cellMinY; cy <= cellMaxY; ++cy) {
                        for (int cx = cellMinX; cx <= cellMaxX; ++cx) {
                            const std::size_t cellIdx = static_cast<std::size_t>(cy * levelWidth + cx);
                            coveredCells += 1;
                            framePassDrawStats.hzbCellsTested += 1;
                            if (z > levelData[cellIdx] + depthSlack) {
                                occludedCells += 1;
                            }
                        }
                    }
                } else {
                    for (int ty = tileMinY; ty <= tileMaxY; ++ty) {
                        for (int tx = tileMinX; tx <= tileMaxX; ++tx) {
                            const std::size_t tileIdx = static_cast<std::size_t>(ty * kOcclusionTilesX + tx);
                            coveredCells += 1;
                            if (z > coarseDepth[tileIdx] + depthSlack) {
                                occludedCells += 1;
                            }
                        }
                    }
                }

                const double occludedCoverage =
                    coveredCells > 0 ? (static_cast<double>(occludedCells) / static_cast<double>(coveredCells)) : 0.0;
                const double projectedVisibleRatio =
                    drawCount > 0
                        ? ((static_cast<double>(impl_->visibleDrawIndices.size() + 1U) /
                            static_cast<double>(drawCount)))
                        : 1.0;
                const bool overVisibilityBudget = projectedVisibleRatio > std::clamp(visBudget.maxVisibleRatio, 0.05, 1.0);
                const bool coverageOverThreshold =
                    occludedCoverage >= std::clamp(visBudget.occlusionCoverageThreshold, 0.0, 1.0);
                const bool occlusionCull = impl_->visibilityBudgetEnabled &&
                                           (coverageOverThreshold ||
                                            (overVisibilityBudget &&
                                             occludedCoverage >= visBudget.occlusionCoverageThreshold * 0.65));
                if (occlusionCull) {
                    framePassDrawStats.cullOcclusionRejectedCount += 1;
                    if (impl_->hzbOcclusionEnabled) {
                        framePassDrawStats.hzbRejectedCount += 1;
                    }
                    continue;
                }

                impl_->visibleDrawIndices.push_back(candidate);
                for (int ty = tileMinY; ty <= tileMaxY; ++ty) {
                    for (int tx = tileMinX; tx <= tileMaxX; ++tx) {
                        const std::size_t tileIdx = static_cast<std::size_t>(ty * kOcclusionTilesX + tx);
                        coarseDepth[tileIdx] = std::min(coarseDepth[tileIdx], z);
                    }
                }
                if (impl_->hzbOcclusionEnabled) {
                    pendingHzbUpdates += 1;
                }
            }
        }
    }

    if (!usedGpuOcclusionPath) {
        framePassDrawStats.cullVisibleCount = static_cast<std::uint32_t>(impl_->visibleDrawIndices.size());
    }
    framePassDrawStats.cullVisibleCount =
        std::min(framePassDrawStats.cullVisibleCount, framePassDrawStats.cullInputCount);
    framePassDrawStats.cullRejectedCount =
        framePassDrawStats.cullInputCount - framePassDrawStats.cullVisibleCount;
    impl_->refreshVisibilityBudgetState(framePassDrawStats, false);

    writeTimestamp(kTsFrameStart);

    if (impl_->shadowMapState != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        const auto toShadowWrite = makeTransitionBarrier(impl_->shadowMap.Get(),
                                                         impl_->shadowMapState,
                                                         D3D12_RESOURCE_STATE_DEPTH_WRITE);
        impl_->commandList->ResourceBarrier(1, &toShadowWrite);
        impl_->shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    const auto shadowDsv = impl_->shadowDsvHandle();
    impl_->commandList->RSSetViewports(1, &impl_->shadowViewport);
    impl_->commandList->RSSetScissorRects(1, &impl_->shadowScissorRect);
    impl_->commandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
    impl_->commandList->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0, 0, nullptr);
    impl_->commandList->SetPipelineState(impl_->shadowPipelineState.Get());
    impl_->commandList->SetGraphicsRootSignature(impl_->rootSignature.Get());
    impl_->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    impl_->commandList->SetGraphicsRootDescriptorTable(1, impl_->textureSrvHandle(0));
    impl_->commandList->SetGraphicsRootDescriptorTable(2, impl_->textureSrvHandle(impl_->shadowMapSrvIndex));
    impl_->commandList->SetGraphicsRoot32BitConstant(3, defaultTexture->descriptorIndex, 0);
    writeTimestamp(kTsShadowStart);

    for (std::size_t i = 0; i < drawCount; ++i) {
        const Impl::DrawItem& item = impl_->drawQueue[i];
        const Impl::GpuMesh* mesh = resolveMesh(item.renderMesh.meshHandle);
        if (mesh == nullptr) {
            continue;
        }

        impl_->commandList->IASetVertexBuffers(0, 1, &mesh->vertexBufferView);
        impl_->commandList->IASetIndexBuffer(&mesh->indexBufferView);
        impl_->updateObjectConstant(i, item.transform, {1.0F, 1.0F, 1.0F});
        impl_->commandList->DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0);
        framePassDrawStats.shadowDrawCalls += 1;
        framePassDrawStats.shadowTriangles += static_cast<std::uint64_t>(mesh->indexCount / 3);
    }

    const auto shadowToSample = makeTransitionBarrier(impl_->shadowMap.Get(),
                                                      impl_->shadowMapState,
                                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    impl_->commandList->ResourceBarrier(1, &shadowToSample);
    impl_->shadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    writeTimestamp(kTsShadowEnd);

    const auto rtvHandle = impl_->currentRtv();
    const auto sceneDsv = impl_->sceneDsvHandle();
    impl_->commandList->RSSetViewports(1, &impl_->viewport);
    impl_->commandList->RSSetScissorRects(1, &impl_->scissorRect);
    impl_->commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &sceneDsv);
    impl_->commandList->SetPipelineState((impl_->debugWireframe ? impl_->wireframePipelineState : impl_->pipelineState).Get());
    impl_->commandList->SetGraphicsRootSignature(impl_->rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = {impl_->srvHeap.Get()};
    impl_->commandList->SetDescriptorHeaps(1, heaps);
    impl_->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    impl_->commandList->SetGraphicsRootDescriptorTable(1, impl_->textureSrvHandle(0));
    impl_->commandList->SetGraphicsRootDescriptorTable(2, impl_->textureSrvHandle(impl_->shadowMapSrvIndex));
    impl_->commandList->SetGraphicsRoot32BitConstant(3, defaultTexture->descriptorIndex, 0);
    if (usedGpuOcclusionPath) {
        for (std::size_t i = 0; i < drawCount; ++i) {
            const Impl::DrawItem& item = impl_->drawQueue[i];
            const Impl::Material* material = resolveMaterial(item.renderMesh.materialHandle);
            if (material == nullptr) {
                continue;
            }
            impl_->updateObjectConstant(i, item.transform, material->baseColor);
        }
    }
    writeTimestamp(kTsMainStart);

    bool usedIndirectMainPath = false;
    if (usedGpuOcclusionPath &&
        impl_->indirectMainDrawReady &&
        impl_->mainDrawCommandSignature &&
        impl_->mainDrawIndirectArgumentBuffer &&
        impl_->mainDrawIndirectCountBuffer) {
        impl_->commandList->ExecuteIndirect(impl_->mainDrawCommandSignature.Get(),
                                            static_cast<UINT>(drawCount),
                                            impl_->mainDrawIndirectArgumentBuffer.Get(),
                                            0,
                                            impl_->mainDrawIndirectCountBuffer.Get(),
                                            0);
        std::array<D3D12_RESOURCE_BARRIER, 2> indirectBackToUav{};
        indirectBackToUav[0] = makeTransitionBarrier(impl_->mainDrawIndirectArgumentBuffer.Get(),
                                                     D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        indirectBackToUav[1] = makeTransitionBarrier(impl_->mainDrawIndirectCountBuffer.Get(),
                                                     D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        impl_->commandList->ResourceBarrier(static_cast<UINT>(indirectBackToUav.size()), indirectBackToUav.data());
        usedIndirectMainPath = true;

        if (drawCount > 0) {
            std::uint32_t estimatedVisible = framePassDrawStats.cullVisibleCount;
            if (estimatedVisible == 0) {
                estimatedVisible = static_cast<std::uint32_t>(drawCount);
            }
            framePassDrawStats.mainDrawCalls = estimatedVisible;
            const double avgTriangles =
                static_cast<double>(mainTrianglesAllDraws) / static_cast<double>(drawCount);
            framePassDrawStats.mainTriangles =
                static_cast<std::uint64_t>(avgTriangles * static_cast<double>(estimatedVisible));
        }
    }

    if (!usedIndirectMainPath) {
        // Fallback direct draw path when indirect pipeline is unavailable or disabled.
        for (std::size_t visiblePos = 0; visiblePos < impl_->visibleDrawIndices.size(); ++visiblePos) {
            const std::size_t drawIndex = impl_->visibleDrawIndices[visiblePos];
            const Impl::DrawItem& item = impl_->drawQueue[drawIndex];
            const Impl::GpuMesh* mesh = resolveMesh(item.renderMesh.meshHandle);
            if (mesh == nullptr) {
                continue;
            }
            const Impl::Material* material = resolveMaterial(item.renderMesh.materialHandle);
            if (material == nullptr) {
                continue;
            }
            const Impl::GpuTexture* texture = resolveTexture(material->textureHandle);
            if (texture == nullptr) {
                continue;
            }

            impl_->commandList->IASetVertexBuffers(0, 1, &mesh->vertexBufferView);
            impl_->commandList->IASetIndexBuffer(&mesh->indexBufferView);
            impl_->commandList->SetGraphicsRoot32BitConstant(3, texture->descriptorIndex, 0);
            impl_->updateObjectConstant(drawIndex, item.transform, material->baseColor);
            impl_->commandList->DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0);
            framePassDrawStats.mainDrawCalls += 1;
            framePassDrawStats.mainTriangles += static_cast<std::uint64_t>(mesh->indexCount / 3);
        }
    }
    writeTimestamp(kTsMainEnd);
    writeTimestamp(kTsDebugStart);

    // Collision debug and HUD share one GPU line-list pass to avoid extra overlay paths.
    const bool hasCollisionDebug =
        impl_->debugShowCollision && (!impl_->debugAabbs.empty() || !impl_->debugContacts.empty());
    const bool hasGpuHudDebug = impl_->debugShowHud;
    if (impl_->mappedDebugLineVertices != nullptr && (hasCollisionDebug || hasGpuHudDebug)) {
        std::vector<DebugVertex> debugVertices;
        const std::size_t collisionVertexBudget =
            hasCollisionDebug ? (impl_->debugAabbs.size() * 24 + impl_->debugContacts.size() * 8) : 0;
        const std::size_t hudVertexBudget = hasGpuHudDebug ? 2048 : 0;
        debugVertices.reserve(std::min<std::size_t>(
            kMaxDebugLineVertices,
            collisionVertexBudget + hudVertexBudget));
        bool debugVertexOverflow = false;

        auto pushLine = [&debugVertices](const math::Vec3& a, const math::Vec3& b, const math::Vec3& color) {
            debugVertices.push_back({{a.x, a.y, a.z}, {color.x, color.y, color.z}});
            debugVertices.push_back({{b.x, b.y, b.z}, {color.x, color.y, color.z}});
        };
        // Stop emitting once the mapped debug vertex buffer budget is exhausted.
        auto tryPushLine =
            [&debugVertices, &pushLine, &debugVertexOverflow](const math::Vec3& a,
                                                              const math::Vec3& b,
                                                              const math::Vec3& color) -> bool {
            if (debugVertexOverflow) {
                return false;
            }
            if (debugVertices.size() + 2 > kMaxDebugLineVertices) {
                debugVertexOverflow = true;
                return false;
            }
            pushLine(a, b, color);
            return true;
        };

        if (hasCollisionDebug) {
            // Existing world-space collision primitives.
            for (const auto& aabb : impl_->debugAabbs) {
                const math::Vec3 center = aabb.transform.position;
                const math::Vec3 half = aabb.collider.halfExtents;

                const std::array<math::Vec3, 8> corners = {
                    center + math::Vec3{-half.x, -half.y, -half.z},
                    center + math::Vec3{half.x, -half.y, -half.z},
                    center + math::Vec3{half.x, half.y, -half.z},
                    center + math::Vec3{-half.x, half.y, -half.z},
                    center + math::Vec3{-half.x, -half.y, half.z},
                    center + math::Vec3{half.x, -half.y, half.z},
                    center + math::Vec3{half.x, half.y, half.z},
                    center + math::Vec3{-half.x, half.y, half.z},
                };

                const std::array<std::array<int, 2>, 12> edges = {
                    std::array<int, 2>{0, 1}, std::array<int, 2>{1, 2}, std::array<int, 2>{2, 3}, std::array<int, 2>{3, 0},
                    std::array<int, 2>{4, 5}, std::array<int, 2>{5, 6}, std::array<int, 2>{6, 7}, std::array<int, 2>{7, 4},
                    std::array<int, 2>{0, 4}, std::array<int, 2>{1, 5}, std::array<int, 2>{2, 6}, std::array<int, 2>{3, 7},
                };

                const math::Vec3 color{0.2F, 1.0F, 0.2F};
                for (const auto& edge : edges) {
                    if (!tryPushLine(corners[static_cast<std::size_t>(edge[0])],
                                     corners[static_cast<std::size_t>(edge[1])],
                                     color)) {
                        break;
                    }
                }
                if (debugVertexOverflow) {
                    break;
                }
            }

            if (!debugVertexOverflow) {
                for (const auto& contact : impl_->debugContacts) {
                    if (debugVertices.size() + 8 > kMaxDebugLineVertices) {
                        debugVertexOverflow = true;
                        break;
                    }

                    const math::Vec3 tangentX{0.08F, 0.0F, 0.0F};
                    const math::Vec3 tangentY{0.0F, 0.08F, 0.0F};
                    const math::Vec3 tangentZ{0.0F, 0.0F, 0.08F};
                    const math::Vec3 red{1.0F, 0.2F, 0.2F};
                    const math::Vec3 yellow{1.0F, 0.85F, 0.2F};

                    tryPushLine(contact.position - tangentX, contact.position + tangentX, red);
                    tryPushLine(contact.position - tangentY, contact.position + tangentY, red);
                    tryPushLine(contact.position - tangentZ, contact.position + tangentZ, red);

                    const math::Vec3 normalDir = math::normalize(contact.normal);
                    tryPushLine(contact.position, contact.position + normalDir * 0.35F, yellow);
                    if (debugVertexOverflow) {
                        break;
                    }
                }
            }
        }

        if (hasGpuHudDebug && !debugVertexOverflow) {
            // Build HUD in normalized [0..1] UI coordinates on a camera-facing plane.
            const float aspect = std::max(0.1F, impl_->cameraAspectRatio);
            const float tanHalfFovY = std::max(0.05F, impl_->cameraTanHalfFovY);
            const float hudDepth = 1.2F;
            const float hudHalfHeight = hudDepth * tanHalfFovY;
            const float hudHalfWidth = hudHalfHeight * aspect;
            const math::Vec3 hudCenter = impl_->cameraPosition + impl_->cameraForwardCached * hudDepth;
            auto hudPoint = [&](float normalizedX, float normalizedY) -> math::Vec3 {
                const float ndcX = normalizedX * 2.0F - 1.0F;
                const float ndcY = 1.0F - normalizedY * 2.0F;
                return hudCenter + impl_->cameraRightCached * (ndcX * hudHalfWidth) +
                       impl_->cameraUpCached * (ndcY * hudHalfHeight);
            };
            auto pushHudLine = [&](float x0, float y0, float x1, float y1, const math::Vec3& color) -> bool {
                return tryPushLine(hudPoint(x0, y0), hudPoint(x1, y1), color);
            };
            auto pushHudRect = [&](float left, float top, float right, float bottom, const math::Vec3& color) -> bool {
                return pushHudLine(left, top, right, top, color) &&
                       pushHudLine(right, top, right, bottom, color) &&
                       pushHudLine(right, bottom, left, bottom, color) &&
                       pushHudLine(left, bottom, left, top, color);
            };
            const auto classifyBudgetColor = [](double ratio) -> math::Vec3 {
                if (ratio <= 0.60) {
                    return {0.55F, 0.84F, 0.52F};
                }
                if (ratio <= 0.90) {
                    return {0.93F, 0.84F, 0.45F};
                }
                if (ratio <= 1.00) {
                    return {1.00F, 0.66F, 0.40F};
                }
                return {0.96F, 0.42F, 0.42F};
            };

            // Snapshot queue depths under lock so HUD reflects a consistent frame state.
            std::size_t streamRequestDepth = 0;
            std::size_t streamReadyDepth = 0;
            {
                std::lock_guard<std::mutex> lock(impl_->streamUploadMutex);
                streamRequestDepth = impl_->streamUploadRequests.size();
                streamReadyDepth = impl_->streamUploadReadyItems.size();
            }

            const bool hasBudgetSamples = impl_->gpuTimingAverage.valid && impl_->gpuTimingHistoryCount > 0;
            const double totalBudgetMs = std::max(0.05, impl_->passBudgetConfig.totalMs);
            const double shadowBudgetMs = std::max(0.05, impl_->passBudgetConfig.shadowMs);
            const double mainBudgetMs = std::max(0.05, impl_->passBudgetConfig.mainMs);
            const double debugBudgetMs = std::max(0.05, impl_->passBudgetConfig.debugMs);
            const std::array<double, 4> passValues = {{
                hasBudgetSamples ? impl_->gpuTimingAverage.totalMs : 0.0,
                hasBudgetSamples ? impl_->gpuTimingAverage.shadowMs : 0.0,
                hasBudgetSamples ? impl_->gpuTimingAverage.mainMs : 0.0,
                hasBudgetSamples ? impl_->gpuTimingAverage.debugMs : 0.0,
            }};
            const std::array<double, 4> passBudgets = {{
                totalBudgetMs,
                shadowBudgetMs,
                mainBudgetMs,
                debugBudgetMs,
            }};
            const double streamBatchBudget = static_cast<double>(std::max<std::size_t>(1, impl_->streamUploadBatchBudgetPerFrame));
            const std::array<double, 4> statusRatios = {{
                std::clamp(static_cast<double>(entityCount) / static_cast<double>(kMaxDrawsPerFrame), 0.0, 1.5),
                std::clamp(static_cast<double>(streamRequestDepth) / 12.0, 0.0, 1.5),
                std::clamp(static_cast<double>(streamReadyDepth) / 12.0, 0.0, 1.5),
                std::clamp(streamBatchBudget / 8.0, 0.0, 1.0),
            }};
            const std::array<math::Vec3, 4> statusColors = {{
                {0.40F, 0.72F, 1.00F},
                {0.44F, 0.86F, 0.86F},
                {0.72F, 0.72F, 1.00F},
                {0.86F, 0.74F, 0.95F},
            }};

            // Panel background grid.
            const float panelLeft = 0.03F;
            const float panelTop = 0.05F;
            const float panelRight = 0.43F;
            const float panelBottom = 0.36F;
            for (int row = 0; row <= 20 && !debugVertexOverflow; ++row) {
                const float t = static_cast<float>(row) / 20.0F;
                const float y = panelTop + (panelBottom - panelTop) * t;
                pushHudLine(panelLeft, y, panelRight, y, {0.08F, 0.11F, 0.14F});
            }
            pushHudRect(panelLeft, panelTop, panelRight, panelBottom, {0.24F, 0.31F, 0.40F});
            pushHudLine(panelLeft + 0.01F, panelTop + 0.045F, panelRight - 0.01F, panelTop + 0.045F, {0.32F, 0.40F, 0.50F});

            const float meterLeft = panelLeft + 0.02F;
            const float meterRight = panelRight - 0.02F;
            const float meterWidth = meterRight - meterLeft;
            const float meterStartY = panelTop + 0.07F;
            const float meterRowStride = 0.044F;
            const float meterHeight = 0.017F;

            // GPU timing bars: Total/Shadow/Main/Debug vs configured budgets.
            for (std::size_t i = 0; i < passValues.size() && !debugVertexOverflow; ++i) {
                const float yTop = meterStartY + static_cast<float>(i) * meterRowStride;
                const float yBottom = yTop + meterHeight;
                const double ratio = passBudgets[i] > 0.0 ? (passValues[i] / passBudgets[i]) : 0.0;
                const double clampedRatio = std::clamp(ratio, 0.0, 1.0);
                const float fillRight = meterLeft + meterWidth * static_cast<float>(clampedRatio);
                const math::Vec3 barColor = hasBudgetSamples ? classifyBudgetColor(ratio) : math::Vec3{0.65F, 0.65F, 0.65F};

                pushHudRect(meterLeft, yTop, meterRight, yBottom, {0.27F, 0.33F, 0.39F});
                for (int band = 0; band < 3 && !debugVertexOverflow; ++band) {
                    const float lineY = yTop + (static_cast<float>(band) + 1.0F) * (meterHeight / 4.0F);
                    pushHudLine(meterLeft, lineY, fillRight, lineY, barColor);
                }

                if (ratio > 1.0) {
                    const float overflowX = meterRight + 0.008F;
                    pushHudLine(overflowX, yTop, overflowX, yBottom, {0.96F, 0.42F, 0.42F});
                    pushHudLine(meterRight, yTop, overflowX, yBottom, {0.96F, 0.42F, 0.42F});
                }
            }

            // Streaming/scene status bars: entity density, request depth, ready depth, batch budget.
            const float statusStartY = meterStartY + meterRowStride * 4.0F + 0.02F;
            const float statusRowStride = 0.030F;
            const float statusHeight = 0.012F;
            for (std::size_t i = 0; i < statusRatios.size() && !debugVertexOverflow; ++i) {
                const float yTop = statusStartY + static_cast<float>(i) * statusRowStride;
                const float yBottom = yTop + statusHeight;
                const float fillRight = meterLeft + meterWidth * static_cast<float>(std::clamp(statusRatios[i], 0.0, 1.0));
                pushHudRect(meterLeft, yTop, meterRight, yBottom, {0.24F, 0.30F, 0.36F});
                pushHudLine(meterLeft, yTop + statusHeight * 0.5F, fillRight, yTop + statusHeight * 0.5F, statusColors[i]);

                if (statusRatios[i] > 1.0) {
                    const float overflowX = meterRight + 0.006F;
                    pushHudLine(overflowX, yTop, overflowX, yBottom, {0.96F, 0.42F, 0.42F});
                }
            }
        }

        if (!debugVertices.empty()) {
            const std::size_t byteSize = debugVertices.size() * sizeof(DebugVertex);
            std::memcpy(impl_->mappedDebugLineVertices, debugVertices.data(), byteSize);

            D3D12_VERTEX_BUFFER_VIEW debugView{};
            debugView.BufferLocation = impl_->debugLineVertexBuffer->GetGPUVirtualAddress();
            debugView.SizeInBytes = static_cast<UINT>(byteSize);
            debugView.StrideInBytes = sizeof(DebugVertex);

            impl_->commandList->SetPipelineState(impl_->debugLinePipelineState.Get());
            impl_->commandList->SetGraphicsRootSignature(impl_->rootSignature.Get());
            impl_->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            impl_->commandList->IASetVertexBuffers(0, 1, &debugView);
            impl_->commandList->SetGraphicsRootDescriptorTable(1, impl_->textureSrvHandle(0));
            impl_->commandList->SetGraphicsRootDescriptorTable(2, impl_->textureSrvHandle(impl_->shadowMapSrvIndex));
            impl_->commandList->SetGraphicsRoot32BitConstant(3, defaultTexture->descriptorIndex, 0);
            impl_->updateDebugConstant();
            impl_->commandList->DrawInstanced(static_cast<UINT>(debugVertices.size()), 1, 0, 0);
            framePassDrawStats.debugDrawCalls += 1;
            framePassDrawStats.debugLines += static_cast<std::uint64_t>(debugVertices.size() / 2);
        }
    }
    writeTimestamp(kTsDebugEnd);

    // Transition back to present and submit frame.
    const auto toPresent = makeTransitionBarrier(impl_->renderTargets[impl_->frameIndex].Get(),
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_PRESENT);
    impl_->commandList->ResourceBarrier(1, &toPresent);
    writeTimestamp(kTsFrameEnd);

    if (gpuProfilingEnabled) {
        const UINT startIndex = impl_->timestampIndex(impl_->frameIndex, kTsFrameStart);
        const UINT count = kGpuTimestampSlotsPerFrame;
        const UINT64 destinationOffset = static_cast<UINT64>(startIndex) * sizeof(std::uint64_t);
        impl_->commandList->ResolveQueryData(impl_->gpuTimestampQueryHeap.Get(),
                                             D3D12_QUERY_TYPE_TIMESTAMP,
                                             startIndex,
                                             count,
                                             impl_->gpuTimestampReadback.Get(),
                                             destinationOffset);
        impl_->gpuTimestampPending[impl_->frameIndex] = true;
    }

    throwIfFailed(impl_->commandList->Close(), "Failed to close command list");

    ID3D12CommandList* commandLists[] = {impl_->commandList.Get()};
    impl_->commandQueue->ExecuteCommandLists(1, commandLists);

    throwIfFailed(impl_->swapChain->Present(impl_->vsync ? 1 : 0, 0), "Failed to present swap chain");
    impl_->moveToNextFrame();

    const auto cpuFrameEnd = std::chrono::steady_clock::now();
    const double cpuFrameMs =
        std::chrono::duration<double, std::milli>(cpuFrameEnd - impl_->cpuFrameStart).count();
    impl_->cpuFrameAccumulatedMs += cpuFrameMs;
    impl_->cpuFrameAccumulatedCount += 1;
    const double rollingCpuMs = impl_->cpuFrameAccumulatedCount == 0
                                    ? cpuFrameMs
                                    : (impl_->cpuFrameAccumulatedMs /
                                       static_cast<double>(impl_->cpuFrameAccumulatedCount));
    impl_->cpuFrameLastMs = rollingCpuMs;
    impl_->cpuFrameLastFps = rollingCpuMs > 0.0 ? (1000.0 / rollingCpuMs) : 0.0;
    impl_->passDrawStatsLast = framePassDrawStats;
    impl_->lastEntityCount = entityCount;
    impl_->updateAutoBudgetSelection(entityCount, impl_->passDrawStatsLast, false, false);
    impl_->refreshVisibilityBudgetState(impl_->passDrawStatsLast, false);

    if (impl_->simulationFrame % 60 == 0) {
        const double averageCpuMs = impl_->cpuFrameAccumulatedCount == 0
                                        ? 0.0
                                        : (impl_->cpuFrameAccumulatedMs /
                                           static_cast<double>(impl_->cpuFrameAccumulatedCount));
        const double estimatedFps = averageCpuMs > 0.0 ? (1000.0 / averageCpuMs) : 0.0;
        const auto budgetState = [](double valueMs, double budgetMs) -> const char* {
            if (budgetMs <= 0.0) {
                return "NA";
            }
            const double ratio = valueMs / budgetMs;
            if (ratio <= 0.60) {
                return "OK";
            }
            if (ratio <= 0.90) {
                return "WATCH";
            }
            if (ratio <= 1.00) {
                return "HIGH";
            }
            return "OVER";
        };
        const bool hasBudgetSamples = impl_->gpuTimingAverage.valid && impl_->gpuTimingHistoryCount > 0;
        const char* totalBudgetState =
            hasBudgetSamples ? budgetState(impl_->gpuTimingAverage.totalMs, impl_->passBudgetConfig.totalMs) : "NA";
        const char* shadowBudgetState =
            hasBudgetSamples ? budgetState(impl_->gpuTimingAverage.shadowMs, impl_->passBudgetConfig.shadowMs) : "NA";
        const char* mainBudgetState =
            hasBudgetSamples ? budgetState(impl_->gpuTimingAverage.mainMs, impl_->passBudgetConfig.mainMs) : "NA";
        const char* debugBudgetState =
            hasBudgetSamples ? budgetState(impl_->gpuTimingAverage.debugMs, impl_->passBudgetConfig.debugMs) : "NA";
        const bool budgetAlert =
            hasBudgetSamples &&
            (std::strcmp(totalBudgetState, "OVER") == 0 ||
             std::strcmp(shadowBudgetState, "OVER") == 0 ||
             std::strcmp(mainBudgetState, "OVER") == 0 ||
             std::strcmp(debugBudgetState, "OVER") == 0);
        std::cout << "[Frame " << impl_->simulationFrame << "] t=" << impl_->simulationTimeSeconds
                  << "s entities=" << entityCount
                  << " cpu(ms)=" << averageCpuMs
                  << " fps~" << estimatedFps
                  << " gpuTotal(ms)=" << (impl_->gpuTimingLast.valid ? impl_->gpuTimingLast.totalMs : 0.0)
                  << " gpuShadow(ms)=" << (impl_->gpuTimingLast.valid ? impl_->gpuTimingLast.shadowMs : 0.0)
                  << " gpuMain(ms)=" << (impl_->gpuTimingLast.valid ? impl_->gpuTimingLast.mainMs : 0.0)
                  << " gpuDebug(ms)=" << (impl_->gpuTimingLast.valid ? impl_->gpuTimingLast.debugMs : 0.0)
                  << " gpuAvgTotal(ms)=" << (impl_->gpuTimingAverage.valid ? impl_->gpuTimingAverage.totalMs : 0.0)
                  << " gpuAvgShadow(ms)=" << (impl_->gpuTimingAverage.valid ? impl_->gpuTimingAverage.shadowMs : 0.0)
                  << " gpuAvgMain(ms)=" << (impl_->gpuTimingAverage.valid ? impl_->gpuTimingAverage.mainMs : 0.0)
                  << " gpuAvgDebug(ms)=" << (impl_->gpuTimingAverage.valid ? impl_->gpuTimingAverage.debugMs : 0.0)
                  << " memDefault(MiB)="
                  << (static_cast<double>(impl_->gpuMemoryStats.defaultBytes) / (1024.0 * 1024.0))
                  << " memUpload(MiB)="
                  << (static_cast<double>(impl_->gpuMemoryStats.uploadBytes) / (1024.0 * 1024.0))
                  << " memReadback(MiB)="
                  << (static_cast<double>(impl_->gpuMemoryStats.readbackBytes) / (1024.0 * 1024.0))
                  << " shDraw=" << impl_->passDrawStatsLast.shadowDrawCalls
                  << " shTri=" << impl_->passDrawStatsLast.shadowTriangles
                  << " mainDraw=" << impl_->passDrawStatsLast.mainDrawCalls
                  << " mainTri=" << impl_->passDrawStatsLast.mainTriangles
                  << " dbgDraw=" << impl_->passDrawStatsLast.debugDrawCalls
                  << " dbgLine=" << impl_->passDrawStatsLast.debugLines
                  << " cullIn=" << impl_->passDrawStatsLast.cullInputCount
                  << " cullVisible=" << impl_->passDrawStatsLast.cullVisibleCount
                  << " cullOut=" << impl_->passDrawStatsLast.cullRejectedCount
                  << " cullFrustum=" << impl_->passDrawStatsLast.cullFrustumRejectedCount
                  << " cullOcclusion=" << impl_->passDrawStatsLast.cullOcclusionRejectedCount
                  << " cullDispatch(F/O)="
                  << impl_->passDrawStatsLast.cullFrustumDispatchGroups << "/"
                  << impl_->passDrawStatsLast.cullOcclusionDispatchGroups
                  << " hzb(L/B/C/R)="
                  << impl_->passDrawStatsLast.hzbLevelsBuilt << "/"
                  << impl_->passDrawStatsLast.hzbBuildDispatchGroups << "/"
                  << impl_->passDrawStatsLast.hzbCellsTested << "/"
                  << impl_->passDrawStatsLast.hzbRejectedCount
                  << " dispatchProto=" << (impl_->computeDispatchPrototypeEnabled ? "on" : "off")
                  << " indirectMain=" << (impl_->indirectMainDrawReady ? "on" : "off")
                  << " frustumGpuReady=" << (impl_->gpuFrustumCullingReady ? "on" : "off")
                  << " occlusionGpuReady=" << (impl_->gpuOcclusionCullingReady ? "on" : "off")
                  << " visBudget=" << (impl_->visibilityBudgetEnabled ? "on" : "off")
                  << " hzb=" << (impl_->hzbOcclusionEnabled ? "on" : "off")
                  << " visState=" << impl_->visibilityBudgetState
                  << " visRatio=" << impl_->visibilityVisibleRatio
                  << " visOccReject=" << impl_->visibilityOcclusionRejectedRatio
                  << " budget(T/S/M/D)="
                  << totalBudgetState << "/" << shadowBudgetState << "/" << mainBudgetState << "/" << debugBudgetState
                  << " budgetAlert=" << (budgetAlert ? "on" : "off")
                  << " budgetMode=" << (impl_->budgetAutoModeEnabled ? "auto" : "manual")
                  << " budgetTier=" << impl_->complexityTierName(impl_->budgetTierCurrent)
                  << " budgetScore=" << impl_->budgetComplexityScore
                  << " budgetProfile=" << impl_->activeBudgetProfileLabel()
                  << " cam=(" << impl_->cameraPosition.x << ", " << impl_->cameraPosition.y << ", " << impl_->cameraPosition.z
                  << ") shadowDebug=" << (impl_->debugShowShadowFactor ? "on" : "off")
                  << " wireframe=" << (impl_->debugWireframe ? "on" : "off")
                  << " collisionOverlay=" << (impl_->debugShowCollision ? "on" : "off")
                  << " hud=" << (impl_->debugShowHud ? "on" : "off")
                  << '\n';
        impl_->cpuFrameAccumulatedMs = 0.0;
        impl_->cpuFrameAccumulatedCount = 0;
    }
}

}  // namespace engine::render

#endif  // _WIN32


