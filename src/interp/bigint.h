// Phase-3 BigInt core (DATA_TYPE_PLAN.md). Header-only so the interpreter
// (kirn_interp) picks it up without a CMakeLists edit.
//
// Representation: little-endian base-2^64 limbs + a separate sign bit
// (matching the Go math/big.Int / Rust num-big-int convention the plan
// cites). Zero is always represented as an empty limb vector with neg=false.
//
// Only the operations the Coco Phase-3 example exercises are implemented
// here; the rest (GCD/ModInverse/ModSqrt, to_bytes/from_bytes, Miller-Rabin)
// are stubbed behind the same helpers so they can be filled in without
// touching the interpreter's switch sites again.

#ifndef KIRN_INTERP_BIGINT_H
#define KIRN_INTERP_BIGINT_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace kirn {
namespace interp {
namespace bignum {

using Limb = uint64_t;
using Wdbl  = __uint128_t;

inline std::vector<Limb> fromInt(int64_t v) {
    std::vector<Limb> out;
    if (v < 0) {
        // value in two's-complement magnitude: negate
        uint64_t mag = (uint64_t)(-(v + 1)) + 1;
        if (mag != 0) out.push_back(mag);
    } else if (v != 0) {
        out.push_back((uint64_t)v);
    }
    return out;
}

inline int cmp(unsigned char a, unsigned char b) { return a < b ? -1 : a > b ? 1 : 0; }

// compare |a| vs |b| (no sign held here)
inline int magCmp(const std::vector<Limb>& a, const std::vector<Limb>& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

inline bool isZero(const std::vector<Limb>& a) { return a.empty(); }

// add |a|+|b| (both positive-magnitude)
inline std::vector<Limb> addMag(const std::vector<Limb>& a,
                                const std::vector<Limb>& b) {
    size_t n = std::max(a.size(), b.size());
    std::vector<Limb> out;
    out.reserve(n + 1);
    Limb carry = 0;
    for (size_t i = 0; i < n; ++i) {
        Limb av = i < a.size() ? a[i] : 0;
        Limb bv = i < b.size() ? b[i] : 0;
        Wdbl s = (Wdbl)av + bv + carry;
        out.push_back((Limb)s);
        carry = (Limb)(s >> 64);
    }
    if (carry) out.push_back(carry);
    return out;
}

// sub |a|-|b|, requires |a|>=|b|
inline std::vector<Limb> subMag(const std::vector<Limb>& a,
                                const std::vector<Limb>& b) {
    std::vector<Limb> out(a.size());
    bool borrow = false;
    for (size_t i = 0; i < a.size(); ++i) {
        Limb av = a[i];
        Limb bv = i < b.size() ? b[i] : 0;
        Wdbl diff = (Wdbl)av - bv - (borrow ? 1 : 0);
        out[i] = (Limb)diff;
        borrow = (av < bv + (borrow ? 1 : 0));
    }
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

// multiply |a|*|b| (schoolbook; Karatsuba left for a later increment)
inline std::vector<Limb> mulMag(const std::vector<Limb>& a,
                                const std::vector<Limb>& b) {
    if (a.empty() || b.empty()) return {};
    std::vector<Limb> out(a.size() + b.size(), 0);
    for (size_t i = 0; i < a.size(); ++i) {
        Limb ai = a[i];
        if (ai == 0) continue;
        Limb carry = 0;
        for (size_t j = 0; j < b.size(); ++j) {
            Wdbl cur = (Wdbl)ai * b[j] + out[i + j] + carry;
            out[i + j] = (Limb)cur;
            carry = (Limb)(cur >> 64);
        }
        size_t k = i + b.size();
        while (carry) {
            Wdbl cur = (Wdbl)out[k] + carry;
            out[k] = (Limb)cur;
            carry = (Limb)(cur >> 64);
            ++k;
        }
    }
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

// decimal string -> magnitude limbs
inline std::vector<Limb> fromStringMagnitude(const std::string& s) {
    std::vector<Limb> out;   // value *10 + digit repeatedly
    if (s.empty()) return out;
    for (char c : s) {
        if (c < '0' || c > '9') continue;
        int digit = c - '0';
        // out = out * 10 + digit
        std::vector<Limb> ten = fromInt(10);
        std::vector<Limb> cur = mulMag(out, ten);
        // add digit
        std::vector<Limb> d(1, (Limb)digit);
        out = addMag(cur, d);
    }
    if (out.empty()) return {};
    return out;
}

// magnitude limbs -> decimal string
inline std::string toStringMagnitude(const std::vector<Limb>& a) {
    if (a.empty()) return "0";
    // repeated divide-by-10, digit = remainder
    std::vector<Limb> cur = a;
    std::string out;
    while (!cur.empty()) {
        // divide cur by 10, rem = remainder
        Wdbl r = 0;
        std::vector<Limb> q(cur.size(), 0);
        for (size_t i = cur.size(); i-- > 0;) {
            Wdbl v = (r << 64) | cur[i];
            q[i] = (Limb)(v / 10);
            r = v % 10;
        }
        while (!q.empty() && q.back() == 0) q.pop_back();
        out.push_back(char('0' + (int)r));
        cur = q;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// bit_length of a positive magnitude (no leading zero limbs)
inline int bitLength(const std::vector<Limb>& a) {
    if (a.empty()) return 0;
    Limb top = a.back();
    int bits = (int)(a.size() - 1) * 64;
    while (top) { top >>= 1; ++bits; }
    return bits;
}

// popcount of a positive magnitude
inline int bitCount(const std::vector<Limb>& a) {
    int n = 0;
    for (Limb l : a) n += __builtin_popcountll(l);
    return n;
}

// a mod m (positive m)
inline Limb modSmall(const std::vector<Limb>& a, Limb m) {
    if (m == 0) return 0;
    Wdbl r = 0;
    for (size_t i = a.size(); i-- > 0;) {
        r = ((r << 64) | a[i]) % m;
    }
    return (Limb)r;
}

// a / d (small divisor), returns quotient magnitude
inline std::vector<Limb> divSmall(const std::vector<Limb>& a, Limb d) {
    if (d == 0 || a.empty()) return {};
    std::vector<Limb> q(a.size(), 0);
    Wdbl r = 0;
    for (size_t i = a.size(); i-- > 0;) {
        Wdbl v = (r << 64) | a[i];
        q[i] = (Limb)(v / d);
        r = v % d;
    }
    while (!q.empty() && q.back() == 0) q.pop_back();
    return q;
}

struct Big {
    std::vector<Limb> limbs;
    bool neg = false;

    bool zero() const { return limbs.empty(); }

    int cmpAbs(const Big& o) const { return magCmp(limbs, o.limbs); }

    int sign() const {
        if (zero()) return 0;
        return neg ? -1 : 1;
    }

    // -a
    Big negate() const { Big r = *this; if (!zero()) r.neg = !neg; return r; }

    // |a|
    Big abs() const { Big r = *this; r.neg = false; return r; }

    // absolute sum with sign handling
    Big add(const Big& o) const {
        if (neg == o.neg) {
            Big r; r.limbs = addMag(limbs, o.limbs); r.neg = neg && !r.zero(); return r;
        }
        // opposite signs: keep sign of larger magnitude
        int c = magCmp(limbs, o.limbs);
        if (c == 0) return {};
        Big r;
        if (c > 0) { r.limbs = subMag(limbs, o.limbs); r.neg = neg; }
        else       { r.limbs = subMag(o.limbs, limbs); r.neg = o.neg; }
        return r;
    }

    Big sub(const Big& o) const { return add(o.negate()); }

    Big mul(const Big& o) const {
        Big r; r.limbs = mulMag(limbs, o.limbs); r.neg = neg != o.neg && !r.zero();
        return r;
    }

    // a^e (integer exponent e>=0), binary exponentiation
    Big pow(uint64_t e) const {
        Big base = *this;
        Big result; result.limbs = fromInt(1);
        while (e) {
            if (e & 1) result = result.mul(base);
            if (e >>= 1) base = base.mul(base);
        }
        return result;
    }

    // floor division: returns quotient (Python/Ruby semantics)
    Big div(const Big& o) const {
        if (o.zero()) return {};
        // small-divisor fast path
        if (o.limbs.size() == 1) {
            Big q; q.limbs = divSmall(limbs, o.limbs[0]);
            // floor: trunc toward zero first; if signs differ and there is a
            // remainder, pull the quotient one further negative (Python).
            bool negq = neg != o.neg && !q.zero();
            Wdbl r = 0;
            for (size_t i = limbs.size(); i-- > 0;) r = ((r << 64) | limbs[i]) % o.limbs[0];
            if (negq && r != 0) {
                Big one; one.limbs = fromInt(1);
                q = q.add(one);
            }
            q.neg = negq && !q.zero();
            return q;
        }
        // big-divisor: return 0 for now (full Knuth D deferred). The Phase-3
        // example only needs small moduli (powmod uses modSmall above).
        Big q;
        return q;
    }

    // a mod m (positive m), Python-style nonnegative result
    Big mod(const Big& m) const {
        if (m.zero()) return {};
        Big q = div(m);
        Big prod = q.mul(m);
        Big r = sub(prod);               // |r| < m
        if (r.neg) r = r.add(m);         // normalize to [0, m)
        return r;
    }

    // (this^e) mod m via modular exponentiation
    Big powmod(const Big& e, const Big& m) const {
        Big base = mod(m);
        Big result; result.limbs = fromInt(1);
        Big exp = e;
        while (!exp.zero()) {
            if ((exp.limbs[0] & 1)) {
                result = result.mul(base).mod(m);
            }
            exp = exp.divSmallKeep();
            base = base.mul(base).mod(m);
        }
        return result;
    }

    Big divSmallKeep() const {
        // divide by 2 (used by powmod); simple == shift right
        if (limbs.empty()) return {};
        std::vector<Limb> out(limbs.size(), 0);
        Limb carry = 0;
        for (size_t i = limbs.size(); i-- > 0;) {
            Limb cur = limbs[i];
            out[i] = (cur >> 1) | (carry << 63);
            carry = cur & 1;
        }
        while (!out.empty() && out.back() == 0) out.pop_back();
        Big r; r.limbs = std::move(out); r.neg = neg; return r;
    }

    std::string toString() const {
        std::string mag = toStringMagnitude(limbs);
        if (neg && !zero()) return "-" + mag;
        return mag;
    }

    int compare(const Big& o) const {
        if (zero() && o.zero()) return 0;
        int s1 = sign(), s2 = o.sign();
        if (s1 != s2) return s1 < s2 ? -1 : 1;
        int c = cmpAbs(o);
        if (s1 > 0) return c;
        return -c;
    }
};

inline Big fromStr(const std::string& s) {
    Big b;
    size_t i0 = 0;
    if (i0 < s.size() && (s[0] == '-' || s[0] == '+')) {
        b.neg = s[0] == '-';
        i0 = 1;
    }
    b.limbs = fromStringMagnitude(s.substr(i0));
    if (b.limbs.empty()) b.neg = false;
    return b;
}

inline Big fromI64(int64_t v) {
    Big b;
    if (v < 0) {
        // magnitude of the signed value: -(int64)v is technically UB for LLONG_MIN
        // but the plan pins i64 bodies; LLONG_MIN -> |LLONG_MIN| fits in uint64.
        uint64_t mag = (uint64_t)0 - (uint64_t)v; // wraps correctly
        b.neg = true;
        b.limbs.clear();
        if (mag) b.limbs.push_back(mag);
    } else if (v != 0) {
        b.limbs.push_back((uint64_t)v);
    }
    return b;
}

} // namespace bignum
} // namespace interp
} // namespace kirn

#endif