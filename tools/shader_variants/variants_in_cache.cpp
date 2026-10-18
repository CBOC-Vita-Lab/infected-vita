// variants_in_cache: which of the GL renderer's program variants a shipped shader cache already holds.
//
//   variants_in_cache <shader_cache dir> <flags file or log> [...]   > variants.txt
//
// The inputs are variant lists (one 0x... flags value per line) or renderer logs (lines "[gl] program ... flags 0x... linked"). A
// variant is kept when both its vertex and its fragment source hash (vitaGL's cache key: XXH3-64 of the raw source text, printed
// %llX) have a file under <dir>/v2/v/<XX>/ and <dir>/v2/f/<XX>/. The result is a list the renderer reads as the shipped list
// (<data>/shader_cache/variants.txt): the programs to link while the game boots, none of which needs the shader compiler.
// Written to stdout in the list's format; counts go to stderr.
//
// Build (from the PortableKit checkout):
//   g++ -O2 -std=c++20 -IPortableKit/host/gpu/gl -IPortableKit/third_party \
//       -DXXH_INLINE_ALL proj/tools/shader_variants/variants_in_cache.cpp PortableKit/host/gpu/gl/gl_shaders.cpp -o variants_in_cache
#include "gl_shaders.hpp"
#include "xxhash/xxhash.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static bool cached(const std::string &root, char kind, const std::string &source) {
    const unsigned long long hash = XXH3_64bits(source.data(), source.size());
    char name[64];
    std::snprintf(name, sizeof name, "%llX", hash);
    char folder[8];
    std::snprintf(folder, sizeof folder, "%02X", static_cast<unsigned>(hash >> 56u));
    return std::filesystem::exists(std::filesystem::path(root) / "v2" / std::string(1, kind) / folder / (std::string(name) + ".gxp"));
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <shader_cache dir> <flags file or log> [...]\n", argv[0]);
        return 2;
    }
    const std::string root = argv[1];
    std::vector<std::uint32_t> order;
    std::set<std::uint32_t> seen;
    for (int i = 2; i < argc; ++i) {
        std::ifstream in(argv[i]);
        std::string line;
        while (std::getline(in, line)) {
            std::size_t at = line.find(" flags 0x");
            std::uint32_t flags = 0;
            if (at != std::string::npos) {
                flags = static_cast<std::uint32_t>(std::strtoul(line.c_str() + at + 7, nullptr, 16));
            } else if (line.rfind("0x", 0) == 0) {
                flags = static_cast<std::uint32_t>(std::strtoul(line.c_str(), nullptr, 16));
            } else {
                continue;
            }
            if (line.find("FAILED") != std::string::npos) continue;
            if (seen.insert(flags).second) order.push_back(flags);
        }
    }
    std::size_t kept = 0;
    std::printf("# portablekit-gl-variants 1\n");
    for (const std::uint32_t flags : order) {
        if (cached(root, 'v', portablekit::gpu::glsl::vertex_source(flags)) && cached(root, 'f', portablekit::gpu::glsl::fragment_source(flags))) {
            std::printf("0x%x\n", static_cast<unsigned>(flags));
            ++kept;
        }
    }
    std::fprintf(stderr, "%zu of %zu variants are in the cache\n", kept, order.size());
    return 0;
}
