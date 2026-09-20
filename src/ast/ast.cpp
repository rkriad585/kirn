#include "ast/ast.h"

namespace kirn {
namespace ast {

TypeP Type::makeName(std::string n, Span s, std::vector<TypeP> gens) {
    auto t = std::make_unique<Type>();
    t->kind = TyKind::Name;
    t->span = s;
    t->name = std::move(n);
    t->generics = std::move(gens);
    return t;
}

TypeP Type::makePointer(TypeP inner, Span s) {
    auto t = std::make_unique<Type>();
    t->kind = TyKind::Pointer;
    t->span = s;
    t->inner = std::move(inner);
    return t;
}

TypeP Type::makeRef(TypeP inner, bool mut, Span s) {
    auto t = std::make_unique<Type>();
    t->kind = TyKind::Ref;
    t->span = s;
    t->refMut = mut;
    t->inner = std::move(inner);
    return t;
}

TypeP Type::makeOptional(TypeP inner, Span s) {
    auto t = std::make_unique<Type>();
    t->kind = TyKind::Optional;
    t->span = s;
    t->inner = std::move(inner);
    return t;
}

TypeP Type::makeFn(std::vector<TypeP> ps, TypeP r, Span s) {
    auto t = std::make_unique<Type>();
    t->kind = TyKind::Fn;
    t->span = s;
    t->params = std::move(ps);
    t->ret = std::move(r);
    return t;
}

TypeP Type::makeTuple(std::vector<TypeP> ts, Span s) {
    auto t = std::make_unique<Type>();
    t->kind = TyKind::Tuple;
    t->span = s;
    t->params = std::move(ts);
    return t;
}

} // namespace ast
} // namespace kirn
