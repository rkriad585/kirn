#include "ast/ast.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace kirn {
namespace ast {
namespace {

std::string ind(int n) { return std::string(static_cast<size_t>(n) * 2, ' '); }

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            default:   out += c;
        }
    }
    out += '"';
    return out;
}

const char* flavorSuffix(StrFlavor f) {
    switch (f) {
        case StrFlavor::Raw:   return " r";
        case StrFlavor::Byte:  return " b";
        case StrFlavor::C:     return " c";
        default:               return "";
    }
}

// ---- types (rendered inline) ------------------------------------------------

void typeInto(const Type& t, std::string& out) {
    switch (t.kind) {
        case TyKind::Name: {
            out += t.name;
            if (!t.generics.empty()) {
                out += '[';
                for (size_t i = 0; i < t.generics.size(); ++i) {
                    if (i) out += ", ";
                    if (t.generics[i]) typeInto(*t.generics[i], out);
                }
                out += ']';
            }
            break;
        }
        case TyKind::Pointer:
            out += '*';
            if (t.inner) typeInto(*t.inner, out);
            break;
        case TyKind::Ref:
            out += '&';
            if (t.refMut) out += "mut ";
            if (t.inner) typeInto(*t.inner, out);
            break;
        case TyKind::Optional:
            if (t.inner) typeInto(*t.inner, out);
            out += '?';
            break;
        case TyKind::Fn: {
            out += "fn(";
            for (size_t i = 0; i < t.params.size(); ++i) {
                if (i) out += ", ";
                if (t.params[i]) typeInto(*t.params[i], out);
            }
            out += ')';
            if (t.ret) {
                out += " -> ";
                typeInto(*t.ret, out);
            }
            break;
        }
        case TyKind::Tuple: {
            out += '(';
            for (size_t i = 0; i < t.params.size(); ++i) {
                if (i) out += ", ";
                if (t.params[i]) typeInto(*t.params[i], out);
            }
            out += ')';
            break;
        }
    }
}

std::string typeStr(const TypeP& t) {
    std::string out;
    if (t) typeInto(*t, out);
    else out = "<inferred>";
    return out;
}

// ---- expressions -------------------------------------------------------------

const char* exName(ExKind k) {
    switch (k) {
        case ExKind::Int:       return "Int";
        case ExKind::Float:     return "Float";
        case ExKind::CharLit:   return "Char";
        case ExKind::Str:       return "Str";
        case ExKind::FString:   return "FString";
        case ExKind::Ident:     return "Ident";
        case ExKind::Unary:     return "Unary";
        case ExKind::Binary:    return "Binary";
        case ExKind::Call:      return "Call";
        case ExKind::Index:     return "Index";
        case ExKind::Slice:     return "Slice";
        case ExKind::Member:    return "Member";
        case ExKind::Try:       return "Try";
        case ExKind::Lambda:    return "Lambda";
        case ExKind::Cond:      return "Cond";
        case ExKind::ListComp:  return "ListComp";
        case ExKind::Generator: return "Generator";
        case ExKind::List:      return "List";
        case ExKind::Dict:      return "Dict";
        case ExKind::Set:       return "Set";
        case ExKind::Tuple:     return "Tuple";
        case ExKind::New:      return "New";
        case ExKind::Cast:     return "Cast";
        case ExKind::Match:    return "Match";
    }
    return "?";
}

void dumpExpr(const Expr& e, int d);
void dumpPat(const Pat& p, int d);

void childExpr(const char* label, const ExprP& e, int d) {
    if (!e) return;
    std::cout << ind(d) << label << ' ';
    if (e->kind == ExKind::Int || e->kind == ExKind::Float ||
        e->kind == ExKind::CharLit || e->kind == ExKind::Str ||
        e->kind == ExKind::Ident)
        std::cout << exName(e->kind) << ' '
                  << quote(e->text) << flavorSuffix(e->flavor) << '\n';
    else
        std::cout << '\n', dumpExpr(*e, d + 1);
}

void dumpClause(const CompClause& c, int d) {
    if (c.isFor) {
        std::cout << ind(d) << "For\n";
        if (c.pat) dumpPat(*c.pat, d + 1);
        if (c.iter) childExpr("Iter", c.iter, d);
    } else if (c.cond) {
        std::cout << ind(d) << "Guard\n";
        childExpr("Cond", c.cond, d + 1);
    }
}

void dumpParts(const std::vector<FStrPart>& parts, int d) {
    for (const auto& p : parts) {
        if (!p.isExpr) {
            std::cout << ind(d) << "Text " << quote(p.text) << '\n';
        } else {
            std::cout << ind(d) << "Interp"
                      << (p.spec.empty() ? "" : " spec=" + p.spec) << '\n';
            if (p.expr) childExpr("Value", p.expr, d + 1);
        }
    }
}

void dumpExpr(const Expr& e, int d) {
    switch (e.kind) {
        case ExKind::Int:
        case ExKind::Float:
        case ExKind::CharLit:
        case ExKind::Str:
        case ExKind::Ident:
            std::cout << ind(d) << exName(e.kind) << ' '
                      << quote(e.text) << flavorSuffix(e.flavor) << '\n';
            return;
        case ExKind::FString:
            std::cout << ind(d) << "FString\n";
            dumpParts(e.parts, d + 1);
            return;
        case ExKind::Unary:
            std::cout << ind(d) << "Unary op=" << quote(e.op) << '\n';
            childExpr("Operand", e.rhs, d + 1);
            return;
        case ExKind::Binary:
            std::cout << ind(d) << "Binary op=" << quote(e.op) << '\n';
            childExpr("Lhs", e.lhs, d + 1);
            childExpr("Rhs", e.rhs, d + 1);
            return;
        case ExKind::Call:
            std::cout << ind(d) << "Call\n";
            childExpr("Callee", e.lhs, d + 1);
            for (const auto& a : e.args) {
                std::cout << ind(d + 1) << "Arg"
                          << (a.name.empty() ? "" : " name=" + a.name) << '\n';
                if (a.value) childExpr("Value", a.value, d + 2);
            }
            return;
        case ExKind::New:
            std::cout << ind(d) << "New type=" << typeStr(e.newType) << '\n';
            for (const auto& a : e.args) {
                std::cout << ind(d + 1) << "Arg"
                          << (a.name.empty() ? "" : " name=" + a.name) << '\n';
                if (a.value) childExpr("Value", a.value, d + 2);
            }
            return;
        case ExKind::Cast:
            std::cout << ind(d) << "Cast as " << typeStr(e.newType) << '\n';
            childExpr("Operand", e.lhs, d + 1);
            return;
        case ExKind::Match:
            std::cout << ind(d) << "Match\n";
            childExpr("Subject", e.lhs, d + 1);
            for (const auto& arm : e.matchArms) {
                std::cout << ind(d + 1) << "Arm\n";
                dumpPat(*arm.pat, d + 2);
                if (arm.guard) childExpr("Guard", arm.guard, d + 2);
                for (const auto& st : arm.body) dump(*st, d + 2);
            }
            return;
        case ExKind::Index:
            std::cout << ind(d) << "Index\n";
            childExpr("Obj", e.lhs, d + 1);
            childExpr("At", e.rhs, d + 1);
            return;
        case ExKind::Slice: {
            std::cout << ind(d) << "Slice\n";
            childExpr("Obj", e.lhs, d + 1);
            const char* labels[3] = {"Lo", "Hi", "Step"};
            for (size_t i = 0; i < 3 && i < e.elems.size(); ++i)
                if (e.elems[i]) childExpr(labels[i], e.elems[i], d + 1);
            return;
        }
        case ExKind::Member:
            std::cout << ind(d) << "Member name=" << quote(e.text)
                      << (e.nilSafe ? " nilsafe" : "") << '\n';
            childExpr("Obj", e.lhs, d + 1);
            return;
        case ExKind::Try:
            std::cout << ind(d) << "Try '?'\n";
            childExpr("Operand", e.lhs, d + 1);
            return;
        case ExKind::Lambda: {
            std::cout << ind(d) << "Lambda params=(";
            if (!e.closureParams.empty()) {
                for (size_t i = 0; i < e.closureParams.size(); ++i) {
                    if (i) std::cout << ", ";
                    std::cout << e.closureParams[i].name
                              << ": " << typeStr(e.closureParams[i].type);
                }
                std::cout << ")\n";
                if (e.retType)
                    std::cout << ind(d + 1) << "Ret " << typeStr(e.retType) << '\n';
                std::cout << ind(d + 1) << "Body\n";
                for (const auto& st : e.lambdaBody) dump(*st, d + 2);
            } else {
                for (size_t i = 0; i < e.lambdaParams.size(); ++i)
                    std::cout << (i ? ", " : "") << e.lambdaParams[i];
                std::cout << ")\n";
                childExpr("Body", e.rhs, d + 1);
            }
            return;
        }
        case ExKind::Cond:
            std::cout << ind(d) << "Cond\n";
            childExpr("If", e.cond, d + 1);
            childExpr("Then", e.lhs, d + 1);
            childExpr("Else", e.rhs, d + 1);
            return;
        case ExKind::ListComp:
        case ExKind::Generator:
            std::cout << ind(d) << exName(e.kind) << '\n';
            if (!e.elems.empty()) childExpr("Element", e.elems[0], d + 1);
            for (const auto& c : e.clauses) dumpClause(c, d + 1);
            return;
        case ExKind::List:
        case ExKind::Set:
        case ExKind::Tuple:
            std::cout << ind(d) << exName(e.kind)
                      << " len=" << e.elems.size() << '\n';
            for (const auto& el : e.elems)
                childExpr("Item", el, d + 1);
            return;
        case ExKind::Dict:
            std::cout << ind(d) << "Dict len=" << e.pairs.size() << '\n';
            for (const auto& pr : e.pairs) {
                std::cout << ind(d + 1) << "Pair\n";
                childExpr("Key", pr.first, d + 2);
                childExpr("Val", pr.second, d + 2);
            }
            return;
    }
}

// ---- patterns ------------------------------------------------------------------

void dumpPat(const Pat& p, int d) {
    switch (p.kind) {
        case PatKind::Wild:
            std::cout << ind(d) << "PatWild\n";
            return;
        case PatKind::Literal:
            std::cout << ind(d) << "PatLiteral\n";
            if (p.literal) childExpr("Value", p.literal, d + 1);
            return;
        case PatKind::Range:
            std::cout << ind(d) << "PatRange "
                      << (p.inclusive ? "..=" : "..") << '\n';
            if (p.lo && p.lo->literal) childExpr("Lo", p.lo->literal, d + 1);
            if (p.hi && p.hi->literal) childExpr("Hi", p.hi->literal, d + 1);
            return;
        case PatKind::Tuple:
            std::cout << ind(d) << "PatTuple len=" << p.elems.size() << '\n';
            for (auto& el : p.elems) dumpPat(*el, d + 1);
            return;
        case PatKind::Ctor:
            std::cout << ind(d) << "PatCtor " << p.ctorName << '\n';
            for (auto& f : p.fields) {
                std::cout << ind(d + 1) << "Field"
                          << (f.name.empty() ? "" : " name=" + f.name) << '\n';
                if (f.pat) dumpPat(*f.pat, d + 2);
            }
            return;
        case PatKind::Bind:
            std::cout << ind(d) << "PatBind " << p.bindName
                      << (p.bindType ? " : " + typeStr(p.bindType) : "") << '\n';
            return;
        case PatKind::BindAlias:
            std::cout << ind(d) << "PatBindAlias " << p.bindName << '\n';
            if (p.aliasSub) dumpPat(*p.aliasSub, d + 1);
            return;
        case PatKind::Or:
            std::cout << ind(d) << "PatOr\n";
            for (auto& a : p.alts) dumpPat(*a, d + 1);
            return;
        case PatKind::Slice:
            std::cout << ind(d) << "PatSlice elems=" << p.elems.size()
                      << (p.hasRest ? (p.restName.empty() ? " .."
                                                          : " .." + p.restName)
                                    : "") << '\n';
            for (auto& el : p.elems) dumpPat(*el, d + 1);
            return;
        case PatKind::Rest:
            std::cout << ind(d) << "PatRest\n";
            return;
        case PatKind::Ref:
            std::cout << ind(d) << "PatRef\n";
            if (p.inner) dumpPat(*p.inner, d + 1);
            return;
    }
}

} // namespace

// ---- statements ------------------------------------------------------------

void dump(const Stmt& s, int d) {
    switch (s.kind) {
        case StKind::FuncDef: {
            std::cout << ind(d) << "FuncDef " << quote(s.name)
                      << (s.pub ? " pub" : "")
                      << (s.externDef ? " extern" : "");
            if (!s.typeParams.empty()) {
                std::cout << " generics=[";
                for (size_t i = 0; i < s.typeParams.size(); ++i) {
                    if (i) std::cout << ", ";
                    std::cout << s.typeParams[i].name;
                    if (s.typeParams[i].bound)
                        std::cout << " is " << typeStr(s.typeParams[i].bound);
                    if (s.typeParams[i].def)
                        std::cout << " = " << typeStr(s.typeParams[i].def);
                }
                std::cout << ']';
            }
            std::cout << '\n';
            for (const auto& p : s.params) {
                std::string head = ind(d + 1) + "Param " + quote(p.name);
                if (p.selfParam) head += " self";
                if (p.variadic) head += " variadic";
                if (p.mutable_) head += " mut";
                head += " : " + typeStr(p.type);
                std::cout << head << '\n';
                if (p.defaultValue) childExpr("Default", p.defaultValue, d + 2);
            }
            if (s.ret) std::cout << ind(d + 1) << "Ret " << typeStr(s.ret) << '\n';
            std::cout << ind(d + 1) << "Body\n";
            for (const auto& st : s.body) dump(*st, d + 2);
            return;
        }
        case StKind::StructDef:
            std::cout << ind(d) << "StructDef " << quote(s.name)
                      << (s.pub ? " pub" : "") << '\n';
            for (const auto& f : s.fields) {
                std::cout << ind(d + 1) << "Field " << quote(f.name)
                          << (f.pub ? " pub" : "") << (f.mutable_ ? " var" : "")
                          << (f.weak ? " weak" : "")
                          << " : " << typeStr(f.type) << '\n';
                if (f.defaultValue) childExpr("Default", f.defaultValue, d + 2);
            }
            for (const auto& m : s.body) dump(*m, d + 1);
            return;
        case StKind::EnumDef:
            std::cout << ind(d) << "EnumDef " << quote(s.name)
                      << (s.pub ? " pub" : "") << '\n';
            for (const auto& v : s.variants) {
                std::cout << ind(d + 1) << "Variant " << v.name;
                if (!v.payload.empty()) {
                    std::cout << " {";
                    for (size_t i = 0; i < v.payload.size(); ++i) {
                        if (i) std::cout << ", ";
                        std::cout << v.payload[i].name << ": "
                                  << typeStr(v.payload[i].type);
                    }
                    std::cout << '}';
                }
                std::cout << '\n';
            }
            return;
        case StKind::TraitDef:
            std::cout << ind(d) << "TraitDef " << quote(s.name)
                      << (s.pub ? " pub" : "") << '\n';
            for (const auto& sig : s.sigs) {
                std::cout << ind(d + 1) << "Sig " << sig.name << '(';
                for (size_t i = 0; i < sig.params.size(); ++i) {
                    if (i) std::cout << ", ";
                    std::cout << sig.params[i].name << ": "
                              << typeStr(sig.params[i].type);
                }
                std::cout << ')';
                if (sig.ret) std::cout << " -> " << typeStr(sig.ret);
                std::cout << '\n';
            }
            for (const auto& m : s.body) dump(*m, d + 1);
            return;
        case StKind::ImplDef:
            std::cout << ind(d) << "ImplDef " << typeStr(s.implTrait)
                      << " for " << typeStr(s.implType) << '\n';
            for (const auto& m : s.body) dump(*m, d + 1);
            return;
        case StKind::ConstDecl:
            std::cout << ind(d) << "ConstDecl\n";
            if (s.target) childExpr("Target", s.target, d + 1);
            if (s.declType)
                std::cout << ind(d + 1) << "Type " << typeStr(s.declType) << '\n';
            if (s.value) childExpr("Value", s.value, d + 1);
            return;
        case StKind::VarDecl:
            std::cout << ind(d) << "VarDecl"
                      << (s.declType ? " : " + typeStr(s.declType) : "") << '\n';
            if (s.target) childExpr("Target", s.target, d + 1);
            if (s.value) childExpr("Value", s.value, d + 1);
            return;
        case StKind::Pass:
            std::cout << ind(d) << "Pass\n";
            return;
        case StKind::ExprStmt:
            std::cout << ind(d) << "ExprStmt\n";
            if (!s.exprs.empty() && s.exprs[0]) childExpr("Value", s.exprs[0], d + 1);
            return;
        case StKind::Assign: {
            std::cout << ind(d) << "Assign\n";
            size_t n = s.exprs.size();
            bool paired = n >= 2 && n % 2 == 0;
            size_t half = paired ? n / 2 : 0;
            for (size_t i = 0; i < n; ++i) {
                if (!s.exprs[i]) continue;
                if (paired)
                    childExpr(i < half ? "Target" : "Value", s.exprs[i], d + 1);
                else
                    childExpr("Expr", s.exprs[i], d + 1);
            }
            return;
        }
        case StKind::AugAssign:
            std::cout << ind(d) << "AugAssign op=" << quote(s.augOp) << '\n';
            if (s.exprs.size() > 0) childExpr("Target", s.exprs[0], d + 1);
            if (s.exprs.size() > 1) childExpr("Value", s.exprs[1], d + 1);
            return;
        case StKind::Return:
            std::cout << ind(d) << "Return\n";
            for (const auto& e : s.exprs)
                childExpr("Value", e, d + 1);
            return;
        case StKind::Raise:
            std::cout << ind(d) << "Raise\n";
            for (const auto& e : s.exprs)
                childExpr("Value", e, d + 1);
            return;
        case StKind::Break:
            std::cout << ind(d) << "Break\n";
            return;
        case StKind::Continue:
            std::cout << ind(d) << "Continue\n";
            return;
        case StKind::Defer:
            std::cout << ind(d) << "Defer\n";
            for (const auto& e : s.exprs)
                childExpr("Call", e, d + 1);
            return;
        case StKind::Spawn:
            std::cout << ind(d) << "Spawn\n";
            for (const auto& e : s.exprs)
                childExpr("Call", e, d + 1);
            return;
        case StKind::If: {
            std::cout << ind(d) << "If\n";
            if (!s.exprs.empty()) childExpr("Cond", s.exprs[0], d + 1);
            std::cout << ind(d + 1) << "Then\n";
            for (const auto& st : s.body) dump(*st, d + 2);
            // elif chains arrive nested as a synthetic If in elseBody[0]
            if (s.elseBody.size() == 1 && s.elseBody[0] &&
                s.elseBody[0]->kind == StKind::If &&
                !s.elseBody[0]->body.empty()) {
                dump(*s.elseBody[0], d);
                return;
            }
            if (!s.elseBody.empty()) {
                std::cout << ind(d + 1) << "Else\n";
                for (const auto& st : s.elseBody) dump(*st, d + 2);
            }
            return;
        }
        case StKind::While:
            std::cout << ind(d) << "While\n";
            if (!s.exprs.empty()) childExpr("Cond", s.exprs[0], d + 1);
            std::cout << ind(d + 1) << "Body\n";
            for (const auto& st : s.body) dump(*st, d + 2);
            return;
        case StKind::DoWhile:
            std::cout << ind(d) << "DoWhile\n";
            if (s.pat) { dumpPat(*s.pat, d + 1);
                         childExpr("Search", s.cond, d + 1); }
            else if (s.cond) childExpr("Cond", s.cond, d + 1);
            std::cout << ind(d + 1) << "Body\n";
            for (const auto& st : s.body) dump(*st, d + 2);
            return;
        case StKind::Gather:
            std::cout << ind(d) << "Gather\n";
            std::cout << ind(d + 1) << "Seeds\n";
            for (const auto& se : s.seedExprs) dumpExpr(*se, d + 2);
            std::cout << ind(d + 1) << "Body\n";
            for (const auto& st : s.body) dump(*st, d + 2);
            return;
        case StKind::GatherStmt:
            std::cout << ind(d) << "GatherItem\n";
            if (!s.exprs.empty()) dumpExpr(*s.exprs[0], d + 1);
            return;
        case StKind::Goto:
            std::cout << ind(d) << "Goto '" << s.label << "'\n";
            return;
        case StKind::For:
            std::cout << ind(d) << "For\n";
            if (s.pat) dumpPat(*s.pat, d + 1);
            if (!s.exprs.empty()) childExpr("Iter", s.exprs[0], d + 1);
            std::cout << ind(d + 1) << "Body\n";
            for (const auto& st : s.body) dump(*st, d + 2);
            return;
        case StKind::Match:
            std::cout << ind(d) << "Match\n";
            if (!s.exprs.empty()) childExpr("Subject", s.exprs[0], d + 1);
            for (const auto& arm : s.arms) {
                std::cout << ind(d + 1) << "Arm\n";
                if (arm.pat) dumpPat(*arm.pat, d + 2);
                if (arm.guard) childExpr("Guard", arm.guard, d + 2);
                std::cout << ind(d + 2) << "Body\n";
                for (const auto& st : arm.body) dump(*st, d + 3);
            }
            return;
        case StKind::Select:
            std::cout << ind(d) << "Select\n";
            for (const auto& arm : s.selArms) {
                std::cout << ind(d + 1) << "SelArm"
                          << (arm.bind.empty() ? "" : " bind=" + arm.bind) << '\n';
                if (arm.chanOp) childExpr("ChanOp", arm.chanOp, d + 2);
                std::cout << ind(d + 2) << "Body\n";
                for (const auto& st : arm.body) dump(*st, d + 3);
            }
            return;
        case StKind::Unsafe:
            std::cout << ind(d) << "Unsafe\n";
            for (const auto& st : s.body) dump(*st, d + 1);
            return;
        case StKind::Try:
            std::cout << ind(d) << "Try\n";
            for (const auto& st : s.body) dump(*st, d + 1);
            std::cout << ind(d + 1) << "Catch"
                      << (s.catchParam.empty() ? "" : " bind=" + s.catchParam)
                      << '\n';
            for (const auto& st : s.elseBody) dump(*st, d + 2);
            return;
        case StKind::Import: {
            std::cout << ind(d) << (s.fromImport ? "FromImport " : "Import ")
                      << s.moduleName;
            if (s.starImport) std::cout << " *";
            if (!s.importAlias.empty()) std::cout << " as " << s.importAlias;
            if (!s.importItems.empty()) {
                std::cout << " items=[";
                for (size_t i = 0; i < s.importItems.size(); ++i) {
                    if (i) std::cout << ", ";
                    std::cout << s.importItems[i].name;
                    if (!s.importItems[i].alias.empty())
                        std::cout << " as " << s.importItems[i].alias;
                }
                std::cout << ']';
            }
            std::cout << '\n';
            return;
        }
        case StKind::Export:
            std::cout << ind(d) << "Export " << s.moduleName << '\n';
            return;

        case StKind::Del:
            std::cout << ind(d) << "Del\n";
            if (!s.exprs.empty()) { dumpExpr(*s.exprs[0], d + 1); }
            return;

        case StKind::LocalDecl:
            std::cout << ind(d) << "LocalDecl '" << (s.target ? s.target->text : "")
                      << "'\n";
            if (s.declType) std::cout << ind(d + 1) << "Type\n";
            if (s.value) dumpExpr(*s.value, d + 1);
            return;

        case StKind::GlobalDecl:
            std::cout << ind(d) << "GlobalDecl '" << (s.target ? s.target->text : "")
                      << "'\n";
            if (s.value) dumpExpr(*s.value, d + 1);
            return;

        case StKind::TempDecl:
            std::cout << ind(d) << "TempDecl '" << s.tempVarName << "' x"
                      << s.tempBudget << "\n";
            if (s.declType) std::cout << ind(d + 1) << "Type\n";
            if (s.value) dumpExpr(*s.value, d + 1);
            return;

        case StKind::BucketDecl:
            std::cout << ind(d) << "BucketDecl '" << s.tempVarName << "' "
                      << (s.bucketRelease ? "release" : "park") << "\n";
            if (s.value) dumpExpr(*s.value, d + 1);
            return;

        case StKind::Yield:
            std::cout << ind(d) << "Yield\n";
            if (!s.exprs.empty()) dumpExpr(*s.exprs[0], d + 1);
            return;
    }
}

} // namespace ast
} // namespace kirn
