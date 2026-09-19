#include "unit_conversion.hpp"

#include <array>
#include <cstddef>
#include <format>
#include <string_view>

namespace {

// Canonical SI labels, indexed by magnitude tier (0 == B, 1 == KB, ... 5 == PB).
// Shared by every entry point: it is the single source of truth for the
// unit <-> tier mapping, so adding a unit here (e.g. EB) automatically
// extends FormatBytes and FormatBytesPerSecond.
constexpr std::array<std::string_view, 6> kLabels{
    "B", "KB", "MB", "GB", "TB", "PB",
};

// Adaptive precision: keep more digits where they carry meaning.
//   value <  10 -> 2 decimals (e.g. 1.50 KB)
//   value < 100 -> 1 decimal  (e.g. 15.0 GB)
//   value >= 100 -> 0 decimals (e.g. 1024 PB)
int Precision(double value) {
    if (value < 10.0)
        return 2;
    if (value < 100.0)
        return 1;
    return 0;
}

// Core byte formatter: auto-picks the nearest unit, applies adaptive
// precision, and appends suffix ("" for a plain count, "/s" for a per-second
// rate). Shared by FormatBytes and FormatBytesPerSecond so the rate
// formatter cannot drift from the plain formatter.
std::string FormatWithSuffix(std::uint64_t bytes, ByteBase base, std::string_view suffix) {
    // Sub-unit values (and zero) are plain bytes.
    if (bytes < base)
        return std::format("{} B{}", bytes, suffix);
    // Walk up the magnitude tiers while the next divisor (b^(tier+1)) still
    // fits within bytes, capped at the largest label. base is 1000 or 1024,
    // so every divisor up to b^5 stays well within uint64_t.
    std::size_t tier = 1;
    std::uint64_t divisor = base;
    while (tier + 1 < kLabels.size()) {
        const std::uint64_t next = divisor * base;
        if (next > bytes)
            break;
        divisor = next;
        ++tier;
    }
    const double value = static_cast<double>(bytes) / static_cast<double>(divisor);
    const int precision = Precision(value);
    return std::format("{0:.{1}f} {2}{3}", value, precision, kLabels[tier], suffix);
}

}  // namespace

std::string UnitConversion::FormatBytes(std::uint64_t bytes, ByteBase base) {
    return FormatWithSuffix(bytes, base, "");
}

std::string UnitConversion::FormatBytesPerSecond(std::uint64_t bytes_per_sec, ByteBase base) {
    // Always the ASCII "/s" suffix, regardless of locale or language.
    return FormatWithSuffix(bytes_per_sec, base, "/s");
}
