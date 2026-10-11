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
    Wdbl borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        Limb av = a[i];
        Wdbl bv = (i < b.size() ? (Wdbl)b[i] : (Wdbl)0) + borrow;
        out[i] = (Limb)((Wdbl)av - bv);
        borrow = av < bv ? 1 : 0;
    }
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

// multiply |a|*|b| (schoolbook; O(n*m))
inline std::vector<Limb> mulSchoolbook(const std::vector<Limb>& a,
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

// monthly a << (64*k) (append k zero limbs)
inline std::vector<Limb> shiftUp(const std::vector<Limb>& a, size_t k) {
    if (a.empty()) return {};
    std::vector<Limb> out(k, 0);
    out.insert(out.end(), a.begin(), a.end());
    return out;
}

// Karatsuba threshold in limbs (CPython uses ~70; 32 keeps tests fast)
static const size_t kKaratsubaCutoff = 32;

inline std::vector<Limb> mulMag(const std::vector<Limb>& a,
                                const std::vector<Limb>& b) {
    size_t n = std::min(a.size(), b.size());
    if (n < kKaratsubaCutoff) return mulSchoolbook(a, b);

    size_t m = n / 2;                       // split point (limbs)
    std::vector<Limb> a0(a.begin(), a.begin() + m);
    std::vector<Limb> a1(a.begin() + m, a.end());
    std::vector<Limb> b0(b.begin(), b.begin() + m);
    std::vector<Limb> b1(b.begin() + m, b.end());
    while (!a0.empty() && a0.back() == 0) a0.pop_back();
    while (!b0.empty() && b0.back() == 0) b0.pop_back();

    std::vector<Limb> z0 = mulMag(a0, b0);
    std::vector<Limb> z2 = mulMag(a1, b1);
    std::vector<Limb> as = addMag(a0, a1);
    std::vector<Limb> bs = addMag(b0, b1);
    std::vector<Limb> z1 = mulMag(as, bs);
    z1 = subMag(z1, z0);
    z1 = subMag(z1, z2);

    std::vector<Limb> out = z0;
    std::vector<Limb> mid = addMag(out, shiftUp(z1, m));
    std::vector<Limb> res = addMag(mid, shiftUp(z2, 2 * m));
    while (!res.empty() && res.back() == 0) res.pop_back();
    return res;
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

inline unsigned clzLimb(Limb x) { return x ? (unsigned)__builtin_clzll(x) : 64u; }

// magnitude << s bits (0<=s<64) in place
inline void shlMagInPlace(std::vector<Limb>& v, unsigned s) {
    if (s == 0 || v.empty()) return;
    v.push_back(0);
    for (size_t i = v.size() - 1; i > 0; --i)
        v[i] = (v[i] << s) | (v[i - 1] >> (64 - s));
    v[0] <<= s;
    while (!v.empty() && v.back() == 0) v.pop_back();
}

// magnitude >> s bits (0<=s<64) in place
inline void shrMagInPlace(std::vector<Limb>& v, unsigned s) {
    if (s == 0 || v.empty()) return;
    for (size_t i = 0; i + 1 < v.size(); ++i)
        v[i] = (v[i] >> s) | (v[i + 1] << (64 - s));
    v.back() >>= s;
    while (!v.empty() && v.back() == 0) v.pop_back();
}

// u[j..j+n] -= qhat*v[0..n-1]; returns true if a borrow out occurred.
inline bool mulSub(std::vector<Limb>& u, const std::vector<Limb>& v,
                   Wdbl qhat, size_t j) {
    size_t n = v.size();
    Wdbl t = 0;
    for (size_t i = 0; i < n; ++i) {
        t += qhat * v[i];
        Wdbl sub = (Wdbl)u[j + i] - (Limb)t;
        u[j + i] = (Limb)sub;
        t = (t >> 64) + ((sub >> 64) & 1);
    }
    Wdbl sub = (Wdbl)u[j + n] - (Limb)t;
    u[j + n] = (Limb)sub;
    return (sub >> 64) != 0;
}

// full Knuth Algorithm D: returns (quotient, remainder) of |u|/|v|.
inline std::pair<std::vector<Limb>, std::vector<Limb>>
divmodMag(const std::vector<Limb>& u0, const std::vector<Limb>& v0) {
    if (v0.empty()) return {{}, {}};
    if (magCmp(u0, v0) < 0) return {{}, u0};
    if (v0.size() == 1) return {divSmall(u0, v0[0]),
                                [&] {
                                    Limb r = 0;
                                    for (size_t i = u0.size(); i-- > 0;)
                                        r = (Limb)((((Wdbl)r << 64) | u0[i]) % v0[0]);
                                    std::vector<Limb> rem;
                                    if (r) rem.push_back(r);
                                    return rem;
                                }()};
    int n = (int)v0.size();
    int m = (int)u0.size() - n;
    int s = (int)clzLimb(v0.back());
    std::vector<Limb> v = v0, u = u0;
    shlMagInPlace(v, (unsigned)s);
    shlMagInPlace(u, (unsigned)s);
    u.resize((size_t)(m + n + 1), 0);
    std::vector<Limb> q((size_t)(m + 1), 0);
    Limb vn1 = v[n - 1], vn2 = v[n - 2];
    for (int j = m; j >= 0; --j) {
        Wdbl num = ((Wdbl)u[j + n] << 64) | u[j + n - 1];
        Wdbl qhat = num / vn1;
        Wdbl rhat = num % vn1;
        while (qhat >= ((Wdbl)1 << 64) ||
               qhat * vn2 > ((rhat << 64) | u[j + n - 2])) {
            --qhat;
            rhat += vn1;
            if (rhat >= ((Wdbl)1 << 64)) break;
        }
        if (mulSub(u, v, qhat, (size_t)j)) {
            --qhat;
            Wdbl c = 0;
            for (int i = 0; i < n; ++i) {
                Wdbl sum = (Wdbl)u[j + i] + v[i] + (Limb)c;
                u[j + i] = (Limb)sum;
                c = sum >> 64;
            }
            u[j + n] = (Limb)((Wdbl)u[j + n] + (Limb)c);
        }
        q[j] = (Limb)qhat;
    }
    while (!q.empty() && q.back() == 0) q.pop_back();
    std::vector<Limb> r(u.begin(), u.begin() + n);
    shrMagInPlace(r, (unsigned)s);
    return {q, r};
}

// bitwise kernels on equal-length limb arrays
inline std::vector<Limb> magAnd(const std::vector<Limb>& a,
                                const std::vector<Limb>& b) {
    size_t n = std::min(a.size(), b.size());
    std::vector<Limb> r(n);
    for (size_t i = 0; i < n; ++i) r[i] = a[i] & b[i];
    while (!r.empty() && r.back() == 0) r.pop_back();
    return r;
}
inline std::vector<Limb> magOr(const std::vector<Limb>& a,
                               const std::vector<Limb>& b) {
    size_t n = std::max(a.size(), b.size());
    std::vector<Limb> r(n);
    for (size_t i = 0; i < n; ++i)
        r[i] = (i < a.size() ? a[i] : 0) | (i < b.size() ? b[i] : 0);
    while (!r.empty() && r.back() == 0) r.pop_back();
    return r;
}
inline std::vector<Limb> magXor(const std::vector<Limb>& a,
                                const std::vector<Limb>& b) {
    size_t n = std::max(a.size(), b.size());
    std::vector<Limb> r(n);
    for (size_t i = 0; i < n; ++i)
        r[i] = (i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0);
    while (!r.empty() && r.back() == 0) r.pop_back();
    return r;
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
        auto qr = divmodMag(limbs, o.limbs);
        Big q; q.limbs = qr.first;
        bool signDiff = (neg != o.neg);
        // floor: if signs differ and there is a nonzero remainder, pull the
        // truncated quotient one further negative (Python).
        if (signDiff && !qr.second.empty())
            q.limbs = addMag(q.limbs, fromInt(1));
        q.neg = signDiff && !q.zero();
        return q;
    }

    // a mod m (Python-style: result has the sign of m)
    Big mod(const Big& m) const {
        if (m.zero()) return {};
        Big q = div(m);
        // with floor division this remainder already lies in [0,m) for m>0 and
        // (m,0] for m<0 — exactly Python/Ruby semantics.
        return sub(q.mul(m));
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

    // base 2..36 representation (lowercase digits)
    std::string toStringBase(unsigned base) const {
        if (base < 2 || base > 36) base = 10;
        if (zero()) return "0";
        std::vector<Limb> cur = limbs;
        std::string out;
        while (!cur.empty()) {
            Wdbl r = 0;
            std::vector<Limb> q(cur.size(), 0);
            for (size_t i = cur.size(); i-- > 0;) {
                Wdbl v = (r << 64) | cur[i];
                q[i] = (Limb)(v / base);
                r = v % base;
            }
            while (!q.empty() && q.back() == 0) q.pop_back();
            int digit = (int)r;
            out.push_back(digit < 10 ? char('0' + digit)
                                     : char('a' + digit - 10));
            cur = q;
        }
        std::reverse(out.begin(), out.end());
        if (neg) out = "-" + out;
        return out;
    }

    int compare(const Big& o) const {
        if (zero() && o.zero()) return 0;
        int s1 = sign(), s2 = o.sign();
        if (s1 != s2) return s1 < s2 ? -1 : 1;
        int c = cmpAbs(o);
        if (s1 > 0) return c;
        return -c;
    }

    bool isEven() const { return limbs.empty() || (limbs[0] & 1) == 0; }

    // left shift by k bits (multiply by 2^k), sign preserved
    Big shlBits(uint64_t k) const {
        if (zero() || k == 0) return *this;
        Big r;
        r.limbs = shiftUp(limbs, (size_t)(k / 64));
        unsigned bits = (unsigned)(k % 64);
        if (bits) shlMagInPlace(r.limbs, bits);
        r.neg = neg && !r.zero();
        return r;
    }

    // arithmetic shift right by k bits = floor(this / 2^k)
    Big shrBits(uint64_t k) const {
        if (zero() || k == 0) return *this;
        Big p2; p2.limbs = fromInt(1);
        p2 = p2.shlBits(k);
        return div(p2);
    }
};

inline Big fromI64(int64_t v) {
    Big b;
    if (v < 0) {
        uint64_t mag = (uint64_t)0 - (uint64_t)v;
        b.neg = true;
        if (mag) b.limbs.push_back(mag);
    } else if (v != 0) {
        b.limbs.push_back((uint64_t)v);
    }
    return b;
}

inline Big fromBig(int64_t v) { return fromI64(v); }

inline uint64_t toU64(const Big& b) {
    return b.limbs.empty() ? 0 : b.limbs[0];
}

// ---- bitwise ops with infinite two's-complement semantics (Python/Ruby) ----

inline std::vector<Limb> toTwosComp(const std::vector<Limb>& mag, bool negv,
                                    size_t n) {
    std::vector<Limb> r(n, 0);
    for (size_t i = 0; i < mag.size() && i < n; ++i) r[i] = mag[i];
    if (negv) {
        for (auto& l : r) l = ~l;
        Wdbl carry = 1;
        for (size_t i = 0; i < n && carry; ++i) {
            Wdbl s = (Wdbl)r[i] + carry;
            r[i] = (Limb)s;
            carry = s >> 64;
        }
    }
    return r;
}

inline Big fromTwosComp(std::vector<Limb> r) {
    Big b;
    if (r.empty()) return b;
    bool negv = (r.back() >> 63) & 1;
    if (!negv) {
        while (!r.empty() && r.back() == 0) r.pop_back();
        b.limbs = std::move(r);
        return b;
    }
    for (auto& l : r) l = ~l;
    Wdbl carry = 1;
    for (size_t i = 0; i < r.size() && carry; ++i) {
        Wdbl s = (Wdbl)r[i] + carry;
        r[i] = (Limb)s;
        carry = s >> 64;
    }
    while (!r.empty() && r.back() == 0) r.pop_back();
    b.limbs = std::move(r);
    b.neg = !b.limbs.empty();
    return b;
}

inline Big bitwiseOp(const Big& a, const Big& b, char op) {
    size_t bits =
        (size_t)std::max(bitLength(a.limbs), bitLength(b.limbs)) + 1;
    size_t n = (bits + 63) / 64;
    if (n == 0) n = 1;
    auto x = toTwosComp(a.limbs, a.neg, n);
    auto y = toTwosComp(b.limbs, b.neg, n);
    std::vector<Limb> z(n);
    for (size_t i = 0; i < n; ++i)
        z[i] = op == '&' ? (x[i] & y[i])
               : op == '|' ? (x[i] | y[i])
                           : (x[i] ^ y[i]);
    return fromTwosComp(std::move(z));
}

// ---- number theory ---------------------------------------------------------

inline Big gcd(const Big& a, const Big& b) {
    Big x = a.abs(), y = b.abs();
    while (!y.zero()) {
        Big r = x.mod(y);
        x = y;
        y = r;
    }
    return x;
}

// extended Euclid; returns true and sets out to a^-1 mod m when it exists
inline bool modInverse(const Big& a, const Big& m, Big& out) {
    Big m0 = m.abs();
    if (m0.zero()) return false;
    Big oldR = a.mod(m0), r = m0;
    Big oldS; oldS.limbs = fromInt(1);
    Big s;
    while (!r.zero()) {
        Big q = oldR.div(r);
        Big nr = oldR.sub(q.mul(r));
        Big ns = oldS.sub(q.mul(s));
        oldR = r; r = nr;
        oldS = s; s = ns;
    }
    Big one; one.limbs = fromInt(1);
    if (oldR.compare(one) != 0) return false;
    out = oldS.mod(m0);
    return true;
}

// Tonelli-Shanks: square root of a mod p (p an odd prime). Returns true on
// success (a is a quadratic residue).
inline bool modSqrt(const Big& a, const Big& p, Big& out) {
    Big one; one.limbs = fromInt(1);
    Big two; two.limbs = fromInt(2);
    Big A = a.mod(p);
    if (A.zero()) { out = A; return true; }
    Big pm1 = p.sub(one);
    // p == 2
    if (pm1.zero()) { out = A.mod(two); return true; }
    // Legendre symbol A^((p-1)/2) mod p
    Big leg = A.powmod(pm1.div(two), p);
    if (leg.compare(one) != 0) return false;
    // p ≡ 3 (mod 4): r = A^((p+1)/4)
    if (modSmall(pm1.limbs, 4) == 3) {
        out = A.powmod(pm1.add(one).div(fromBig(4)), p);
        return true;
    }
    // factor p-1 = q * 2^s
    Big q = pm1;
    int s = 0;
    while (q.isEven()) { q = q.shrBits(1); ++s; }
    // find a quadratic non-residue z
    Big z; z.limbs = fromInt(2);
    Big pz = p;
    Big zz = z;
    for (;;) {
        Big e = pz.sub(one).div(two);
        if (zz.powmod(e, pz).compare(pm1) == 0) break;
        zz = zz.add(one);
    }
    Big m = fromBig((int64_t)s);
    Big c = zz.powmod(q, p);
    Big t = A.powmod(q, p);
    Big r = A.powmod(q.add(one).div(two), p);
    while (t.compare(one) != 0) {
        int i = 0;
        Big tt = t;
        while (tt.compare(one) != 0) { tt = tt.mul(tt).mod(p); ++i; }
        // b = c^(2^(m-i-1))
        Big b = c;
        for (int k = 0; k < (int)(toU64(m) - i - 1); ++k) b = b.mul(b).mod(p);
        m = fromBig((int64_t)i);
        c = b.mul(b).mod(p);
        t = t.mul(c).mod(p);
        r = r.mul(b).mod(p);
    }
    out = r.mod(p);
    return true;
}

// deterministic Miller-Rabin (bases valid for n < 3.3e24)
inline bool isPrime(const Big& n0) {
    Big one; one.limbs = fromInt(1);
    Big two; two.limbs = fromInt(2);
    if (n0.neg || n0.zero()) return false;
    Big n = n0;
    if (n.compare(one) == 0) return false;
    if (n.compare(two) == 0) return true;
    if (n.isEven()) return false;
    // small trial division
    static const uint64_t small[] = {3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    for (uint64_t sp : small) {
        if (modSmall(n.limbs, sp) == 0) {
            Big b; b.limbs = fromInt((int64_t)sp);
            return n.compare(b) == 0;
        }
    }
    Big nm1 = n.sub(one);
    Big d = nm1;
    int s = 0;
    while (d.isEven()) { d = d.shrBits(1); ++s; }
    static const uint64_t bases[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    for (uint64_t a : bases) {
        Big base; base.limbs = fromInt((int64_t)a);
        if (base.compare(n) >= 0) continue;
        Big x = base.powmod(d, n);
        if (x.compare(one) == 0 || x.compare(nm1) == 0) continue;
        bool ok = false;
        for (int i = 1; i < s; ++i) {
            x = x.mul(x).mod(n);
            if (x.compare(nm1) == 0) { ok = true; break; }
        }
        if (!ok) return false;
    }
    return true;
}

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

} // namespace bignum
} // namespace interp
} // namespace kirn

#endif