// File: Dx12RendererResources.cpp
// Purpose: Manages mesh/texture registration, upload batching, and streaming upload workers.

#ifdef _WIN32

#include "engine/render/dx12/Dx12RendererPrivate.h"

namespace engine::render {

void Dx12Renderer::Impl::startStreamUploadWorker() {
        if (streamUploadWorkerRunning) {
            return;
        }

        streamUploadStopRequested = false;
        // Worker thread only performs CPU-side preparation. D3D12 resource creation/copy stays on render thread.
        streamUploadWorker = std::thread([this]() {
            while (true) {
                StreamUploadRequest request{};
                {
                    std::unique_lock<std::mutex> lock(streamUploadMutex);
                    streamUploadCv.wait(lock, [this]() {
                        return streamUploadStopRequested || !streamUploadRequests.empty();
                    });
                    if (streamUploadStopRequested && streamUploadRequests.empty()) {
                        break;
                    }

                    request = std::move(streamUploadRequests.front());
                    streamUploadRequests.pop_front();
                }

                try {
                    StreamUploadReadyItem readyItem{};
                    if (auto* meshRequest = std::get_if<StreamMeshRequest>(&request); meshRequest != nullptr) {
                        readyItem = prepareMeshUpload(meshRequest->handle, meshRequest->mesh);
                    } else if (auto* textureRequest = std::get_if<StreamTextureRequest>(&request);
                               textureRequest != nullptr) {
                        readyItem = prepareTextureUpload(
                            textureRequest->handle,
                            textureRequest->descriptorIndex,
                            textureRequest->texture);
                    } else {
                        continue;
                    }

                    // Hand prepared payload back to render thread for actual GPU upload.
                    std::lock_guard<std::mutex> lock(streamUploadMutex);
                    streamUploadReadyItems.push_back(std::move(readyItem));
                } catch (const std::exception& ex) {
                    StreamUploadError error{};
                    if (auto* meshRequest = std::get_if<StreamMeshRequest>(&request); meshRequest != nullptr) {
                        error.isMesh = true;
                        error.handle = meshRequest->handle;
                    } else if (auto* textureRequest = std::get_if<StreamTextureRequest>(&request);
                               textureRequest != nullptr) {
                        error.isMesh = false;
                        error.handle = textureRequest->handle;
                    }
                    error.message = ex.what();
                    std::lock_guard<std::mutex> lock(streamUploadMutex);
                    streamUploadReadyItems.push_back(std::move(error));
                }
            }
        });
        streamUploadWorkerRunning = true;
    }

void Dx12Renderer::Impl::stopStreamUploadWorker() {
        if (!streamUploadWorkerRunning) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(streamUploadMutex);
            streamUploadStopRequested = true;
        }
        streamUploadCv.notify_all();

        if (streamUploadWorker.joinable()) {
            streamUploadWorker.join();
        }
        streamUploadWorkerRunning = false;

        std::lock_guard<std::mutex> lock(streamUploadMutex);
        streamUploadRequests.clear();
        streamUploadReadyItems.clear();
    }

void Dx12Renderer::Impl::enqueueStreamUploadRequest(StreamUploadRequest request) {
        {
            std::lock_guard<std::mutex> lock(streamUploadMutex);
            streamUploadRequests.push_back(std::move(request));
        }
        streamUploadCv.notify_one();
    }

void Dx12Renderer::Impl::processReadyStreamUploads(std::size_t maxUploads) {
        if (maxUploads == 0) {
            return;
        }

        // Drain only a bounded number per frame to keep frame-time stable.
        std::vector<StreamUploadReadyItem> readyItems;
        readyItems.reserve(maxUploads);
        {
            std::lock_guard<std::mutex> lock(streamUploadMutex);
            while (!streamUploadReadyItems.empty() && readyItems.size() < maxUploads) {
                readyItems.push_back(std::move(streamUploadReadyItems.front()));
                streamUploadReadyItems.pop_front();
            }
        }

        if (readyItems.empty()) {
            return;
        }

        // Upload on render thread where command list/fence ownership is defined.
        bool hadGpuResourceUpdate = false;
        for (auto& readyItem : readyItems) {
            if (auto* mesh = std::get_if<PreparedMeshUpload>(&readyItem); mesh != nullptr) {
                pendingMeshHandles.erase(mesh->handle);
                try {
                    hadGpuResourceUpdate |= uploadPreparedMesh(*mesh);
                } catch (const std::exception& ex) {
                    std::cout << "[Streaming] Mesh upload failed handle=" << mesh->handle
                              << " reason=" << ex.what() << '\n';
                }
                continue;
            }

            if (auto* texture = std::get_if<PreparedTextureUpload>(&readyItem); texture != nullptr) {
                pendingTextureHandles.erase(texture->handle);
                try {
                    hadGpuResourceUpdate |= uploadPreparedTexture(*texture);
                } catch (const std::exception& ex) {
                    std::cout << "[Streaming] Texture upload failed handle=" << texture->handle
                              << " reason=" << ex.what() << '\n';
                }
                continue;
            }

            const auto* error = std::get_if<StreamUploadError>(&readyItem);
            if (error != nullptr) {
                if (error->isMesh) {
                    pendingMeshHandles.erase(error->handle);
                    std::cout << "[Streaming] Mesh upload preparation failed handle=" << error->handle
                              << " reason=" << error->message << '\n';
                } else {
                    pendingTextureHandles.erase(error->handle);
                    std::cout << "[Streaming] Texture upload preparation failed handle=" << error->handle
                              << " reason=" << error->message << '\n';
                }
            }
        }

        if (hadGpuResourceUpdate) {
            rebuildGpuMemoryStats();
        }
    }

Dx12Renderer::Impl::PreparedMeshUpload Dx12Renderer::Impl::prepareMeshUpload(
    std::uint32_t handle, const assets::MeshData& mesh) const {
        if (mesh.vertices.empty() || mesh.indices.empty()) {
            throw std::runtime_error("Mesh registration failed: empty vertex/index data");
        }

        PreparedMeshUpload prepared{};
        prepared.handle = handle;
        prepared.indices = mesh.indices;
        prepared.vertices.reserve(mesh.vertices.size());

        math::Vec3 boundsMin = mesh.vertices.front().position;
        math::Vec3 boundsMax = mesh.vertices.front().position;
        for (const auto& sourceVertex : mesh.vertices) {
            const math::Vec3 normal = math::normalize(sourceVertex.normal);
            const float r = std::clamp(std::abs(normal.x) * 0.8F + 0.2F, 0.0F, 1.0F);
            const float g = std::clamp(std::abs(normal.y) * 0.8F + 0.2F, 0.0F, 1.0F);
            const float b = std::clamp(std::abs(normal.z) * 0.8F + 0.2F, 0.0F, 1.0F);

            boundsMin.x = std::min(boundsMin.x, sourceVertex.position.x);
            boundsMin.y = std::min(boundsMin.y, sourceVertex.position.y);
            boundsMin.z = std::min(boundsMin.z, sourceVertex.position.z);
            boundsMax.x = std::max(boundsMax.x, sourceVertex.position.x);
            boundsMax.y = std::max(boundsMax.y, sourceVertex.position.y);
            boundsMax.z = std::max(boundsMax.z, sourceVertex.position.z);

            prepared.vertices.push_back(Vertex{
                {sourceVertex.position.x, sourceVertex.position.y, sourceVertex.position.z},
                {normal.x, normal.y, normal.z},
                {r, g, b},
                {sourceVertex.u, sourceVertex.v},
            });
        }

        const math::Vec3 halfExtents = (boundsMax - boundsMin) * 0.5F;
        prepared.boundsCenter = (boundsMin + boundsMax) * 0.5F;
        prepared.boundsRadius = std::max(0.05F, math::length(halfExtents));
        return prepared;
    }

bool Dx12Renderer::Impl::uploadPreparedMesh(const PreparedMeshUpload& prepared) {
        if (prepared.vertices.empty() || prepared.indices.empty()) {
            return false;
        }

        GpuMesh gpuMesh{};
        gpuMesh.localBoundsCenter = prepared.boundsCenter;
        gpuMesh.localBoundsRadius = prepared.boundsRadius;

        const std::size_t vertexBufferSize = sizeof(Vertex) * prepared.vertices.size();
        const std::size_t indexBufferSize = sizeof(std::uint32_t) * prepared.indices.size();

        D3D12_HEAP_PROPERTIES defaultHeapProps{};
        defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_HEAP_PROPERTIES uploadHeapProps{};
        uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC vertexDesc = makeBufferDesc(vertexBufferSize);
        throwIfFailed(device->CreateCommittedResource(&defaultHeapProps,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &vertexDesc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr,
                                                      IID_PPV_ARGS(&gpuMesh.vertexBuffer)),
                      "Failed to create vertex buffer");
        ComPtr<ID3D12Resource> vertexUploadBuffer;
        throwIfFailed(device->CreateCommittedResource(&uploadHeapProps,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &vertexDesc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr,
                                                      IID_PPV_ARGS(&vertexUploadBuffer)),
                      "Failed to create vertex upload buffer");

        void* vertexData = nullptr;
        throwIfFailed(vertexUploadBuffer->Map(0, nullptr, &vertexData), "Failed to map vertex upload buffer");
        std::memcpy(vertexData, prepared.vertices.data(), vertexBufferSize);
        vertexUploadBuffer->Unmap(0, nullptr);

        gpuMesh.vertexBufferView.BufferLocation = gpuMesh.vertexBuffer->GetGPUVirtualAddress();
        gpuMesh.vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBufferSize);
        gpuMesh.vertexBufferView.StrideInBytes = sizeof(Vertex);

        D3D12_RESOURCE_DESC indexDesc = makeBufferDesc(indexBufferSize);
        throwIfFailed(device->CreateCommittedResource(&defaultHeapProps,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &indexDesc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr,
                                                      IID_PPV_ARGS(&gpuMesh.indexBuffer)),
                      "Failed to create index buffer");
        ComPtr<ID3D12Resource> indexUploadBuffer;
        throwIfFailed(device->CreateCommittedResource(&uploadHeapProps,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &indexDesc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr,
                                                      IID_PPV_ARGS(&indexUploadBuffer)),
                      "Failed to create index upload buffer");

        void* indexData = nullptr;
        throwIfFailed(indexUploadBuffer->Map(0, nullptr, &indexData), "Failed to map index upload buffer");
        std::memcpy(indexData, prepared.indices.data(), indexBufferSize);
        indexUploadBuffer->Unmap(0, nullptr);

        beginUploadRecording();
        uploadCommandList->CopyBufferRegion(gpuMesh.vertexBuffer.Get(), 0, vertexUploadBuffer.Get(), 0, vertexBufferSize);
        uploadCommandList->CopyBufferRegion(gpuMesh.indexBuffer.Get(), 0, indexUploadBuffer.Get(), 0, indexBufferSize);

        std::array<D3D12_RESOURCE_BARRIER, 2> toRenderBuffers{};
        toRenderBuffers[0] = makeTransitionBarrier(gpuMesh.vertexBuffer.Get(),
                                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                                   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        toRenderBuffers[1] = makeTransitionBarrier(gpuMesh.indexBuffer.Get(),
                                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                                   D3D12_RESOURCE_STATE_INDEX_BUFFER);
        uploadCommandList->ResourceBarrier(static_cast<UINT>(toRenderBuffers.size()), toRenderBuffers.data());
        enqueueUploadResource(std::move(vertexUploadBuffer));
        enqueueUploadResource(std::move(indexUploadBuffer));

        gpuMesh.indexBufferView.BufferLocation = gpuMesh.indexBuffer->GetGPUVirtualAddress();
        gpuMesh.indexBufferView.SizeInBytes = static_cast<UINT>(indexBufferSize);
        gpuMesh.indexBufferView.Format = DXGI_FORMAT_R32_UINT;
        gpuMesh.indexCount = static_cast<UINT>(prepared.indices.size());
        gpuMeshes[prepared.handle] = std::move(gpuMesh);
        return true;
    }

std::uint32_t Dx12Renderer::Impl::registerMeshInternal(const assets::MeshData& mesh) {
        if (mesh.vertices.empty() || mesh.indices.empty()) {
            throw std::runtime_error("Mesh registration failed: empty vertex/index data");
        }

        // Return handle immediately; mesh becomes renderable after queued upload is processed.
        const std::uint32_t handle = nextMeshHandle++;
        pendingMeshHandles.insert(handle);
        StreamMeshRequest request{};
        request.handle = handle;
        request.mesh = mesh;
        enqueueStreamUploadRequest(request);
        return handle;
    }

void Dx12Renderer::Impl::createDefaultMesh() {
        assets::MeshData defaultMesh{};
        defaultMesh.vertices = {
            {{-0.5F, -0.5F, -0.5F}, {-1.0F, -1.0F, -1.0F}, 0.0F, 0.0F},
            {{-0.5F, 0.5F, -0.5F}, {-1.0F, 1.0F, -1.0F}, 0.0F, 1.0F},
            {{0.5F, 0.5F, -0.5F}, {1.0F, 1.0F, -1.0F}, 1.0F, 1.0F},
            {{0.5F, -0.5F, -0.5F}, {1.0F, -1.0F, -1.0F}, 1.0F, 0.0F},
            {{-0.5F, -0.5F, 0.5F}, {-1.0F, -1.0F, 1.0F}, 0.0F, 0.0F},
            {{-0.5F, 0.5F, 0.5F}, {-1.0F, 1.0F, 1.0F}, 0.0F, 1.0F},
            {{0.5F, 0.5F, 0.5F}, {1.0F, 1.0F, 1.0F}, 1.0F, 1.0F},
            {{0.5F, -0.5F, 0.5F}, {1.0F, -1.0F, 1.0F}, 1.0F, 0.0F},
        };

        defaultMesh.indices = {
            0, 1, 2, 0, 2, 3,
            4, 6, 5, 4, 7, 6,
            4, 5, 1, 4, 1, 0,
            3, 2, 6, 3, 6, 7,
            1, 5, 6, 1, 6, 2,
            4, 0, 3, 4, 3, 7,
        };

        defaultMeshHandle = nextMeshHandle++;
        const PreparedMeshUpload prepared = prepareMeshUpload(defaultMeshHandle, defaultMesh);
        if (!uploadPreparedMesh(prepared)) {
            throw std::runtime_error("Failed to upload default mesh");
        }
        // Default assets must be ready synchronously because they are used as fallback immediately.
        flushUploadCommands();
        waitForPendingUploads();
    }

Dx12Renderer::Impl::PreparedTextureUpload Dx12Renderer::Impl::prepareTextureUpload(
    std::uint32_t handle, std::uint32_t descriptorIndex, const assets::TextureData& textureData) const {
        if (textureData.width == 0 || textureData.height == 0 || textureData.rgba.empty()) {
            throw std::runtime_error("Texture registration failed: empty texture data");
        }

        const std::size_t expectedBytes =
            static_cast<std::size_t>(textureData.width) * static_cast<std::size_t>(textureData.height) * 4;
        if (textureData.rgba.size() < expectedBytes) {
            throw std::runtime_error("Texture registration failed: invalid RGBA payload length");
        }

        PreparedTextureUpload prepared{};
        prepared.handle = handle;
        prepared.descriptorIndex = descriptorIndex;
        prepared.width = textureData.width;
        prepared.height = textureData.height;
        prepared.rgba = textureData.rgba;
        return prepared;
    }

bool Dx12Renderer::Impl::uploadPreparedTexture(const PreparedTextureUpload& prepared) {
        if (prepared.width == 0 || prepared.height == 0 || prepared.rgba.empty()) {
            return false;
        }

        GpuTexture gpuTexture{};

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC textureDesc{};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Alignment = 0;
        textureDesc.Width = prepared.width;
        textureDesc.Height = prepared.height;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.SampleDesc.Quality = 0;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        throwIfFailed(device->CreateCommittedResource(&defaultHeap,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &textureDesc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr,
                                                      IID_PPV_ARGS(&gpuTexture.texture)),
                      "Failed to create GPU texture resource");

        UINT64 uploadBufferSize = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT numRows = 0;
        UINT64 rowSizeInBytes = 0;
        device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &uploadBufferSize);
        (void)rowSizeInBytes;

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC uploadDesc = makeBufferDesc(uploadBufferSize);

        ComPtr<ID3D12Resource> uploadBuffer;
        throwIfFailed(device->CreateCommittedResource(&uploadHeap,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &uploadDesc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr,
                                                      IID_PPV_ARGS(&uploadBuffer)),
                      "Failed to create texture upload buffer");

        std::uint8_t* mappedUpload = nullptr;
        throwIfFailed(uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mappedUpload)),
                      "Failed to map texture upload buffer");

        const std::size_t sourceRowPitch = static_cast<std::size_t>(prepared.width) * 4;
        for (UINT row = 0; row < numRows; ++row) {
            std::uint8_t* destination = mappedUpload + footprint.Offset + row * footprint.Footprint.RowPitch;
            const std::uint8_t* source = prepared.rgba.data() + static_cast<std::size_t>(row) * sourceRowPitch;
            std::memcpy(destination, source, sourceRowPitch);
        }
        uploadBuffer->Unmap(0, nullptr);

        beginUploadRecording();

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = gpuTexture.texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = uploadBuffer.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;

        uploadCommandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        const auto toPixelShader = makeTransitionBarrier(gpuTexture.texture.Get(),
                                                         D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        uploadCommandList->ResourceBarrier(1, &toPixelShader);
        enqueueUploadResource(std::move(uploadBuffer));

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += static_cast<SIZE_T>(prepared.descriptorIndex) * srvDescriptorSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0F;
        device->CreateShaderResourceView(gpuTexture.texture.Get(), &srvDesc, cpuHandle);
        gpuTexture.descriptorIndex = prepared.descriptorIndex;
        gpuTextures[prepared.handle] = std::move(gpuTexture);
        return true;
    }

std::uint32_t Dx12Renderer::Impl::registerTextureInternal(const assets::TextureData& textureData) {
        if (nextSrvIndex >= kMaxTextures) {
            throw std::runtime_error("Texture registration failed: SRV heap capacity reached");
        }

        // Return handle + reserved descriptor immediately; pixel data is streamed in background.
        const std::uint32_t handle = nextTextureHandle++;
        const std::uint32_t descriptorIndex = nextSrvIndex++;
        pendingTextureHandles.insert(handle);

        StreamTextureRequest request{};
        request.handle = handle;
        request.descriptorIndex = descriptorIndex;
        request.texture = textureData;
        enqueueStreamUploadRequest(request);
        return handle;
    }

void Dx12Renderer::Impl::createDefaultTexture() {
        assets::TextureData defaultTexture{};
        defaultTexture.width = 2;
        defaultTexture.height = 2;
        defaultTexture.rgba = {
            255, 255, 255, 255, 40, 40, 40, 255,
            40, 40, 40, 255, 255, 255, 255, 255,
        };
        if (nextSrvIndex >= kMaxTextures) {
            throw std::runtime_error("Default texture registration failed: SRV heap capacity reached");
        }

        defaultTextureHandle = nextTextureHandle++;
        const std::uint32_t descriptorIndex = nextSrvIndex++;
        const PreparedTextureUpload prepared =
            prepareTextureUpload(defaultTextureHandle, descriptorIndex, defaultTexture);
        if (!uploadPreparedTexture(prepared)) {
            throw std::runtime_error("Failed to upload default texture");
        }
        // Default assets must be ready synchronously because they are used as fallback immediately.
        flushUploadCommands();
        waitForPendingUploads();
    }

std::uint32_t Dx12Renderer::Impl::registerMaterialInternal(const MaterialDesc& materialDesc) {
        // Material may reference a streamed texture; keep handle if it is pending.
        Material material{};
        material.textureHandle = materialDesc.textureHandle;
        if (material.textureHandle == 0 ||
            (!gpuTextures.contains(material.textureHandle) &&
             !pendingTextureHandles.contains(material.textureHandle))) {
            material.textureHandle = defaultTextureHandle;
        }
        material.baseColor = materialDesc.baseColor;
        const std::uint32_t handle = nextMaterialHandle++;
        materials.emplace(handle, material);
        return handle;
    }

void Dx12Renderer::Impl::createDefaultMaterial() {
        // Neutral fallback material used whenever user material/texture is unavailable.
        MaterialDesc materialDesc{};
        materialDesc.textureHandle = defaultTextureHandle;
        materialDesc.baseColor = {1.0F, 1.0F, 1.0F};
        defaultMaterialHandle = registerMaterialInternal(materialDesc);
    }

void Dx12Renderer::Impl::createConstantBuffer() {
        // One upload buffer holds per-frame/per-draw constants for all frames in flight.
        const std::size_t totalSize = kFrameCount * kMaxConstantEntries * kConstantBufferStride;

        D3D12_HEAP_PROPERTIES uploadHeapProps{};
        uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC cbDesc = makeBufferDesc(totalSize);
        throwIfFailed(device->CreateCommittedResource(&uploadHeapProps,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &cbDesc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr,
                                                      IID_PPV_ARGS(&constantBuffer)),
                      "Failed to create constant buffer");

        void* mappedData = nullptr;
        throwIfFailed(constantBuffer->Map(0, nullptr, &mappedData), "Failed to map constant buffer");
        mappedConstantData = static_cast<std::uint8_t*>(mappedData);
    }

void Dx12Renderer::Impl::createDebugResources() {
        // Persistently mapped upload buffer for dynamic debug line vertices.
        const std::size_t bufferSize = sizeof(DebugVertex) * kMaxDebugLineVertices;

        D3D12_HEAP_PROPERTIES uploadHeapProps{};
        uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufferDesc = makeBufferDesc(bufferSize);

        throwIfFailed(device->CreateCommittedResource(&uploadHeapProps,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &bufferDesc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr,
                                                      IID_PPV_ARGS(&debugLineVertexBuffer)),
                      "Failed to create debug line buffer");

        void* mappedData = nullptr;
        throwIfFailed(debugLineVertexBuffer->Map(0, nullptr, &mappedData), "Failed to map debug line buffer");
        mappedDebugLineVertices = static_cast<std::uint8_t*>(mappedData);
    }

void Dx12Renderer::Impl::createIndirectMainDrawResources() {
        // Allocate buffers required for GPU-built indirect main pass draws.
        indirectMainDrawReady = false;
        mappedMainDrawMetadata = nullptr;

        if (!mainDrawCommandSignature || !computeMainBuildPipelineState || !computeMainBuildRootSignature) {
            std::cout << "[IndirectMain] Disabled: command signature / build pipeline unavailable\n";
            return;
        }

        try {
            const std::size_t indirectBufferSize = sizeof(GpuMainIndirectCommand) * kMaxDrawsPerFrame;
            const std::size_t metadataBufferSize = sizeof(GpuMainDrawMetadata) * kMaxDrawsPerFrame;
            const std::size_t countBufferSize = sizeof(std::uint32_t);

            D3D12_HEAP_PROPERTIES defaultHeapProps{};
            defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC indirectDesc = makeBufferDesc(indirectBufferSize);
            indirectDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            throwIfFailed(device->CreateCommittedResource(&defaultHeapProps,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &indirectDesc,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                          nullptr,
                                                          IID_PPV_ARGS(&mainDrawIndirectArgumentBuffer)),
                          "Failed to create main indirect argument buffer");

            D3D12_RESOURCE_DESC countDesc = makeBufferDesc(countBufferSize);
            countDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            throwIfFailed(device->CreateCommittedResource(&defaultHeapProps,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &countDesc,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                          nullptr,
                                                          IID_PPV_ARGS(&mainDrawIndirectCountBuffer)),
                          "Failed to create main indirect count buffer");

            D3D12_HEAP_PROPERTIES uploadHeapProps{};
            uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC metadataDesc = makeBufferDesc(metadataBufferSize);
            throwIfFailed(device->CreateCommittedResource(&uploadHeapProps,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &metadataDesc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ,
                                                          nullptr,
                                                          IID_PPV_ARGS(&mainDrawMetadataBuffer)),
                          "Failed to create main draw metadata buffer");

            void* mappedData = nullptr;
            throwIfFailed(mainDrawMetadataBuffer->Map(0, nullptr, &mappedData),
                          "Failed to map main draw metadata buffer");
            mappedMainDrawMetadata = static_cast<GpuMainDrawMetadata*>(mappedData);
            std::memset(mappedMainDrawMetadata, 0, metadataBufferSize);

            indirectMainDrawReady = true;
            std::cout << "[IndirectMain] Enabled gpu-driven path maxDraws=" << kMaxDrawsPerFrame << '\n';
        } catch (const std::exception& ex) {
            std::cout << "[IndirectMain] Disabled: " << ex.what() << '\n';
            mappedMainDrawMetadata = nullptr;
            mainDrawMetadataBuffer.Reset();
            mainDrawIndirectCountBuffer.Reset();
            mainDrawIndirectArgumentBuffer.Reset();
            indirectMainDrawReady = false;
        }
    }

void Dx12Renderer::Impl::createComputeCullingResources() {
        // Allocate compute resources for GPU frustum culling and compacted index output.
        gpuFrustumCullingReady = false;
        gpuFrustumCullFailureLogged = false;
        mappedComputeCullSpheres = nullptr;
        gpuFrustumResultPending.fill(false);
        gpuFrustumValidSphereCount.fill(0);
        gpuFrustumDispatchGroups.fill(0);

        if (!computeCullPipelineState || !computeCullRootSignature) {
            std::cout << "[DispatchGPU] Disabled: compute pipeline not available\n";
            return;
        }

        try {
            const std::size_t sphereBufferSize = sizeof(GpuCullSphere) * kMaxDrawsPerFrame;
            const std::size_t visibleBufferSize = sizeof(std::uint32_t) * kMaxDrawsPerFrame;
            const std::size_t compactIndexBufferSize = sizeof(std::uint32_t) * kMaxDrawsPerFrame;
            const std::size_t compactCounterBufferSize = sizeof(std::uint32_t);

            D3D12_HEAP_PROPERTIES uploadHeapProps{};
            uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC sphereDesc = makeBufferDesc(sphereBufferSize);
            throwIfFailed(device->CreateCommittedResource(&uploadHeapProps,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &sphereDesc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ,
                                                          nullptr,
                                                          IID_PPV_ARGS(&computeCullSphereBuffer)),
                          "Failed to create frustum sphere upload buffer");

            void* mappedData = nullptr;
            throwIfFailed(computeCullSphereBuffer->Map(0, nullptr, &mappedData),
                          "Failed to map frustum sphere upload buffer");
            mappedComputeCullSpheres = static_cast<GpuCullSphere*>(mappedData);
            std::memset(mappedComputeCullSpheres, 0, sphereBufferSize);
            for (std::size_t i = 0; i < kMaxDrawsPerFrame; ++i) {
                mappedComputeCullSpheres[i].radius = -1.0F;
            }

            D3D12_HEAP_PROPERTIES defaultHeapProps{};
            defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC visibleDesc = makeBufferDesc(visibleBufferSize);
            visibleDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            throwIfFailed(device->CreateCommittedResource(&defaultHeapProps,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &visibleDesc,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                          nullptr,
                                                          IID_PPV_ARGS(&computeCullVisibleBuffer)),
                          "Failed to create frustum visibility UAV buffer");

            D3D12_RESOURCE_DESC compactIndexDesc = makeBufferDesc(compactIndexBufferSize);
            compactIndexDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            throwIfFailed(device->CreateCommittedResource(&defaultHeapProps,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &compactIndexDesc,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                          nullptr,
                                                          IID_PPV_ARGS(&computeCullCompactIndexBuffer)),
                          "Failed to create frustum compact index UAV buffer");

            D3D12_RESOURCE_DESC compactCounterDesc = makeBufferDesc(compactCounterBufferSize);
            compactCounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            throwIfFailed(device->CreateCommittedResource(&defaultHeapProps,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &compactCounterDesc,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                          nullptr,
                                                          IID_PPV_ARGS(&computeCullCompactCounterBuffer)),
                          "Failed to create frustum compact counter UAV buffer");

            D3D12_RESOURCE_DESC counterResetDesc = makeBufferDesc(compactCounterBufferSize);
            throwIfFailed(device->CreateCommittedResource(&uploadHeapProps,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &counterResetDesc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ,
                                                          nullptr,
                                                          IID_PPV_ARGS(&computeCullCounterResetUploadBuffer)),
                          "Failed to create frustum counter reset upload buffer");
            void* mappedCounterReset = nullptr;
            throwIfFailed(computeCullCounterResetUploadBuffer->Map(0, nullptr, &mappedCounterReset),
                          "Failed to map frustum counter reset upload buffer");
            mappedComputeCullCounterReset = static_cast<std::uint32_t*>(mappedCounterReset);
            *mappedComputeCullCounterReset = 0U;
            gpuFrustumResultPending.fill(false);
            gpuFrustumValidSphereCount.fill(0);
            gpuFrustumDispatchGroups.fill(0);

            gpuFrustumCullingReady = true;
            std::cout << "[DispatchGPU] Enabled compute frustum pass groupsz="
                      << kComputeCullThreadGroupSize
                      << " maxDraws=" << kMaxDrawsPerFrame
                      << '\n';
        } catch (const std::exception& ex) {
            if (!gpuFrustumCullFailureLogged) {
                std::cout << "[DispatchGPU] Disabled: " << ex.what() << '\n';
                gpuFrustumCullFailureLogged = true;
            }
            gpuFrustumCullingReady = false;
            if (computeCullSphereBuffer && mappedComputeCullSpheres != nullptr) {
                computeCullSphereBuffer->Unmap(0, nullptr);
            }
            mappedComputeCullSpheres = nullptr;
            if (computeCullCounterResetUploadBuffer && mappedComputeCullCounterReset != nullptr) {
                computeCullCounterResetUploadBuffer->Unmap(0, nullptr);
            }
            mappedComputeCullCounterReset = nullptr;
            computeCullSphereBuffer.Reset();
            computeCullVisibleBuffer.Reset();
            computeCullCompactIndexBuffer.Reset();
            computeCullCompactCounterBuffer.Reset();
            computeCullCounterResetUploadBuffer.Reset();
            gpuFrustumResultPending.fill(false);
            gpuFrustumValidSphereCount.fill(0);
            gpuFrustumDispatchGroups.fill(0);
        }
    }

void Dx12Renderer::Impl::createComputeOcclusionResources() {
        // Allocate compute resources for occlusion stats, visibility lists, and readback.
        gpuOcclusionCullingReady = false;
        gpuOcclusionCullFailureLogged = false;
        mappedComputeOcclusionCamSpace = nullptr;
        mappedComputeOcclusionCandidates = nullptr;
        gpuOcclusionResultPending.fill(false);
        gpuOcclusionCandidateCount.fill(0);
        gpuOcclusionDispatchGroups.fill(0);

        if (!computeOcclusionPipelineState || !computeOcclusionRootSignature) {
            std::cout << "[OcclusionGPU] Disabled: compute pipeline not available\n";
            return;
        }

        try {
            const std::size_t camSpaceUploadSize = sizeof(XMFLOAT4) * kMaxDrawsPerFrame;
            const std::size_t visibleIndexBufferSize = sizeof(std::uint32_t) * kMaxDrawsPerFrame;
            const std::size_t statsBufferSize = sizeof(std::uint32_t) * 8;

            D3D12_HEAP_PROPERTIES uploadHeapProps{};
            uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC camSpaceUploadDesc = makeBufferDesc(camSpaceUploadSize);
            throwIfFailed(device->CreateCommittedResource(&uploadHeapProps,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &camSpaceUploadDesc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ,
                                                          nullptr,
                                                          IID_PPV_ARGS(&computeOcclusionCamSpaceUploadBuffer)),
                          "Failed to create occlusion cam-space upload buffer");
            void* mappedCamSpace = nullptr;
            throwIfFailed(computeOcclusionCamSpaceUploadBuffer->Map(0, nullptr, &mappedCamSpace),
                          "Failed to map occlusion cam-space upload buffer");
            mappedComputeOcclusionCamSpace = static_cast<XMFLOAT4*>(mappedCamSpace);
            std::memset(mappedComputeOcclusionCamSpace, 0, camSpaceUploadSize);

            D3D12_HEAP_PROPERTIES defaultHeapProps{};
            defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC visibleIndexDesc = makeBufferDesc(visibleIndexBufferSize);
            visibleIndexDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            throwIfFailed(device->CreateCommittedResource(&defaultHeapProps,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &visibleIndexDesc,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                          nullptr,
                                                          IID_PPV_ARGS(&computeOcclusionVisibleIndexBuffer)),
                          "Failed to create occlusion visible index UAV buffer");

            D3D12_RESOURCE_DESC statsDesc = makeBufferDesc(statsBufferSize);
            statsDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            throwIfFailed(device->CreateCommittedResource(&defaultHeapProps,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &statsDesc,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                          nullptr,
                                                          IID_PPV_ARGS(&computeOcclusionStatsBuffer)),
                          "Failed to create occlusion stats UAV buffer");

            D3D12_RESOURCE_DESC statsResetDesc = makeBufferDesc(statsBufferSize);
            throwIfFailed(device->CreateCommittedResource(&uploadHeapProps,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &statsResetDesc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ,
                                                          nullptr,
                                                          IID_PPV_ARGS(&computeOcclusionStatsResetUploadBuffer)),
                          "Failed to create occlusion stats reset upload buffer");
            void* mappedStatsReset = nullptr;
            throwIfFailed(computeOcclusionStatsResetUploadBuffer->Map(0, nullptr, &mappedStatsReset),
                          "Failed to map occlusion stats reset upload buffer");
            std::memset(mappedStatsReset, 0, statsBufferSize);
            computeOcclusionStatsResetUploadBuffer->Unmap(0, nullptr);

            D3D12_HEAP_PROPERTIES readbackHeapProps{};
            readbackHeapProps.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC statsReadbackDesc = makeBufferDesc(statsBufferSize);
            for (std::uint32_t i = 0; i < kFrameCount; ++i) {
                throwIfFailed(device->CreateCommittedResource(&readbackHeapProps,
                                                              D3D12_HEAP_FLAG_NONE,
                                                              &statsReadbackDesc,
                                                              D3D12_RESOURCE_STATE_COPY_DEST,
                                                              nullptr,
                                                              IID_PPV_ARGS(&computeOcclusionStatsReadbackBuffers[i])),
                              "Failed to create occlusion stats readback buffer");
            }
            gpuOcclusionResultPending.fill(false);
            gpuOcclusionCandidateCount.fill(0);
            gpuOcclusionDispatchGroups.fill(0);

            gpuOcclusionCullingReady = true;
            std::cout << "[OcclusionGPU] Enabled candidates=" << kMaxDrawsPerFrame
                      << " tiles=" << kOcclusionTilesX << "x" << kOcclusionTilesY
                      << '\n';
        } catch (const std::exception& ex) {
            if (!gpuOcclusionCullFailureLogged) {
                std::cout << "[OcclusionGPU] Disabled: " << ex.what() << '\n';
                gpuOcclusionCullFailureLogged = true;
            }
            if (computeOcclusionCamSpaceUploadBuffer && mappedComputeOcclusionCamSpace != nullptr) {
                computeOcclusionCamSpaceUploadBuffer->Unmap(0, nullptr);
            }
            mappedComputeOcclusionCamSpace = nullptr;
            mappedComputeOcclusionCandidates = nullptr;
            computeOcclusionCamSpaceUploadBuffer.Reset();
            computeOcclusionCandidateUploadBuffer.Reset();
            computeOcclusionVisibleIndexBuffer.Reset();
            computeOcclusionStatsBuffer.Reset();
            computeOcclusionStatsResetUploadBuffer.Reset();
            for (auto& readback : computeOcclusionStatsReadbackBuffers) {
                readback.Reset();
            }
            gpuOcclusionResultPending.fill(false);
            gpuOcclusionCandidateCount.fill(0);
            gpuOcclusionDispatchGroups.fill(0);
            gpuOcclusionCullingReady = false;
        }
    }



}  // namespace engine::render

#endif  // _WIN32

