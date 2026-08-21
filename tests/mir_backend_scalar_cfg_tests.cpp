#include "gti/cpp_backend.h"
#include "gti/cpp_emitter.h"
#include "gti/frontend.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view marker = "// GTI verified-MIR body: scalar-cfg-v1";

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::size_t count(std::string_view text, std::string_view needle) {
  std::size_t result = 0;
  for (std::size_t position = text.find(needle); position != std::string::npos;
       position = text.find(needle, position + needle.size())) {
    ++result;
  }
  return result;
}

std::string_view functionDefinition(std::string_view generated,
                                    std::string_view sourceName) {
  const std::string needle = std::string{"_"} + std::string{sourceName} + "(";
  std::size_t definition = std::string_view::npos;
  for (std::size_t name = generated.find(needle);
       name != std::string_view::npos;
       name = generated.find(needle, name + needle.size())) {
    const std::size_t lineEnd = generated.find('\n', name);
    const std::size_t brace = generated.find(" {\n", name);
    if (brace != std::string_view::npos &&
        (lineEnd == std::string_view::npos || brace < lineEnd)) {
      definition = brace;
      break;
    }
  }
  const std::size_t end = definition == std::string_view::npos
                              ? definition
                              : generated.find("\n  }", definition);
  return definition == std::string_view::npos || end == std::string_view::npos
             ? std::string_view{}
             : generated.substr(definition, end + 4 - definition);
}

const lang::HirFunctionInstance *
findHirFunction(const lang::HirProgram &program, std::string_view name) {
  const auto found = std::find_if(
      program.functionInstances().begin(), program.functionInstances().end(),
      [name](const lang::HirFunctionInstance &instance) {
        return instance.source != nullptr &&
               instance.source->name().lexeme == name;
      });
  return found == program.functionInstances().end() ? nullptr : &*found;
}

const lang::MirFunctionInstance *findMirFunction(const lang::HirProgram &hir,
                                                 const lang::MirProgram &mir,
                                                 std::string_view name) {
  const lang::HirFunctionInstance *function = findHirFunction(hir, name);
  return function == nullptr ? nullptr : mir.findFunctionInstance(function->id);
}

lang::MirFunctionInstance *findMirFunction(const lang::HirProgram &hir,
                                           lang::MirProgram &mir,
                                           std::string_view name) {
  const lang::HirFunctionInstance *function = findHirFunction(hir, name);
  if (function == nullptr) {
    return nullptr;
  }
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      mir.functionInstances());
  const auto found =
      std::find_if(functions.begin(), functions.end(),
                   [&](const lang::MirFunctionInstance &candidate) {
                     return candidate.id == function->id;
                   });
  return found == functions.end() ? nullptr : &*found;
}

bool hasOperation(const lang::MirFunctionInstance *function,
                  lang::MirOperation operation) {
  if (function == nullptr) {
    return false;
  }
  for (const lang::MirBlock &block : function->body.blocks) {
    if (std::any_of(block.instructions.begin(), block.instructions.end(),
                    [operation](const lang::MirInstruction &instruction) {
                      return instruction.operation == operation;
                    })) {
      return true;
    }
  }
  return false;
}

bool hasInstructionKind(const lang::MirFunctionInstance *function,
                        lang::MirInstructionKind kind) {
  if (function == nullptr) {
    return false;
  }
  for (const lang::MirBlock &block : function->body.blocks) {
    if (std::any_of(block.instructions.begin(), block.instructions.end(),
                    [kind](const lang::MirInstruction &instruction) {
                      return instruction.kind == kind;
                    })) {
      return true;
    }
  }
  return false;
}

bool hasTerminator(const lang::MirFunctionInstance *function,
                   lang::MirTerminatorKind kind) {
  return function != nullptr &&
         std::any_of(function->body.blocks.begin(), function->body.blocks.end(),
                     [kind](const lang::MirBlock &block) {
                       return block.terminator.kind == kind;
                     });
}

bool hasBackedge(const lang::MirFunctionInstance *function) {
  if (function == nullptr) {
    return false;
  }
  for (const lang::MirBlock &block : function->body.blocks) {
    const lang::MirTerminator &terminator = block.terminator;
    if ((terminator.kind == lang::MirTerminatorKind::Goto ||
         terminator.kind == lang::MirTerminatorKind::Branch) &&
        terminator.target != 0 && terminator.target <= block.id) {
      return true;
    }
    if (terminator.kind == lang::MirTerminatorKind::Branch &&
        terminator.elseTarget != 0 && terminator.elseTarget <= block.id) {
      return true;
    }
  }
  return false;
}

lang::OptimizedProgram optimize(const lang::FrontendResult &frontend,
                                lang::OptimizationLevel level,
                                const lang::OptimizationResult &compatibility) {
  return lang::OptimizationPipeline().run({.hir = frontend.hir,
                                           .mir = frontend.mir,
                                           .level = level,
                                           .compatibility = &compatibility});
}

lang::BackendArtifact emit(const lang::FrontendResult &frontend,
                           const lang::MirProgram &mir,
                           const lang::OptimizationResult &compatibility) {
  return lang::CppBackend().generate({.program = frontend.program,
                                      .semantics = frontend.semantics,
                                      .hir = frontend.hir,
                                      .mir = mir,
                                      .sourceMir = &frontend.mir,
                                      .optimizations = compatibility});
}

void expectEmissionRejected(const lang::FrontendResult &frontend,
                            const lang::MirProgram &mir,
                            const lang::OptimizationResult &compatibility,
                            std::string_view message) {
  bool rejected = false;
  try {
    (void)emit(frontend, mir, compatibility);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected, message);
}

void expectSelectedDefinitions(std::string_view generated) {
  constexpr std::string_view selected[] = {
      "cfg_not",   "cfg_char",   "cfg_bits",  "cfg_less", "cfg_choose",
      "cfg_local", "cfg_switch", "cfg_short", "cfg_loop", "cfg_fold",
  };
  for (const std::string_view name : selected) {
    expect(functionDefinition(generated, name).find(marker) !=
               std::string_view::npos,
           std::string{"the selected scalar CFG body should carry the MIR "
                       "authority marker: "} +
               std::string{name});
  }

  constexpr std::string_view failureSelected[] = {
      "compatibility_checked",
      "compatibility_call_target",
      "compatibility_call",
      "compatibility_reference",
  };
  constexpr std::string_view failureMarker =
      "// GTI verified-MIR body: scalar-cfg-failure-v1";
  for (const std::string_view name : failureSelected) {
    expect(
        functionDefinition(generated, std::string{name} + "__gti_mir_failure")
                    .find(failureMarker) != std::string_view::npos &&
            functionDefinition(generated, name).empty(),
        std::string{"the closed component should emit only its explicit "
                    "failure-form MIR sibling: "} +
            std::string{name});
  }
}

// Finds one deferred member definition. The emitted name carries a mangling
// prefix and a call site can share a line with an opening brace, so the match
// requires the qualified `Owner::` spelling on the definition line itself.
std::string_view memberDefinition(std::string_view generated,
                                  std::string_view sourceName) {
  const std::string needle = std::string{"_"} + std::string{sourceName} + "(";
  for (std::size_t name = generated.find(needle);
       name != std::string_view::npos;
       name = generated.find(needle, name + needle.size())) {
    const std::size_t lineStart = generated.rfind('\n', name);
    const std::size_t lineEnd = generated.find('\n', name);
    const std::size_t brace = generated.find(" {\n", name);
    const std::string_view line = generated.substr(
        lineStart == std::string_view::npos ? 0 : lineStart + 1,
        lineEnd - (lineStart == std::string_view::npos ? 0 : lineStart + 1));
    if (line.find("::") == std::string_view::npos) {
      continue;
    }
    if (brace != std::string_view::npos &&
        (lineEnd == std::string_view::npos || brace < lineEnd)) {
      const std::size_t end = generated.find("\n  }", brace);
      return end == std::string_view::npos
                 ? std::string_view{}
                 : generated.substr(brace, end - brace + 4);
    }
  }
  return {};
}

std::string_view specializedMemberDefinition(std::string_view generated,
                                             std::string_view owner,
                                             std::string_view sourceName) {
  const std::string needle = std::string{"_"} + std::string{sourceName} + "(";
  for (std::size_t name = generated.find(needle);
       name != std::string_view::npos;
       name = generated.find(needle, name + needle.size())) {
    const std::size_t lineStart = generated.rfind('\n', name);
    const std::size_t lineEnd = generated.find('\n', name);
    const std::size_t brace = generated.find(" {\n", name);
    const std::string_view line = generated.substr(
        lineStart == std::string_view::npos ? 0 : lineStart + 1,
        lineEnd - (lineStart == std::string_view::npos ? 0 : lineStart + 1));
    if (line.find("template <>") == std::string_view::npos ||
        line.find(owner) == std::string_view::npos) {
      continue;
    }
    if (brace != std::string_view::npos &&
        (lineEnd == std::string_view::npos || brace < lineEnd)) {
      const std::size_t end = generated.find("\n  }", brace);
      return end == std::string_view::npos
                 ? std::string_view{}
                 : generated.substr(brace, end - brace + 4);
    }
  }
  return {};
}

void testSelectedFamily(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), readFile(fixture));
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "the scalar CFG backend fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  expect(
      hasOperation(findMirFunction(frontend.hir, frontend.mir, "cfg_not"),
                   lang::MirOperation::LogicalNot) &&
          hasOperation(findMirFunction(frontend.hir, frontend.mir, "cfg_bits"),
                       lang::MirOperation::BitwiseXor) &&
          hasOperation(findMirFunction(frontend.hir, frontend.mir, "cfg_less"),
                       lang::MirOperation::Less),
      "the fixture should retain logical, bitwise, and comparison scalar "
      "MIR operations");
  expect(
      hasTerminator(findMirFunction(frontend.hir, frontend.mir, "cfg_choose"),
                    lang::MirTerminatorKind::Branch) &&
          hasTerminator(
              findMirFunction(frontend.hir, frontend.mir, "cfg_switch"),
              lang::MirTerminatorKind::Switch) &&
          hasTerminator(
              findMirFunction(frontend.hir, frontend.mir, "cfg_short"),
              lang::MirTerminatorKind::Branch),
      "the fixture should retain branch, switch, and short-circuit CFG "
      "terminators");
  const lang::MirFunctionInstance *local =
      findMirFunction(frontend.hir, frontend.mir, "cfg_local");
  expect(hasInstructionKind(local, lang::MirInstructionKind::Initialize) &&
             hasInstructionKind(local, lang::MirInstructionKind::Assign),
         "the fixture should retain scalar local initialization and "
         "assignment");
  expect(hasBackedge(findMirFunction(frontend.hir, frontend.mir, "cfg_loop")),
         "the fixture should retain a verified loop backedge");
  expect(hasOperation(findMirFunction(frontend.hir, frontend.mir, "cfg_fold"),
                      lang::MirOperation::Identity),
         "the O0 fixture should retain grouped scalar identities for the "
         "optimizer authority check");

  const lang::OptimizationPipeline pipeline;
  const lang::OptimizationResult o0Compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O0);
  const lang::OptimizationResult o1Compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O1);
  const lang::OptimizationResult o3Compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O3);
  const lang::OptimizedProgram o0 =
      optimize(frontend, lang::OptimizationLevel::O0, o0Compatibility);
  const lang::OptimizedProgram o1 =
      optimize(frontend, lang::OptimizationLevel::O1, o1Compatibility);
  const lang::OptimizedProgram o3 =
      optimize(frontend, lang::OptimizationLevel::O3, o3Compatibility);
  expect(o0.valid() && lang::verifyMirProgram(o0.mir).valid() && o1.valid() &&
             lang::verifyMirProgram(o1.mir).valid() && o3.valid() &&
             lang::verifyMirProgram(o3.mir).valid(),
         "the scalar CFG production MIR should verify at O0, O1, and O3");
  if (!o0.valid() || !o1.valid() || !o3.valid()) {
    return;
  }

  const lang::BackendArtifact o0Artifact =
      emit(frontend, o0.mir, o0Compatibility);
  const lang::BackendArtifact o1Artifact =
      emit(frontend, o1.mir, o1Compatibility);
  const lang::BackendArtifact o3Artifact =
      emit(frontend, o3.mir, o3Compatibility);
  const lang::BackendArtifact o1WithO0Compatibility =
      emit(frontend, o1.mir, o0Compatibility);
  expectSelectedDefinitions(o0Artifact.contents);
  expectSelectedDefinitions(o1Artifact.contents);
  expectSelectedDefinitions(o3Artifact.contents);

  bool missingSourceRejected = false;
  try {
    (void)lang::CppBackend().generate({.program = frontend.program,
                                       .semantics = frontend.semantics,
                                       .hir = frontend.hir,
                                       .mir = o0.mir,
                                       .optimizations = o0Compatibility});
  } catch (const std::logic_error &) {
    missingSourceRejected = true;
  }
  expect(missingSourceRejected,
         "production C++ emission must fail closed without its canonical "
         "pre-optimization MIR snapshot");

  const std::string_view o0Fold =
      functionDefinition(o0Artifact.contents, "cfg_fold");
  const std::string_view o1Fold =
      functionDefinition(o1Artifact.contents, "cfg_fold");
  const std::string_view o3Fold =
      functionDefinition(o3Artifact.contents, "cfg_fold");
  expect(!o0Fold.empty() && o0Fold != o1Fold,
         "the marked CFG body should reflect the O1 identity-fold rewrite");
  expect(!o1Fold.empty() && o1Fold == o3Fold,
         "O1 and O3 should emit the same verified grouped-literal CFG body");
  expect(functionDefinition(o1WithO0Compatibility.contents, "cfg_fold") ==
             o1Fold,
         "verified optimized CFG MIR should control the selected body even "
         "when compatibility emission retains its O0 optimization result");

  lang::MirProgram swapped = o0.mir;
  const lang::HirFunctionInstance *choose =
      findHirFunction(frontend.hir, "cfg_choose");
  lang::MirBlock *branch = nullptr;
  if (choose != nullptr) {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        swapped.functionInstances());
    if (choose->id > 0 && choose->id <= functions.size()) {
      for (lang::MirBlock &block : functions[choose->id - 1].body.blocks) {
        if (block.terminator.kind == lang::MirTerminatorKind::Branch) {
          branch = &block;
          break;
        }
      }
    }
  }
  expect(branch != nullptr,
         "the branch-authority fixture should locate cfg_choose's MIR branch");
  if (branch == nullptr) {
    return;
  }
  std::swap(branch->terminator.target, branch->terminator.elseTarget);
  expect(lang::verifyMirProgram(swapped).valid(),
         "swapping two type-compatible branch successors should remain valid "
         "MIR");
  if (lang::verifyMirProgram(swapped).valid()) {
    expectEmissionRejected(
        frontend, swapped, o0Compatibility,
        "scalar-cfg-v1 must reject a branch-successor swap that has no "
        "optimizer rewrite provenance");
  }

  lang::MirProgram substitutedOperation = o0.mir;
  lang::MirFunctionInstance *less =
      findMirFunction(frontend.hir, substitutedOperation, "cfg_less");
  lang::MirInstruction *comparison = nullptr;
  if (less != nullptr) {
    for (lang::MirBlock &block : less->body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return instruction.operation == lang::MirOperation::Less;
          });
      if (found != block.instructions.end()) {
        comparison = &*found;
        break;
      }
    }
  }
  expect(comparison != nullptr,
         "the operation-authority fixture should locate cfg_less's MIR "
         "comparison");
  if (comparison != nullptr) {
    comparison->operation = lang::MirOperation::Greater;
    expect(lang::verifyMirProgram(substitutedOperation).valid(),
           "a same-domain comparison substitution should remain valid generic "
           "MIR");
    if (lang::verifyMirProgram(substitutedOperation).valid()) {
      expectEmissionRejected(
          frontend, substitutedOperation, o0Compatibility,
          "scalar-cfg-v1 must reject a same-typed operation substitution that "
          "has no optimizer rewrite provenance");
    }
  }
}

// The scalar-cfg family now admits an ordinary non-static, non-virtual,
// non-operator, read-only member of one concrete non-generic class. Emission
// stays keyed per source declaration, so the concrete-owner requirement and
// every graceful decline below are part of the family contract.
void testConcreteMemberSelection() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("mir-scalar-cfg-member.gti", R"(
class Chooser {
  mut int stored;

public:
  Chooser(int input) : stored(input) {}
  int mask(bool pick, int left, int right) {
    mut int result = 0;
    if (pick) {
      result = left & right;
    } else {
      result = left | right;
    }
    return result;
  }
  bool same(int left, int right) { return left == right; }
  int reads_this() { return this.stored; }
  void store(int next) mut { this.stored = next; }
  bool same_as(Chooser& other) { return this.stored == other.stored; }
  bool twins_with(Chooser& other) { return this.same_as(other); }
  void bump(int next) mut { this.store(next); }
};

int main() {
  mut Chooser chooser = Chooser(3);
  if (chooser.mask(true, 6, 3) != 2 or !chooser.same(4, 4) or
      chooser.reads_this() != 3) {
    return 1;
  }
  chooser.store(9);
  if (chooser.reads_this() != 9) {
    return 2;
  }
  mut Chooser twin = Chooser(9);
  if (!chooser.same_as(twin)) {
    return 3;
  }
  if (!chooser.twins_with(twin)) {
    return 4;
  }
  chooser.bump(11);
  if (chooser.reads_this() != 11) {
    return 5;
  }
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the concrete-member fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::OptimizedProgram optimized =
      optimize(frontend, lang::OptimizationLevel::O0, compatibility);
  expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
         "the concrete-member fixture should retain valid MIR");
  if (!optimized.valid() || !lang::verifyMirProgram(optimized.mir).valid()) {
    return;
  }
  const lang::BackendArtifact artifact =
      emit(frontend, optimized.mir, compatibility);
  expect(memberDefinition(artifact.contents, "mask").find(marker) !=
             std::string_view::npos,
         "a this-free read-only member of a concrete class should emit from "
         "verified MIR");
  expect(memberDefinition(artifact.contents, "same").find(marker) !=
             std::string_view::npos,
         "a second eligible member of the same class should emit from "
         "verified MIR");
  expect(memberDefinition(artifact.contents, "reads_this").find(marker) !=
             std::string_view::npos,
         "a read-only member reading one scalar field through `this` should "
         "emit from verified MIR");
  expect(memberDefinition(artifact.contents, "reads_this")
                 .find("(*this).stored") != std::string_view::npos,
         "the emitted field place should bind by reference to the live "
         "member spelling");
  const std::string_view storeBody =
      memberDefinition(artifact.contents, "store");
  expect(storeBody.find(marker) != std::string_view::npos,
         "a mutable-receiver member storing to one scalar field should emit "
         "from verified MIR");
  expect(storeBody.find("auto &__gti_mir_p_") != std::string_view::npos &&
             storeBody.find("const auto &__gti_mir_p_") ==
                 std::string_view::npos &&
             storeBody.find(" = (*this).stored;") != std::string_view::npos,
         "the mutable receiver must bind its field place as a non-const "
         "reference to the live member");
  const std::string_view sameBody =
      memberDefinition(artifact.contents, "same_as");
  expect(sameBody.find(marker) != std::string_view::npos &&
             sameBody.find("= &__gti_mir_arg_0;") != std::string_view::npos &&
             sameBody.find("(*__gti_mir_p_") != std::string_view::npos,
         "a reference parameter should bind its pointer carrier and read "
         "the other object's field through the dereference chain (ADR 018)");
  // The frontend marks the two call-forwarding members may-raise, so the
  // admission selector prefers their failure form (0.246.0): the staged
  // receiver-call vocabulary lives only in the transformed sibling. The
  // ordinary source name must not regain a terminating boundary wrapper.
  const std::string_view twinsBody =
      memberDefinition(artifact.contents, "twins_with__gti_mir_failure");
  expect(twinsBody.find("// GTI verified-MIR body: scalar-cfg-failure-v1") !=
                 std::string_view::npos &&
             twinsBody.find("stages a borrowed place") !=
                 std::string_view::npos &&
             twinsBody.find("(*this).::__gti_program::") !=
                 std::string_view::npos &&
             twinsBody.find("((*__gti_mir_p_") != std::string_view::npos,
         "a receiver-carrying call should spell its staged borrowed places "
         "as the receiver expression and the qualified member name");
  expect(artifact.contents.find("scalar-cfg-v1 constructor-instance") !=
                 std::string::npos &&
             artifact.contents.find(
                 "Chooser::Chooser(std::int32_t __gti_mir_arg_0) : "
                 "stored(__gti_mir_arg_0) {") != std::string::npos,
         "the concrete constructor should spell its verified field "
         "initializer in the native initializer list and emit its body from "
         "general MIR");
  expect(artifact.contents.find("scalar-cfg-v1 field-initializers-instance") !=
                 std::string::npos &&
             artifact.contents.find(
                 "scalar-cfg-v1 static-field-initializers-instance") !=
                 std::string::npos,
         "the concrete class's passive initializer bodies should publish "
         "their verified schedule and verified-empty markers");
  const std::string_view bumpBody =
      memberDefinition(artifact.contents, "bump__gti_mir_failure");
  expect(
      bumpBody.find("// GTI verified-MIR body: scalar-cfg-failure-v1") !=
              std::string_view::npos &&
          bumpBody.find("stages a borrowed place") != std::string_view::npos &&
          bumpBody.find("(*this).::__gti_program::") != std::string_view::npos,
      "a mutable-receiver call should stage its write borrow and spell "
      "the qualified member name exactly like the read form");
  expect(functionDefinition(artifact.contents, "entry__gti_mir_failure")
                 .find("// GTI verified-MIR body: scalar-cfg-failure-v1") !=
             std::string_view::npos,
         "the hosted entry should join the failure component through its "
         "explicit transformed body");
  expect(count(artifact.contents, "// GTI verified-MIR body: scalar-cfg-v1 "
                                  "constructor-instance") == 1 &&
             count(artifact.contents, "// GTI verified-MIR body: scalar-cfg-v1 "
                                      "field-initializers-instance") == 1 &&
             count(artifact.contents,
                   "// GTI verified-MIR body: scalar-cfg-v1 "
                   "static-field-initializers-instance") == 1 &&
             count(artifact.contents, "// GTI verified-MIR body: scalar-cfg-v1 "
                                      "module-instance") == 1,
         "the concrete constructor, initializer bodies, and empty module "
         "body should each carry their family marker exactly once");
}

// A member access whose object is a local binding rather than `this` reads
// through a Binding-rooted projected place, which the family does not admit;
// the graceful HIR gate must keep the body compatible rather than fail
// closed.
void testForeignObjectFieldReadStaysCompatibility() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("mir-scalar-cfg-foreign-field.gti", R"(
class Holder {
public:
  int stored;
  Holder(int input) : stored(input) {}
};

class Reader {
public:
  Reader() {}
  int read_other(Holder holder) { return holder.stored; }
};

int main() {
  Holder holder = Holder(4);
  Reader reader = Reader();
  return reader.read_other(holder) - 4;
}
)");
  expect(frontend.canGenerateCode(),
         "the foreign-field fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::OptimizedProgram optimized =
      optimize(frontend, lang::OptimizationLevel::O0, compatibility);
  expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
         "the foreign-field fixture should retain valid MIR");
  if (!optimized.valid() || !lang::verifyMirProgram(optimized.mir).valid()) {
    return;
  }
  const lang::BackendArtifact artifact =
      emit(frontend, optimized.mir, compatibility);
  expect(memberDefinition(artifact.contents, "read_other").find(marker) ==
             std::string_view::npos,
         "a field read on a non-receiver object lies outside the family and "
         "must stay wholly on compatibility emission");
}

// Calls are admitted per body: an eligible call names a static
// proved-failure-free source free function, and the callee's own authority is
// decided independently, so no closed-graph selection is required and no
// failure channel can cross into a differently-emitted neighbor.
void testPerBodyCallSelection() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("mir-scalar-cfg-per-body-call.gti", R"(
int mask_bits(int left, int right) { return left & right; }

int checked_add(int left, int right) { return left + right; }

class Widget {
  int stored;

public:
  Widget(int input) : stored(input) {}
  int combined(int other) { return mask_bits(this.stored, other); }
  int risky(int other) { return checked_add(this.stored, other); }
};

int main() {
  Widget widget = Widget(6);
  if (widget.combined(3) == 2 and widget.risky(1) == 7) {
    return 0;
  }
  return 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the per-body call fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::OptimizedProgram optimized =
      optimize(frontend, lang::OptimizationLevel::O0, compatibility);
  expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
         "the per-body call fixture should retain valid MIR");
  if (!optimized.valid() || !lang::verifyMirProgram(optimized.mir).valid()) {
    return;
  }
  const lang::BackendArtifact artifact =
      emit(frontend, optimized.mir, compatibility);
  expect(memberDefinition(artifact.contents, "combined").find(marker) !=
             std::string_view::npos,
         "a member calling a proved-failure-free free function should emit "
         "from verified MIR without closed-graph selection");
  expect(functionDefinition(artifact.contents, "mask_bits").find(marker) !=
             std::string_view::npos,
         "the called free function keeps its own independent body authority");
  expect(memberDefinition(artifact.contents, "risky").find(marker) ==
             std::string_view::npos,
         "a call to a may-raise target must decline gracefully to "
         "compatibility rather than fail closed");
}

void testGenericOwnerMemberStaysCompatibility() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("mir-scalar-cfg-generic-owner.gti", R"(
class Wrap<T> {
  T stored;

public:
  Wrap(T input) : stored(input) {}
  bool matches(int left, int right) { return left == right; }
};

int main() {
  Wrap<int> wrap = Wrap<int>(1);
  return wrap.matches(2, 2) ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the generic-owner fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::OptimizedProgram optimized =
      optimize(frontend, lang::OptimizationLevel::O0, compatibility);
  expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
         "the generic-owner fixture should retain valid MIR");
  if (!optimized.valid() || !lang::verifyMirProgram(optimized.mir).valid()) {
    return;
  }
  // A member of a generic owner publishes per admitted concrete instance
  // as an explicit member specialization (0.184.0): the deferred template
  // definition stays for unadmitted instantiations, and the declaration-
  // keyed substituted-body hazard that once regressed examples 07/17 does
  // not apply to the per-instance form.
  const lang::BackendArtifact artifact =
      emit(frontend, optimized.mir, compatibility);
  expect(specializedMemberDefinition(artifact.contents, "Wrap<std::int32_t>",
                                     "matches")
                 .find(marker) != std::string_view::npos,
         "the generic owner's admitted member instance should emit one "
         "specialized verified-MIR body");
  expect(functionDefinition(artifact.contents, "entry__gti_mir_failure")
                 .find("// GTI verified-MIR body: scalar-cfg-failure-v1") !=
             std::string_view::npos,
         "the generic-owner fixture's hosted entry should join the failure "
         "component through its explicit transformed body");
  expect(artifact.contents.find(
             "template <> bool __gti_program::Wrap<std::int32_t>::") !=
             std::string::npos,
         "the admitted member should publish as the explicit specialization "
         "of its concrete owner");
  // The generic owner's per-instance initializer bodies are verified
  // passive (no field carries an initializer stage and the static body is
  // empty), so each emits from MIR alongside the member specialization.
  expect(count(artifact.contents, "// GTI verified-MIR body: scalar-cfg-v1 "
                                  "field-initializers-instance") == 1,
         "the generic owner's field-initializer body should emit from "
         "verified MIR");
  expect(count(artifact.contents, "// GTI verified-MIR body: scalar-cfg-v1 "
                                  "static-field-initializers-instance") == 1,
         "the generic owner's static-initializer body should emit from "
         "verified MIR");
}

void testIncoherentSwitchRejected(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), readFile(fixture));
  expect(frontend.canGenerateCode(),
         "the adversarial switch fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::OptimizationPipeline pipeline;
  const lang::OptimizationResult compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O0);
  const lang::OptimizedProgram optimized =
      optimize(frontend, lang::OptimizationLevel::O0, compatibility);
  expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
         "the adversarial switch fixture should begin with verified O0 MIR");
  if (!optimized.valid() || !lang::verifyMirProgram(optimized.mir).valid()) {
    return;
  }

  lang::MirProgram incoherent = optimized.mir;
  const lang::HirFunctionInstance *switchFunction =
      findHirFunction(frontend.hir, "cfg_switch");
  lang::MirTerminator *switchTerminator = nullptr;
  if (switchFunction != nullptr) {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        incoherent.functionInstances());
    if (switchFunction->id > 0 && switchFunction->id <= functions.size()) {
      for (lang::MirBlock &block :
           functions[switchFunction->id - 1].body.blocks) {
        if (block.terminator.kind == lang::MirTerminatorKind::Switch) {
          switchTerminator = &block.terminator;
          break;
        }
      }
    }
  }
  expect(switchTerminator != nullptr &&
             switchTerminator->switchTargets.size() >= 2 &&
             switchTerminator->switchTargets[0].value &&
             switchTerminator->switchTargets[1].value,
         "the adversarial fixture should expose two concrete switch cases");
  if (switchTerminator == nullptr ||
      switchTerminator->switchTargets.size() < 2 ||
      !switchTerminator->switchTargets[0].value ||
      !switchTerminator->switchTargets[1].value) {
    return;
  }

  switchTerminator->switchTargets[1].value =
      switchTerminator->switchTargets[0].value;
  const lang::MirVerificationResult verification =
      lang::verifyMirProgram(incoherent);
  expect(!verification.valid(),
         "the generic MIR verifier should reject a duplicate switch case "
         "before backend family selection");

  lang::MirProgram outOfDomain = optimized.mir;
  lang::MirFunctionInstance *outOfDomainFunction =
      findMirFunction(frontend.hir, outOfDomain, "cfg_switch");
  lang::MirTerminator *outOfDomainSwitch = nullptr;
  if (outOfDomainFunction != nullptr) {
    for (lang::MirBlock &block : outOfDomainFunction->body.blocks) {
      if (block.terminator.kind == lang::MirTerminatorKind::Switch) {
        outOfDomainSwitch = &block.terminator;
        break;
      }
    }
  }
  expect(outOfDomainSwitch != nullptr &&
             !outOfDomainSwitch->switchTargets.empty() &&
             outOfDomainSwitch->switchTargets.front().value,
         "the switch-domain mutation should locate one concrete case");
  if (outOfDomainSwitch != nullptr &&
      !outOfDomainSwitch->switchTargets.empty() &&
      outOfDomainSwitch->switchTargets.front().value) {
    outOfDomainSwitch->switchTargets.front().value->value = {
        .negative = false,
        .magnitude = std::numeric_limits<std::uint64_t>::max()};
    expect(!lang::verifyMirProgram(outOfDomain).valid(),
           "the generic MIR verifier should reject an out-of-domain switch "
           "case before native emission");
  }

  lang::MirProgram negativeZero = optimized.mir;
  lang::MirFunctionInstance *negativeZeroFunction =
      findMirFunction(frontend.hir, negativeZero, "cfg_switch");
  lang::MirTerminator *negativeZeroSwitch = nullptr;
  if (negativeZeroFunction != nullptr) {
    for (lang::MirBlock &block : negativeZeroFunction->body.blocks) {
      if (block.terminator.kind == lang::MirTerminatorKind::Switch) {
        negativeZeroSwitch = &block.terminator;
        break;
      }
    }
  }
  expect(negativeZeroSwitch != nullptr &&
             !negativeZeroSwitch->switchTargets.empty() &&
             negativeZeroSwitch->switchTargets.front().value,
         "the negative-zero mutation should locate one integer case");
  if (negativeZeroSwitch != nullptr &&
      !negativeZeroSwitch->switchTargets.empty() &&
      negativeZeroSwitch->switchTargets.front().value) {
    negativeZeroSwitch->switchTargets.front().value->value = {.negative = true,
                                                              .magnitude = 0};
    expect(!lang::verifyMirProgram(negativeZero).valid(),
           "the generic MIR verifier should reject noncanonical signed "
           "negative zero before it becomes a duplicate native case label");
  }

  lang::MirProgram emptyBooleanSwitch = optimized.mir;
  lang::MirFunctionInstance *emptyBooleanFunction =
      findMirFunction(frontend.hir, emptyBooleanSwitch, "cfg_switch");
  lang::MirTerminator *emptyBooleanTerminator = nullptr;
  if (emptyBooleanFunction != nullptr) {
    for (lang::MirBlock &block : emptyBooleanFunction->body.blocks) {
      if (block.terminator.kind == lang::MirTerminatorKind::Switch) {
        emptyBooleanTerminator = &block.terminator;
        break;
      }
    }
  }
  if (emptyBooleanTerminator != nullptr) {
    emptyBooleanTerminator->value =
        lang::MirOperand{.kind = lang::MirOperandKind::Constant,
                         .literal = lang::Literal{true},
                         .type = lang::SemanticType::Bool};
    emptyBooleanTerminator->switchTargets.clear();
    (void)lang::rebuildMirValueUses(emptyBooleanFunction->body);
  }
  expect(emptyBooleanTerminator != nullptr &&
             !lang::verifyMirProgram(emptyBooleanSwitch).valid(),
         "the generic verifier should reject an unsupported boolean switch "
         "selector even when it has no concrete case labels");

  lang::MirProgram forgedEnumeratorOwner = optimized.mir;
  lang::MirFunctionInstance *enumFunction =
      findMirFunction(frontend.hir, forgedEnumeratorOwner, "cfg_switch");
  lang::MirTerminator *enumTerminator = nullptr;
  lang::MirPlaceId enumSelectorPlace = 0;
  if (enumFunction != nullptr) {
    const auto parameter = std::find_if(
        enumFunction->body.places.begin(), enumFunction->body.places.end(),
        [](const lang::MirPlace &place) {
          return place.root == lang::MirPlaceRootKind::Binding &&
                 place.initiallyAvailable;
        });
    if (parameter != enumFunction->body.places.end()) {
      enumSelectorPlace = parameter->id;
    }
    for (lang::MirBlock &block : enumFunction->body.blocks) {
      if (block.terminator.kind == lang::MirTerminatorKind::Switch) {
        enumTerminator = &block.terminator;
        break;
      }
    }
  }
  constexpr lang::EnumId syntheticEnum = 777;
  const lang::SemanticType enumType =
      lang::SemanticType::enumType(syntheticEnum);
  if (enumTerminator != nullptr && enumSelectorPlace != 0) {
    enumTerminator->value = lang::MirOperand{.kind = lang::MirOperandKind::Copy,
                                             .place = enumSelectorPlace,
                                             .type = enumType};
    for (lang::MirSwitchTarget &target : enumTerminator->switchTargets) {
      if (!target.value) {
        continue;
      }
      target.value->kind = lang::SwitchCaseKind::Enumerator;
      target.value->type = enumType;
      target.value->enumOwner = syntheticEnum;
    }
    (void)lang::rebuildMirValueUses(enumFunction->body);
  }
  expect(enumTerminator != nullptr && enumSelectorPlace != 0 &&
             lang::verifyMirProgram(forgedEnumeratorOwner).valid(),
         "a well-formed synthetic enum switch should satisfy the generic "
         "selector and case-domain contract");
  if (enumTerminator != nullptr && !enumTerminator->switchTargets.empty() &&
      enumTerminator->switchTargets.front().value &&
      lang::verifyMirProgram(forgedEnumeratorOwner).valid()) {
    enumTerminator->switchTargets.front().value->enumOwner = syntheticEnum + 1;
    expect(!lang::verifyMirProgram(forgedEnumeratorOwner).valid(),
           "an enumerator case owner must match the switch enum identity");
  }

  lang::MirProgram forgedTraits = optimized.mir;
  lang::MirFunctionInstance *traitFunction =
      findMirFunction(frontend.hir, forgedTraits, "cfg_choose");
  lang::MirPlace *parameterPlace = nullptr;
  if (traitFunction != nullptr) {
    const auto found = std::find_if(
        traitFunction->body.places.begin(), traitFunction->body.places.end(),
        [](const lang::MirPlace &place) {
          return place.root == lang::MirPlaceRootKind::Binding &&
                 place.initiallyAvailable;
        });
    if (found != traitFunction->body.places.end()) {
      parameterPlace = &*found;
    }
  }
  expect(parameterPlace != nullptr,
         "the trait-forgery mutation should locate a scalar parameter place");
  if (parameterPlace != nullptr) {
    parameterPlace->traits.copyable = false;
    expect(lang::verifyMirProgram(forgedTraits).valid(),
           "a forged scalar copyability flag should remain structurally valid "
           "MIR so family coherence owns rejection");
    if (lang::verifyMirProgram(forgedTraits).valid()) {
      expectEmissionRejected(
          frontend, forgedTraits, compatibility,
          "forged scalar ownership traits must not enter native CFG emission "
          "or demote the source body to AST emission");
    }
  }

  lang::MirProgram duplicatedStorage = optimized.mir;
  lang::MirFunctionInstance *localFunction =
      findMirFunction(frontend.hir, duplicatedStorage, "cfg_local");
  lang::MirPlaceId localPlaceId = 0;
  if (localFunction != nullptr) {
    const auto localPlace = std::find_if(
        localFunction->body.places.begin(), localFunction->body.places.end(),
        [](const lang::MirPlace &place) {
          return place.root == lang::MirPlaceRootKind::Binding &&
                 !place.initiallyAvailable;
        });
    if (localPlace != localFunction->body.places.end()) {
      localPlaceId = localPlace->id;
      lang::MirPlace duplicate = *localPlace;
      duplicate.id = localFunction->body.places.size() + 1;
      localFunction->body.places.push_back(std::move(duplicate));
      bool retargeted = false;
      for (lang::MirBlock &block : localFunction->body.blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::Load &&
              instruction.operands.size() == 1 &&
              instruction.operands.front().kind == lang::MirOperandKind::Copy &&
              instruction.operands.front().place == localPlaceId) {
            instruction.operands.front().place =
                localFunction->body.places.back().id;
            retargeted = true;
          }
        }
      }
      expect(retargeted,
             "the duplicate-place mutation should retarget cfg_local's "
             "scalar load");
    }
  }
  expect(localPlaceId != 0 && lang::verifyMirProgram(duplicatedStorage).valid(),
         "two MIR places for one scalar binding should remain generically "
         "valid so backend storage materialization owns rejection");
  if (localPlaceId != 0 && lang::verifyMirProgram(duplicatedStorage).valid()) {
    expectEmissionRejected(
        frontend, duplicatedStorage, compatibility,
        "one verified scalar storage identity must not materialize as two "
        "independent C++ locals");
  }

  lang::MirProgram forgedPlaceKey = optimized.mir;
  lang::MirFunctionInstance *keyFunction =
      findMirFunction(frontend.hir, forgedPlaceKey, "cfg_choose");
  lang::MirPlace *keyPlace = nullptr;
  if (keyFunction != nullptr) {
    const auto found = std::find_if(
        keyFunction->body.places.begin(), keyFunction->body.places.end(),
        [](const lang::MirPlace &place) {
          return place.root == lang::MirPlaceRootKind::Binding &&
                 place.initiallyAvailable && place.key;
        });
    if (found != keyFunction->body.places.end()) {
      keyPlace = &*found;
    }
  }
  expect(keyPlace != nullptr,
         "the place-key mutation should locate a parameter binding key");
  if (keyPlace != nullptr) {
    const lang::PlaceKey original = *keyPlace->key;
    keyPlace->key->root += 100000;
    const lang::PlaceKey replacement = *keyPlace->key;
    for (lang::MirBlock &block : keyFunction->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.ownership && instruction.ownership->place == original) {
          instruction.ownership->place = replacement;
        }
      }
    }
    expect(lang::verifyMirProgram(forgedPlaceKey).valid(),
           "a self-consistent forged ownership key should remain generic MIR "
           "so the HIR snapshot contract owns rejection");
    if (lang::verifyMirProgram(forgedPlaceKey).valid()) {
      expectEmissionRejected(
          frontend, forgedPlaceKey, compatibility,
          "a scalar binding place key must exactly match its current HIR "
          "symbol and place domain");
    }
  }
}

void testProvenanceAndSnapshotCoherence(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), readFile(fixture));
  expect(frontend.canGenerateCode(),
         "the scalar-CFG coherence fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::OptimizationPipeline pipeline;
  const lang::OptimizationResult compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O0);
  const lang::OptimizedProgram optimized =
      optimize(frontend, lang::OptimizationLevel::O0, compatibility);
  expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
         "the scalar-CFG coherence fixture should begin with verified O0 MIR");
  if (!optimized.valid() || !lang::verifyMirProgram(optimized.mir).valid()) {
    return;
  }

  lang::MirProgram forgedLiteral = optimized.mir;
  lang::MirFunctionInstance *fold =
      findMirFunction(frontend.hir, forgedLiteral, "cfg_fold");
  lang::MirInstruction *sourceLiteral = nullptr;
  if (fold != nullptr) {
    for (lang::MirBlock &block : fold->body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return instruction.operation == lang::MirOperation::Literal &&
                   instruction.literalProvenance.kind ==
                       lang::MirLiteralProvenanceKind::Source;
          });
      if (found != block.instructions.end()) {
        sourceLiteral = &*found;
        break;
      }
    }
  }
  expect(sourceLiteral != nullptr,
         "the literal-provenance mutation should locate a source literal");
  if (sourceLiteral != nullptr) {
    sourceLiteral->literal = lang::Literal{std::uint64_t{99}};
    expect(lang::verifyMirProgram(forgedLiteral).valid(),
           "an in-domain source-literal payload forgery should remain "
           "structurally valid MIR");
    if (lang::verifyMirProgram(forgedLiteral).valid()) {
      expectEmissionRejected(
          frontend, forgedLiteral, compatibility,
          "a source-provenance literal that disagrees with HIR should be "
          "rejected before scalar-CFG emission");
    }
  }

  const lang::OptimizationResult o1Compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O1);
  const lang::OptimizedProgram o1 =
      optimize(frontend, lang::OptimizationLevel::O1, o1Compatibility);
  expect(o1.valid() && lang::verifyMirProgram(o1.mir).valid(),
         "the CFG provenance mutation should begin with verified O1 MIR");
  if (o1.valid() && lang::verifyMirProgram(o1.mir).valid()) {
    lang::MirProgram launderedFold = o1.mir;
    lang::MirFunctionInstance *optimizedFold =
        findMirFunction(frontend.hir, launderedFold, "cfg_fold");
    lang::MirInstruction *rewritten = nullptr;
    if (optimizedFold != nullptr) {
      for (lang::MirBlock &block : optimizedFold->body.blocks) {
        const auto found =
            std::find_if(block.instructions.begin(), block.instructions.end(),
                         [](const lang::MirInstruction &instruction) {
                           return instruction.literalProvenance.kind ==
                                  lang::MirLiteralProvenanceKind::IdentityFold;
                         });
        if (found != block.instructions.end()) {
          rewritten = &*found;
          break;
        }
      }
    }
    expect(rewritten != nullptr,
           "O1 cfg_fold should retain an identity-fold provenance proof");
    if (rewritten != nullptr) {
      rewritten->literalProvenance = {
          .kind = lang::MirLiteralProvenanceKind::Source};
      expect(lang::verifyMirProgram(launderedFold).valid(),
             "an identity fold relabeled as a source literal should remain "
             "structurally valid so CFG provenance coherence owns rejection");
      if (lang::verifyMirProgram(launderedFold).valid()) {
        expectEmissionRejected(
            frontend, launderedFold, compatibility,
            "scalar-cfg-v1 must reject an optimized grouping literal whose "
            "identity-fold proof was laundered as source provenance");
      }
    }
  }

  lang::MirProgram forgedShortCircuit = optimized.mir;
  lang::MirFunctionInstance *shortCircuit =
      findMirFunction(frontend.hir, forgedShortCircuit, "cfg_short");
  lang::MirOperand *syntheticConstant = nullptr;
  if (shortCircuit != nullptr) {
    for (lang::MirBlock &block : shortCircuit->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.operands.size() == 1 &&
            instruction.operands.front().kind ==
                lang::MirOperandKind::Constant) {
          syntheticConstant = &instruction.operands.front();
          break;
        }
      }
      if (syntheticConstant != nullptr) {
        break;
      }
    }
  }
  expect(syntheticConstant != nullptr && syntheticConstant->literal &&
             std::holds_alternative<bool>(*syntheticConstant->literal),
         "the short-circuit mutation should locate its synthesized boolean");
  if (syntheticConstant != nullptr && syntheticConstant->literal &&
      std::holds_alternative<bool>(*syntheticConstant->literal)) {
    syntheticConstant->literal =
        lang::Literal{!std::get<bool>(*syntheticConstant->literal)};
    expect(lang::verifyMirProgram(forgedShortCircuit).valid(),
           "a flipped synthesized short-circuit constant should remain "
           "structurally valid MIR");
    if (lang::verifyMirProgram(forgedShortCircuit).valid()) {
      expectEmissionRejected(
          frontend, forgedShortCircuit, compatibility,
          "a synthesized short-circuit constant that disagrees with its HIR "
          "logical operator should be rejected");
    }
  }

  lang::MirProgram missingInitialization = optimized.mir;
  lang::MirFunctionInstance *missingInitializationFunction =
      findMirFunction(frontend.hir, missingInitialization, "cfg_short");
  lang::MirBlockId merge = 0;
  if (missingInitializationFunction != nullptr) {
    for (const lang::MirPlace &place :
         missingInitializationFunction->body.places) {
      if (place.root != lang::MirPlaceRootKind::Temporary ||
          place.initiallyAvailable) {
        continue;
      }
      for (const lang::MirBlock &block :
           missingInitializationFunction->body.blocks) {
        const bool loadsPlace = std::any_of(
            block.instructions.begin(), block.instructions.end(),
            [&](const lang::MirInstruction &instruction) {
              return instruction.kind == lang::MirInstructionKind::Load &&
                     instruction.operands.size() == 1 &&
                     instruction.operands.front().kind ==
                         lang::MirOperandKind::Copy &&
                     instruction.operands.front().place == place.id;
            });
        if (loadsPlace) {
          merge = block.id;
          break;
        }
      }
      if (merge != 0) {
        break;
      }
    }
  }
  lang::MirBlock *entry =
      missingInitializationFunction == nullptr ||
              missingInitializationFunction->body.entry == 0 ||
              missingInitializationFunction->body.entry >
                  missingInitializationFunction->body.blocks.size()
          ? nullptr
          : &missingInitializationFunction->body
                 .blocks[missingInitializationFunction->body.entry - 1];
  const bool bypassedInitialization =
      entry != nullptr &&
      entry->terminator.kind == lang::MirTerminatorKind::Branch && merge != 0;
  if (bypassedInitialization) {
    entry->terminator.target = merge;
    lang::rebuildMirReachability(missingInitializationFunction->body);
    (void)lang::rebuildMirValueUses(missingInitializationFunction->body);
  }
  const lang::MirVerificationResult initializationVerification =
      lang::verifyMirProgram(missingInitialization);
  expect(bypassedInitialization && !initializationVerification.valid(),
         "the generic MIR verifier should reject a scalar-CFG path that "
         "reaches a temporary load before initialization");

  lang::MirProgram staleFullExpression = optimized.mir;
  lang::MirFunctionInstance *choose =
      findMirFunction(frontend.hir, staleFullExpression, "cfg_choose");
  lang::MirFullExpression *boundary =
      choose == nullptr || choose->body.fullExpressions.empty()
          ? nullptr
          : &choose->body.fullExpressions.front();
  expect(boundary != nullptr,
         "the full-expression mutation should locate a selected boundary");
  if (boundary != nullptr) {
    boundary->statement = 0;
    boundary->constructorInitializer =
        std::numeric_limits<lang::HirStatementId>::max();
    for (lang::MirBlock &block : choose->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.fullExpressionEnd == boundary->id) {
          instruction.hirStatement = 0;
          instruction.hirValue = 0;
        }
      }
    }
    expect(lang::verifyMirProgram(staleFullExpression).valid(),
           "an internally coherent but HIR-stale full-expression table should "
           "remain structurally valid MIR");
    if (lang::verifyMirProgram(staleFullExpression).valid()) {
      expectEmissionRejected(
          frontend, staleFullExpression, compatibility,
          "a selected scalar-CFG body with HIR-stale full-expression facts "
          "should fail closed");
    }
  }

  lang::MirProgram constantReturn = optimized.mir;
  lang::MirFunctionInstance *constantChoose =
      findMirFunction(frontend.hir, constantReturn, "cfg_choose");
  bool changedReturn = false;
  if (constantChoose != nullptr) {
    for (lang::MirBlock &block : constantChoose->body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Return ||
          !block.terminator.value ||
          block.terminator.value->type != lang::SemanticType::Int32) {
        continue;
      }
      block.terminator.value =
          lang::MirOperand{.kind = lang::MirOperandKind::Constant,
                           .literal = lang::Literal{std::uint64_t{99}},
                           .type = lang::SemanticType::Int32};
      changedReturn = true;
      break;
    }
    (void)lang::rebuildMirValueUses(constantChoose->body);
  }
  expect(changedReturn && lang::verifyMirProgram(constantReturn).valid(),
         "the constant-return mutation should remain valid MIR");
  if (changedReturn && lang::verifyMirProgram(constantReturn).valid()) {
    expectEmissionRejected(
        frontend, constantReturn, compatibility,
        "a provenance-free constant return in an independently eligible "
        "scalar-CFG source body must fail closed");
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: gti_mir_backend_scalar_cfg_tests <fixture>\n";
    return 2;
  }
  testSelectedFamily(std::filesystem::path(argv[1]));
  testConcreteMemberSelection();
  testForeignObjectFieldReadStaysCompatibility();
  testPerBodyCallSelection();
  testGenericOwnerMemberStaysCompatibility();
  testIncoherentSwitchRejected(std::filesystem::path(argv[1]));
  testProvenanceAndSnapshotCoherence(std::filesystem::path(argv[1]));
  return failures == 0 ? 0 : 1;
}
