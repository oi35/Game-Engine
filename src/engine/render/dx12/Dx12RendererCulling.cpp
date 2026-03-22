// File: Dx12RendererCulling.cpp
// Purpose: Implements CPU/GPU frustum and occlusion culling preparation and result handling.

#ifdef _WIN32

#include "engine/render/dx12/Dx12RendererPrivate.h"

namespace engine::render {

bool Dx12Renderer::Impl::runGpuFrustumCull(const std::vector<GpuCullSphere>& spheres,
                                           const math::Vec3& cameraForward,
                                           const math::Vec3& cameraRight,
                                           const math::Vec3& cameraUp,
                                           std::vector<std::size_t>& outCandidates,
                                           PassDrawStats& outStats) {
    // Compute pass that marks and compacts frustum-visible draw indices.
    (void)outCandidates;

    if (!gpuFrustumCullingReady ||
        !computeCommandList ||
        !computeCullPipelineState ||
        !computeCullRootSignature ||
        !computeCullSphereBuffer ||
        !computeCullVisibleBuffer ||
        !computeCullCompactIndexBuffer ||
        !computeCullCompactCounterBuffer ||
        !computeCullCounterResetUploadBuffer ||
        mappedComputeCullCounterReset == nullptr ||
        mappedComputeCullSpheres == nullptr) {
        return false;
    }

    const std::size_t drawCount = spheres.size();
    if (drawCount == 0) {
        return true;
    }
    if (drawCount > kMaxDrawsPerFrame) {
        return false;
    }

    try {
        // Upload sphere input payload for this frame.
        const std::size_t sphereBytes = drawCount * sizeof(GpuCullSphere);
        std::memcpy(mappedComputeCullSpheres, spheres.data(), sphereBytes);

        throwIfFailed(computeCommandAllocators[frameIndex]->Reset(),
                      "Failed to reset compute command allocator");
        throwIfFailed(computeCommandList->Reset(computeCommandAllocators[frameIndex].Get(),
                                                computeCullPipelineState.Get()),
                      "Failed to reset compute command list");

        computeCommandList->SetPipelineState(computeCullPipelineState.Get());
        computeCommandList->SetComputeRootSignature(computeCullRootSignature.Get());

        const float nearPlane = 0.1F;
        const float farPlane = 100.0F;
        const float aspect = (height > 0) ? (static_cast<float>(width) / static_cast<float>(height)) : 1.0F;
        const float tanHalfFovY = std::tan(DirectX::XMConvertToRadians(60.0F) * 0.5F);

        GpuFrustumCullParams params{};
        params.cameraPositionNear = {cameraPosition.x, cameraPosition.y, cameraPosition.z, nearPlane};
        params.cameraForwardFar = {cameraForward.x, cameraForward.y, cameraForward.z, farPlane};
        params.cameraRightAspect = {cameraRight.x, cameraRight.y, cameraRight.z, aspect};
        params.cameraUpTanHalfFovY = {cameraUp.x, cameraUp.y, cameraUp.z, tanHalfFovY};
        params.drawCount = static_cast<std::uint32_t>(drawCount);
        computeCommandList->SetComputeRoot32BitConstants(0, sizeof(GpuFrustumCullParams) / 4, &params, 0);
        computeCommandList->SetComputeRootShaderResourceView(1, computeCullSphereBuffer->GetGPUVirtualAddress());
        computeCommandList->SetComputeRootUnorderedAccessView(2, computeCullVisibleBuffer->GetGPUVirtualAddress());
        computeCommandList->SetComputeRootUnorderedAccessView(3, computeCullCompactIndexBuffer->GetGPUVirtualAddress());
        computeCommandList->SetComputeRootUnorderedAccessView(4, computeCullCompactCounterBuffer->GetGPUVirtualAddress());

        const auto counterToCopyDest = makeTransitionBarrier(computeCullCompactCounterBuffer.Get(),
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                             D3D12_RESOURCE_STATE_COPY_DEST);
        computeCommandList->ResourceBarrier(1, &counterToCopyDest);
        computeCommandList->CopyBufferRegion(computeCullCompactCounterBuffer.Get(),
                                             0,
                                             computeCullCounterResetUploadBuffer.Get(),
                                             0,
                                             sizeof(std::uint32_t));
        const auto counterToUav = makeTransitionBarrier(computeCullCompactCounterBuffer.Get(),
                                                        D3D12_RESOURCE_STATE_COPY_DEST,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        computeCommandList->ResourceBarrier(1, &counterToUav);

        const std::uint32_t dispatchGroups = static_cast<std::uint32_t>(
            (drawCount + kComputeCullThreadGroupSize - 1) / kComputeCullThreadGroupSize);
        computeCommandList->Dispatch(dispatchGroups, 1, 1);

        std::array<D3D12_RESOURCE_BARRIER, 3> uavBarriers{};
        uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarriers[0].UAV.pResource = computeCullVisibleBuffer.Get();
        uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarriers[1].UAV.pResource = computeCullCompactIndexBuffer.Get();
        uavBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarriers[2].UAV.pResource = computeCullCompactCounterBuffer.Get();
        computeCommandList->ResourceBarrier(static_cast<UINT>(uavBarriers.size()), uavBarriers.data());

        throwIfFailed(computeCommandList->Close(), "Failed to close compute command list");
        ID3D12CommandList* computeLists[] = {computeCommandList.Get()};
        commandQueue->ExecuteCommandLists(1, computeLists);

        outStats.cullFrustumDispatchGroups += dispatchGroups;
        return true;
    } catch (const std::exception& ex) {
        if (!gpuFrustumCullFailureLogged) {
            std::cout << "[DispatchGPU] Runtime fallback to CPU frustum path: " << ex.what() << '\n';
            gpuFrustumCullFailureLogged = true;
        }
        gpuFrustumCullingReady = false;
        gpuFrustumResultPending.fill(false);
        gpuFrustumValidSphereCount.fill(0);
        gpuFrustumDispatchGroups.fill(0);
        return false;
    }
}

bool Dx12Renderer::Impl::runGpuOcclusionCull(const std::vector<float>& camSpaceX,
                                             const std::vector<float>& camSpaceY,
                                             const std::vector<float>& camSpaceZ,
                                             const std::vector<float>& worldRadius,
                                             const std::vector<std::size_t>& sortedFrustumCandidates,
                                             std::size_t drawCount,
                                             const VisibilityTierBudget& visBudget,
                                             std::vector<std::size_t>& outVisibleDrawIndices,
                                             PassDrawStats& outStats) {
    // Compute pass that estimates visibility via HZB/occlusion and builds indirect draw inputs.
    (void)sortedFrustumCandidates;
    outVisibleDrawIndices.clear();

    const std::uint32_t slot = frameIndex % kFrameCount;
    ID3D12Resource* statsReadback = computeOcclusionStatsReadbackBuffers[slot].Get();
    if (!gpuOcclusionCullingReady ||
        !computeCommandList ||
        !computeOcclusionPipelineState ||
        !computeOcclusionRootSignature ||
        !computeMainBuildPipelineState ||
        !computeMainBuildRootSignature ||
        !computeOcclusionCamSpaceUploadBuffer ||
        !computeOcclusionVisibleIndexBuffer ||
        !computeOcclusionStatsBuffer ||
        !computeOcclusionStatsResetUploadBuffer ||
        !computeCullCompactIndexBuffer ||
        !computeCullCompactCounterBuffer ||
        !computeCullCounterResetUploadBuffer ||
        !mainDrawIndirectArgumentBuffer ||
        !mainDrawIndirectCountBuffer ||
        !mainDrawMetadataBuffer ||
        !statsReadback ||
        mappedComputeOcclusionCamSpace == nullptr) {
        return false;
    }

    if (drawCount == 0 || drawCount > kMaxDrawsPerFrame) {
        return drawCount == 0;
    }
    if (camSpaceX.size() < drawCount || camSpaceY.size() < drawCount || camSpaceZ.size() < drawCount ||
        worldRadius.size() < drawCount) {
        return false;
    }

    try {
        if (gpuOcclusionResultPending[slot]) {
            // Consume previous frame async stats readback.
            std::array<std::uint32_t, 8> stats{};
            const D3D12_RANGE statsReadRange{0, sizeof(std::uint32_t) * stats.size()};
            std::uint32_t* mappedStats = nullptr;
            throwIfFailed(statsReadback->Map(0, &statsReadRange, reinterpret_cast<void**>(&mappedStats)),
                          "Failed to map async occlusion stats readback buffer");
            std::memcpy(stats.data(), mappedStats, sizeof(std::uint32_t) * stats.size());
            const D3D12_RANGE writeRange{0, 0};
            statsReadback->Unmap(0, &writeRange);

            const std::uint32_t frustumCandidates = std::min<std::uint32_t>(
                stats[6],
                static_cast<std::uint32_t>(drawCount));
            const std::uint32_t visibleCount = std::min<std::uint32_t>(stats[0], frustumCandidates);
            outStats.cullVisibleCount = visibleCount;
            outStats.cullFrustumRejectedCount += static_cast<std::uint32_t>(drawCount) - frustumCandidates;
            outStats.cullOcclusionRejectedCount += stats[1];
            outStats.hzbCellsTested += stats[2];
            outStats.hzbRejectedCount += stats[3];
            outStats.hzbLevelsBuilt = std::max(outStats.hzbLevelsBuilt, stats[4]);
            outStats.hzbBuildDispatchGroups += stats[5];
            outStats.cullOcclusionDispatchGroups +=
                std::max<std::uint32_t>(1, gpuOcclusionDispatchGroups[slot]);
            gpuOcclusionResultPending[slot] = false;
            gpuOcclusionCandidateCount[slot] = 0;
            gpuOcclusionDispatchGroups[slot] = 0;
        }

        for (std::size_t i = 0; i < drawCount; ++i) {
            mappedComputeOcclusionCamSpace[i] = XMFLOAT4{
                camSpaceX[i],
                camSpaceY[i],
                camSpaceZ[i],
                worldRadius[i],
            };
        }

        throwIfFailed(computeCommandAllocators[frameIndex]->Reset(),
                      "Failed to reset occlusion compute command allocator");
        throwIfFailed(computeCommandList->Reset(computeCommandAllocators[frameIndex].Get(),
                                                computeOcclusionPipelineState.Get()),
                      "Failed to reset occlusion compute command list");

        // Reset UAV counters/stats before dispatch.
        const auto statsToCopyDest = makeTransitionBarrier(computeOcclusionStatsBuffer.Get(),
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                           D3D12_RESOURCE_STATE_COPY_DEST);
        const auto countToCopyDest = makeTransitionBarrier(mainDrawIndirectCountBuffer.Get(),
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                           D3D12_RESOURCE_STATE_COPY_DEST);
        std::array<D3D12_RESOURCE_BARRIER, 2> resetToCopyDest{statsToCopyDest, countToCopyDest};
        computeCommandList->ResourceBarrier(static_cast<UINT>(resetToCopyDest.size()), resetToCopyDest.data());
        computeCommandList->CopyBufferRegion(computeOcclusionStatsBuffer.Get(),
                                             0,
                                             computeOcclusionStatsResetUploadBuffer.Get(),
                                             0,
                                             8 * sizeof(std::uint32_t));
        computeCommandList->CopyBufferRegion(mainDrawIndirectCountBuffer.Get(),
                                             0,
                                             computeCullCounterResetUploadBuffer.Get(),
                                             0,
                                             sizeof(std::uint32_t));
        const auto statsToUavAfterReset = makeTransitionBarrier(computeOcclusionStatsBuffer.Get(),
                                                                D3D12_RESOURCE_STATE_COPY_DEST,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto countToUavAfterReset = makeTransitionBarrier(mainDrawIndirectCountBuffer.Get(),
                                                                D3D12_RESOURCE_STATE_COPY_DEST,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        std::array<D3D12_RESOURCE_BARRIER, 2> resetToUav{statsToUavAfterReset, countToUavAfterReset};
        computeCommandList->ResourceBarrier(static_cast<UINT>(resetToUav.size()), resetToUav.data());

        // Run occlusion culling kernel.
        computeCommandList->SetPipelineState(computeOcclusionPipelineState.Get());
        computeCommandList->SetComputeRootSignature(computeOcclusionRootSignature.Get());

        const float aspect = (height > 0) ? (static_cast<float>(width) / static_cast<float>(height)) : 1.0F;
        const float tanHalfFovY = std::tan(DirectX::XMConvertToRadians(60.0F) * 0.5F);
        GpuOcclusionCullParams params{};
        params.aspect = aspect;
        params.tanHalfFovY = tanHalfFovY;
        params.maxVisibleRatio = static_cast<float>(std::clamp(visBudget.maxVisibleRatio, 0.05, 1.0));
        params.occlusionCoverageThreshold = static_cast<float>(std::clamp(visBudget.occlusionCoverageThreshold, 0.0, 1.0));
        params.depthSlack = static_cast<float>(visBudget.depthSlack);
        params.drawCount = static_cast<std::uint32_t>(drawCount);
        params.candidateCount = static_cast<std::uint32_t>(drawCount);
        params.visibilityBudgetEnabled = visibilityBudgetEnabled ? 1U : 0U;
        params.hzbEnabled = hzbOcclusionEnabled ? 1U : 0U;
        params.occlusionDispatchGroupSize = std::max<std::uint32_t>(1, computeOcclusionDispatchGroupSize);

        const auto compactIndexToSrv = makeTransitionBarrier(computeCullCompactIndexBuffer.Get(),
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const auto compactCounterToSrv = makeTransitionBarrier(computeCullCompactCounterBuffer.Get(),
                                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        std::array<D3D12_RESOURCE_BARRIER, 2> compactToSrv{compactIndexToSrv, compactCounterToSrv};
        computeCommandList->ResourceBarrier(static_cast<UINT>(compactToSrv.size()), compactToSrv.data());

        computeCommandList->SetComputeRoot32BitConstants(0, sizeof(GpuOcclusionCullParams) / 4, &params, 0);
        computeCommandList->SetComputeRootShaderResourceView(1, computeOcclusionCamSpaceUploadBuffer->GetGPUVirtualAddress());
        computeCommandList->SetComputeRootShaderResourceView(2, computeCullCompactIndexBuffer->GetGPUVirtualAddress());
        computeCommandList->SetComputeRootShaderResourceView(3, computeCullCompactCounterBuffer->GetGPUVirtualAddress());
        computeCommandList->SetComputeRootUnorderedAccessView(4, computeOcclusionVisibleIndexBuffer->GetGPUVirtualAddress());
        computeCommandList->SetComputeRootUnorderedAccessView(5, computeOcclusionStatsBuffer->GetGPUVirtualAddress());
        computeCommandList->SetComputeRootUnorderedAccessView(6, mainDrawIndirectCountBuffer->GetGPUVirtualAddress());
        const std::uint32_t occlusionDispatchGroups = static_cast<std::uint32_t>(
            (drawCount + kComputeCullThreadGroupSize - 1) / kComputeCullThreadGroupSize);
        computeCommandList->Dispatch(std::max<std::uint32_t>(1, occlusionDispatchGroups), 1, 1);

        std::array<D3D12_RESOURCE_BARRIER, 3> occlusionUavBarriers{};
        occlusionUavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        occlusionUavBarriers[0].UAV.pResource = computeOcclusionVisibleIndexBuffer.Get();
        occlusionUavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        occlusionUavBarriers[1].UAV.pResource = computeOcclusionStatsBuffer.Get();
        occlusionUavBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        occlusionUavBarriers[2].UAV.pResource = mainDrawIndirectCountBuffer.Get();
        computeCommandList->ResourceBarrier(static_cast<UINT>(occlusionUavBarriers.size()), occlusionUavBarriers.data());

        const auto statsToCopy = makeTransitionBarrier(computeOcclusionStatsBuffer.Get(),
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        const auto countToCopyDestFromStats = makeTransitionBarrier(mainDrawIndirectCountBuffer.Get(),
                                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                    D3D12_RESOURCE_STATE_COPY_DEST);
        std::array<D3D12_RESOURCE_BARRIER, 2> statsCopyBarriers{statsToCopy, countToCopyDestFromStats};
        computeCommandList->ResourceBarrier(static_cast<UINT>(statsCopyBarriers.size()), statsCopyBarriers.data());
        computeCommandList->CopyBufferRegion(statsReadback,
                                             0,
                                             computeOcclusionStatsBuffer.Get(),
                                             0,
                                             8 * sizeof(std::uint32_t));
        computeCommandList->CopyBufferRegion(mainDrawIndirectCountBuffer.Get(),
                                             0,
                                             computeOcclusionStatsBuffer.Get(),
                                             0,
                                             sizeof(std::uint32_t));
        const auto statsToUav = makeTransitionBarrier(computeOcclusionStatsBuffer.Get(),
                                                      D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto countBackToUav = makeTransitionBarrier(mainDrawIndirectCountBuffer.Get(),
                                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        std::array<D3D12_RESOURCE_BARRIER, 2> statsBackToUav{statsToUav, countBackToUav};
        computeCommandList->ResourceBarrier(static_cast<UINT>(statsBackToUav.size()), statsBackToUav.data());

        const auto visibleToSrv = makeTransitionBarrier(computeOcclusionVisibleIndexBuffer.Get(),
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        computeCommandList->ResourceBarrier(1, &visibleToSrv);

        // Build indirect draw argument buffer from visible indices.
        computeCommandList->SetPipelineState(computeMainBuildPipelineState.Get());
        computeCommandList->SetComputeRootSignature(computeMainBuildRootSignature.Get());
        std::array<std::uint32_t, 4> buildParams{
            static_cast<std::uint32_t>(drawCount),
            0U,
            0U,
            0U,
        };
        computeCommandList->SetComputeRoot32BitConstants(0, static_cast<UINT>(buildParams.size()), buildParams.data(), 0);
        computeCommandList->SetComputeRootShaderResourceView(1, computeOcclusionVisibleIndexBuffer->GetGPUVirtualAddress());
        computeCommandList->SetComputeRootShaderResourceView(2, mainDrawMetadataBuffer->GetGPUVirtualAddress());
        computeCommandList->SetComputeRootUnorderedAccessView(3, mainDrawIndirectArgumentBuffer->GetGPUVirtualAddress());
        computeCommandList->SetComputeRootUnorderedAccessView(4, mainDrawIndirectCountBuffer->GetGPUVirtualAddress());
        const std::uint32_t dispatchGroups = static_cast<std::uint32_t>(
            (drawCount + kComputeCullThreadGroupSize - 1) / kComputeCullThreadGroupSize);
        computeCommandList->Dispatch(dispatchGroups, 1, 1);

        std::array<D3D12_RESOURCE_BARRIER, 2> mainBuildUavBarriers{};
        mainBuildUavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        mainBuildUavBarriers[0].UAV.pResource = mainDrawIndirectArgumentBuffer.Get();
        mainBuildUavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        mainBuildUavBarriers[1].UAV.pResource = mainDrawIndirectCountBuffer.Get();
        computeCommandList->ResourceBarrier(static_cast<UINT>(mainBuildUavBarriers.size()), mainBuildUavBarriers.data());

        const auto visibleToUav = makeTransitionBarrier(computeOcclusionVisibleIndexBuffer.Get(),
                                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        computeCommandList->ResourceBarrier(1, &visibleToUav);

        std::array<D3D12_RESOURCE_BARRIER, 2> compactToUav{};
        compactToUav[0] = makeTransitionBarrier(computeCullCompactIndexBuffer.Get(),
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compactToUav[1] = makeTransitionBarrier(computeCullCompactCounterBuffer.Get(),
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        computeCommandList->ResourceBarrier(static_cast<UINT>(compactToUav.size()), compactToUav.data());

        std::array<D3D12_RESOURCE_BARRIER, 2> toIndirect{};
        toIndirect[0] = makeTransitionBarrier(mainDrawIndirectArgumentBuffer.Get(),
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                              D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        toIndirect[1] = makeTransitionBarrier(mainDrawIndirectCountBuffer.Get(),
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                              D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        computeCommandList->ResourceBarrier(static_cast<UINT>(toIndirect.size()), toIndirect.data());

        throwIfFailed(computeCommandList->Close(), "Failed to close occlusion compute command list");
        ID3D12CommandList* lists[] = {computeCommandList.Get()};
        commandQueue->ExecuteCommandLists(1, lists);

        // Mark async stats as pending; they are consumed on a later frame.
        gpuOcclusionResultPending[slot] = true;
        gpuOcclusionCandidateCount[slot] = static_cast<std::uint32_t>(drawCount);
        gpuOcclusionDispatchGroups[slot] = std::max<std::uint32_t>(1, occlusionDispatchGroups);
        return true;
    } catch (const std::exception& ex) {
        if (!gpuOcclusionCullFailureLogged) {
            std::cout << "[OcclusionGPU] Runtime fallback to CPU occlusion path: " << ex.what() << '\n';
            gpuOcclusionCullFailureLogged = true;
        }
        gpuOcclusionCullingReady = false;
        gpuOcclusionResultPending.fill(false);
        gpuOcclusionCandidateCount.fill(0);
        gpuOcclusionDispatchGroups.fill(0);
        return false;
    }
}

}  // namespace engine::render

#endif  // _WIN32

