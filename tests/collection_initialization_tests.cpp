#include "../src/compiler/cpp_mir_body_emitter.h"
#include "../src/compiler/cpp_mir_representation_snapshot.h"
#include "cpp_backend_test_support.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <iostream>

namespace {
int failures = 0;
lang::FrontendResult analyze(const std::string &path,
                             const std::string &source) {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze(path, source, {library / "prelude.gti"}, {},
                                  {library});
}
void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
const std::string collection = R"(
class Numbers {
public:
  mut int total = 0;
  Numbers() {}
  static Numbers collection_init(uint64_t count) { return Numbers(); }
  static Numbers collection_insert(mut Numbers values, int value) {
    values.total += value;
    return std::move(values);
  }
};
)";
void rejected(const std::string &source, const std::string &fragment) {
  const auto result = analyze("collection-invalid.gti", source);
  expect(!result.canGenerateCode(), "invalid collection must stop compilation");
  const auto found = std::find_if(
      result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &d) {
        return d.code == "GTI-S2092" &&
               d.message.find(fragment) != std::string::npos;
      });
  expect(found != result.diagnostics.end(), "diagnostic: " + fragment);
  if (found == result.diagnostics.end()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  if (found != result.diagnostics.end()) {
    expect(found->primary.source.ends_with("/collection-invalid.gti") &&
               found->primary.start < found->primary.end &&
               found->primary.end <= source.size() && found->fixes.empty(),
           "collection diagnostic span and no speculative fix");
  }
}

void returnCleanupTail() {
  const auto result = analyze("collection-return.gti", R"(
class Item {
public:
  int value;
  Item(int n) : value(n) {}
  ~Item() { int check = 1 / this.value; }
};
void consume(Item item) {}
class Bag {
  Item held;
public:
  Bag() : held(Item(9)) {}
  static Bag collection_init(uint64_t count) { return Bag(); }
  static Bag collection_insert(Bag bag, Item item) {
    // The unmoved witness's fallible cleanup splits the return tail; the
    // moved-from parameter shells retire after it without destructors.
    Item witness{3};
    consume(std::move(item));
    return std::move(bag);
  }
};
int main() { Bag values = {Item(1), Item(2)}; return 0; }
)");
  expect(result.canGenerateCode(), "owning protocol return cleanup verifies");
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  const auto lowered = gti_test::lowerProgram(result, result.mir, result.mir);
  const lang::CppMirBodyEmissionMap map(
      lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23));
  expect(lang::CppMirBodyEmitter(result.mir, map).analyzeProgram().ready(),
         "moved-from return cleanup tail has a representation");
  bool mutated = false;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Invoke)
        continue;
      const auto *tail = function.body.findBlock(block.terminator.target);
      if (tail == nullptr)
        continue;
      for (const auto &move : block.instructions) {
        if (move.kind != lang::MirInstructionKind::Move ||
            move.operands.size() != 1 ||
            move.info.type != function.body.returnType)
          continue;
        for (const auto &drop : tail->instructions) {
          if (drop.kind != lang::MirInstructionKind::Drop ||
              drop.destination != move.operands.front().place)
            continue;
          auto forged = result.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              forged.findFunctionInstance(function.id));
          for (auto &candidate : changed->body.blocks) {
            if (candidate.id != tail->id)
              continue;
            for (auto &instruction : candidate.instructions) {
              if (instruction.id == drop.id)
                instruction.kind = lang::MirInstructionKind::Initialize;
            }
          }
          expect(!lang::CppMirBodyEmitter(forged, map)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Function, function.id}),
                 "return tail must reject a source reinitialization");
          mutated = true;
        }
      }
    }
  }
  expect(mutated, "regression exercises a multi-block moved-from cleanup tail");
}

void constrainedHooks() {
  const std::string declarations = R"(
class Batch<T> {
public:
  mut int count = 0;
  Batch(int seed) : count(seed) {}
  static Batch<T> collection_init(uint64_t count)
      requires std::default_initializable<T> { return Batch<T>(0); }
  static Batch<T> collection_insert(mut Batch<T> batch, T element)
      requires std::integral<T> {
    batch.count++;
    return std::move(batch);
  }
};
class NoDefault { public: NoDefault(int value) {} };
Batch<T> wrap<std::integral T>(T element) {
  Batch<T> result = {element};
  return result;
}
)";
  const auto valid = analyze("collection-constraints.gti", declarations + R"(
int main() {
  Batch<int> values = {1, 2};
  Batch<int> empty = {};
  auto generic = wrap<int>(3);
  Batch<NoDefault> explicit_only{4};
  return values.count + generic.count + explicit_only.count;
}
)");
  expect(
      valid.canGenerateCode(),
      "constrained hooks compose with generic calls and explicit construction");
  if (valid.canGenerateCode()) {
    expect(lang::verifyMirProgram(valid.mir).valid() &&
               !gti_test::emitCppText(valid).empty(),
           "constrained collection calls execute through verified MIR");
  } else {
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  for (const std::string type : {"float", "NoDefault"}) {
    const std::string source = declarations + "int main() { Batch<" + type +
                               "> values = {}; return 0; }";
    const auto invalid = analyze("collection-constraint-invalid.gti", source);
    expect(!invalid.canGenerateCode(),
           "both hook constraints apply to empty braces");
    const auto found = std::find_if(
        invalid.diagnostics.begin(), invalid.diagnostics.end(),
        [](const auto &diagnostic) { return diagnostic.code == "GTI-S2029"; });
    expect(found != invalid.diagnostics.end(),
           "constraint failure stays semantic");
    if (found != invalid.diagnostics.end()) {
      expect(found->primary.source.ends_with(
                 "/collection-constraint-invalid.gti") &&
                 found->primary.start == source.rfind("{}") &&
                 found->primary.end == found->primary.start + 1 &&
                 found->fixes.empty(),
             "constraint diagnostic identifies exact brace and offers no "
             "unsafe fix");
      expect(std::any_of(found->related.begin(), found->related.end(),
                         [](const auto &related) {
                           return related.message.find(
                                      "collection initialization hook") !=
                                  std::string::npos;
                         }),
             "constraint diagnostic relates the required hook");
    }
  }
}
} // namespace

int main() {
  constrainedHooks();
  returnCleanupTail();
  const auto generic = analyze("generic-collection.gti", R"(
#include <std/vector>
std::vector<T> wrap<T>(T element) requires std::vector_element<T> {
  std::vector<T> result = {std::move(element)};
  return std::move(result);
}
int main() {
  std::vector<int> values = wrap<int>(9);
  return values[std::size_t(0)];
}
)");
  expect(generic.canGenerateCode(),
         "generic body specializes its contextual collection plan");
  for (const auto &diagnostic : generic.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  if (generic.canGenerateCode())
    expect(!gti_test::emitCppText(generic).empty(),
           "generic collection plan emits through MIR");
  const std::string source = collection + R"(
struct Config { Numbers numbers{}; };
int main() {
  Numbers empty = {};
  Numbers numbers = {1, 2, 3};
  Config config = {.numbers = {4, 5}};
  return numbers.total + config.numbers.total;
}
)";
  auto result = analyze("collection.gti", source);
  for (const auto &d : result.diagnostics)
    std::cerr << d.code << ": " << d.message << '\n';
  expect(result.canGenerateCode(),
         "user-defined collection protocol and designated nesting");
  const auto formatted = lang::Formatter().format(source);
  expect(lang::Formatter().format(formatted) == formatted,
         "format idempotence");
  expect(analyze("formatted.gti", formatted).canGenerateCode(),
         "format preserves meaning");
  if (result.canGenerateCode()) {
    expect(lang::verifyMirProgram(result.mir).valid(),
           "collection MIR verifies");
    expect(!gti_test::emitCppText(result).empty(),
           "production MIR backend emits collections");
    auto forged = result.mir;
    bool mutated = false;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             forged.functionInstances())) {
      for (auto &block : function.body.blocks) {
        for (auto &instruction : block.instructions) {
          if (!mutated && instruction.kind == lang::MirInstructionKind::Call &&
              instruction.parameterTypes.size() == 2 &&
              instruction.parameterTypes.front().kind ==
                  lang::SemanticType::Class) {
            instruction.parameterTypes.back() = lang::SemanticType::Bool;
            mutated = true;
          }
        }
      }
    }
    expect(mutated && !lang::verifyMirProgram(forged).valid(),
           "forged insertion type rejected");
    forged = result.mir;
    mutated = false;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             forged.functionInstances())) {
      for (auto &block : function.body.blocks) {
        if (mutated || block.terminator.kind != lang::MirTerminatorKind::Invoke)
          continue;
        for (const auto &instruction : block.instructions) {
          if (instruction.id == block.terminator.invokeInstruction &&
              instruction.kind == lang::MirInstructionKind::Call &&
              instruction.parameterTypes.size() == 2 &&
              instruction.parameterTypes.front().kind ==
                  lang::SemanticType::Class) {
            block.terminator.kind = lang::MirTerminatorKind::Goto;
            mutated = true;
            break;
          }
        }
      }
    }
    expect(mutated && !lang::verifyMirProgram(forged).valid(),
           "insertion without its failure edge rejected");
  }
  rejected(collection + "int main(){ Numbers n = {true}; return 0; }",
           "collection element");
  rejected(collection +
               "int main(){ int64_t wide = 1; Numbers n = {wide}; return 0; }",
           "collection element");
  rejected("#include <std/vector>\n#include <std/string>\n"
           "int main(){ std::string value{\"owned\"}; "
           "std::vector<std::string> values = {value}; return 0; }",
           "collection element");
  rejected("class C { public: C(){} }; int main(){ C c = {}; return 0; }",
           "Collection initialization requires");
  rejected("class C { public: C(){} static C collection_init(uint64_t "
           "n){return C();} "
           "static C collection_insert(C c, int& x){return std::move(c);} }; "
           "int main(){ C c = {1}; return 0; }",
           "owned value parameters");
  rejected("class C { C(){} static C collection_init(uint64_t n){return C();} "
           "static C collection_insert(C c, int x){return std::move(c);} }; "
           "int main(){ C c = {}; return 0; }",
           "public");
  return failures == 0 ? 0 : 1;
}
