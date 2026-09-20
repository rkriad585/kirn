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
    Str,
    Char,
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
inline TyP boolTy()     { return Ty::make(TyK::Bool); }
inline TyP strTy()      { return Ty::make(TyK::Str); }
inline TyP charTy()     { return Ty::make(TyK::Char); }
inline TyP intTy(std::string n = "i64")  { return Ty::named(TyK::Int, std::move(n)); }
inline TyP floatTy(std::string n = "f64") { return Ty::named(TyK::Float, std::move(n)); }
inline TyP listTy(TyP e)  { return Ty::one(TyK::List, std::move(e)); }
inline TyP setTy(TyP e)   { return Ty::one(TyK::Set, std::move(e)); }
inline TyP chanTy(TyP e)  { return Ty::one(TyK::Chan, std::move(e)); }
inline TyP rangeTy(TyP e) { return Ty::one(TyK::Range, std::move(e)); }
inline TyP genTy(TyP e)   { return Ty::one(TyK::Gen, std::move(e)); }
inline TyP optTy(TyP i)   { return Ty::one(TyK::Opt, std::move(i)); }
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
    return t.k == TyK::Int || t.k == TyK::Float;
}

// Structural equality (ignores span-ish metadata; none exists).
bool equal(const TyP& a, const TyP& b);

std::string toString(const Ty& t);

} // namespace sema
} // namespace kirn
