#include "compiler_test_fixture.h"

namespace {

void checkIndependentDependencies() {
  const auto frontend = lang::Frontend().analyze("composed-borrows.gti", R"(
struct View {
  int& first;
  int& second;
  View(int& a, int& b) : first(a), second(b) {}
  int sum() { return first + second; }
};
View relay(View value) { return value; }
View factory(int& first, int& second) { return View(first, second); }
View same_owner(int& first, int& unused) { return View(first, first); }
int main() {
  int a = 20; int b = 22;
  View view{a, b};
  auto copy = view;
  auto repeated = same_owner(a, b);
  return copy.sum() + repeated.sum() - 82;
}
)");
  expect(frontend.canGenerateCode(),
         "user-defined two-owner carrier should pass complete MIR lowering");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "composed owner dependencies should independently verify");
  const auto semantic = std::find_if(
      frontend.semantics.loans().begin(), frontend.semantics.loans().end(),
      [](const lang::SemanticLoanInfo &loan) {
        return !loan.entry && !loan.additionalDependencies.empty();
      });
  expect(semantic != frontend.semantics.loans().end(),
         "semantic loan must retain the second independent place");
  if (semantic != frontend.semantics.loans().end()) {
    expect(semantic->place.root !=
               semantic->additionalDependencies.front().place.root,
           "independent owners must not collapse into a fabricated root");
    expect(semantic->carriers.size() == 2,
           "copy construction must retain both aliases of the dependency set");
  }
  bool inspected = false;
  bool inspectedPaths = false;
  bool inspectedEffects = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (!function.returnBorrowParameters.empty()) {
      inspectedEffects = true;
      expect(function.effectSummary.argumentsEscape ==
                 std::vector<bool>({true, true}),
             "effect summaries must retain both zero-based parameter escapes");
    }
    for (const auto &loan : function.body.loans) {
      if (loan.entry && !loan.additionalDesignatedFields().empty()) {
        inspectedPaths = true;
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        instance->body.loans[loan.id - 1].additionalDependencies.clear();
        expect(
            !lang::verifyMirProgram(malformed).valid(),
            "MIR must reject dropping an opaque parameter's second field path");
        malformed = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        instance->body.loans[loan.id - 1]
            .additionalDependencies.front()
            .access = lang::AccessMode::Mutable;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "MIR must reject upgrading a shared stored field's permission");
      }
      if (loan.additionalSources().empty()) {
        continue;
      }
      inspected = true;
      auto malformed = frontend.mir;
      auto *instance = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      instance->body.loans[loan.id - 1].additionalDependencies.clear();
      expect(!lang::verifyMirProgram(malformed).valid(),
             "MIR must reject erasing an independent retained owner");
      malformed = frontend.mir;
      instance = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      instance->body.loans[loan.id - 1].additionalDependencies.front().source =
          0;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "MIR must reject an unresolved independent owner place");
      if (!loan.entry && !loan.designatedFields.empty()) {
        malformed = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        instance->body.loans[loan.id - 1]
            .additionalDependencies.front()
            .designatedFields.clear();
        expect(!lang::verifyMirProgram(malformed).valid(),
               "MIR must reject losing a local field-to-owner identity");
        malformed = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        instance->body.loans[loan.id - 1]
            .additionalDependencies.front()
            .designatedFields = loan.designatedFields;
        expect(
            !lang::verifyMirProgram(malformed).valid(),
            "MIR must reject relabeling the second owner as the first field");
      }
      break;
    }
  }
  expect(inspected, "MIR must retain an explicit multi-owner dependency set");
  expect(inspectedPaths, "MIR must retain all opaque parameter field paths");
  expect(inspectedEffects, "factory must publish its complete escape summary");
}

void checkMixedDependencies() {
  const auto frontend = lang::Frontend().analyze("mixed-services.gti", R"(
struct Services {
  int& database;
  mut int& renderer;
  int& logger;
  mut int& cache;
  int& clock;
  Services(int& db, mut int& render, int& log, mut int& entries, int& time)
      : database(db), renderer(render), logger(log), cache(entries), clock(time) {}
  int update() mut {
    renderer = database + logger;
    cache = renderer + clock;
    return cache;
  }
};
int main() {
  int database = 10;
  mut int renderer = 0;
  int logger = 20;
  mut int cache = 0;
  int clock = 12;
  mut Services services{database, renderer, logger, cache, clock};
  return services.update() - 42;
}
)");
  expect(frontend.canGenerateCode(),
         "five mixed borrowed fields should retain independent permissions");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "mixed stored dependencies must pass independent MIR verification");
  const auto loan = std::find_if(
      frontend.semantics.loans().begin(), frontend.semantics.loans().end(),
      [](const lang::SemanticLoanInfo &candidate) {
        return !candidate.entry && candidate.additionalDependencies.size() == 4;
      });
  expect(loan != frontend.semantics.loans().end(),
         "all five independent dependencies must survive semantic analysis");
  if (loan != frontend.semantics.loans().end()) {
    expect(loan->access == lang::AccessMode::ReadOnly &&
               loan->additionalDependencies[0].access ==
                   lang::AccessMode::Mutable &&
               loan->additionalDependencies[1].access ==
                   lang::AccessMode::ReadOnly &&
               loan->additionalDependencies[2].access ==
                   lang::AccessMode::Mutable &&
               loan->additionalDependencies[3].access ==
                   lang::AccessMode::ReadOnly,
           "a mixed carrier must not flatten its fields to one access mode");
  }
}

void checkMixedAccessRules() {
  const std::string declarations = R"(
struct Mixed {
  int& read;
  mut int& write;
  mut int& other;
  Mixed(int& a, mut int& b, mut int& c) : read(a), write(b), other(c) {}
  int update() mut { write = read; other = write + 1; return other; }
};
struct MutableFirst {
  mut int& write;
  int& read;
  mut int& other;
  MutableFirst(mut int& a, int& b, mut int& c) : write(a), read(b), other(c) {}
  int update() mut { write = read; other = write + 1; return other; }
};
struct State { mut int a = 1; mut int b = 2; mut int c = 3; };
int main() {
  mut int a = 1; mut int b = 2; mut int c = 3;
)";
  const auto check = [&](std::string_view body, bool valid,
                         std::string_view description) {
    const auto frontend = lang::Frontend().analyze(
        "mixed-access.gti", declarations + std::string(body) + "\n}");
    const bool semanticError =
        std::any_of(frontend.diagnostics.begin(), frontend.diagnostics.end(),
                    [](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code.starts_with("GTI-S");
                    });
    expect(valid ? frontend.canGenerateCode() : semanticError,
           std::string(description));
    if ((valid && !frontend.canGenerateCode()) || (!valid && !semanticError))
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << description << ": " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
  };
  check("mut Mixed v{a, b, c}; int read = a; return v.update() + read;", true,
        "shared owner stays readable beside exclusive dependencies");
  check("int result = Mixed(a, b, c).update(); b = 7; c = 8; return result;",
        true,
        "an immediate temporary method call retains all mixed dependencies");
  check("mut Mixed v{a, b, c}; int read = c; return v.update() + read;", false,
        "later exclusive dependency prevents direct owner reads");
  check("mut Mixed v{a, b, c}; c = 9; return v.update();", false,
        "later exclusive dependency prevents competing owner writes");
  check("mut Mixed v{a, a, c}; return v.update();", false,
        "overlapping shared and exclusive constructor inputs are rejected");
  check("mut Mixed v{a, b, b}; return v.update();", false,
        "overlapping exclusive constructor inputs are rejected");
  check("mut Mixed v{a, b, c}; auto copied = v; return copied.update();", false,
        "a shared first field cannot make an exclusive carrier copyable");
  check("mut Mixed v{a, b, c}; int result = v.update(); b = 10; c = 20; return "
        "result;",
        true, "all independent owners reactivate after the carrier's last use");
  check("mut MutableFirst v{b, a, c}; int read = a; return v.update() + read;",
        true, "mutable first field does not make a shared sibling exclusive");
  check("mut State state{}; mut Mixed v{state.a, state.b, state.c}; return "
        "v.update();",
        true, "disjoint fields of one owner can supply mixed dependencies");
  check("mut Mixed v{a, b, c}; if (a > 0) { [[discard]] v.update(); } "
        "else { [[discard]] v.update(); } b = 7; c = 8; return b + c;",
        true, "branch-joined last uses reactivate all exclusive owners");
  check("for (mut int i = 0; i < 2; i++) { mut Mixed v{a, b, c}; "
        "[[discard]] v.update(); } b = 7; c = 8; return b + c;",
        true, "loop-local mixed loans end before the next iteration");
  check("mut Mixed v{a, b, c}; if (a > 0) { c = 7; } "
        "return v.update();",
        false, "a later carrier use retains every dependency across branches");
  check("for (mut int i = 0; i < 2; i++) { mut Mixed v{a, b, c}; "
        "c = 7; [[discard]] v.update(); } return 0;",
        false,
        "loop-local borrowed owners cannot be mutated before the final use");
  check("mut Mixed v{a, b, c}; [[discard]] v.update(); "
        "for (mut int i = 0; i < 2; i++) { mut Mixed next{a, b, c}; "
        "[[discard]] next.update(); } return b + c;",
        true, "entering a later loop does not revive ended outer dependencies");
  check("mut Mixed v{a, b, c}; "
        "for (mut int i = 0; i < 2; i++) { c = 7; [[discard]] v.update(); } "
        "return 0;",
        false, "a dependency used inside a loop survives its backedge");
  check("mut Mixed v{a, b, c}; [[discard]] v.update(); "
        "for (mut int i = 0; i < 2; i++) { c = 7; } return v.update();",
        false, "a dependency used after the loop cannot end before loop entry");
}

void checkFieldDependencyIdentity() {
  const auto shared = lang::Frontend().analyze("repeated-shared-fields.gti", R"(
struct Repeated {
  int& first;
  int& second;
  int& third;
  Repeated(int& value) : first(value), second(value), third(value) {}
  int sum() { return first + second + third; }
};
int main() { int value = 14; Repeated view{value}; return view.sum() - 42; }
)");
  expect(
      shared.canGenerateCode(),
      "one shared constructor parameter may initialize distinct stored fields");
  if (!shared.canGenerateCode())
    for (const auto &diagnostic : shared.diagnostics)
      std::cerr << "repeated shared: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
  const auto loan = std::find_if(
      shared.semantics.loans().begin(), shared.semantics.loans().end(),
      [](const auto &candidate) {
        return !candidate.entry && candidate.additionalDependencies.size() == 2;
      });
  expect(loan != shared.semantics.loans().end(),
         "repeated shared origins must retain all three field identities");
  if (loan != shared.semantics.loans().end()) {
    expect(loan->designatedFields.size() == 1 &&
               loan->additionalDependencies[0].designatedFields.size() == 1 &&
               loan->additionalDependencies[1].designatedFields.size() == 1 &&
               loan->designatedFields !=
                   loan->additionalDependencies[0].designatedFields &&
               loan->additionalDependencies[0].designatedFields !=
                   loan->additionalDependencies[1].designatedFields &&
               loan->place == loan->additionalDependencies[0].place &&
               loan->place == loan->additionalDependencies[1].place,
           "field identity must not be deduplicated by owner identity");
  }

  const auto exclusive = lang::Frontend().analyze("duplicate-exclusive.gti", R"(
struct Duplicate {
  mut int& first;
  mut int& second;
  Duplicate(mut int& value) : first(value), second(value) {}
};
int main() { mut int value = 0; Duplicate view{value}; return 0; }
)");
  expect(std::any_of(exclusive.diagnostics.begin(), exclusive.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code == "GTI-S2045" &&
                              diagnostic.message.find(
                                  "duplicate an exclusive") !=
                                  std::string::npos;
                     }),
         "duplicate exclusive fields must be rejected at the source contract");
  expect(std::none_of(exclusive.diagnostics.begin(),
                      exclusive.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code == "GTI-B0001";
                      }),
         "invalid exclusive field composition must never reach MIR as an ICE");
}

void checkNestedMixedTransfer() {
  const auto differingPermissions = lang::Frontend().analyze(
      "nested-field-permissions.gti", R"(
#include <std/utility>
struct Exclusive {
  mut int& value;
  Exclusive(mut int& source) : value(source) {}
};
struct Combined {
  int& input;
  mut Exclusive output;
  Combined(int& source, Exclusive target)
      : input(source), output(std::move(target)) {}
  int run() mut { output.value = input; return output.value; }
};
int main() {
  int a = 42; mut int b = 0;
  Exclusive item{b};
  mut Combined group{a, std::move(item)};
  return group.run() - 42;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(differingPermissions.canGenerateCode(),
         "nested publication uses the nested field's permission, not the "
         "outer carrier's first dependency");
  if (differingPermissions.canGenerateCode()) {
    expect(!gti_test::emitCppText(differingPermissions).empty(),
           "backend resolves nested receiver reference projections");
    bool inspected = false;
    for (const auto &function : differingPermissions.mir.functionInstances()) {
      for (const auto &place : function.body.places) {
        if (place.root != lang::MirPlaceRootKind::This ||
            place.projections.size() != 3 ||
            place.projections[0].kind != lang::MirProjectionKind::Field ||
            place.projections[1].kind != lang::MirProjectionKind::Field ||
            place.projections[2].kind != lang::MirProjectionKind::Dereference)
          continue;
        inspected = true;
        auto changed = differingPermissions.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        instance->body.places[place.id - 1].projections.pop_back();
        bool rejected = false;
        try {
          static_cast<void>(
              gti_test::emitCpp(differingPermissions, changed, changed));
        } catch (const std::logic_error &) {
          rejected = true;
        }
        expect(rejected,
               "backend cannot erase a nested reference dereference step");
      }
    }
    expect(inspected, "nested receiver regression exercises its exact path");
  }
  const std::string declarations = R"(
#include <std/utility>
struct Mixed {
  int& read;
  mut int& write;
  Mixed(int& a, mut int& b) : read(a), write(b) {}
  int update() mut { write = read; return write; }
};
struct Envelope<T> {
  mut T payload;
  Envelope(T value) : payload(std::move(value)) {}
};
int main() {
  int a = 42; mut int b = 0;
  Mixed view{a, b};
  mut Envelope<Mixed> outer{std::move(view)};
)";
  const auto analyze = [&](std::string_view body) {
    return lang::Frontend().analyze(
        "nested-mixed-transfer.gti", declarations + std::string(body) + "\n}",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  };
  auto valid = analyze("return outer.payload.update() - 42;");
  expect(valid.canGenerateCode(),
         "a generic nominal wrapper can retain every moved mixed dependency");
  if (!valid.canGenerateCode())
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << "nested mixed: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
  for (const auto &constructor : valid.mir.constructorInstances()) {
    for (const auto &loan : constructor.body.loans) {
      if (!loan.escapes || loan.additionalDependencies.empty())
        continue;
      auto changed = valid.mir;
      auto *instance = const_cast<lang::MirConstructorInstance *>(
          changed.findConstructorInstance(constructor.id));
      instance->body.loans[loan.id - 1].additionalDependencies.clear();
      expect(!lang::verifyMirProgram(changed).valid(),
             "nested field publication cannot discard a secondary dependency");
    }
  }
  const auto invalid = analyze("b = 9; return outer.payload.update();");
  expect(std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code == "GTI-S2017";
                     }),
         "a moved nested carrier still excludes access to its later owner");
}

void checkProjectedMixedReborrows() {
  const std::string declarations = R"(
struct Mixed {
  int& read;
  mut int& write;
  mut int& other;
  Mixed(int& a, mut int& b, mut int& c) : read(a), write(b), other(c) {}
};
int main() {
  mut int a = 42; mut int b = 0; mut int c = 3;
  mut Mixed view{a, b, c};
)";
  const auto analyze = [&](std::string_view body) {
    return lang::Frontend().analyze("projected-mixed-reborrow.gti",
                                    declarations + std::string(body) + "\n}");
  };
  for (const auto body :
       {"mut int& selected = view.write; selected = 42; return b - 42;",
        "int& selected = view.read; return selected - 42;",
        "int& first = view.read; int& second = view.read; "
        "return first + second + view.read - 126;",
        "mut int& selected = view.write; view.other = 3; "
        "selected = view.read; return b + c - 45;",
        "mut int& first = view.write; mut int& second = view.other; "
        "first = 42; second = 3; return b + c - 45;"}) {
    const auto valid = analyze(body);
    expect(valid.canGenerateCode(),
           "a projected dependency carries its own access and owner");
    if (!valid.canGenerateCode())
      for (const auto &diagnostic : valid.diagnostics)
        std::cerr << "projected mixed: " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
    if (valid.canGenerateCode())
      for (const auto &function : valid.mir.functionInstances())
        for (const auto &loan : function.body.loans) {
          if (loan.parent == 0 || loan.access != lang::AccessMode::Mutable)
            continue;
          auto changed = valid.mir;
          auto *instance = const_cast<lang::MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          auto &parent = instance->body.loans[loan.parent - 1];
          for (auto &dependency : parent.additionalDependencies)
            dependency.access = lang::AccessMode::ReadOnly;
          expect(!lang::verifyMirProgram(changed).valid(),
                 "a mutable child cannot obtain authority from shared "
                 "dependencies");
        }
  }
  for (const auto body :
       {"mut int& selected = view.write; b = 9; return selected;",
        "mut int& selected = view.write; view.write = 9; return selected;",
        "mut int& first = view.write; mut int& second = view.write; "
        "first = 1; return second;",
        "mut int& selected = view.read; selected = 1; return selected;"}) {
    const auto invalid = analyze(body);
    expect(
        !invalid.canGenerateCode() &&
            std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code == "GTI-S2017";
                        }),
        "a projected borrow rejects conflicting access or permission upgrades");
  }
}

void checkMixedReturnOrigins() {
  const auto frontend = lang::Frontend().analyze("mixed-return-origins.gti", R"(
struct Services {
  int& first;
  mut int& second;
  mut int& third;
  Services(int& a, mut int& b, mut int& c) : first(a), second(b), third(c) {}
  int& read() { return this.first; }
  mut int& write() mut { return this.second; }
};
Services factory(mut int& last, int& first, mut int& middle) {
  return Services(first, middle, last);
}

int main() {
  int a = 40; mut int b = 0; mut int c = 2;
  mut auto services = factory(c, a, b);
  mut int& target = services.write();
  target = 40;
  int first = services.read();
  return first + b + c - 82;
}
)");
  expect(frontend.canGenerateCode(), "mixed factories and accessors preserve "
                                     "their exact returned field origins");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << "mixed returns: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
  bool forgedReturn = false;
  bool erasedConstruction = false;
  if (frontend.canGenerateCode())
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          if (!instruction.constructorTarget ||
              instruction.borrowFields.empty())
            continue;
          auto malformed = frontend.mir;
          auto *instance = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &changedBlock : instance->body.blocks)
            for (auto &changedInstruction : changedBlock.instructions)
              if (changedInstruction.id == instruction.id)
                changedInstruction.borrowFields.clear();
          erasedConstruction = true;
          expect(
              !lang::verifyMirProgram(malformed).valid(),
              "constructor publication cannot omit its borrowed field mapping");
        }
      if (function.owner || function.returnBorrowFields.size() != 3)
        continue;
      forgedReturn = true;
      auto changed = frontend.mir;
      auto *factory = const_cast<lang::MirFunctionInstance *>(
          changed.findFunctionInstance(function.id));
      factory->returnBorrowFields[1].parameter =
          factory->returnBorrowFields[2].parameter;
      for (const auto &caller : changed.functionInstances()) {
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(caller.id));
        for (auto &block : instance->body.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.functionTarget == function.id)
              instruction.borrowFields = factory->returnBorrowFields;
      }
      expect(!lang::verifyMirProgram(changed).valid(),
             "matching caller/callee metadata cannot forge the callee's "
             "returned field owner");
      bool forgedNonCall = false;
      auto malformed = frontend.mir;
      auto *instance = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      for (auto &block : instance->body.blocks)
        for (auto &instruction : block.instructions) {
          if (forgedNonCall || instruction.tombstone ||
              instruction.kind == lang::MirInstructionKind::Call ||
              instruction.kind == lang::MirInstructionKind::Construct)
            continue;
          instruction.borrowFields = function.returnBorrowFields;
          forgedNonCall = true;
        }
      expect(forgedNonCall && !lang::verifyMirProgram(malformed).valid(),
             "non-call instructions cannot smuggle returned-field contracts");
    }
  expect(forgedReturn, "return-contract forgery exercised a mapped factory");
  expect(erasedConstruction,
         "constructor-contract mutation exercised a publication");
}

void checkMixedReturnComposition() {
  const auto frontend =
      lang::Frontend().analyze("mixed-return-composition.gti", R"(
struct Services {
  int& first;
  mut int& second;
  mut int& third;
  Services(int& a, mut int& b, mut int& c) : first(a), second(b), third(c) {}
  int& read() { return this.first; }
  mut int& write() mut { return this.second; }
};
Services relay(Services value) { return std::move(value); }
int inspect(Services& value) { return value.read(); }
int main() {
  int a = 40; mut int b = 0; mut int c = 2;
  Services original{a, b, c};
  mut Services moved = relay(std::move(original));
  mut int& target = moved.write();
  target = 40;
  int first = inspect(moved);
  return first + b + c - 82;
}
)",
                               {standardLibraryPrelude()});
  expect(frontend.canGenerateCode(),
         "mixed borrowed aggregates compose through value relay and reference "
         "parameters");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << "mixed composition: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
}

void checkGenericMixedReturnComposition() {
  const auto frontend = lang::Frontend().analyze("generic-mixed-return.gti", R"(
struct Services {
  int& first;
  mut int& second;
  mut int& third;
  Services(int& a, mut int& b, mut int& c) : first(a), second(b), third(c) {}
};
T relay<T>(T value) { return std::move(value); }
struct Wrapper<T> {
  T& first;
  mut T& second;
  mut T& third;
  Wrapper(T& a, mut T& b, mut T& c) : first(a), second(b), third(c) {}
  T& read() { return this.first; }
  mut T& write() mut { return this.second; }
};
int main() {
  int a = 40; mut int b = 0; mut int c = 2;
  Services original{a, b, c};
  mut Services moved = relay<Services>(std::move(original));
  moved.second = moved.first;
  mut Wrapper<int> wrapped{a, b, c};
  int source = wrapped.read();
  mut int& destination = wrapped.write();
  destination = source;
  return b + c - 42;
}
)",
                                                 {standardLibraryPrelude()});
  expect(frontend.canGenerateCode(), "concrete generic relay retains each "
                                     "borrowed field's origin and permission");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << "generic mixed return: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
}

void checkCompactInputComposition() {
  const auto unsupported =
      lang::Frontend().analyze("single-input-reshape.gti", R"(
struct Reading {
  int& value;
  Reading(int& source) : value(source) {}
};
struct Readings {
  Reading first;
  Reading second;
  Readings(Reading left, Reading right) : first(left), second(right) {}
};
Readings repeat(Reading input) { return Readings(input, input); }
)");
  expect(!unsupported.canGenerateCode() &&
             std::any_of(unsupported.diagnostics.begin(),
                         unsupported.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2085";
                         }),
         "single-input by-value return remains bounded to its exact carrier "
         "dependency");
  const auto frontend =
      lang::Frontend().analyze("compact-input-composition.gti", R"(
struct Reading {
  int& value;
  Reading(int& source) : value(source) {}
};
struct Readings {
  Reading first;
  Reading second;
  Readings(Reading left, Reading right) : first(left), second(right) {}
};
Readings combine(Reading left, Reading right) { return Readings(left, right); }
Readings reverse(Reading left, Reading right) { return Readings(right, left); }
int main() {
  int a = 17; int b = 25;
  Reading left{a}; Reading right{b};
  auto combined = combine(left, right);
  auto reversed = reverse(left, right);
  auto repeated = combine(left, left);
  return combined.first.value + reversed.first.value + repeated.second.value - 59;
}
)");
  expect(frontend.canGenerateCode(),
         "composing compact inputs preserves each exact external identity");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << "compact composition: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
    return;
  }
  bool inspected = false;
  bool mutatedIndependentSources = false;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (!instruction.constructorTarget || !instruction.loan ||
            instruction.borrowFields.size() != 2)
          continue;
        const auto *result = function.body.findLoan(*instruction.loan);
        if (result == nullptr || result->additionalDependencies.size() != 1)
          continue;
        const auto entry =
            std::find_if(function.body.loans.begin(), function.body.loans.end(),
                         [&](const auto &loan) {
                           return loan.entry &&
                                  loan.kind == lang::MirLoanKind::Stored &&
                                  loan.source == result->source;
                         });
        if (entry == function.body.loans.end())
          continue;
        inspected = true;
        expect(
            function.body.findPlace(result->source)->projections.empty(),
            "compact input retains its opaque entry rather than shell fields");
        if (result->source == result->additionalDependencies.front().source)
          continue;
        mutatedIndependentSources = true;
        auto changed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        instance->body.loans[result->id - 1].source =
            result->additionalDependencies.front().source;
        expect(!lang::verifyMirProgram(changed).valid(),
               "composed result cannot substitute the other compact input");
        changed = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        instance->body.loans[result->id - 1]
            .additionalDependencies.front()
            .source = result->source;
        expect(!lang::verifyMirProgram(changed).valid(),
               "composed result cannot collapse its second independent input");
        changed = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        for (auto &changedBlock : instance->body.blocks)
          for (auto &changedInstruction : changedBlock.instructions)
            if (changedInstruction.id == instruction.id)
              std::swap(changedInstruction.borrowFields[0].parameter,
                        changedInstruction.borrowFields[1].parameter);
        expect(!lang::verifyMirProgram(changed).valid(),
               "composed result cannot forge reordered constructor input "
               "mappings");
        changed = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        for (auto &changedBlock : instance->body.blocks)
          for (auto &changedInstruction : changedBlock.instructions)
            if (changedInstruction.id == instruction.id)
              std::swap(changedInstruction.borrowFields[0].resultFields,
                        changedInstruction.borrowFields[1].resultFields);
        expect(!lang::verifyMirProgram(changed).valid(),
               "composed result cannot relabel destination field identities");
      }
  expect(inspected,
         "compact input composition exercised an entry-backed result");
  expect(mutatedIndependentSources,
         "compact input mutations exercised distinct external owners");
}

void checkMixedOperatorOrigins() {
  const auto analyze = [](std::string_view body) {
    return lang::Frontend().analyze("mixed-operator.gti", R"(
struct Services {
  int& first;
  mut int& second;
  mut int& third;
  Services(int& a, mut int& b, mut int& c) : first(a), second(b), third(c) {}
  mut int& operator*() mut { return this.second; }
};
int main() {
  int a = 40; mut int b = 0; mut int c = 2;
  mut Services value{a, b, c};
)" + std::string(body) + "\n}");
  };
  const auto valid =
      analyze("mut int& selected = *value; selected = 40; return b + c - 42;");
  expect(
      valid.canGenerateCode(),
      "operator return preserves the selected field, not the first dependency");
  if (!valid.canGenerateCode())
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << "mixed operator: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
  const auto invalid =
      analyze("mut int& selected = *value; b = 9; return selected;");
  expect(!invalid.canGenerateCode() &&
             std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2017";
                         }),
         "operator-derived borrow keeps its exact owner protected");
}

constexpr NamedTest kTests[] = {
    {"independent_dependencies", checkIndependentDependencies},
    {"mixed_dependencies", checkMixedDependencies},
    {"mixed_access_rules", checkMixedAccessRules},
    {"field_dependency_identity", checkFieldDependencyIdentity},
    {"nested_mixed_transfer", checkNestedMixedTransfer},
    {"projected_mixed_reborrows", checkProjectedMixedReborrows},
    {"mixed_return_origins", checkMixedReturnOrigins},
    {"mixed_return_composition", checkMixedReturnComposition},
    {"generic_mixed_return_composition", checkGenericMixedReturnComposition},
    {"compact_input_composition", checkCompactInputComposition},
    {"mixed_operator_origins", checkMixedOperatorOrigins},
};

} // namespace

int main(int argc, char **argv) {
  lang::installCrashHandlers("gti_borrowing_composition_tests");
  return runNamedTests(argc, argv, kTests, "borrowing composition");
}
