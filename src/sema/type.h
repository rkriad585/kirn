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
    Int,        // name = Int4..UInt256, ISize/USize (default "Int64")
    Float,      // f32 | f64
    BigInt,     // arbitrary-precision integer (VK::BigInt)
    Rational,   // exact rational num/den (VK::Rational)
    Complex,    // rectangular a+bi (VK::Complex)
    Decimal,    // arbitrary-precision decimal (VK::Decimal)
    Str,
    Char,
    Bytes,       // immutable byte buffer, VK::Bytes at runtime
    ByteArray,   // mutable byte buffer, VK::ByteArray at runtime
    MemoryView,  // zero-copy view over bytes/bytearray, VK::View at runtime
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
inline TyP boolTy(std::string n = "Bool") { return Ty::named(TyK::Bool, std::move(n)); }
inline TyP strTy(std::string n = "Str") { return Ty::named(TyK::Str, std::move(n)); }
inline TyP charTy(std::string n = "Char") { return Ty::named(TyK::Char, std::move(n)); }
inline TyP bytesTy()      { return Ty::named(TyK::Bytes, "bytes"); }
inline TyP byteArrayTy(std::string n = "bytearray") { return Ty::named(TyK::ByteArray, std::move(n)); }
inline TyP memoryViewTy(std::string n = "memoryview") { return Ty::named(TyK::MemoryView, std::move(n)); }
inline TyP intTy(std::string n = "Int64")  { return Ty::named(TyK::Int, std::move(n)); }
inline TyP floatTy(std::string n = "Float64") { return Ty::named(TyK::Float, std::move(n)); }
inline TyP bigIntTy(std::string n = "BigInt") { return Ty::named(TyK::BigInt, std::move(n)); }
inline TyP rationalTy(std::string n = "Rational") { return Ty::named(TyK::Rational, std::move(n)); }
inline TyP complexTy(std::string n = "Complex") { return Ty::named(TyK::Complex, std::move(n)); }
inline TyP decimalTy(std::string n = "Decimal") { return Ty::named(TyK::Decimal, std::move(n)); }
inline TyP listTy(TyP e)  { return Ty::one(TyK::List, std::move(e)); }
inline TyP setTy(TyP e)   { return Ty::one(TyK::Set, std::move(e)); }
inline TyP chanTy(TyP e)  { return Ty::one(TyK::Chan, std::move(e)); }
inline TyP rangeTy(TyP e) { return Ty::one(TyK::Range, std::move(e)); }
inline TyP genTy(TyP e)   { return Ty::one(TyK::Gen, std::move(e)); }
inline TyP optTy(TyP i)   { return Ty::one(TyK::Opt, std::move(i)); }

// Natural size in bytes for a *scalar primitive* type spelling. Width types
// are bit counts; a sub-byte width (Int4, UInt4, i4, u4, f4) rounds up to the
// minimum storable unit of 1 byte. Aggregate/container spellings are not
// scalar and return -1. Used by the `sizeof` operator (see the parser).
inline int typeSizeBytes(const std::string& n) {
    if (n == "bytes") return 1;
    if (n == "none" || n == "unit" || n == "None") return 0;
    if (n == "Bool" || n == "bool")
        return 1;
    if (n == "Char" || n == "char")
        return 4;
    if (n == "Str" || n == "str" || n == "String" || n == "string" ||
        n == "strings" || n == "Int" || n == "int" || n == "UInt" ||
        n == "uint" || n == "ISize" || n == "isize" || n == "USize" ||
        n == "usize" || n == "Float" || n == "float" || n == "UFloat")
        return 8;
    // <Prefix><digits>: i/u/f/uf/b (lowercase micros) and Int/UInt/Float/
    // UFloat/Bool/Char (capitalized), e.g. i8, u256, f64, Int128, UFloat32.
    static const struct { const char* p; size_t l; } kPrefixes[] = {
        {"UFloat", 6}, {"UInt", 4}, {"Float", 5}, {"Bool", 4}, {"Int", 3},
        {"Char", 4}, {"char", 4}, {"uf", 2}, {"b", 1}, {"f", 1},
        {"u", 1}, {"i", 1}, {nullptr, 0}};
    for (int k = 0; kPrefixes[k].p; ++k) {
        size_t pl = kPrefixes[k].l;
        if (n.size() <= pl || n.compare(0, pl, kPrefixes[k].p) != 0) continue;
        if (n.find_first_not_of("0123456789", pl) != std::string::npos) continue;
        int bits = 0;
        for (size_t j = pl; j < n.size(); ++j) {
            bits = bits * 10 + (n[j] - '0');
            if (bits > 4096) return -1;
        }
        if (bits == 0) return -1;
        return (bits + 7) / 8;
    }
    return -1;
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
    return t.k == TyK::Int || t.k == TyK::Float || t.k == TyK::BigInt ||
           t.k == TyK::Rational || t.k == TyK::Complex || t.k == TyK::Decimal;
}

// Structural equality (ignores span-ish metadata; none exists).
bool equal(const TyP& a, const TyP& b);

std::string toString(const Ty& t);

} // namespace sema
} // namespace kirn
