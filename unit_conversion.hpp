#ifndef TWRP_UNIT_CONVERSION_HPP
#define TWRP_UNIT_CONVERSION_HPP

#include <cstdint>
#include <string>

// Magnitude base for byte formatting. kDecimal uses powers of 1000
// (1 KB == 1000 B); kBinary uses powers of 1024 (1 KB == 1024 B, the
// convention used throughout TWRP). Labels are always the SI forms
// (B, KB, MB, ...) regardless of base. ByteBase is an alias for
// std::uint64_t, so kDecimal/kBinary convert to it with no static_cast.
using ByteBase = uint64_t;

enum : ByteBase {
    kDecimal = 1000,
    kBinary = 1024,
};

// Formats an exact byte count as a human-readable string, automatically
// choosing the nearest unit. Labels: B, KB, MB, GB, TB, PB. Precision is
// adaptive to the value's magnitude: <10 keeps 2 decimals, <100 keeps 1,
// >=100 keeps none.
//   FormatBytes(1024)                  == "1.00 KB"   (binary, default)
//   FormatBytes(1000, kDecimal)         == "1.00 KB"   (decimal)
//   FormatBytes(16106127360)            == "15.0 GB"   (binary)
//   FormatBytes(16106127360, kDecimal)  == "16.1 GB"   (decimal)
//   FormatBytes(1ull << 60)             == "1024 PB"   (binary)
class UnitConversion {
public:
    static std::string FormatBytes(std::uint64_t bytes, ByteBase base = kBinary);

    // Like FormatBytes, but appends "/s" to denote a per-second rate. The
    // "/s" suffix is always ASCII, regardless of locale or language.
    //   FormatBytesPerSecond(1536)      == "1.50 KB/s"  (binary, default)
    //   FormatBytesPerSecond(0)          == "0 B/s"
    //   FormatBytesPerSecond(102400)     == "100 KB/s"
    static std::string FormatBytesPerSecond(std::uint64_t bytes_per_sec, ByteBase base = kBinary);

private:
    UnitConversion() = delete;
};

#endif  // TWRP_UNIT_CONVERSION_HPP
