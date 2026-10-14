// Portable 128-bit integers for the interpreter.
//
// The numeric core (checked/saturating arithmetic, rationals, BigInt) needs
// 128-bit intermediates so 64-bit products and wide shifts can be range-checked
// without overflow. GCC and Clang provide `__int128`/`__uint128_t`, but MSVC has
// no 128-bit integer type at all, which broke the Windows CI build the moment
// the BigInt work landed.
//
// This header provides `kirn::i128` and `kirn::u128` implemented purely in
// standard C++ (two 64-bit limbs) so the same code compiles everywhere. It is
// deliberately a drop-in stand-in for the native types: unsigned/signed
// two's-complement arithmetic, truncating division, arithmetic right shift and
// the usual relational/bitwise operators.

#ifndef KIRN_INTERP_WIDE_H
#define KIRN_INTERP_WIDE_H

#include <cstdint>
#include <type_traits>

namespace kirn {

struct u128;
struct i128;

namespace wide_detail {

// 64x64 -> 128 via 32-bit limbs (no compiler 128-bit type required).
inline void mul64(uint64_t a, uint64_t b, uint64_t& lo, uint64_t& hi) {
    uint64_t a0 = a & 0xffffffffu, a1 = a >> 32;
    uint64_t b0 = b & 0xffffffffu, b1 = b >> 32;
    uint64_t p00 = a0 * b0;
    uint64_t p01 = a0 * b1;
    uint64_t p10 = a1 * b0;
    uint64_t p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    lo = (p00 & 0xffffffffu) | (mid << 32);
    hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

} // namespace wide_detail

// Portable replacements for the GCC/Clang builtins the numeric core used to
// rely on. MSVC spells these differently (or lacks them), so provide our own.

inline int popcount64(uint64_t x) noexcept {
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return static_cast<int>((x * 0x0101010101010101ull) >> 56);
}

inline unsigned clz64(uint64_t x) noexcept {
    if (x == 0) return 64u;
    unsigned n = 0;
    if ((x >> 32) == 0) { n += 32; x <<= 32; }
    if ((x >> 48) == 0) { n += 16; x <<= 16; }
    if ((x >> 56) == 0) { n += 8; x <<= 8; }
    if ((x >> 60) == 0) { n += 4; x <<= 4; }
    if ((x >> 62) == 0) { n += 2; x <<= 2; }
    if ((x >> 63) == 0) { n += 1; }
    return n;
}

inline uint64_t bswap64(uint64_t x) noexcept {
    return ((x & 0xff00000000000000ull) >> 56) |
           ((x & 0x00ff000000000000ull) >> 40) |
           ((x & 0x0000ff0000000000ull) >> 24) |
           ((x & 0x000000ff00000000ull) >> 8) |
           ((x & 0x00000000ff000000ull) << 8) |
           ((x & 0x0000000000ff0000ull) << 24) |
           ((x & 0x000000000000ff00ull) << 40) |
           ((x & 0x00000000000000ffull) << 56);
}

struct u128 {
    uint64_t lo = 0;
    uint64_t hi = 0;

    constexpr u128() = default;

    template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    constexpr u128(T v) noexcept {
        if constexpr (std::is_signed_v<T>) {
            int64_t s = static_cast<int64_t>(v);
            lo = static_cast<uint64_t>(s);
            hi = s < 0 ? ~uint64_t(0) : uint64_t(0);
        } else {
            lo = static_cast<uint64_t>(v);
            hi = 0;
        }
    }

    template <typename T,
              typename = std::enable_if_t<std::is_integral_v<T> &&
                                          !std::is_same_v<T, bool>>>
    explicit constexpr operator T() const noexcept {
        return static_cast<T>(lo);
    }
    explicit constexpr operator bool() const noexcept { return lo || hi; }
};

struct i128 {
    uint64_t lo = 0;
    uint64_t hi = 0;

    constexpr i128() = default;

    template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    constexpr i128(T v) noexcept {
        if constexpr (std::is_signed_v<T>) {
            int64_t s = static_cast<int64_t>(v);
            lo = static_cast<uint64_t>(s);
            hi = s < 0 ? ~uint64_t(0) : uint64_t(0);
        } else {
            lo = static_cast<uint64_t>(v);
            hi = 0;
        }
    }

    template <typename T,
              typename = std::enable_if_t<std::is_integral_v<T> &&
                                          !std::is_same_v<T, bool>>>
    explicit constexpr operator T() const noexcept {
        return static_cast<T>(lo);
    }
    explicit constexpr operator bool() const noexcept { return lo || hi; }
};

// ---- bit-level helpers (shared by both signednesses) -----------------------

inline constexpr u128 uMake(uint64_t lo, uint64_t hi) noexcept {
    u128 r;
    r.lo = lo;
    r.hi = hi;
    return r;
}

inline constexpr u128 uFrom(const i128& a) noexcept { return uMake(a.lo, a.hi); }
inline constexpr i128 iFrom(const u128& a) noexcept {
    i128 r;
    r.lo = a.lo;
    r.hi = a.hi;
    return r;
}

inline constexpr int uCmp(const u128& a, const u128& b) noexcept {
    if (a.hi != b.hi) return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
    return 0;
}

inline constexpr u128 uAdd(const u128& a, const u128& b) noexcept {
    uint64_t lo = a.lo + b.lo;
    uint64_t hi = a.hi + b.hi + (lo < a.lo ? 1 : 0);
    return uMake(lo, hi);
}

inline constexpr u128 uSub(const u128& a, const u128& b) noexcept {
    uint64_t lo = a.lo - b.lo;
    uint64_t hi = a.hi - b.hi - (a.lo < b.lo ? 1 : 0);
    return uMake(lo, hi);
}

inline constexpr u128 uNeg(const u128& a) noexcept {
    return uSub(uMake(0, 0), a);
}

// low 128 bits of a*b
inline u128 uMul(const u128& a, const u128& b) noexcept {
    uint64_t ll, lh, xl, xh, yl, yh;
    wide_detail::mul64(a.lo, b.lo, ll, lh);
    wide_detail::mul64(a.lo, b.hi, xl, xh);
    wide_detail::mul64(a.hi, b.lo, yl, yh);
    (void)xh;
    (void)yh;
    return uMake(ll, lh + xl + yl);
}

inline constexpr u128 uAnd(const u128& a, const u128& b) noexcept {
    return uMake(a.lo & b.lo, a.hi & b.hi);
}
inline constexpr u128 uOr(const u128& a, const u128& b) noexcept {
    return uMake(a.lo | b.lo, a.hi | b.hi);
}
inline constexpr u128 uXor(const u128& a, const u128& b) noexcept {
    return uMake(a.lo ^ b.lo, a.hi ^ b.hi);
}
inline constexpr u128 uNot(const u128& a) noexcept {
    return uMake(~a.lo, ~a.hi);
}

inline constexpr u128 uShl(const u128& a, int s) noexcept {
    if (s <= 0) return a;
    if (s >= 128) return uMake(0, 0);
    if (s >= 64) return uMake(0, a.lo << (s - 64));
    return uMake(a.lo << s, (a.hi << s) | (a.lo >> (64 - s)));
}

inline constexpr u128 uShr(const u128& a, int s) noexcept {
    if (s <= 0) return a;
    if (s >= 128) return uMake(0, 0);
    if (s >= 64) return uMake(a.hi >> (s - 64), 0);
    return uMake((a.lo >> s) | (a.hi << (64 - s)), a.hi >> s);
}

// unsigned 128/128 division with remainder (binary long division).
inline void uDivMod(const u128& n, const u128& d, u128& q, u128& r) noexcept {
    q = uMake(0, 0);
    r = uMake(0, 0);
    if (d.lo == 0 && d.hi == 0) return;                 // n/0: caller guards
    if (uCmp(n, d) < 0) { r = n; return; }
    for (int i = 127; i >= 0; --i) {
        uint64_t carry = r.hi >> 63;
        r.hi = (r.hi << 1) | (r.lo >> 63);
        r.lo <<= 1;
        uint64_t bit = (i >= 64) ? ((n.hi >> (i - 64)) & 1) : ((n.lo >> i) & 1);
        r.lo |= bit;
        if (carry || uCmp(r, d) >= 0) {
            r = uSub(r, d);
            if (i >= 64) q.hi |= (uint64_t(1) << (i - 64));
            else         q.lo |= (uint64_t(1) << i);
        }
    }
}

// ---- u128 operators --------------------------------------------------------

inline constexpr bool operator==(const u128& a, const u128& b) noexcept {
    return a.lo == b.lo && a.hi == b.hi;
}
inline constexpr bool operator!=(const u128& a, const u128& b) noexcept {
    return !(a == b);
}
inline constexpr bool operator<(const u128& a, const u128& b) noexcept {
    return uCmp(a, b) < 0;
}
inline constexpr bool operator>(const u128& a, const u128& b) noexcept {
    return uCmp(a, b) > 0;
}
inline constexpr bool operator<=(const u128& a, const u128& b) noexcept {
    return uCmp(a, b) <= 0;
}
inline constexpr bool operator>=(const u128& a, const u128& b) noexcept {
    return uCmp(a, b) >= 0;
}

inline constexpr u128 operator+(const u128& a, const u128& b) noexcept {
    return uAdd(a, b);
}
inline constexpr u128 operator-(const u128& a, const u128& b) noexcept {
    return uSub(a, b);
}
inline u128 operator*(const u128& a, const u128& b) noexcept {
    return uMul(a, b);
}
inline u128 operator/(const u128& a, const u128& b) noexcept {
    u128 q, r;
    uDivMod(a, b, q, r);
    return q;
}
inline u128 operator%(const u128& a, const u128& b) noexcept {
    u128 q, r;
    uDivMod(a, b, q, r);
    return r;
}
inline constexpr u128 operator&(const u128& a, const u128& b) noexcept {
    return uAnd(a, b);
}
inline constexpr u128 operator|(const u128& a, const u128& b) noexcept {
    return uOr(a, b);
}
inline constexpr u128 operator^(const u128& a, const u128& b) noexcept {
    return uXor(a, b);
}
inline constexpr u128 operator~(const u128& a) noexcept { return uNot(a); }
inline constexpr u128 operator-(const u128& a) noexcept { return uNeg(a); }

template <typename S, typename = std::enable_if_t<std::is_integral_v<S>>>
inline constexpr u128 operator<<(const u128& a, S s) noexcept {
    return uShl(a, static_cast<int>(s));
}
template <typename S, typename = std::enable_if_t<std::is_integral_v<S>>>
inline constexpr u128 operator>>(const u128& a, S s) noexcept {
    return uShr(a, static_cast<int>(s));
}

inline u128& operator+=(u128& a, const u128& b) noexcept { a = uAdd(a, b); return a; }
inline u128& operator-=(u128& a, const u128& b) noexcept { a = uSub(a, b); return a; }
inline u128& operator*=(u128& a, const u128& b) noexcept { a = uMul(a, b); return a; }
inline u128& operator/=(u128& a, const u128& b) noexcept { a = a / b; return a; }
inline u128& operator%=(u128& a, const u128& b) noexcept { a = a % b; return a; }
inline constexpr u128& operator&=(u128& a, const u128& b) noexcept { a = uAnd(a, b); return a; }
inline constexpr u128& operator|=(u128& a, const u128& b) noexcept { a = uOr(a, b); return a; }
inline constexpr u128& operator^=(u128& a, const u128& b) noexcept { a = uXor(a, b); return a; }
template <typename S, typename = std::enable_if_t<std::is_integral_v<S>>>
inline constexpr u128& operator<<=(u128& a, S s) noexcept { a = uShl(a, static_cast<int>(s)); return a; }
template <typename S, typename = std::enable_if_t<std::is_integral_v<S>>>
inline constexpr u128& operator>>=(u128& a, S s) noexcept { a = uShr(a, static_cast<int>(s)); return a; }

inline constexpr u128& operator++(u128& a) noexcept { a = uAdd(a, uMake(1, 0)); return a; }
inline constexpr u128& operator--(u128& a) noexcept { a = uSub(a, uMake(1, 0)); return a; }

// ---- i128 operators --------------------------------------------------------

inline constexpr bool iNeg(const i128& a) noexcept { return (a.hi >> 63) != 0; }

inline constexpr int iCmp(const i128& a, const i128& b) noexcept {
    bool na = iNeg(a), nb = iNeg(b);
    if (na != nb) return na ? -1 : 1;
    return uCmp(uFrom(a), uFrom(b));
}

inline i128 iDivMod(const i128& a, const i128& b, bool wantRem) noexcept {
    if (b.lo == 0 && b.hi == 0) return i128(0);
    bool an = iNeg(a), bn = iNeg(b);
    u128 ua = an ? uNeg(uFrom(a)) : uFrom(a);
    u128 ub = bn ? uNeg(uFrom(b)) : uFrom(b);
    u128 q, r;
    uDivMod(ua, ub, q, r);
    if (wantRem) return an ? iFrom(uNeg(r)) : iFrom(r);
    bool qn = an != bn;
    return qn ? iFrom(uNeg(q)) : iFrom(q);
}

inline constexpr i128 iShr(const i128& a, int s) noexcept {
    if (s <= 0) return a;
    bool neg = iNeg(a);
    if (s >= 128) return neg ? i128(-1) : i128(0);
    // logical shift, then fill the vacated top bits with the sign.
    u128 r = uShr(uFrom(a), s);
    if (neg) {
        if (s < 64) {
            r.hi |= (~uint64_t(0) << (64 - s));
        } else {
            r.hi = ~uint64_t(0);
            if (s > 64) r.lo |= (~uint64_t(0) << (128 - s));
        }
    }
    return iFrom(r);
}

inline constexpr bool operator==(const i128& a, const i128& b) noexcept {
    return a.lo == b.lo && a.hi == b.hi;
}
inline constexpr bool operator!=(const i128& a, const i128& b) noexcept {
    return !(a == b);
}
inline constexpr bool operator<(const i128& a, const i128& b) noexcept {
    return iCmp(a, b) < 0;
}
inline constexpr bool operator>(const i128& a, const i128& b) noexcept {
    return iCmp(a, b) > 0;
}
inline constexpr bool operator<=(const i128& a, const i128& b) noexcept {
    return iCmp(a, b) <= 0;
}
inline constexpr bool operator>=(const i128& a, const i128& b) noexcept {
    return iCmp(a, b) >= 0;
}

inline constexpr i128 operator+(const i128& a, const i128& b) noexcept {
    return iFrom(uAdd(uFrom(a), uFrom(b)));
}
inline constexpr i128 operator-(const i128& a, const i128& b) noexcept {
    return iFrom(uSub(uFrom(a), uFrom(b)));
}
inline i128 operator*(const i128& a, const i128& b) noexcept {
    return iFrom(uMul(uFrom(a), uFrom(b)));
}
inline i128 operator/(const i128& a, const i128& b) noexcept {
    return iDivMod(a, b, false);
}
inline i128 operator%(const i128& a, const i128& b) noexcept {
    return iDivMod(a, b, true);
}
inline constexpr i128 operator&(const i128& a, const i128& b) noexcept {
    return iFrom(uAnd(uFrom(a), uFrom(b)));
}
inline constexpr i128 operator|(const i128& a, const i128& b) noexcept {
    return iFrom(uOr(uFrom(a), uFrom(b)));
}
inline constexpr i128 operator^(const i128& a, const i128& b) noexcept {
    return iFrom(uXor(uFrom(a), uFrom(b)));
}
inline constexpr i128 operator~(const i128& a) noexcept {
    return iFrom(uNot(uFrom(a)));
}
inline constexpr i128 operator-(const i128& a) noexcept {
    return iFrom(uNeg(uFrom(a)));
}

template <typename S, typename = std::enable_if_t<std::is_integral_v<S>>>
inline constexpr i128 operator<<(const i128& a, S s) noexcept {
    return iFrom(uShl(uFrom(a), static_cast<int>(s)));
}
template <typename S, typename = std::enable_if_t<std::is_integral_v<S>>>
inline constexpr i128 operator>>(const i128& a, S s) noexcept {
    return iShr(a, static_cast<int>(s));
}

inline constexpr i128& operator+=(i128& a, const i128& b) noexcept { a = a + b; return a; }
inline constexpr i128& operator-=(i128& a, const i128& b) noexcept { a = a - b; return a; }
inline i128& operator*=(i128& a, const i128& b) noexcept { a = a * b; return a; }
inline i128& operator/=(i128& a, const i128& b) noexcept { a = a / b; return a; }
inline i128& operator%=(i128& a, const i128& b) noexcept { a = a % b; return a; }
inline constexpr i128& operator&=(i128& a, const i128& b) noexcept { a = a & b; return a; }
inline constexpr i128& operator|=(i128& a, const i128& b) noexcept { a = a | b; return a; }
inline constexpr i128& operator^=(i128& a, const i128& b) noexcept { a = a ^ b; return a; }
template <typename S, typename = std::enable_if_t<std::is_integral_v<S>>>
inline constexpr i128& operator<<=(i128& a, S s) noexcept { a = a << s; return a; }
template <typename S, typename = std::enable_if_t<std::is_integral_v<S>>>
inline constexpr i128& operator>>=(i128& a, S s) noexcept { a = a >> s; return a; }

inline constexpr i128& operator++(i128& a) noexcept { a = a + i128(1); return a; }
inline constexpr i128& operator--(i128& a) noexcept { a = a - i128(1); return a; }

} // namespace kirn

#endif
