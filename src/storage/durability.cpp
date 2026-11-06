#include "storage/durability.hpp"

#include <array>
#include <string>
#include <utility>

namespace database {

namespace {

struct ModeName {
    std::string_view name;
    DurabilityMode mode;
};

constexpr std::array<ModeName, 8> kModeNames = {{
    {"strict", DurabilityMode::Strict},
    {"sync", DurabilityMode::Strict},
    {"group_commit", DurabilityMode::GroupCommit},
    {"batched", DurabilityMode::GroupCommit},
    {"relaxed", DurabilityMode::Relaxed},
    {"async", DurabilityMode::Relaxed},
    {"memory_only", DurabilityMode::MemoryOnly},
    {"ephemeral", DurabilityMode::MemoryOnly},
}};

char ToLowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (idx_t i = 0; i < a.size(); i++) {
        if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

Result<DurabilityMode> ParseDurabilityMode(std::string_view s) {
    for (const auto& entry : kModeNames) {
        if (EqualsIgnoreCase(s, entry.name)) {
            return entry.mode;
        }
    }
    return MAKE_ERROR(ErrorCode::InvalidArgument,
                      "unknown durability mode '{}' (expected strict|group_commit|relaxed|memory_only)",
                      s);
}

std::string_view ToString(DurabilityMode m) {
    switch (m) {
        case DurabilityMode::Strict:      return "strict";
        case DurabilityMode::GroupCommit: return "group_commit";
        case DurabilityMode::Relaxed:     return "relaxed";
        case DurabilityMode::MemoryOnly:  return "memory_only";
    }
    return "unknown";
}

} // namespace database
