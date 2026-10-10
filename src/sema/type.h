#pragma once
// Resolved (semantic) types for Kirn sema.
// Structural value type with shared_ptr indirection; Ty::None is the unit type.
// Error/Unknown are "poison"/"dynamic" markers that suppress cascading reports.
#include <memory>
#include <string>
#include <vector>

namespace kirn {
namespace sema {

enum class TyK {
    Error,      // poisoned: errors already reported
    Unknown,    // unchecked/dynamic: lambda params, import roots, ...
    None,       // unit ()
    Bool,
    Int,        // name = i8..u64, usize (default "int" -> i64)
    Float,      // f32 | f64
    BigInt,     // arbitrary-precision integer (VK::BigInt)
    Str,
    Char,
    Bytes,       // immutable byte buffer, VK::Bytes at runtime
    List,       // args[0] elem
    Dict,       // args[0]=K args[1]=V
    Set,        // args[0]
    Chan,       // args[0]
    Range,      // args[0] elem
    Gen,        // generator/view: args[0] elem
    Tuple,      // args = items
    Fn,         // args = params, ret
    Opt,        // T?
    Ptr,        // *T
    Ref,        // &T
    Struct,     // nominal, name
    EnumName,   // nominal, name
    EnumVal,    // variant instance: name=Enum, variant=variantName
    TraitObj,   // dynamic dispatch, name = trait
    TypeVar,    // generic placeholder, name
};

struct Ty;
using TyP = std::shared_ptr<const Ty>;

struct Ty {
    TyK k = TyK::Unknown;
    std::string name;                 // Int / Struct / Enum / TraitObj / TypeVar
    std::string variant;              // EnumVal variant name
    std::vector<TyP> args;            // containers/tuple/fn params
    TyP inner;                        // Opt/Ptr/Ref/Fn ret

  private:
    static std::shared_ptr<Ty> mut(TyK k) {
        auto t = std::shared_ptr<Ty>(new Ty());
        t->k = k;
        return t;
    }

  public:
    static TyP make(TyK k) { return mut(k); }
    static TyP named(TyK k, std::string n) {
        auto t = mut(k); t->name = std::move(n); return t;
    }
    static TyP one(TyK k, TyP a) {
        auto t = mut(k); t->args.push_back(std::move(a)); return t;
    }
    static TyP two(TyK k, TyP a, TyP b) {
        auto t = mut(k);
        t->args.push_back(std::move(a));
        t->args.push_back(std::move(b));
        return t;
    }

    bool is(TyK k_) const { return k == k_; }
    bool isError() const { return k == TyK::Error; }
    bool isUnknown() const { return k == TyK::Unknown; }
};

// ---- constructors -----------------------------------------------------------

inline TyP errTy()      { return Ty::make(TyK::Error); }
inline TyP unkTy()      { return Ty::make(TyK::Unknown); }
inline TyP noneTy()     { return Ty::make(TyK::None); }
inline TyP boolTy(std::string n = "bool") { return Ty::named(TyK::Bool, std::move(n)); }
inline TyP strTy(std::string n = "string") { return Ty::named(TyK::Str, std::move(n)); }
inline TyP charTy(std::string n = "char") { return Ty::named(TyK::Char, std::move(n)); }
inline TyP bytesTy()      { return Ty::named(TyK::Bytes, "bytes"); }
inline TyP intTy(std::string n = "i64")  { return Ty::named(TyK::Int, std::move(n)); }
inline TyP floatTy(std::string n = "f64") { return Ty::named(TyK::Float, std::move(n)); }
inline TyP bigIntTy(std::string n = "bigint") { return Ty::named(TyK::BigInt, std::move(n)); }
inline TyP listTy(TyP e)  { return Ty::one(TyK::List, std::move(e)); }
inline TyP setTy(TyP e)   { return Ty::one(TyK::Set, std::move(e)); }
inline TyP chanTy(TyP e)  { return Ty::one(TyK::Chan, std::move(e)); }
inline TyP rangeTy(TyP e) { return Ty::one(TyK::Range, std::move(e)); }
inline TyP genTy(TyP e)   { return Ty::one(TyK::Gen, std::move(e)); }
inline TyP optTy(TyP i)   { return Ty::one(TyK::Opt, std::move(i)); }

// Natural size in bytes for a *scalar primitive* type spelling. The numeric
// widths are bit counts; a sub-byte width (i4, u4, f4, b4) rounds up to the
// minimum storable unit of 1 byte. Aggregate/container spellings are not
// scalar and return -1. Used by the `sizeof` operator (see the parser).
inline int typeSizeBytes(const std::string& n) {
    if (n == "int" || n == "isize" || n == "usize") return 8;
    if (n == "float") return 8;
    if (n == "bool") return 1;
    if (n == "char") return 4;
    if (n == "string" || n == "str" || n == "strings") return 8;
    if (n == "bytes" || n == "none" || n == "unit" || n == "None") return n == "bytes" ? 1 : 0;
    // <prefix><digits>: iN, uN, fN, ufN, bN, charN
    int p = 0;
    if (n.rfind("char", 0) == 0) p = 4;
    else if (n.rfind("uf", 0) == 0) p = 2;
    else if (n.rfind("i", 0) == 0)   p = 1;
    else if (n.rfind("u", 0) == 0)   p = 1;
    else if (n.rfind("f", 0) == 0)   p = 1;
    else if (n.rfind("b", 0) == 0)   p = 1;
    else return -1;
    std::string digits = n.substr(p);
    // only "char16"-style tails: for i/u/f/uf/b the whole tail is digits
    if (n.rfind("char", 0) == 0) digits = n.substr(4);
    if (digits.empty()) return -1;
    int bits = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return -1;
        bits = bits * 10 + (c - '0');
    }
    if (bits == 0 || bits > 4096) return -1;
    return (bits + 7) / 8;
}
inline TyP ptrTy(TyP i)   { return Ty::one(TyK::Ptr, std::move(i)); }
inline TyP dictTy(TyP kk, TyP vv) { return Ty::two(TyK::Dict, std::move(kk), std::move(vv)); }
inline TyP tupleTy(std::vector<TyP> items) {
    auto t = std::shared_ptr<Ty>(new Ty());
    t->k = TyK::Tuple;
    for (auto& i : items) t->args.push_back(std::move(i));
    return t;
}
inline TyP fnTy(std::vector<TyP> ps, TyP r) {
    auto t = std::shared_ptr<Ty>(new Ty());
    t->k = TyK::Fn;
    for (auto& p : ps) t->args.push_back(std::move(p));
    t->inner = std::move(r);
    return t;
}

inline bool isNumeric(const Ty& t) {
    return t.k == TyK::Int || t.k == TyK::Float || t.k == TyK::BigInt;
}

// Structural equality (ignores span-ish metadata; none exists).
bool equal(const TyP& a, const TyP& b);

std::string toString(const Ty& t);

} // namespace sema
} // namespace kirn
