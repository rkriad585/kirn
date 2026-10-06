// Phase 4 LLVM backend — MIR to llvm::Module for the scalar subset.
// See emit_llvm.h and BUILDER-PLAN §4.7.
//
// The module is lowered from the alloca/load/store CFG the MIR builder emits
// (mir.h §Values and memory), so it runs PromotePass (mem2reg) to recover SSA
// before serialization. Everything outside the scalar subset is rejected with a
// diagnostic; nothing is silently miscompiled.
#include "backends/llvm/emit_llvm.h"

#include "mir/mir.h"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace kirn::backends::llvm {

namespace {

using namespace ::llvm;

constexpr uint32_t kPrintBufSize = 1024;

// What a `call_indirect ; <name>` resolves to when used as a Call's callee.
struct Callee {
    enum class Kind { Func, Print } kind = Kind::Func;
    uint32_t funcIndex = 0;   // meaningful only for Kind::Func
};

// A Const whose opName is `str:...` is deferred: string data is only ever
// legal as a print argument, and lowerPrint reads the text directly from the
// defining Inst. Anywhere else it is a non-scalar rejection.
bool isStrConst(const mir::Inst& in) {
    return in.op == mir::Op::Const && in.opName.rfind("str:", 0) == 0;
}

static const char* opLabel(mir::Op o) {
    switch (o) {
        case mir::Op::Ret:            return "return";
        case mir::Op::Br:             return "goto";
        case mir::Op::CondBr:         return "branch";
        case mir::Op::Unreachable:    return "unreachable";
        case mir::Op::Alloca:         return "alloca";
        case mir::Op::Load:           return "load";
        case mir::Op::Store:          return "store";
        case mir::Op::Const:          return "constant";
        case mir::Op::Unary:          return "unary";
        case mir::Op::Binary:         return "binary";
        case mir::Op::Cast:           return "cast";
        case mir::Op::Index:          return "subscript";
        case mir::Op::Field:          return "field";
        case mir::Op::MakeList:       return "list";
        case mir::Op::MakeSet:        return "set";
        case mir::Op::MakeTuple:      return "tuple";
        case mir::Op::MakeDict:       return "dict";
        case mir::Op::MakeStruct:     return "struct";
        case mir::Op::MakeEnum:       return "enum";
        case mir::Op::Call:           return "call";
        case mir::Op::CallIndirect:   return "dynamic call";
        case mir::Op::Phi:            return "phi";
        case mir::Op::EnumTag:        return "enum tag";
        case mir::Op::Aggregate:      return "aggregate";
        case mir::Op::Discriminant:   return "discriminant";
    }
    return "instruction";
}

class Emitter {
public:
    Emitter(const mir::Program& p, const EmitOptions& opts)
        : prog_(p), opts_(opts), ctx_(), mod_(std::make_unique<Module>("kirn", ctx_)) {}

    bool run(std::vector<EmitDiag>& diags, std::string& outLl,
             std::string& outBc);

private:
    const mir::Program& prog_;
    const EmitOptions& opts_;
    LLVMContext ctx_;
    std::unique_ptr<Module> mod_;

    // Per-function maps, cleared at the top of emitFunctionBody.
    std::map<mir::Value, Value*> vals_;
    std::map<mir::Value, Callee> callees_;
    std::map<mir::Value, const mir::Inst*> defs_;
    std::map<uint32_t, AllocaInst*> allocas_;
    std::map<mir::BlockId, BasicBlock*> blocks_;
    std::vector<Function*> userFuncs_;          // indexed by prog_.funcs order
    std::map<std::string, uint32_t> funcByName_;

    std::string curFuncName_;
    bool failed_ = false;
    std::vector<EmitDiag>* diags_ = nullptr;

    // Module-wide services created once.
    Function* snprintf_ = nullptr;
    Function* strtod_ = nullptr;
    Function* puts_ = nullptr;
    Function* fmtF64_ = nullptr;

    // Format/string globals created lazily.
    GlobalVariable* fmtLld_ = nullptr;
    GlobalVariable* fmtPctS_ = nullptr;
    GlobalVariable* strTrue_ = nullptr;
    GlobalVariable* strFalse_ = nullptr;

    // ---- helpers ---------------------------------------------------------------
    void fail(const ast::Span& sp, const std::string& msg) {
        EmitDiag d;
        d.line = sp.line;
        d.col = sp.col;
        d.message = msg;
        diags_->push_back(std::move(d));
        failed_ = true;
    }
    void failModule(const std::string& msg) { fail(ast::Span{}, msg); }

    Type* mapType(const mir::Func& f, mir::Type t, bool forReturn = false);
    Value* scalarConst(const mir::Inst& in);
    const mir::Inst* defOf(mir::Value v) const;
    Value* resolve(const mir::Func& f, mir::Value v);
    AllocaInst* allocaFor(uint32_t local);
    GlobalVariable* gstring(const std::string& s, bool nul,
                            const std::string& name);
    Value* formatPtr(GlobalVariable* g, IRBuilder<>& B, const char* n);

    // ---- the passes ------------------------------------------------------------
    void declareFunctions();
    void emitFunctionBody(const mir::Func& f, Function* fn);
    void emitMainWrapper();
    void runMem2Reg();
    void verifyAndSerialize(std::string& outLl, std::string& outBc);

    // ---- instruction lowering ---------------------------------------------------
    void lowerInst(IRBuilder<>& B, const mir::Func& f, const mir::Inst& in);
    void lowerArith(IRBuilder<>& B, const mir::Func& f, const mir::Inst& in);
    void lowerIndirect(IRBuilder<>& B, const mir::Func& f, const mir::Inst& in);
    void lowerCall(IRBuilder<>& B, const mir::Func& f, const mir::Inst& in);
    void lowerPrint(IRBuilder<>& B, const mir::Func& f, const mir::Inst& in);
    void ptrAdvance(IRBuilder<>& B, Value*& cur, Value*& rem, Value* written);

    // i64 @kirn.fmt_f64(double, ptr buf, i64 cap) = the toStr contract for
    // floats (runtime.cpp): NaN -> "NaN", +inf -> "inf", -inf -> "-inf", else
    // the first of %.15g / %.16g / %.17g that round-trips through strtod, with
    // ".0" appended when the text has neither a '.' nor an 'e'.
    void makeFmtF64();
};

Type* Emitter::mapType(const mir::Func& f, mir::Type t, bool forReturn) {
    if (forReturn && t == mir::NO_VALUE) return Type::getVoidTy(ctx_);
    if (t == mir::NO_VALUE || t >= f.types.size()) return nullptr;
    const mir::TypeEntry& te = f.types[t];
    switch (te.kind) {
        case mir::Ty::Bool:  return Type::getInt1Ty(ctx_);
        case mir::Ty::Int:   return Type::getInt64Ty(ctx_);
        case mir::Ty::Float: return Type::getDoubleTy(ctx_);
        // A `-> none` function is the language's void; the MIR interner stores
        // it as the Void kind (`ret=none` in mir.cpp normalizes to this id).
        case mir::Ty::Void:  return forReturn ? Type::getVoidTy(ctx_) : nullptr;
        default:             return nullptr;
    }
}

Value* Emitter::scalarConst(const mir::Inst& in) {
    const std::string& s = in.opName;
    if (s.rfind("int:", 0) == 0)
        return ConstantInt::get(ctx_, APInt(64, (uint64_t)std::strtoll(
                                                s.c_str() + 4, nullptr, 10)));
    if (s.rfind("char:", 0) == 0)
        return ConstantInt::get(ctx_, APInt(64, (uint64_t)std::strtoll(
                                                s.c_str() + 5, nullptr, 10)));
    if (s.rfind("float:", 0) == 0)
        return ConstantFP::get(Type::getDoubleTy(ctx_),
                               std::strtod(s.c_str() + 6, nullptr));
    if (s.rfind("bool:", 0) == 0)
        return s.compare(5, 4, "true") == 0 ? ConstantInt::getTrue(ctx_)
                                            : ConstantInt::getFalse(ctx_);
    return nullptr;
}

const mir::Inst* Emitter::defOf(mir::Value v) const {
    auto it = defs_.find(v);
    return it == defs_.end() ? nullptr : it->second;
}

Value* Emitter::resolve(const mir::Func& f, mir::Value v) {
    if (v == mir::NO_VALUE) return nullptr;
    auto it = vals_.find(v);
    return it == vals_.end() ? nullptr : it->second;
}

AllocaInst* Emitter::allocaFor(uint32_t local) {
    auto it = allocas_.find(local);
    return it == allocas_.end() ? nullptr : it->second;
}

GlobalVariable* Emitter::gstring(const std::string& s, bool nul,
                                 const std::string& name) {
    Constant* c = ConstantDataArray::getString(ctx_, s, nul);
    return new GlobalVariable(*mod_, c->getType(), true,
                              GlobalValue::PrivateLinkage, c, name);
}

Value* Emitter::formatPtr(GlobalVariable* g, IRBuilder<>& B, const char* n) {
    Type* i64 = Type::getInt64Ty(ctx_);
    return B.CreateInBoundsGEP(g->getValueType(), g,
                               {ConstantInt::get(i64, 0),
                                ConstantInt::get(i64, 0)},
                               n);
}

// ---------------------------------------------------------------------------
// Pass 1: declare every function (the real kirn `main` becomes `kirn.main`;
// the IR `main` is the wrapper the host needs).
// ---------------------------------------------------------------------------
void Emitter::declareFunctions() {
    funcByName_.clear();
    userFuncs_.clear();
    for (uint32_t i = 0; i < prog_.funcs.size(); ++i) {
        const mir::Func& f = prog_.funcs[i];
        Type* ret = mapType(f, f.returnType, /*forReturn=*/true);
        if (!ret) {
            std::string tyName = "void";
            if (f.returnType < f.types.size()) {
                const mir::TypeEntry& te = f.types[f.returnType];
                tyName = !te.name.empty() ? te.name
                         : te.kind == mir::Ty::Any ? "any"
                         : te.kind == mir::Ty::Void ? "none"
                                                   : "?";
            }
            fail(f.span, "function '" + f.name + "' returns a non-scalar type '" +
                             tyName +
                             "'; only int, float, bool and void are in the "
                             "scalar subset");
            return;
        }
        if (f.isVariadic) {
            fail(f.span, "function '" + f.name +
                             "' is variadic, which the scalar subset does not "
                             "support");
            return;
        }
        std::vector<Type*> pars;
        for (mir::Value pv : f.paramValues) {
            Type* pt = mapType(f, f.valueTypes[pv]);
            if (!pt) {
                fail(f.span, "function '" + f.name +
                                 "' has a non-scalar parameter; only int, "
                                 "float and bool parameters are in the scalar "
                                 "subset");
                return;
            }
            pars.push_back(pt);
        }
        Function* fn = Function::Create(
            FunctionType::get(ret, pars, false), Function::ExternalLinkage,
            f.name == "main" ? "kirn.main" : f.name, mod_.get());
        userFuncs_.push_back(fn);
        funcByName_[f.name] = i;
    }
}

// ---------------------------------------------------------------------------
// Pass 2: function bodies, alloca/load/store -> LLVM IR.
// ---------------------------------------------------------------------------
void Emitter::emitFunctionBody(const mir::Func& f, Function* fn) {
    curFuncName_ = f.name;
    vals_.clear();
    callees_.clear();
    defs_.clear();
    allocas_.clear();

    for (const auto& b : f.blocks)
        for (const auto& in : b.insts)
            if (in.result != mir::NO_VALUE) defs_[in.result] = &in;

    blocks_.clear();
    for (const auto& b : f.blocks)
        blocks_[b.id] = BasicBlock::Create(ctx_, "b" + std::to_string(b.id),
                                           fn);

    // Bind the parameter Values to the LLVM argument slots before the entry
    // stores expect them: mir.cpp materializes every param as an entry-block
    // store into its alloca (round 6a's sig-type fallback keeps it a store so
    // mem2reg can promote it), and those stores resolve the param Value.
    {
        auto aIt = fn->arg_begin();
        for (unsigned i = 0; i < f.paramValues.size() && aIt != fn->arg_end();
             ++i, ++aIt)
            vals_[f.paramValues[i]] = aIt;
    }

    IRBuilder<> B(ctx_);

    // Allocas first, at the very top of the entry block, as LLVM requires.
    if (!f.blocks.empty()) {
        const auto& entry = f.blocks[0];
        for (const auto& in : entry.insts) {
            if (in.op != mir::Op::Alloca) continue;
            Type* ty = mapType(f, in.type);
            if (!ty) {
                fail(in.span, "local '" + in.opName +
                                  "' is not a scalar type; only int, float "
                                  "and bool locals are in the scalar subset");
                return;
            }
            B.SetInsertPoint(blocks_[entry.id]);
            uint32_t li = (uint32_t)std::strtoul(in.opName.c_str(), nullptr, 10);
            AllocaInst* a = B.CreateAlloca(ty, nullptr, "local" + in.opName);
            allocas_[li] = a;
        }
    }

    for (const auto& b : f.blocks) {
        BasicBlock* bb = blocks_[b.id];
        for (const auto& in : b.insts) {
            if (in.op == mir::Op::Alloca) continue;
            B.SetInsertPoint(bb);
            lowerInst(B, f, in);
            if (failed_) return;
        }
    }
}

void Emitter::lowerInst(IRBuilder<>& B, const mir::Func& f,
                        const mir::Inst& in) {
    switch (in.op) {
        case mir::Op::Ret: {
            if (in.args.empty()) {
                B.CreateRetVoid();
            } else {
                Value* v = resolve(f, in.args[in.args.size() - 1]);
                if (!v) { fail(in.span, "return of an unavailable value"); break; }
                B.CreateRet(v);
            }
            break;
        }
        case mir::Op::Br:
            B.CreateBr(blocks_[in.succ]);
            break;
        case mir::Op::CondBr: {
            if (in.args.empty()) {
                fail(in.span, "branch with no condition: this is the phase-3 "
                              "for-loop stub, which the scalar subset does not "
                              "support yet");
                break;
            }
            Value* c = resolve(f, in.args[0]);
            if (!c) { fail(in.span, "branch condition is unavailable"); break; }
            B.CreateCondBr(c, blocks_[in.succ], blocks_[in.elseSucc]);
            break;
        }
        case mir::Op::Unreachable:
            B.CreateUnreachable();
            break;
        case mir::Op::Alloca:
            break;   // handled in the entry prepass
        case mir::Op::Load: {
            AllocaInst* a = allocaFor(
                (uint32_t)std::strtoul(in.opName.c_str(), nullptr, 10));
            if (!a) { fail(in.span, "load of an unknown local '" + in.opName + "'"); break; }
            vals_[in.result] = B.CreateLoad(a->getAllocatedType(), a, "v");
            break;
        }
        case mir::Op::Store: {
            AllocaInst* a = allocaFor(
                (uint32_t)std::strtoul(in.opName.c_str(), nullptr, 10));
            if (!a) { fail(in.span, "store to an unknown local '" + in.opName + "'"); break; }
            Value* v = resolve(f, in.args[0]);
            if (!v) { fail(in.span, "store of an unavailable value"); break; }
            B.CreateStore(v, a);
            break;
        }
        case mir::Op::Const:
            if (isStrConst(in)) {
                // Deferred: string constants are only meaningful as print
                // arguments, which read the text off the defining Inst.
                break;
            }
            if (Value* v = scalarConst(in)) {
                vals_[in.result] = v;
            } else {
                std::string what = in.opName.empty() ? "constant" : in.opName;
                fail(in.span, "constant '" + what +
                                  "' is not a scalar int/float/bool value");
            }
            break;
        case mir::Op::Unary:
        case mir::Op::Binary:
            lowerArith(B, f, in);
            break;
        case mir::Op::Cast: {
            Value* src = resolve(f, in.args[0]);
            if (!src) { fail(in.span, "cast of an unavailable value"); break; }
            Type* dst = mapType(f, in.type);
            Type* st = src->getType();
            Type* i1 = Type::getInt1Ty(ctx_);
            Type* i64 = Type::getInt64Ty(ctx_);
            Type* f64 = Type::getDoubleTy(ctx_);
            if (!dst) { fail(in.span, "cast to a non-scalar type '" + in.opName + "'"); break; }
            if (st == dst) {
                vals_[in.result] = src;
            } else if (dst == i1 && st == i64) {
                vals_[in.result] = B.CreateICmpNE(src, ConstantInt::get(i64, 0), "c");
            } else if (dst == i1 && st == f64) {
                vals_[in.result] =
                    B.CreateFCmpUNE(src, ConstantFP::get(f64, 0.0), "c");
            } else if (dst == i64 && st == i1) {
                vals_[in.result] = B.CreateZExt(src, dst, "c");
            } else if (dst == i64 && st == f64) {
                vals_[in.result] = B.CreateFPToSI(src, dst, "c");
            } else if (dst == f64 && st == i1) {
                vals_[in.result] = B.CreateUIToFP(src, dst, "c");
            } else if (dst == f64 && st == i64) {
                vals_[in.result] = B.CreateSIToFP(src, dst, "c");
            } else {
                fail(in.span, "unsupported cast (op '" + in.opName + "')");
            }
            break;
        }
        case mir::Op::Phi: {
            Type* ty = mapType(f, in.type);
            if (!ty) {
                fail(in.span, "phi of a non-scalar type");
                break;
            }
            PHINode* ph = B.CreatePHI(ty, in.incoming.size(), "phi");
            for (const auto& inc : in.incoming) {
                Value* pi = resolve(f, inc.val);
                if (!pi) {
                    fail(in.span, "phi operand is unavailable");
                    break;
                }
                ph->addIncoming(pi, blocks_[inc.pred]);
            }
            if (failed_) break;
            vals_[in.result] = ph;
            break;
        }
        case mir::Op::Call:
            lowerCall(B, f, in);
            break;
        case mir::Op::CallIndirect:
            lowerIndirect(B, f, in);
            break;
        default:
            fail(in.span, std::string(opLabel(in.op)) +
                              " is outside the scalar subset (this passes "
                              "reject, not miscompile)");
            break;
    }
}

void Emitter::lowerArith(IRBuilder<>& B, const mir::Func& f,
                         const mir::Inst& in) {
    Value* a = resolve(f, in.args[0]);
    if (!a) { fail(in.span, "operator operand is unavailable"); return; }
    Type* i64 = Type::getInt64Ty(ctx_);
    Type* f64 = Type::getDoubleTy(ctx_);

    if (in.op == mir::Op::Unary) {
        const std::string& o = in.opName;
        if (o == "+") { vals_[in.result] = a; return; }
        if (o == "-") {
            if (a->getType() == f64) { vals_[in.result] = B.CreateFNeg(a, "neg"); return; }
            if (a->getType() == i64) { vals_[in.result] = B.CreateNSWSub(ConstantInt::get(i64, 0), a, "neg"); return; }
            fail(in.span, "unary '-' on a non-numeric value"); return;
        }
        if (o == "not" && a->getType() == Type::getInt1Ty(ctx_)) {
            vals_[in.result] = B.CreateNot(a, "not"); return;
        }
        if (o == "~" && a->getType() == i64) {
            vals_[in.result] = B.CreateXor(a, ConstantInt::get(i64, -1), "not"); return;
        }
        fail(in.span, "unary operator '" + o + "' is outside the scalar subset");
        return;
    }

    Value* b = resolve(f, in.args[1]);
    if (!b) { fail(in.span, "operator operand is unavailable"); return; }
    const std::string& o = in.opName;
    Type* t = a->getType();
    const bool isFloat = t == f64;

    if (o == "==" || o == "!=" || o == "<" || o == "<=" || o == ">" || o == ">=") {
        CmpInst::Predicate p = isFloat ? (o == "==" ? CmpInst::FCMP_OEQ
                                      : o == "!=" ? CmpInst::FCMP_UNE
                                      : o == "<"  ? CmpInst::FCMP_OLT
                                      : o == "<=" ? CmpInst::FCMP_OLE
                                      : o == ">"  ? CmpInst::FCMP_OGT
                                                  : CmpInst::FCMP_OGE)
                                      : (o == "==" ? CmpInst::ICMP_EQ
                                      : o == "!=" ? CmpInst::ICMP_NE
                                      : o == "<"  ? CmpInst::ICMP_SLT
                                      : o == "<=" ? CmpInst::ICMP_SLE
                                      : o == ">"  ? CmpInst::ICMP_SGT
                                                  : CmpInst::ICMP_SGE);
        vals_[in.result] = isFloat ? (Value*)B.CreateFCmp(p, a, b, "c")
                                   : (Value*)B.CreateICmp(p, a, b, "c");
        return;
    }
    if (o == "+" || o == "-" || o == "*" || o == "/" || o == "%") {
        if (isFloat) {
            if (o == "%") {
                fail(in.span, "'%' on a float is outside the scalar subset");
                return;
            }
            vals_[in.result] = o == "+" ? (Value*)B.CreateFAdd(a, b, "a")
                             : o == "-" ? (Value*)B.CreateFSub(a, b, "a")
                             : o == "*" ? (Value*)B.CreateFMul(a, b, "a")
                                        : (Value*)B.CreateFDiv(a, b, "a");
            return;
        }
        if (t == i64) {
            vals_[in.result] = o == "+" ? (Value*)B.CreateAdd(a, b, "a")
                             : o == "-" ? (Value*)B.CreateSub(a, b, "a")
                             : o == "*" ? (Value*)B.CreateMul(a, b, "a")
                             : o == "/" ? (Value*)B.CreateSDiv(a, b, "a")
                                        : (Value*)B.CreateSRem(a, b, "a");
            return;
        }
        fail(in.span, "arithmetic on a non-numeric value");
        return;
    }
    if (o == "&" && t == i64) { vals_[in.result] = B.CreateAnd(a, b, "a"); return; }
    if (o == "|" && t == i64) { vals_[in.result] = B.CreateOr(a, b, "a"); return; }
    if (o == "^" && t == i64) { vals_[in.result] = B.CreateXor(a, b, "a"); return; }
    if (o == "<<" && t == i64) { vals_[in.result] = B.CreateShl(a, b, "a"); return; }
    if (o == ">>" && t == i64) { vals_[in.result] = B.CreateAShr(a, b, "a"); return; }
    fail(in.span, "binary operator '" + o + "' is outside the scalar subset");
}

// call_indirect ; <name>. With no arg operands it is a *callee resolution*
// (print, a user function, or the true/false constants MIR represents by
// name), recorded so the follow-up Call can find the Function. `and`/`or`
// carry boolean operands and are lowered to i1 | / & right here.
void Emitter::lowerIndirect(IRBuilder<>& B, const mir::Func& f,
                            const mir::Inst& in) {
    const std::string& name = in.opName;
    if (name == "true" && in.args.empty()) {
        vals_[in.result] = ConstantInt::getTrue(ctx_);
        return;
    }
    if (name == "false" && in.args.empty()) {
        vals_[in.result] = ConstantInt::getFalse(ctx_);
        return;
    }
    if (name == "and" || name == "or") {
        if (in.args.size() != 2) {
            fail(in.span, "'" + name + "' needs two operands");
            return;
        }
        Value* x = resolve(f, in.args[0]);
        Value* y = resolve(f, in.args[1]);
        if (!x || !y) { fail(in.span, "'" + name + "' operand is unavailable"); return; }
        vals_[in.result] = name == "and" ? (Value*)B.CreateAnd(x, y, "and")
                                         : (Value*)B.CreateOr(x, y, "or");
        return;
    }
    if (in.args.empty()) {
        if (name == "print") {
            callees_[in.result] = Callee{Callee::Kind::Print, 0};
            return;
        }
        auto it = funcByName_.find(name);
        if (it != funcByName_.end()) {
            callees_[in.result] = Callee{Callee::Kind::Func, it->second};
            return;
        }
        fail(in.span, "dynamic value '" + name +
                          "' has no scalar lowering (builtins beyond print "
                          "are rejected, not miscompiled)");
        return;
    }
    fail(in.span, "dynamic call '" + name +
                      "' is outside the scalar subset");
}

void Emitter::lowerCall(IRBuilder<>& B, const mir::Func& f,
                        const mir::Inst& in) {
    if (in.args.empty()) { fail(in.span, "call with no operands"); return; }
    auto it = callees_.find(in.args[0]);
    if (it == callees_.end()) {
        fail(in.span, "call whose callee was never resolved");
        return;
    }
    if (it->second.kind == Callee::Kind::Print) {
        lowerPrint(B, f, in);
        return;
    }
    uint32_t idx = it->second.funcIndex;
    if (idx >= userFuncs_.size()) {
        fail(in.span, "call to an unknown function");
        return;
    }
    Function* cal = userFuncs_[idx];
    std::vector<Value*> args;
    for (size_t k = 1; k < in.args.size(); ++k) {
        Value* v = resolve(f, in.args[k]);
        if (!v) { fail(in.span, "call argument is unavailable"); return; }
        args.push_back(v);
    }
    if (cal->getReturnType()->isVoidTy()) {
        B.CreateCall(cal, args);
        return;
    }
    Value* r = B.CreateCall(cal, args, "call");
    if (in.result != mir::NO_VALUE) vals_[in.result] = r;
}

// print(...) -> Interpreter::toStr per arg, args joined by ' ', then '\n'
// (which `puts` supplies). Implemented with a per-call buffer and snprintf
// appends so the bytes match the interpreter's.
void Emitter::lowerPrint(IRBuilder<>& B, const mir::Func& f,
                         const mir::Inst& in) {
    if (!fmtLld_) {
        fmtLld_ = gstring("%lld", true, ".f.lld");
        fmtPctS_ = gstring("%s", true, ".f.s");
        strTrue_ = gstring("true", true, ".s.true");
        strFalse_ = gstring("false", true, ".s.false");
    }
    Type* i64 = Type::getInt64Ty(ctx_);
    Type* f64 = Type::getDoubleTy(ctx_);

    if (in.args.size() <= 1) {   // print() with no arguments -> just "\n"
        GlobalVariable* empty = gstring("", true, ".s.empty");
        B.CreateCall(puts_, {formatPtr(empty, B, "pe")});
        return;
    }

    AllocaInst* buf = B.CreateAlloca(Type::getInt8Ty(ctx_),
                                     ConstantInt::get(i64, kPrintBufSize),
                                     "p.buf");
    Value* cur = B.CreateGEP(Type::getInt8Ty(ctx_), buf, ConstantInt::get(i64, 0),
                             "p.cur");
    Value* rem = ConstantInt::get(i64, kPrintBufSize);

    for (size_t k = 1; k < in.args.size(); ++k) {
        if (k != 1) {
            Value* w = B.CreateCall(
                snprintf_,
                {cur, rem, formatPtr(gstring(" ", true, ".f.sp"), B, "sp")},
                "w");
            ptrAdvance(B, cur, rem, w);
        }
        const mir::Inst* ad = defOf(in.args[k]);
        if (ad && isStrConst(*ad)) {
            GlobalVariable* g = gstring(ad->opName.substr(4), true, ".s.lit");
            Value* w = B.CreateCall(
                snprintf_, {cur, rem, formatPtr(fmtPctS_, B, "s"), formatPtr(g, B, "sl")},
                "w");
            ptrAdvance(B, cur, rem, w);
            continue;
        }
        Value* v = resolve(f, in.args[k]);
        if (!v) {
            fail(in.span, "print of a value that is not int/float/bool/str");
            return;
        }
        if (v->getType() == i64) {
            Value* w = B.CreateCall(
                snprintf_, {cur, rem, formatPtr(fmtLld_, B, "lld"), v}, "w");
            ptrAdvance(B, cur, rem, w);
        } else if (v->getType() == f64) {
            Value* w = B.CreateCall(fmtF64_, {v, cur, rem}, "flt");
            ptrAdvance(B, cur, rem, w);
        } else {   // i1
            Value* sel = B.CreateSelect(v, strTrue_, strFalse_, "bs");
            Value* w = B.CreateCall(
                snprintf_, {cur, rem, formatPtr(fmtPctS_, B, "s"), sel}, "w");
            ptrAdvance(B, cur, rem, w);
        }
    }
    B.CreateCall(puts_, {buf});
}

void Emitter::ptrAdvance(IRBuilder<>& B, Value*& cur, Value*& rem,
                         Value* written) {
    Type* i64 = Type::getInt64Ty(ctx_);
    Value* w = B.CreateZExt(written, i64, "n");
    Value* over = B.CreateICmpUGT(w, rem, "over");
    Value* wc = B.CreateSelect(over, rem, w, "w");
    cur = B.CreateGEP(Type::getInt8Ty(ctx_), cur, wc, "cur");
    rem = B.CreateSub(rem, wc, "rem");
}

// @kirn.fmt_f64: the toStr contract, byte for byte.
void Emitter::makeFmtF64() {
    if (fmtF64_) return;
    Type* i8 = Type::getInt8Ty(ctx_);
    Type* i64 = Type::getInt64Ty(ctx_);
    Type* f64 = Type::getDoubleTy(ctx_);
    PointerType* ptr = PointerType::get(ctx_, 0);

    snprintf_ = Function::Create(FunctionType::get(Type::getInt32Ty(ctx_),
                                                   {ptr, i64, ptr}, true),
                                 Function::ExternalLinkage, "snprintf", *mod_);
    strtod_ = Function::Create(FunctionType::get(f64, {ptr, ptr}, false),
                               Function::ExternalLinkage, "strtod", *mod_);
    puts_ = Function::Create(FunctionType::get(Type::getInt32Ty(ctx_), {ptr}, false),
                             Function::ExternalLinkage, "puts", *mod_);

    GlobalVariable* F15 = gstring("%.15g", true, ".f.15");
    GlobalVariable* F16 = gstring("%.16g", true, ".f.16");
    GlobalVariable* F17 = gstring("%.17g", true, ".f.17");
    GlobalVariable* DOT0 = gstring("%s", true, ".f.dot");
    GlobalVariable* PARSED_DOT = gstring(".0", true, ".s.dot0");

    FunctionType* ft = FunctionType::get(i64, {f64, ptr, i64}, false);
    fmtF64_ = Function::Create(ft, Function::InternalLinkage, "kirn.fmt_f64",
                               *mod_);
    auto argIt = fmtF64_->arg_begin();
    Value* v = argIt++;
    Value* buf = argIt++;
    Value* cap = argIt++;
    v->setName("v");
    buf->setName("buf");
    cap->setName("cap");

    auto* entry = BasicBlock::Create(ctx_, "entry", fmtF64_);
    auto* nan = BasicBlock::Create(ctx_, "nan", fmtF64_);
    auto* pos = BasicBlock::Create(ctx_, "pos", fmtF64_);
    auto* neg = BasicBlock::Create(ctx_, "neg", fmtF64_);
    auto* infCk = BasicBlock::Create(ctx_, "infck", fmtF64_);
    auto* negCk = BasicBlock::Create(ctx_, "negck", fmtF64_);
    auto* try15 = BasicBlock::Create(ctx_, "try15", fmtF64_);
    auto* try16 = BasicBlock::Create(ctx_, "try16", fmtF64_);
    auto* try17 = BasicBlock::Create(ctx_, "try17", fmtF64_);
    auto* scan = BasicBlock::Create(ctx_, "scan", fmtF64_);
    auto* scanBody = BasicBlock::Create(ctx_, "scanbody", fmtF64_);
    auto* hasDot = BasicBlock::Create(ctx_, "hasdot", fmtF64_);
    auto* scanEnd = BasicBlock::Create(ctx_, "scanend", fmtF64_);

    // Fixed text: copy into buf, return its length.
    IRBuilder<> B(ctx_);
    auto storeText = [&](BasicBlock* bb, const char* text, uint32_t n) {
        B.SetInsertPoint(bb);
        GlobalVariable* g = gstring(text, false, ".s.txt");
        B.CreateMemCpy(buf, Align(1), g, Align(1), ConstantInt::get(i64, n));
        // The text is not NUL-terminated by the memcpy itself; the print
        // caller relies on puts() which needs the terminator, and the scan
        // loop reads until the fitted length -- leave the char after the
        // text zero so an over-running scan/puts stops cleanly too.
        B.CreateStore(ConstantInt::get(i8, 0),
                      B.CreateGEP(i8, buf, ConstantInt::get(i64, n), "nul"));
        B.CreateRet(ConstantInt::get(i64, n));
    };

    // Each output block holds exactly one storeText (which terminates it);
    // the sign comparisons live in the infCk/negCk blocks so nothing is ever
    // inserted after a terminator.
    storeText(nan, "NaN", 3);
    storeText(pos, "inf", 3);
    storeText(neg, "-inf", 4);

    B.SetInsertPoint(entry);
    Value* isnan = B.CreateFCmpUNO(v, v, "isnan");
    B.CreateCondBr(isnan, nan, infCk);

    B.SetInsertPoint(infCk);
    Value* isPos = B.CreateFCmpOEQ(v, ConstantFP::getInfinity(f64), "ispos");
    B.CreateCondBr(isPos, pos, negCk);

    // +inf and -inf are distinguishable: fcmp oeq handles both signs.
    B.SetInsertPoint(negCk);
    Value* isNeg = B.CreateFCmpOEQ(v, ConstantFP::getInfinity(f64, true),
                                   "isneg");
    B.CreateCondBr(isNeg, neg, try15);

    B.SetInsertPoint(try15);
    Value* n15 = B.CreateCall(
        snprintf_, {buf, cap, formatPtr(F15, B, "f15"), v}, "n15");
    Value* p15 = B.CreateCall(strtod_, {buf, ConstantPointerNull::get(ptr)},
                              "p15");
    Value* ok15 = B.CreateFCmpOEQ(p15, v, "ok15");
    Value* len15 = B.CreateZExt(n15, i64, "len15");
    B.CreateCondBr(ok15, scan, try16);

    B.SetInsertPoint(try16);
    Value* n16 = B.CreateCall(
        snprintf_, {buf, cap, formatPtr(F16, B, "f16"), v}, "n16");
    Value* p16 = B.CreateCall(strtod_, {buf, ConstantPointerNull::get(ptr)},
                              "p16");
    Value* ok16 = B.CreateFCmpOEQ(p16, v, "ok16");
    Value* len16 = B.CreateZExt(n16, i64, "len16");
    B.CreateCondBr(ok16, scan, try17);

    // %.17g is the longest of the three and is already canonical, so no
    // round-trip check runs for it -- the contract's chain is "first of the
    // three that round-trips", which a canonical 17-digit rendering always is.
    B.SetInsertPoint(try17);
    Value* n17 = B.CreateCall(
        snprintf_, {buf, cap, formatPtr(F17, B, "f17"), v}, "n17");
    Value* len17 = B.CreateZExt(n17, i64, "len17");
    B.CreateBr(scan);

    // Scan the *fitted* text for '.' (46) or 'e' (101). Skipping the scan when
    // the text comes from try15/16 is the bug the interpreter's contract is
    // precise about: "6.0" prints as "6.0" even though %.15g yields the text
    // "6", so the round-trip check alone is NOT the whole formatting rule.
    B.SetInsertPoint(scan);
    PHINode* len = B.CreatePHI(i64, 3, "scan.len");
    len->addIncoming(len15, try15);
    len->addIncoming(len16, try16);
    len->addIncoming(len17, try17);
    PHINode* idx = B.CreatePHI(i64, 4, "scan.idx");
    idx->addIncoming(ConstantInt::get(i64, 0), try15);
    idx->addIncoming(ConstantInt::get(i64, 0), try16);
    idx->addIncoming(ConstantInt::get(i64, 0), try17);
    Value* inLen = B.CreateICmpULT(idx, len, "inLen");
    Value* inCap = B.CreateICmpULT(idx, cap, "inCap");
    Value* go = B.CreateAnd(inLen, inCap, "go");
    B.CreateCondBr(go, scanBody, scanEnd);

    B.SetInsertPoint(scanBody);
    Value* ch = B.CreateLoad(i8, B.CreateGEP(i8, buf, idx, "gp"), "ch");
    Value* isDot = B.CreateICmpEQ(ch, ConstantInt::get(i8, 46), "dot");
    Value* isE = B.CreateICmpEQ(ch, ConstantInt::get(i8, 101), "e");
    Value* hit = B.CreateOr(isDot, isE, "hit");
    Value* idx2 = B.CreateAdd(idx, ConstantInt::get(i64, 1), "idx2");
    // The loop phis above start on each fitting edge; the backedge feeds idx2
    // and carries len around -- add them now that idx2 exists.
    idx->addIncoming(idx2, scanBody);
    len->addIncoming(len, scanBody);
    B.CreateCondBr(hit, hasDot, scan);

    // hasDot: the fitted text already contains '.'/'e'; return it unchanged.
    B.SetInsertPoint(hasDot);
    B.CreateRet(len);

    // scanEnd: the fitted text has neither '.' nor 'e', so it needs the ".0"
    // suffix the runtime never prints without. Write "0" and keep the '.';
    // snprintf's "%s" on PARSED_DOT leaves the trailing NUL exactly as the
    // `len+2` return expects ('.' + '0' replaces nothing, the NUL follows).
    B.SetInsertPoint(scanEnd);
    Value* tail = B.CreateGEP(i8, buf, len, "tail");
    Value* room = B.CreateSub(cap, len, "room");
    Value* w = B.CreateCall(
        snprintf_, {tail, room, formatPtr(DOT0, B, "dot0"),
                    formatPtr(PARSED_DOT, B, "dot0s")},
        "w");
    (void)w;
    B.CreateRet(B.CreateAdd(len, ConstantInt::get(i64, 2), "txtlen"));
}

// ---------------------------------------------------------------------------
// The wrapper: kirn's `main` (i64 or void) becomes a real `i32 main(i32, ptr)`
// for the host. The interpreter's exit-code rule is ((int)main.value) for an
// Int-returning main, else 0 (driver.cpp runProgramSrc).
// ---------------------------------------------------------------------------
void Emitter::emitMainWrapper() {
    auto it = funcByName_.find("main");
    if (it == funcByName_.end()) return;
    Function* kmain = userFuncs_[it->second];
    Function* m = Function::Create(
        FunctionType::get(Type::getInt32Ty(ctx_),
                          {Type::getInt32Ty(ctx_), PointerType::get(ctx_, 0)}, false),
        Function::ExternalLinkage, "main", mod_.get());
    BasicBlock* entry = BasicBlock::Create(ctx_, "entry", m);
    IRBuilder<> B(entry);
    if (kmain->getReturnType()->isVoidTy()) {
        B.CreateCall(kmain, {});
        B.CreateRet(ConstantInt::get(Type::getInt32Ty(ctx_), 0));
        return;
    }
    Value* r = B.CreateCall(kmain, {}, "r");
    if (kmain->getReturnType()->isIntegerTy()) {
        B.CreateRet(B.CreateTrunc(r, Type::getInt32Ty(ctx_), "code"));
    } else {
        B.CreateRet(ConstantInt::get(Type::getInt32Ty(ctx_), 0));
    }
}

void Emitter::runMem2Reg() {
    if (mod_->empty()) return;
    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    FunctionPassManager FPM;
    FPM.addPass(PromotePass());
    ModulePassManager MPM;
    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
    MPM.run(*mod_, MAM);
}

void Emitter::verifyAndSerialize(std::string& outLl, std::string& outBc) {
    std::string err;
    raw_string_ostream errs(err);
    if (verifyModule(*mod_, &errs)) {
        failModule("generated IR failed LLVM verification: " + err);
        return;
    }
    raw_string_ostream ll(outLl);
    mod_->print(ll, nullptr);
    ll.flush();
    if (opts_.emitBitcode) {
        raw_string_ostream bc(outBc);
        WriteBitcodeToFile(*mod_, bc);
        bc.flush();
    }
}

bool Emitter::run(std::vector<EmitDiag>& diags, std::string& outLl,
                  std::string& outBc) {
    diags_ = &diags;
    failed_ = false;

    declareFunctions();
    if (failed_) return false;
    makeFmtF64();
    for (uint32_t i = 0; i < prog_.funcs.size(); ++i) {
        if (i >= userFuncs_.size()) break;
        emitFunctionBody(prog_.funcs[i], userFuncs_[i]);
        if (failed_) return false;
    }
    emitMainWrapper();
    if (failed_) return false;
    if (opts_.promoteAllocas) runMem2Reg();
    verifyAndSerialize(outLl, outBc);
    return !failed_;
}

}  // namespace

bool emitModule(const mir::Program& prog, const EmitOptions& opts,
                std::vector<EmitDiag>& diags, std::string& outLl,
                std::string* outBc) {
    Emitter emitter(prog, opts);
    std::string bc;
    bool ok = emitter.run(diags, outLl, bc);
    if (ok && opts.emitBitcode && outBc) *outBc = std::move(bc);
    return ok;
}

}  // namespace llvm