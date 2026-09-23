// Implementation of the Phase 8.2 scalar-native backend. See native.h.
#include "backend/native.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace kirn {
namespace backend {

using ast::ExKind;
using ast::StKind;
using sema::TyK;
using sema::TyP;

bool isScalarTy(const TyP& t) {
    if (!t) return false;
    switch (t->k) {
        case TyK::Int:
        case TyK::Float:
        case TyK::Bool:
            return true;
        default:
            return false;
    }
}

static bool isNoneTy(const TyP& t) { return !t || t->k == TyK::None; }

// C++ scalar type name for a scalar TyP; "" for non-scalar.
static const char* cppTy(const TyP& t) {
    if (!t) return "";
    switch (t->k) {
        case TyK::Int:   return "int64_t";
        case TyK::Float: return "double";
        case TyK::Bool:  return "bool";
        case TyK::Char:  return "char32_t";
        default:         return "";
    }
}

// Resolve a declaration Type node to a scalar TyP (without the checker, whose
// resolveType is private). Only plain scalar names are mapped.
static TyP declTy(const ast::TypeP& t) {
    if (!t || t->kind != ast::TyKind::Name) return sema::unkTy();
    const std::string& n = t->name;
    if (n == "int" || n == "i8" || n == "i16" || n == "i32" || n == "i64" ||
        n == "u8" || n == "u16" || n == "u32" || n == "u64" || n == "usize" ||
        n == "isize")
        return sema::intTy();
    if (n == "float" || n == "f32" || n == "f64") return sema::floatTy();
    if (n == "bool") return sema::boolTy();
    return sema::unkTy();
}

// Sanitize a Kirn identifier into a valid C identifier (locals prefixed v_).
static std::string cIdent(const std::string& s, const char* pre) {
    std::string r = pre;
    for (char c : s) r += (isalnum((unsigned char)c) ? c : '_');
    return r.empty() ? std::string("_") : r;
}

namespace {

// ---------------------------------------------------------------------------
// Analysis: fixed-point lowering of scalar functions.
// ---------------------------------------------------------------------------
class Analyzer {
public:
    Analyzer(const std::vector<ast::StmtP>& prog, const sema::Checker& chk)
        : prog_(&prog), chk_(&chk) {
        for (const auto& s : prog)
            if (s->kind == StKind::FuncDef) topFuncs_[s->name] = s.get();
    }

    std::unordered_map<const ast::Stmt*, NativeFunc> lower() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& s : *prog_) {
                if (s->kind != StKind::FuncDef) continue;
                if (lowered_.count(s.get())) continue;
                if (tryLower(*s)) changed = true;
            }
        }
        return lowered_;
    }

    const std::unordered_map<const ast::Stmt*, NativeFunc>& lowered() const {
        return lowered_;
    }

private:
    const std::vector<ast::StmtP>* prog_;
    const sema::Checker* chk_;
    std::unordered_map<std::string, const ast::Stmt*> topFuncs_;
    std::unordered_map<const ast::Stmt*, NativeFunc> lowered_;

    TyP typeOf(const ast::Expr& e) const { return chk_->typeOf(e); }

    bool tryLower(const ast::Stmt& s) {
        if (s.typeParams.size() || s.externDef || s.body.empty()) return false;
        if (s.ret && s.ret->kind == ast::TyKind::Name && s.ret->name == "result")
            return false;
        TyP rt = s.ret ? declTy(s.ret) : sema::noneTy();
        if (!isNoneTy(rt) && !isScalarTy(rt)) return false;
        for (const auto& p : s.params) {
            if (p.variadic || p.selfParam || p.defaultValue || !p.type)
                return false;
            if (!isScalarTy(declTy(p.type))) return false;
        }
        // Self-recursion tolerance: an in-progress hook is added so the body
        // walk treats self-calls as lowerable. Non-self calls must already be
        // in `lowered_` (previous fixpoint rounds).
        CurFn cur{&s, rt};
        bool ok = bodyLowerable(s.body, cur);
        if (!ok) return false;

        NativeFunc nf;
        nf.fn = &s;
        nf.name = s.name;
        nf.cName = uniqueCName(s.name);
        nf.retCpp = (rt && rt->k != TyK::None) ? cppTy(rt) : "";
        for (const auto& p : s.params) {
            nf.params.push_back(p.name);
            nf.pTypes.push_back(cppTy(declTy(p.type)));
        }
        lowered_[&s] = std::move(nf);
        return true;
    }

    struct CurFn {
        const ast::Stmt* fn;
        TyP ret;
    };

    bool callLowerable(const ast::Expr& e, const CurFn& cur) const {
        if (!e.lhs || e.lhs->kind != ExKind::Ident) return false;
        const std::string& callee = e.lhs->text;
        // self-call: lowerable if it's the current function (scalar args).
        if (cur.fn && cur.fn->name == callee) {
            for (const auto& a : e.args)
                if (a.name != "" || !a.value || !exprLowerable(*a.value, cur))
                    return false;
            return true;
        }
        auto it = topFuncs_.find(callee);
        if (it == topFuncs_.end()) return false;
        auto lf = lowered_.find(it->second);
        if (lf == lowered_.end()) return false;
        if (e.args.size() != lf->second.params.size()) return false;
        for (const auto& a : e.args)
            if (a.name != "" || !a.value || !exprLowerable(*a.value, cur))
                return false;
        return true;
    }

    bool exprLowerable(const ast::Expr& e, const CurFn& cur) const {
        TyP t = typeOf(e);
        switch (e.kind) {
            case ExKind::Int:
            case ExKind::Float:
                return true;
            case ExKind::Ident:
                return isScalarTy(t) || e.text == "true" || e.text == "false";
            case ExKind::Unary:
                if (e.rhs && !exprLowerable(*e.rhs, cur)) return false;
                return (e.op == "-" || e.op == "+" || e.op == "not") &&
                       isScalarTy(t);
            case ExKind::Binary:
                if (e.lhs && !exprLowerable(*e.lhs, cur)) return false;
                if (e.rhs && !exprLowerable(*e.rhs, cur)) return false;
                return binopLowerable(e, t);
            case ExKind::Call:
                return callLowerable(e, cur);
            default:
                return false;
        }
    }

    static bool binopLowerable(const ast::Expr& e, const TyP& t) {
        const std::string& op = e.op;
        if (op == "and" || op == "or" || op == "==" || op == "!=" ||
            op == "<" || op == "<=" || op == ">" || op == ">=")
            return t && t->k == TyK::Bool;
        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "//" ||
            op == "%" || op == "**" || op == "<<" || op == ">>" || op == "&" ||
            op == "|" || op == "^")
            return isScalarTy(t);
        return false;
    }

    bool stmtLowerable(const ast::Stmt& s, const CurFn& cur) const {
        switch (s.kind) {
            case StKind::Pass:
            case StKind::Break:
            case StKind::Continue:
                return true;
            case StKind::VarDecl:
            case StKind::ConstDecl:
                if (!s.value || !exprLowerable(*s.value, cur)) return false;
                return isScalarTy(typeOf(*s.value));
            case StKind::Assign:
            case StKind::AugAssign:
                return assignLowerable(s, cur);
            case StKind::Return: {
                if (s.exprs.empty()) return isNoneTy(cur.ret);
                if (s.exprs.size() != 1) return false;
                TyP vt = typeOf(*s.exprs[0]);
                bool ok = exprLowerable(*s.exprs[0], cur);
                if (cur.ret && cur.ret->k != TyK::None)
                    ok = ok && vt && vt->k == cur.ret->k;
                return ok;
            }
            case StKind::ExprStmt:
                return s.exprs.size() == 1 && s.exprs[0] &&
                       exprLowerable(*s.exprs[0], cur);
            case StKind::If:
                if (!s.exprs.empty() && s.exprs[0] &&
                    !exprLowerable(*s.exprs[0], cur))
                    return false;
                if (!bodyLowerable(s.body, cur)) return false;
                if (!bodyLowerable(s.elseBody, cur)) return false;
                return true;
            case StKind::While:
                return !s.exprs.empty() && s.exprs[0] &&
                       exprLowerable(*s.exprs[0], cur) &&
                       bodyLowerable(s.body, cur);
            case StKind::For:
                return forLowerable(s, cur);
            default:
                return false;
        }
    }

    bool forLowerable(const ast::Stmt& s, const CurFn& cur) const {
        if (s.exprs.size() != 1 || !s.exprs[0]) return false;
        if (!s.pat || s.pat->kind != ast::PatKind::Bind ||
            s.pat->bindName.empty())
            return false;
        const ast::Expr& it = *s.exprs[0];
        if (it.kind != ExKind::Binary || (it.op != ".." && it.op != "..="))
            return false;
        if (!it.lhs || !it.rhs) return false;
        if (!exprLowerable(*it.lhs, cur) || !exprLowerable(*it.rhs, cur))
            return false;
        if (typeOf(*it.lhs)->k != TyK::Int || typeOf(*it.rhs)->k != TyK::Int)
            return false;
        return bodyLowerable(s.body, cur);
    }

    bool assignLowerable(const ast::Stmt& s, const CurFn& cur) const {
        if (s.exprs.size() != 2) return false;
        const ast::ExprP& tgt = s.exprs[0];
        if (!tgt || tgt->kind != ExKind::Ident) return false;
        TyP tt = typeOf(*tgt);
        if (!isScalarTy(tt)) return false;
        const ast::ExprP& val = s.exprs[1];
        if (!val || !exprLowerable(*val, cur)) return false;
        TyP vt = typeOf(*val);
        return vt && vt->k == tt->k;  // same scalar type keeps C++ semantics
    }

    bool bodyLowerable(const std::vector<ast::StmtP>& body,
                       const CurFn& cur) const {
        for (const auto& s : body)
            if (!stmtLowerable(*s, cur)) return false;
        return true;
    }

    std::string uniqueCName(const std::string& name) {
        unsigned long idx = (unsigned long)(++cNameSeq_);
        return "co_native_" + std::to_string(idx) + "_" + cIdent(name, "");
    }
    int cNameSeq_ = 0;
};

// ---------------------------------------------------------------------------
// Emission: write native C++ for the lowered functions.
// ---------------------------------------------------------------------------
class Emitter {
public:
    Emitter(std::ostream& out,
            const std::unordered_map<const ast::Stmt*, NativeFunc>& lowered,
            const sema::Checker& chk)
        : out_(out), lowered_(&lowered), chk_(&chk) {}

    void emitPreamble() {
        out_ << "#include <cmath>\n"
             << "namespace kirn_native {\n"
             << "static inline void co_panic(const char* m){ "
                "kirn::interp::panicHere(m); }\n"
             << "static inline int64_t co_floordiv(int64_t a,int64_t b){"
                " if(b==0)co_panic(\"division by zero\"); int64_t q=a/b;"
                " if((a%b!=0)&&((a<0)!=(b<0)))--q; return q; }\n"
             << "static inline int64_t co_mod(int64_t a,int64_t b){"
                " if(b==0)co_panic(\"modulo by zero\"); int64_t m=a%b;"
                " if(m!=0&&((m<0)!=(b<0)))m+=b; return m; }\n"
             << "static inline double co_fdiv(double a,double b){"
                " if(b==0.0)co_panic(\"division by zero\"); return a/b; }\n"
             << "static inline double co_fmodm(double a,double b){"
                " return std::fmod(std::fmod(a,b)+b,b); }\n"
             << "static inline int64_t co_ipow(int64_t b,int64_t e){"
                " int64_t o=1; while(e>0){ if(e&1)o*=b; e>>=1; if(e)b*=b;}"
                " return o; }\n"
             << "static inline int64_t co_shl(int64_t a,int64_t b){"
                " if(b<0||b>=63)kirn::interp::panicHere(\"shift\");"
                " return (int64_t)((uint64_t)a<<b); }\n"
             << "static inline int64_t co_shr(int64_t a,int64_t b){"
                " if(b<0||b>=63)kirn::interp::panicHere(\"shift\");"
                " return a>>b; }\n";
    }

    void emitFunctions(const std::vector<ast::StmtP>& prog) {
        for (const auto& s : prog) {
            if (s->kind != StKind::FuncDef) continue;
            auto it = lowered_->find(s.get());
            if (it != lowered_->end()) emitFunction(*s, it->second);
        }
        out_ << "}\n";  // close namespace kirn_native
    }

private:
    std::ostream& out_;
    const std::unordered_map<const ast::Stmt*, NativeFunc>* lowered_;
    const sema::Checker* chk_;
    const NativeFunc* cur_ = nullptr;
    std::unordered_map<std::string, std::string> localTypes_;  // name -> cpp type
    std::unordered_map<std::string, const NativeFunc*> calleeFunc_;

    TyP typeOf(const ast::Expr& e) const { return chk_->typeOf(e); }

    void emitFunction(const ast::Stmt& s, const NativeFunc& nf) {
        cur_ = &nf;
        localTypes_.clear();
        for (size_t i = 0; i < nf.params.size(); ++i)
            localTypes_[nf.params[i]] = nf.pTypes[i];
        for (auto& [name, fn] : *lowered_) calleeFunc_[fn.name] = &fn;
        // scalar-core signature: plain params -> plain scalar return. The
        // registration thunk (out_registerAll) only boxes/unboxes at the edges;
        // native->native calls pass scalars straight through.
        std::string sig;
        for (size_t i = 0; i < nf.params.size(); ++i)
            sig += (i ? ", " : "") + nf.pTypes[i] + " " +
                   cIdent(nf.params[i], "v_");
        out_ << "static " << (nf.retCpp.empty() ? "void" : nf.retCpp) << " "
             << nf.cName << "(" << sig << ") {\n";
        for (const auto& st : s.body) emitStmt(*st, 1);
        // implicit fallthrough return
        if (nf.retCpp.empty())
            out_ << "    return;\n";
        else
            out_ << "    return " << zeroOf(nf.retCpp) << ";\n";
        out_ << "}\n";
        cur_ = nullptr;
    }

    static const char* valueCtor(const std::string& ty) {
        return ty == "int64_t" ? "integer" : ty == "double" ? "floating"
                                                            : "boolean";
    }
    static const char* envField(const std::string& ty) {
        return ty == "int64_t" ? "i" : ty == "double" ? "d" : "b";
    }
    static const char* zeroOf(const std::string& ty) {
        return ty == "int64_t" ? "0" : ty == "double" ? "0.0" : "false";
    }

    const NativeFunc* findCallee(const std::string& name) const {
        for (auto& [fn, nf] : *lowered_)
            if (nf.name == name) return &nf;
        return nullptr;
    }

    // ---- statements --------------------------------------------------------
    void emitStmt(const ast::Stmt& s, int ind) {
        std::string pad(4 * ind, ' ');
        switch (s.kind) {
            case StKind::Pass:
                return;
            case StKind::VarDecl:
            case StKind::ConstDecl: {
                TyP vt = typeOf(*s.value);
                std::string ty = cppTy(vt);
                std::string name = cIdent(s.target->text, "v_");
                out_ << pad << ty << " " << name << " = " << emitExpr(*s.value)
                     << ";\n";
                localTypes_[s.target->text] = ty;
                return;
            }
            case StKind::Assign:
            case StKind::AugAssign: {
                const std::string& cname = s.exprs[0]->text;
                std::string name = cIdent(cname, "v_");
                std::string val;
                if (s.kind == StKind::AugAssign) {
                    // rebuild target op value with binop semantics
                    val = emitBinop(s.augOp, *s.exprs[0], *s.exprs[1]);
                } else {
                    val = emitExpr(*s.exprs[1]);
                }
                auto lf = localTypes_.find(cname);
                if (lf != localTypes_.end()) {
                    out_ << pad << name << " = " << val << ";\n";
                } else {
                    // first (possibly implicit) binding of a mutable local:
                    // emit the declaration so later assignments stay plain.
                    const std::string ty = cppTy(typeOf(*s.exprs[0]));
                    localTypes_[cname] = ty;
                    out_ << pad << ty << " " << name << " = " << val << ";\n";
                }
                return;
            }
            case StKind::Return: {
                if (s.exprs.empty()) {
                    out_ << pad << "return;\n";
                } else {
                    out_ << pad << "return " << emitExpr(*s.exprs[0]) << ";\n";
                }
                return;
            }
            case StKind::ExprStmt: {
                out_ << pad << "(void)" << emitExpr(*s.exprs[0]) << ";\n";
                return;
            }
            case StKind::If:
                emitIf(s, ind);
                return;
            case StKind::While: {
                out_ << pad << "while (" << emitExpr(*s.exprs[0]) << ") {\n";
                for (const auto& st : s.body) emitStmt(*st, ind + 1);
                out_ << pad << "}\n";
                return;
            }
            case StKind::For:
                emitFor(s, ind);
                return;
            case StKind::Break:
                out_ << pad << "break;\n";
                return;
            case StKind::Continue:
                out_ << pad << "continue;\n";
                return;
            default:
                return;
        }
    }

    void emitIf(const ast::Stmt& s, int ind) {
        std::string pad(4 * ind, ' ');
        out_ << pad << "if (" << emitExpr(*s.exprs[0]) << ") {\n";
        for (const auto& st : s.body) emitStmt(*st, ind + 1);
        if (!s.elseBody.empty()) {
            out_ << pad << "} else {\n";
            for (const auto& st : s.elseBody) emitStmt(*st, ind + 1);
        }
        out_ << pad << "}\n";
    }

    void emitFor(const ast::Stmt& s, int ind) {
        std::string pad(4 * ind, ' ');
        const ast::Expr& it = *s.exprs[0];
        bool inclusive = it.op == "..=";
        std::string loopVar = cIdent(s.pat->bindName, "v_");
        std::string lo = emitExpr(*it.lhs);
        std::string hi = emitExpr(*it.rhs);
        std::string cmp = inclusive ? "<=" : "<";
        out_ << pad << "for (int64_t " << loopVar << " = (" << lo << "); "
             << loopVar << " " << cmp << " (" << hi << "); ++" << loopVar
             << ") {\n";
        localTypes_[s.pat->bindName] = "int64_t";
        for (const auto& st : s.body) emitStmt(*st, ind + 1);
        out_ << pad << "}\n";
    }

    // ---- expressions -------------------------------------------------------
    std::string emitExpr(const ast::Expr& e) {
        TyP t = typeOf(e);
        switch (e.kind) {
            case ExKind::Int: {
                try {
                    long long v = std::stoll(e.text);
                    return std::to_string(v);
                } catch (...) {
                    return "0";
                }
            }
            case ExKind::Float: {
                std::string s = e.text;
                if (s.find_first_of(".eE") == std::string::npos) s += ".0";
                return s;
            }
            case ExKind::Ident:
                if (e.text == "true") return "true";
                if (e.text == "false") return "false";
                return cIdent(e.text, "v_");
            case ExKind::Unary: {
                std::string r = emitExpr(*e.rhs);
                if (e.op == "-") return "(-(" + r + "))";
                if (e.op == "+") return "(+(" + r + "))";
                if (e.op == "not") return "(!(" + r + "))";
                return r;
            }
            case ExKind::Binary:
                return emitBinop(e.op, *e.lhs, *e.rhs);
            case ExKind::Call: {
                const std::string& callee = e.lhs->text;
                const NativeFunc* cf = findCallee(callee);
                std::string s = cf->cName + "(";
                for (size_t i = 0; i < e.args.size(); ++i) {
                    if (i) s += ", ";
                    s += emitExpr(*e.args[i].value);
                }
                return s + ")";
            }
            default:
                return "0";
        }
    }

    std::string emitBinop(const std::string& op, const ast::Expr& l,
                          const ast::Expr& r) {
        TyP tl = typeOf(l), tr = typeOf(r);
        std::string le = emitExpr(l), re = emitExpr(r);
        bool lint = tl && tl->k == TyK::Int;
        bool rint = tr && tr->k == TyK::Int;
        bool lflt = tl && tl->k == TyK::Float;
        bool rflt = tr && tr->k == TyK::Float;
        bool mixed = (lint && rflt) || (lflt && rint);

        if (op == "and") return "(" + le + " && " + re + ")";
        if (op == "or") return "(" + le + " || " + re + ")";
        if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
            op == ">=") {
            if (mixed || lflt || rflt)
                return "((" + le + ") " + relOp(op) + " (" + re + "))";
            return "(" + le + " " + relOp(op) + " " + re + ")";
        }

        // arithmetic
        if (op == "+" || op == "-" || op == "*") {
            if (mixed) return "((" + le + ") " + op + " (" + re + "))";
            return "(" + le + " " + op + " " + re + ")";
        }
        if (op == "/") {
            return "kirn_native::co_fdiv((double)(" + le + "),(double)(" + re +
                   "))";
        }
        if (op == "//") {
            if (lint && rint) return "kirn_native::co_floordiv(" + le + "," + re + ")";
            return "std::floor((" + le + ")/(" + re + "))";
        }
        if (op == "%") {
            if (lint && rint) return "kirn_native::co_mod(" + le + "," + re + ")";
            return "kirn_native::co_fmodm((" + le + "),(" + re + "))";
        }
        if (op == "**") {
            if (lint && rint) return "kirn_native::co_ipow(" + le + "," + re + ")";
            return "std::pow((" + le + "),(" + re + "))";
        }
        if (op == "<<") return "kirn_native::co_shl(" + le + "," + re + ")";
        if (op == ">>") return "kirn_native::co_shr(" + le + "," + re + ")";
        if (op == "&") return "(" + le + " & " + re + ")";
        if (op == "|") return "(" + le + " | " + re + ")";
        if (op == "^") return "(" + le + " ^ " + re + ")";
        return "(" + le + ")";
    }

    static const char* relOp(const std::string& op) {
        if (op == "==") return "==";
        if (op == "!=") return "!=";
        if (op == "<=") return "<=";
        if (op == ">=") return ">=";
        if (op == "<") return "<";
        return ">";
    }
};

}  // namespace

NativeProgram funcsFrom(
    const std::unordered_map<const ast::Stmt*, NativeFunc>& lowered);
void out_registerAll(
    std::ostream& out,
    const std::unordered_map<const ast::Stmt*, NativeFunc>& lowered);

NativeProgram emitNative(std::ostream& out,
                         const std::vector<ast::StmtP>& prog,
                         const sema::Checker& chk) {
    Analyzer an(prog, chk);
    auto lowered = an.lower();
    if (lowered.empty())
        return { {}, false };

    Emitter em(out, lowered, chk);
    em.emitPreamble();
    em.emitFunctions(prog);
    // Registration helper that wires each lowered function into an Interpreter.
    out_registerAll(out, lowered);
    return funcsFrom(lowered);
}

NativeProgram funcsFrom(
    const std::unordered_map<const ast::Stmt*, NativeFunc>& lowered) {
    NativeProgram np;
    np.any = !lowered.empty();
    for (auto& [fn, nf] : lowered) np.funcs.push_back(nf);
    return np;
}

void out_registerAll(
    std::ostream& out,
    const std::unordered_map<const ast::Stmt*, NativeFunc>& lowered) {
    auto envField = [](const std::string& t) -> const char* {
        return t == "int64_t" ? "i" : t == "double" ? "d" : "b";
    };
    auto valueCtor = [](const std::string& t) -> const char* {
        return t == "int64_t" ? "integer" : t == "double" ? "floating"
                                                          : "boolean";
    };
    out << "inline void kirn_native_register(kirn::interp::Interpreter& interp,"
           " const std::vector<kirn::ast::StmtP>& prog) {\n";
    for (auto& [fn, nf] : lowered) {
        out << "    for (const auto& d : prog) if (d->kind == "
               "kirn::ast::StKind::FuncDef && d->name == \""
            << nf.name
            << "\") { interp.enableNative(); interp.registerNative(d.get(), "
               "[&interp](kirn::interp::Env e) -> kirn::interp::Value {\n";
        // read scalar params out of the caller's Env
        for (size_t i = 0; i < nf.params.size(); ++i) {
            std::string v = "v_";
            for (char c : nf.params[i]) v += isalnum((unsigned char)c) ? c : '_';
            out << "        " << nf.pTypes[i] << " " << v
                            << " = interp.nativeEnvVar(e, \""
                            << nf.params[i] << "\")." << envField(nf.pTypes[i])
                            << ";\n";
        }
        std::string call = "kirn_native::" + nf.cName + "(";
        for (size_t i = 0; i < nf.params.size(); ++i) {
            if (i) call += ", ";
            std::string v = "v_";
            for (char c : nf.params[i]) v += isalnum((unsigned char)c) ? c : '_';
            call += v;
        }
        call += ")";
        if (nf.retCpp.empty())
            out << "        " << call << "; return kirn::interp::Value::none();\n";
        else
            out << "        return kirn::interp::Value::" << valueCtor(nf.retCpp)
                << "(" << call << ");\n";
        out << "    }); }\n";
    }
    out << "}\n";
}

} // namespace backend
} // namespace kirn
