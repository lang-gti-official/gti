#include "gti/frontend.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

int main() {
  auto result = lang::Frontend().analyze("field-symbol-identity.gti", R"gti(
class Pair {
  mut int first = 0;
  static mut int middle = 7;
  mut int second = 1;
public:
  mut int& b() mut { return this.second; }
  mut int& a() mut { return this.first; }
};
class Derived : public Pair { int third = 2; };
struct Box<T> { T payload; Box(T value) : payload(value) {} };
int main() {
  mut Pair pair{};
  [[discard]] pair.b();
  [[discard]] pair.a();
  Box<int> number{3};
  Box<bool> flag{true};
  return 0;
}
)gti");
  if (!result.canGenerateCode()) {
    for (const auto &d : result.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
    return 1;
  }
  const auto find = [&](const std::string &name) -> const lang::SymbolRecord * {
    for (const auto &symbol : result.semantics.database().symbols())
      if (symbol.qualifiedName == name)
        return &symbol;
    return nullptr;
  };
  const auto *first = find("Pair::first");
  const auto *middle = find("Pair::middle");
  const auto *second = find("Pair::second");
  if (!first || !middle || !second ||
      !(first->id < middle->id && middle->id < second->id) ||
      first->type != lang::SemanticType::Int32 ||
      second->type != lang::SemanticType::Int32 || !first->mutableBinding ||
      !second->mutableBinding || !middle->staticMember) {
    std::cerr << "field identities or resolved metadata do not follow active "
                 "declaration order\n";
    return 1;
  }
  for (const auto &owner : result.mir.classInstances()) {
    for (const auto &field : owner.declaredFields) {
      const auto *symbol = result.semantics.database().findSymbol(field.symbol);
      if (!symbol || symbol->kind != lang::SymbolKind::Field) {
        std::cerr
            << "concrete MIR field lost its primary declaration identity\n";
        return 1;
      }
    }
  }
  std::cout << "field declaration identity contract passed\n";
}
