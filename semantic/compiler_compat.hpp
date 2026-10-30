#ifndef NYLON_SEMANTIC_COMPILER_COMPAT_HPP
#define NYLON_SEMANTIC_COMPILER_COMPAT_HPP

#include <array>
#include <cctype>
#include <concepts>
#include <numeric>
#include <ostream>
#include <string>
#include <type_traits>

// LLVM libc++ uses a 128-bit representation for Windows file_time_type.
// It intentionally has no standard ostream overload. Keep the source
// fingerprint lossless without depending on a compiler-specific spelling.
template <typename T>
requires (
    sizeof(T) > sizeof(unsigned long long) &&
    requires(T value)
    {
        { value < 0 } -> std::convertible_to<bool>;
        value / 10;
        value % 10;
        -value;
    })
inline std::ostream& operator<<(std::ostream& stream, T value)
{
    using unsigned_t = std::make_unsigned_t<T>;
    const bool negative = value < 0;
    unsigned_t magnitude{};
    if (negative)
    {
        // Avoid signed-min overflow: -(value + 1) is representable.
        magnitude = static_cast<unsigned_t>(-(value + 1));
        ++magnitude;
    }
    else
    {
        magnitude = static_cast<unsigned_t>(value);
    }

    char digits[64]{};
    char* cursor = digits + sizeof(digits);
    do
    {
        const auto digit = static_cast<unsigned>(magnitude % static_cast<unsigned_t>(10));
        *--cursor = static_cast<char>('0' + digit);
        magnitude /= static_cast<unsigned_t>(10);
    } while (magnitude != 0);
    if (negative) *--cursor = '-';
    return stream.write(cursor, static_cast<std::streamsize>((digits + sizeof(digits)) - cursor));
}

#endif
