// File: AssetManager.cpp
// Purpose: Implements OBJ/PPM parsing, cache lookup, and path resolution for runtime assets.

#include "engine/assets/AssetManager.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::assets {

namespace {

// OBJ triplet (position/uv/normal) index key.
struct ObjIndex {
    int v = 0;
    int vt = 0;
    int vn = 0;

    bool operator==(const ObjIndex& rhs) const {
        return v == rhs.v && vt == rhs.vt && vn == rhs.vn;
    }
};

// Hash for ObjIndex so it can be used in unordered_map dedup tables.
struct ObjIndexHash {
    std::size_t operator()(const ObjIndex& index) const noexcept {
        std::size_t hash = static_cast<std::size_t>(index.v);
        hash = (hash * 1315423911U) ^ static_cast<std::size_t>(index.vt);
        hash = (hash * 1315423911U) ^ static_cast<std::size_t>(index.vn);
        return hash;
    }
};

// OBJ indices are 1-based for positive and relative-to-end for negative values.
int resolveObjIndex(int rawIndex, std::size_t count) {
    if (rawIndex > 0) {
        const int resolved = rawIndex - 1;
        if (resolved < 0 || static_cast<std::size_t>(resolved) >= count) {
            throw std::runtime_error("OBJ index out of range");
        }
        return resolved;
    }

    if (rawIndex < 0) {
        const int resolved = static_cast<int>(count) + rawIndex;
        if (resolved < 0 || static_cast<std::size_t>(resolved) >= count) {
            throw std::runtime_error("OBJ negative index out of range");
        }
        return resolved;
    }

    throw std::runtime_error("OBJ index cannot be zero");
}

// Parse one face token such as "1/2/3", "1//3", or "1/2".
ObjIndex parseObjIndexToken(const std::string& token) {
    ObjIndex result{};

    std::size_t firstSlash = token.find('/');
    if (firstSlash == std::string::npos) {
        result.v = std::stoi(token);
        return result;
    }

    result.v = std::stoi(token.substr(0, firstSlash));
    std::size_t secondSlash = token.find('/', firstSlash + 1);
    if (secondSlash == std::string::npos) {
        const std::string vtToken = token.substr(firstSlash + 1);
        if (!vtToken.empty()) {
            result.vt = std::stoi(vtToken);
        }
        return result;
    }

    const std::string vtToken = token.substr(firstSlash + 1, secondSlash - firstSlash - 1);
    if (!vtToken.empty()) {
        result.vt = std::stoi(vtToken);
    }

    const std::string vnToken = token.substr(secondSlash + 1);
    if (!vnToken.empty()) {
        result.vn = std::stoi(vnToken);
    }

    return result;
}

// PPM allows comment lines in headers; consume them before reading each scalar.
void skipPpmComments(std::istream& input) {
    while (true) {
        input >> std::ws;
        if (input.peek() != '#') {
            return;
        }
        std::string comment;
        std::getline(input, comment);
    }
}

template <typename T>
T readPpmValue(std::istream& input) {
    skipPpmComments(input);
    T value{};
    input >> value;
    if (!input) {
        throw std::runtime_error("Invalid PPM header");
    }
    return value;
}

// Build indexed mesh data from Wavefront OBJ text.
MeshData parseObjFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Failed to open OBJ file: " + path.string());
    }

    std::vector<math::Vec3> positions;
    std::vector<math::Vec3> normals;
    std::vector<std::pair<float, float>> texcoords;

    MeshData mesh;
    std::unordered_map<ObjIndex, std::uint32_t, ObjIndexHash> vertexMap;
    std::vector<math::Vec3> generatedNormals;
    std::vector<bool> normalFromFile;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::istringstream lineStream(line);
        std::string prefix;
        lineStream >> prefix;
        if (prefix == "v") {
            float x = 0.0F;
            float y = 0.0F;
            float z = 0.0F;
            lineStream >> x >> y >> z;
            positions.emplace_back(x, y, z);
        } else if (prefix == "vn") {
            float x = 0.0F;
            float y = 0.0F;
            float z = 0.0F;
            lineStream >> x >> y >> z;
            normals.emplace_back(x, y, z);
        } else if (prefix == "vt") {
            float u = 0.0F;
            float v = 0.0F;
            lineStream >> u >> v;
            texcoords.emplace_back(u, v);
        } else if (prefix == "f") {
            std::vector<ObjIndex> faceIndices;
            std::string token;
            while (lineStream >> token) {
                faceIndices.push_back(parseObjIndexToken(token));
            }
            if (faceIndices.size() < 3) {
                continue;
            }

            auto getVertexIndex = [&](const ObjIndex& index) -> std::uint32_t {
                auto existing = vertexMap.find(index);
                if (existing != vertexMap.end()) {
                    return existing->second;
                }

                MeshVertex vertex{};
                const int posIndex = resolveObjIndex(index.v, positions.size());
                vertex.position = positions[static_cast<std::size_t>(posIndex)];

                if (index.vt != 0) {
                    const int uvIndex = resolveObjIndex(index.vt, texcoords.size());
                    const auto [u, v] = texcoords[static_cast<std::size_t>(uvIndex)];
                    vertex.u = u;
                    vertex.v = v;
                }

                bool hasNormal = false;
                if (index.vn != 0) {
                    const int normalIndex = resolveObjIndex(index.vn, normals.size());
                    vertex.normal = math::normalize(normals[static_cast<std::size_t>(normalIndex)]);
                    hasNormal = true;
                }

                const std::uint32_t newIndex = static_cast<std::uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(vertex);
                generatedNormals.emplace_back(0.0F, 0.0F, 0.0F);
                normalFromFile.push_back(hasNormal);
                vertexMap.emplace(index, newIndex);
                return newIndex;
            };

            // Fan triangulation for faces with more than 3 vertices.
            for (std::size_t tri = 1; tri + 1 < faceIndices.size(); ++tri) {
                const std::uint32_t i0 = getVertexIndex(faceIndices[0]);
                const std::uint32_t i1 = getVertexIndex(faceIndices[tri]);
                const std::uint32_t i2 = getVertexIndex(faceIndices[tri + 1]);

                mesh.indices.push_back(i0);
                mesh.indices.push_back(i1);
                mesh.indices.push_back(i2);

                const auto& p0 = mesh.vertices[i0].position;
                const auto& p1 = mesh.vertices[i1].position;
                const auto& p2 = mesh.vertices[i2].position;

                const math::Vec3 faceNormal = math::normalize(math::cross(p1 - p0, p2 - p0));
                if (!normalFromFile[i0]) {
                    generatedNormals[i0] += faceNormal;
                }
                if (!normalFromFile[i1]) {
                    generatedNormals[i1] += faceNormal;
                }
                if (!normalFromFile[i2]) {
                    generatedNormals[i2] += faceNormal;
                }
            }
        }
    }

    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (!normalFromFile[i]) {
            mesh.vertices[i].normal = math::normalize(generatedNormals[i]);
        }
    }

    if (mesh.vertices.empty() || mesh.indices.empty()) {
        throw std::runtime_error("OBJ produced empty mesh: " + path.string());
    }

    return mesh;
}

// Load P3/P6 PPM into normalized 8-bit RGBA pixels.
TextureData parsePpmFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open PPM file: " + path.string());
    }

    std::string magic;
    file >> magic;
    if (!file || (magic != "P6" && magic != "P3")) {
        throw std::runtime_error("Unsupported PPM format (expected P3/P6): " + path.string());
    }

    const int width = readPpmValue<int>(file);
    const int height = readPpmValue<int>(file);
    const int maxValue = readPpmValue<int>(file);

    if (width <= 0 || height <= 0 || maxValue <= 0) {
        throw std::runtime_error("Invalid PPM dimensions/range: " + path.string());
    }

    TextureData texture{};
    texture.width = static_cast<std::uint32_t>(width);
    texture.height = static_cast<std::uint32_t>(height);
    texture.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);

    auto scaleChannel = [maxValue](int value) -> std::uint8_t {
        value = std::clamp(value, 0, maxValue);
        if (maxValue == 255) {
            return static_cast<std::uint8_t>(value);
        }
        const float scaled = (static_cast<float>(value) / static_cast<float>(maxValue)) * 255.0F;
        return static_cast<std::uint8_t>(std::round(std::clamp(scaled, 0.0F, 255.0F)));
    };

    if (magic == "P6") {
        file.get();
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
        file.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
        if (file.gcount() != static_cast<std::streamsize>(rgb.size())) {
            throw std::runtime_error("Unexpected EOF while reading PPM binary payload: " + path.string());
        }

        for (std::size_t i = 0, out = 0; i < rgb.size(); i += 3, out += 4) {
            texture.rgba[out] = scaleChannel(rgb[i]);
            texture.rgba[out + 1] = scaleChannel(rgb[i + 1]);
            texture.rgba[out + 2] = scaleChannel(rgb[i + 2]);
            texture.rgba[out + 3] = 255;
        }
    } else {
        for (std::size_t out = 0; out < texture.rgba.size(); out += 4) {
            const int r = readPpmValue<int>(file);
            const int g = readPpmValue<int>(file);
            const int b = readPpmValue<int>(file);
            texture.rgba[out] = scaleChannel(r);
            texture.rgba[out + 1] = scaleChannel(g);
            texture.rgba[out + 2] = scaleChannel(b);
            texture.rgba[out + 3] = 255;
        }
    }

    return texture;
}

}  // namespace

AssetManager::AssetManager(std::filesystem::path rootPath) : rootPath_(std::move(rootPath)) {
}

std::shared_ptr<const MeshData> AssetManager::loadMeshObj(const std::filesystem::path& relativePath) {
    const std::filesystem::path fullPath = resolvePath(relativePath);
    const std::string key = toKey(fullPath);

    if (auto it = meshCache_.find(key); it != meshCache_.end()) {
        if (auto cached = it->second.lock()) {
            // Reuse existing mesh instance when still alive.
            return cached;
        }
    }

    auto mesh = std::make_shared<MeshData>(parseObjFile(fullPath));
    meshCache_[key] = mesh;
    return mesh;
}

std::shared_ptr<const TextureData> AssetManager::loadTexturePpm(const std::filesystem::path& relativePath) {
    const std::filesystem::path fullPath = resolvePath(relativePath);
    const std::string key = toKey(fullPath);

    if (auto it = textureCache_.find(key); it != textureCache_.end()) {
        if (auto cached = it->second.lock()) {
            // Reuse existing texture instance when still alive.
            return cached;
        }
    }

    auto texture = std::make_shared<TextureData>(parsePpmFile(fullPath));
    textureCache_[key] = texture;
    return texture;
}

void AssetManager::releaseUnused() {
    // Weak cache entries are removed once all shared owners have released the asset.
    auto eraseExpired = [](auto& cache) {
        for (auto it = cache.begin(); it != cache.end();) {
            if (it->second.expired()) {
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
    };

    eraseExpired(meshCache_);
    eraseExpired(textureCache_);
}

std::size_t AssetManager::meshCacheSize() const {
    return meshCache_.size();
}

std::size_t AssetManager::textureCacheSize() const {
    return textureCache_.size();
}

std::filesystem::path AssetManager::resolvePath(const std::filesystem::path& relativePath) const {
    if (relativePath.is_absolute()) {
        return relativePath.lexically_normal();
    }
    return (rootPath_ / relativePath).lexically_normal();
}

std::string AssetManager::toKey(const std::filesystem::path& path) const {
    return path.lexically_normal().string();
}

}  // namespace engine::assets

