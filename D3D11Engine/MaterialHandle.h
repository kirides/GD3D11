#pragma once
#include <cstdint>

struct MaterialHandle {
    struct IdxGen {
        uint32_t index      : 24;
        uint32_t generation : 8; // zero gen means invalid
    };
    
    union {
        uint32_t handle = 0;
        IdxGen idxgen;
    };

    bool isValid() const { return handle != 0; }
    
    MaterialHandle() noexcept: handle(0) {}
    MaterialHandle(uint32_t idx, uint8_t gen) noexcept {
        idxgen.index = idx;
        idxgen.generation = gen;
    }

    bool operator>(const MaterialHandle& material) const { return handle > material.handle; }
    bool operator<(const MaterialHandle& material) const { return handle < material.handle; }
    bool operator==(const MaterialHandle& material) const { return handle == material.handle; }
    
    explicit operator bool() const { return isValid(); }
};
