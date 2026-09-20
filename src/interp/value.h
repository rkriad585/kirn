#pragma once
// Runtime value model for the Kirn tree-walking interpreter (plan §10.4).
//
// Copy semantics encode language semantics directly:
//   - Struct instances are copied by value on assignment (plan §6.1).
//   - List / Dict / Set are reference types (shared_ptr), matching the
//     aliasing behaviour exercised by examples 16 and 30.
//   - `new T(...)` produces a refcounted heap handle (VK::Heap); a field
//     declared `weak` stores VK::Weak so cycles can break (examples 27/28).
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ast/ast.h"

namespace kirn {
namespace interp {

struct Value;
struct EnvS;
using Env = std::shared_ptr<EnvS>;

struct ChanImpl;
struct HeapObj;
struct ThreadImpl;

enum class VK {
    None, Bool, Int, Float, Str, Bytes, Char,
    List, Dict, Set, Tuple, Range,
    Struct,                                   // by-value instance
    Heap, Weak,                               // `new` handles
    EnumV,                                    // enum variant w/ payload
    Result,                                   // ok(v) / err(e)
    Fn,                                       // named function or lambda
    Builtin,                                  // builtin global fn / method impl
    Gen,                                      // generator (yield) — eager list
    Chan, ThreadH, Timer, Module, File, Arena, Ptr,
};

using BuiltinFn = std::function<Value(std::vector<Value>&)>;

struct HeapObj {
    std::string typeName;
    std::vector<std::pair<std::string, Value>> fields;
    std::vector<std::string> weakFields;       // `weak var` slots hold VK::Weak
};

struct Value {
    VK k = VK::None;

    // scalars
    bool b = false;
    int64_t i = 0;
    double d = 0.0;
    char32_t ch = 0;
    std::string s;                            // Str content / names below

    // containers (aliased)
    std::shared_ptr<std::vector<Value>> vec;                    // List/Set/Tuple
    std::shared_ptr<std::vector<std::pair<Value, Value>>> map;  // Dict, insertion order

    // range
    int64_t lo = 0, hi = 0;
    bool inclusive = false;

    // struct instance (copied by value)
    std::vector<std::pair<std::string, Value>> fields;

    // heap / weak
    std::shared_ptr<HeapObj> heap;
    std::weak_ptr<HeapObj> weak;

    // enum variant / result payload
    std::string typeName;                     // Struct name / Module name / Enum name
    std::string variant;                      // "ok" | "err" | enum variant
    std::vector<Value> payload;

    // functions
    const ast::Stmt* fn = nullptr;            // named FuncDef
    const ast::Expr* lam = nullptr;           // Lambda expr
    std::vector<Env> boundEnvs;               // closure envs (self env slot 0)
    Env env;                                  // defining environment

    BuiltinFn bi;                             // Builtin impl
    std::vector<std::string> biParams;        // named-arg positions for builtins
    std::vector<Value> boundSelf;             // receiver for bound methods (slot 0)

    // runtime objects
    std::shared_ptr<ChanImpl> chan;
    std::shared_ptr<ThreadImpl> thread;
    int64_t deadlineMs = 0;                   // Timer (epoch ms)

    // ---- constructors -------------------------------------------------------
    static Value none() { return Value{}; }
    static Value boolean(bool v) { Value x; x.k = VK::Bool; x.b = v; return x; }
    static Value integer(int64_t v) { Value x; x.k = VK::Int; x.i = v; return x; }
    static Value floating(double v) { Value x; x.k = VK::Float; x.d = v; return x; }
    static Value str(std::string v) { Value x; x.k = VK::Str; x.s = std::move(v); return x; }
    static Value chr(char32_t c) { Value x; x.k = VK::Char; x.ch = c; return x; }
    static Value list(std::vector<Value> vs = {}) {
        Value x; x.k = VK::List;
        x.vec = std::make_shared<std::vector<Value>>(std::move(vs));
        return x;
    }
    static Value tuple(std::vector<Value> vs) {
        Value x; x.k = VK::Tuple;
        x.vec = std::make_shared<std::vector<Value>>(std::move(vs));
        return x;
    }
    static Value set(std::vector<Value> vs = {}) {
        Value x; x.k = VK::Set;
        x.vec = std::make_shared<std::vector<Value>>(std::move(vs));
        return x;
    }
    static Value dict() {
        Value x; x.k = VK::Dict;
        x.map = std::make_shared<std::vector<std::pair<Value, Value>>>();
        return x;
    }
    static Value rangeV(int64_t lo, int64_t hi, bool incl) {
        Value x; x.k = VK::Range; x.lo = lo; x.hi = hi; x.inclusive = incl;
        return x;
    }
    static Value structV(std::string name) {
        Value x; x.k = VK::Struct; x.typeName = std::move(name);
        return x;
    }
    static Value enumV(std::string ename, std::string varName) {
        Value x; x.k = VK::EnumV; x.typeName = std::move(ename);
        x.variant = std::move(varName);
        return x;
    }
    static Value resultOk(Value v) {
        Value x; x.k = VK::Result; x.variant = "ok";
        x.payload.push_back(std::move(v)); return x;
    }
    static Value resultErr(Value e) {
        Value x; x.k = VK::Result; x.variant = "err";
        x.payload.push_back(std::move(e)); return x;
    }
    static Value module(std::string name) {
        Value x; x.k = VK::Module; x.typeName = std::move(name);
        return x;
    }
    static Value gen(std::vector<Value> vs) {
        Value x; x.k = VK::Gen;
        x.vec = std::make_shared<std::vector<Value>>(std::move(vs));
        return x;
    }

    bool isNone() const { return k == VK::None; }
};

// ---- rendering ---------------------------------------------------------------

// repr: inside containers; strings get quotes, etc.
std::string repr(const Value& v);
// to_str: top-level print conversion; strings bare.
std::string toStr(const Value& v);
bool truthy(const Value& v);

} // namespace interp
} // namespace kirn
