#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>

int main() {
  int failures = 0;
  const auto expect = [&](bool condition, const char *message) {
    if (!condition) {
      std::cerr << message << '\n';
      ++failures;
    }
  };
  const auto analyze = [](const std::string &source) {
    return lang::Frontend().analyze("initialization-native-identity.gti",
                                    source);
  };
  const auto rejected = [&](const std::string &source, const char *code) {
    auto result = analyze(source);
    const bool located =
        std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const auto &d) {
                      return d.code == code && !d.primary.source.empty() &&
                             d.primary.end > d.primary.start;
                    });
    if (!located)
      for (const auto &d : result.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
    expect(!result.canGenerateCode(), "invalid source must not reach codegen");
    expect(located, "invalid source needs its located owning diagnostic");
  };
  auto nullable = analyze(R"gti(
class Empty {
  int marker = 7;
public:
  Empty(nullptr_t value) {}
  int read() { return this.marker; }
};
class Parent {
  Empty first;
  Empty second = nullptr;
public:
  Parent() : first(nullptr) {}
  int read() { return this.first.read() + this.second.read(); }
};
int main() { Parent parent{}; return parent.read() - 14; }
)gti");
  expect(nullable.canGenerateCode(),
         "unrelated nominal nullptr constructor must initialize a field");
  if (nullable.canGenerateCode())
    expect(!gti_test::emitCpp(nullable).contents.empty(),
           "nullable field construction must retain executable authority");
  rejected(R"gti(
class Empty { public: Empty(nullptr_t value) {} };
void take(Empty value) {}
int main() { take(nullptr); return 0; }
)gti",
           "GTI-S2003");
  rejected(R"gti(
class Empty { Empty(nullptr_t value) {} };
class Parent { Empty field; public: Parent() : field(nullptr) {} };
int main() { return 0; }
)gti",
           "GTI-S2007");
  rejected("int main() { mut int value; value = 4; return value; }",
           "GTI-S2000");
  rejected("int main() { mut float value; return 0; }", "GTI-S2000");
  auto initialized =
      analyze("int main() { mut int value = 0; value = 4; return value - 4; }");
  expect(initialized.canGenerateCode(),
         "explicit local initialization remains valid");

  auto native = analyze(R"gti(
extern "C" {
  uint64_t strlen(const uint8_t* text);
  [[link_name("unrelated_native_length")]] uint64_t length(const uint8_t* text);
}
int probe(const uint8_t* text) {
  unsafe { return strlen(text) == length(text) ? 0 : 1; }
}
int main() { return 0; }
)gti");
  expect(native.canGenerateCode(), "native functions keep their semantic ABI");
  if (!native.canGenerateCode())
    for (const auto &d : native.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  if (native.canGenerateCode()) {
    const auto artifact = gti_test::emitCpp(native);
    int checked = 0;
    for (const auto &function : native.mir.functionInstances()) {
      if (function.linkage != lang::LanguageLinkage::C)
        continue;
      ++checked;
      expect(function.returnType == lang::SemanticType::UInt64,
             "private declaration isolation must not remap uint64_t");
      const auto name = "__gti_c_fn_" + std::to_string(function.declaration);
      expect(artifact.contents.find(name + "(") != std::string::npos,
             "native declaration and call must share private identity");
      expect(artifact.contents.find("__asm__(\"_" + function.externalSymbol +
                                    "\")") != std::string::npos,
             "Darwin link label must retain exact selected symbol");
      expect(artifact.contents.find("__asm__(\"" + function.externalSymbol +
                                    "\")") != std::string::npos,
             "non-Darwin link label must retain exact selected symbol");
    }
    expect(checked == 2, "both native declarations must retain identity");
  }
  rejected("extern \"C\" { [[link_name(\"bad\\\"label\")]] int32_t call(); } "
           "int main() { return 0; }",
           "GTI-S2054");
  rejected("extern \"C\" { [[link_name(\"__gti_c_fn_1\")]] int32_t call(); } "
           "int main() { return 0; }",
           "GTI-S2054");
  rejected(R"gti(
using Native = (int32_t) -> int32_t;
extern "C" { int32_t native(int32_t); }
int main() { Native callback = native; return 0; }
)gti", "GTI-S2076");

  auto unnamed = analyze(R"gti(
interface I { int32_t inspect(int32_t, uint32_t); };
int32_t discard(int32_t, uint32_t) { return 0; }
int main() { return discard(1, uint32_t(2)); }
)gti");
  expect(unnamed.canGenerateCode(),
         "unnamed bodyless and executable formals need physical identities");
  for (const std::string shape : {"int32_t", "int32_t&", "mut int32_t&",
                                  "int32_t*"}) {
    for (const std::string &declaration : {
             "int32_t f(" + shape + ");",
             "class C { public: int32_t f(" + shape + "); };",
             "interface I { int32_t f(" + shape + "); };"}) {
      auto cell = analyze(declaration + " int main() { return 0; }");
      expect(cell.canGenerateCode(),
             "each bodyless unnamed formal shape must retain canonical MIR");
      if (cell.canGenerateCode())
        expect(!gti_test::emitCpp(cell).contents.empty(),
               "each bodyless unnamed shape must reach production emission");
    }
    if (shape == "int32_t" || shape == "int32_t*") {
      auto cell = analyze("extern \"C\" { int32_t f(" + shape +
                          "); } int main() { return 0; }");
      expect(cell.canGenerateCode(),
             "unnamed valid C ABI shape must retain canonical MIR");
      if (cell.canGenerateCode())
        expect(!gti_test::emitCpp(cell).contents.empty(),
               "unnamed C ABI shape must reach production emission");
    }
  }
  if (unnamed.canGenerateCode()) {
    std::set<lang::SymbolId> ids;
    std::set<std::pair<std::size_t, std::size_t>> spans;
    for (const auto &symbol : unnamed.semantics.database().symbols()) {
      if (symbol.kind != lang::SymbolKind::Parameter)
        continue;
      expect(symbol.generated, "unnamed formal must remain tooling-hidden");
      expect(symbol.id != 0 && !symbol.nameSpan.source.empty() &&
                 symbol.nameSpan.end > symbol.nameSpan.start,
             "unnamed formal needs a real type source anchor");
      expect(unnamed.semantics.database().occurrencesForSymbol(symbol.id).empty(),
             "synthetic formal must not become a source name occurrence");
      ids.insert(symbol.id);
      spans.emplace(symbol.nameSpan.start, symbol.nameSpan.end);
    }
    expect(ids.size() == 4 && spans.size() == 4,
           "each owner and formal index must retain distinct identities");
    const auto lowered =
        gti_test::lowerProgram(unnamed, unnamed.mir, unnamed.mir);
    int emptyNames = 0;
    for (const auto &declaration : lowered.declarations()) {
      const auto *function =
          std::get_if<lang::LoweredFunctionDeclaration>(&declaration.payload);
      if (function == nullptr)
        continue;
      for (const auto &parameter : function->parameters) {
        expect(parameter.name.empty(),
               "Lowered formal must not acquire a manufactured public name");
        expect(parameter.symbol != 0,
               "Lowered formal must retain exact physical symbol");
        ++emptyNames;
      }
    }
    expect(emptyNames == 4, "both unnamed signatures must reach publication");
    expect(!gti_test::emitCpp(unnamed).contents.empty(),
           "unnamed formals must reach verified executable emission");
  }
  auto constexprUnnamed = analyze(
      "constexpr int32_t reads(int32_t*) { return 0; }\n"
      "int main() { return 0; }");
  expect(!constexprUnnamed.canGenerateCode(),
         "raw pointer constexpr restriction remains intact");
  const auto constDiagnostic = std::find_if(
      constexprUnnamed.diagnostics.begin(), constexprUnnamed.diagnostics.end(),
      [](const auto &d) { return d.code == "GTI-S2057"; });
  expect(constDiagnostic != constexprUnnamed.diagnostics.end() &&
             constDiagnostic->primary.start == 24 &&
             constDiagnostic->primary.end == 31,
         "unnamed constexpr diagnostic must anchor the actual type token");

  auto wrongOrigin = analyze(R"gti(
class Holder {
  int32_t& value;
public:
  Holder(int32_t& source) : value(source) {}
};
Holder build(int32_t&) {
  int32_t local = 2;
  return Holder(local);
}
int main() { return 0; }
)gti");
  const auto originDiagnostic = std::find_if(
      wrongOrigin.diagnostics.begin(), wrongOrigin.diagnostics.end(),
      [](const auto &d) { return d.code == "GTI-S2045"; });
  expect(!wrongOrigin.canGenerateCode() &&
             originDiagnostic != wrongOrigin.diagnostics.end() &&
             originDiagnostic->message.find("unnamed read-only parameter 1") !=
                 std::string::npos &&
             originDiagnostic->related.size() == 1 &&
             !originDiagnostic->related.front().span.source.empty() &&
             originDiagnostic->related.front().span.end >
                 originDiagnostic->related.front().span.start,
         "borrow origin diagnostic needs the unnamed formal's real type span");

  auto grouped = analyze(R"gti(
class Owned {
  int32_t value;
public:
  Owned(int32_t value) : value(value) {}
  Owned(Owned& other) = delete;
  Owned(Owned&& other) = default;
  ~Owned() {}
  int32_t read() { return this.value; }
};
Owned make() { return (((Owned(1)))); }
int main() { Owned value = ((make())); return value.read() - 1; }
)gti");
  expect(grouped.canGenerateCode(),
         "grouping must retain the selected owned producer and cleanup");
  if (grouped.canGenerateCode()) {
    int wrappers = 0;
    for (const auto &function : grouped.hir.functionInstances()) {
      for (const auto &value : function.body.values) {
        if (value.kind != lang::HirValueKind::Grouping ||
            value.info.category != lang::ValueCategory::Value)
          continue;
        ++wrappers;
        const auto *input = function.body.findValue(value.operands.front());
        expect(input != nullptr && input->dropObligation == value.dropObligation,
               "a grouping wrapper must share the producer's obligation");
        const auto *mir = grouped.mir.findFunctionInstance(function.id);
        expect(mir != nullptr &&
                   std::none_of(mir->body.values.begin(), mir->body.values.end(),
                                [&](const auto &row) {
                                  return row.sourceValue == value.id;
                                }),
               "parentheses must not materialize a second runtime value");
      }
    }
    expect(wrappers == 5, "all written owned wrappers need transparent evidence");
    expect(!gti_test::emitCpp(grouped).contents.empty(),
           "transparent ownership must reach production emission");
    bool mutated = false;
    for (const auto &function : grouped.mir.functionInstances()) {
      auto body = function.body;
      for (auto &block : body.blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Lifecycle ||
              instruction.lifecycle.empty() ||
              instruction.lifecycle.front().kind !=
                  lang::MirLifecycleEventKind::TransferOut)
            continue;
          instruction.hirValue += 10000;
          expect(!lang::verifyMirBody(body, function.id).valid(),
                 "a forged wrapper/producer cleanup identity must be rejected");
          mutated = true;
          break;
        }
        if (mutated)
          break;
      }
      if (mutated)
        break;
    }
    expect(mutated, "owned return must retain a verifiable transfer identity");
  }
  rejected(R"gti(
class Owned {
public:
  Owned() {}
  Owned(Owned& other) = delete;
  Owned(Owned&& other) = default;
};
int main() { Owned original{}; Owned copied = (original); return 0; }
)gti", "GTI-S2003");
  return failures ? 1 : 0;
}
