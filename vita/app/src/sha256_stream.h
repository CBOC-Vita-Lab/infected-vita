// sha256_stream.h - incremental SHA-256 (psprecomp only offers whole buffers and files; the ISO is 590 MB and is read in chunks).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace app {

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t size);
    std::string hex_digest();   // finishes the hash; call reset() to reuse

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t fill_ = 0;
    uint64_t total_ = 0;
};

}  // namespace app
