#pragma once
// Symbols & lexical scopes for Kirn sema.
#include "sema/type.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace kirn {

namespace ast { struct Stmt; }   // forward decl for Symbol::importStmt

namespace sema {

enum class SymK {
    Var,          // local binding (`x = ...` / `var x = ...`)
    Param,
    Const,
    Func,
    Struct,
    EnumName,
    EnumVariant,  // ctor-able variant
    Trait,
    ImportRoot,   // module alias from import (dynamically typed v1)
};

struct FuncSig {
    std::vector<TyP> params;
    std::vector<std::string> names;   // parallel; empty string = unnamed
    TyP ret;
    bool variadic = false;
    size_t required = static_cast<size_t>(-1);  // min arg count (params beyond are optional)

    size_t arity() const { return params.size(); }
};

struct Symbol {
    SymK kind{};
    std::string name;
    TyP type;                         // value type (Var/Param/Const/EnumVal/...)
    bool mut = false;
    bool pub = false;
    bool builtin = false;         // prelude builtin: overridable by user FuncDef

    // Lint/use-information (PLAN phase 2). Set during the body-checking pass.
    bool used = false;                // referenced anywhere as a value
    bool mutated = false;             // assigned via `=`/aug-assign or `var`
    uint32_t declLine = 0;
    uint32_t declCol = 0;
    const ast::Stmt* importStmt = nullptr;   // ImportRoot -> owning `import`

    FuncSig sig;                      // Func

    // Generic Func: type-param name -> resolved default type (`def f[T = int]`).
    // Empty when the function declares no defaults. Read at call sites so an
    // unconstrained TypeVar in the return type becomes its default.
    std::map<std::string, TyP> typeDefaults;

    // Struct: fields in decl order; methods resolved separately below.
    std::vector<std::pair<std::string, TyP>> fields;
    std::map<std::string, FuncSig> methods;     // inherent methods (self-bound)

    // Trait: required signatures (+ default bodies checked like funcs).
    std::map<std::string, FuncSig> sigs;

    // Enum variant payload (named).
    std::vector<std::pair<std::string, TyP>> payloads;
    std::string enumOf;               // EnumVariant -> owning enum name

    // EnumName: variant name -> resolved payload fields
    std::map<std::string, std::vector<std::pair<std::string, TyP>>> variantPayloads;
    std::vector<std::string> variants;   // all variant names in declaration order

    int homeScope = 0;                // declaring Scope::id (capture detection)

    // `temp <name> <N>`: remaining allowed uses (positive = active budget).
    // Decremented in checkExpr for each Ident read; declared with tempBudget=N.
    int tempRemaining = -1;           // -1 = not a temp
    int tempBudget = 0;               // original budget (diagnostics only)
};

using SymP = std::shared_ptr<Symbol>;

struct Scope {
    explicit Scope(Scope* parent_ = nullptr) : parent(parent_), id(nextId()++) {}

    Symbol* find(const std::string& n) {
        for (Scope* s = this; s; s = s->parent) {
            auto it = s->syms.find(n);
            if (it != s->syms.end()) return it->second.get();
        }
        return nullptr;
    }

    // Returns nullptr on success, or the clashing symbol.
    Symbol* declare(SymP sym) {
        auto [it, inserted] = syms.emplace(sym->name, std::move(sym));
        return inserted ? nullptr : it->second.get();
    }

    Scope* parent;
    std::map<std::string, SymP> syms;
    int id = 0;

private:
    static unsigned& nextId() {
        static unsigned n = 0;
        return n;
    }
};

struct Scoped {
    explicit Scoped(Scope*& cur, Scope* next) : cur(cur), prev(cur) { cur = next; }
    ~Scoped() { delete cur; cur = prev; }
    Scope*& cur;
    Scope* const prev;
};

} // namespace sema
} // namespace kirn
