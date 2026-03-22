// File: AssetManager.h
// Purpose: Declares the asset loading and caching interface for meshes and textures.

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include "engine/assets/AssetTypes.h"

namespace engine::assets {

// Centralized asset loader with weak-cache reuse semantics.
class AssetManager {
public:
    explicit AssetManager(std::filesystem::path rootPath = ".");

    // Load mesh/texture and cache the resulting shared objects by canonicalized key.
    std::shared_ptr<const MeshData> loadMeshObj(const std::filesystem::path& relativePath);
    std::shared_ptr<const TextureData> loadTexturePpm(const std::filesystem::path& relativePath);

    // Drop expired weak entries to keep cache maps compact.
    void releaseUnused();
    std::size_t meshCacheSize() const;
    std::size_t textureCacheSize() const;

private:
    // Resolve caller-relative path under the configured asset root.
    std::filesystem::path resolvePath(const std::filesystem::path& relativePath) const;
    // Convert a file path into a stable cache key.
    std::string toKey(const std::filesystem::path& path) const;

    std::filesystem::path rootPath_;
    std::unordered_map<std::string, std::weak_ptr<const MeshData>> meshCache_;
    std::unordered_map<std::string, std::weak_ptr<const TextureData>> textureCache_;
};

}  // namespace engine::assets

