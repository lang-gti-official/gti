#include "cpp_mir_body_emitter_test_fixture.h"
#include "gti/language_queries.h"

using namespace gti_test::cpp_mir_body_emitter_fixture;

void checkFreshResults() {
  const auto frontend = analyzeWithStandardLibrary("fresh-results.gti", R"(
class Point {
public:
  int value;
  Point(int value) : value(value) {}
};
Point make() { return Point(4); }
Point relay() { return Point(Point((make()))); }
int main() {
  Point a = Point(make());
  Point b{Point((relay()))};
  Point existing{7};
  Point copied = Point(Point(existing));
  Point moved = Point(Point((std::move(existing))));
  return a.value + b.value + copied.value + moved.value == 22 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(), "fresh wrappers compose through HIR/MIR");
  if (!frontend.canGenerateCode())
    return;
  expect(lang::verifyHirProgramPlans(frontend.semantics, frontend.hir).valid(),
         "fresh-result source aliases retain an exact semantic HIR witness");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "the shared producer retains verified MIR ownership/cleanup");
  std::size_t aliases = 0;
  std::size_t copies = 0;
  std::size_t moves = 0;
  for (const auto &function : frontend.hir.functionInstances())
    aliases += function.body.directInitializations.size();
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        copies += instruction.kind == lang::MirInstructionKind::Construct &&
                  instruction.constructorKind == lang::ConstructorKind::Copy;
        moves += instruction.kind == lang::MirInstructionKind::Construct &&
                 instruction.constructorKind == lang::ConstructorKind::Move;
      }
  expect(aliases == 10, "typed fresh wrappers and parentheses share values");
  expect(copies == 1 && moves == 1,
         "existing-object copy and move remain real lifecycle operations");
  std::size_t selectedFresh = 0;
  for (const auto &occurrence : frontend.semantics.database().occurrences(
           frontend.sourceGraph.entryUnit())) {
    if (!occurrence.selectedConstruction ||
        !occurrence.selectedConstruction->directInitialization)
      continue;
    const auto hover = lang::LanguageQueries().hover(
        frontend, occurrence.sourceUnit, occurrence.span.start);
    expect(hover &&
               std::any_of(hover->notes.begin(), hover->notes.end(),
                           [](const auto &note) {
                             return note.find("no additional copy, move") !=
                                    std::string::npos;
                           }),
           "compiler-owned hover identifies direct initialization, not a move");
    ++selectedFresh;
  }
  expect(selectedFresh == 6,
         "typed fresh call sites retain selected occurrences");
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, standard));
    expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
           "the complete production MIR vocabulary admits the original ICE");
    expect(!gti_test::emitCpp(frontend, standard).contents.empty(),
           "fresh results use production emission, not an AST fallback");
  }

  auto corrupted = frontend.hir;
  for (const auto &function : corrupted.functionInstances()) {
    if (function.body.directInitializations.empty())
      continue;
    auto *body = const_cast<lang::HirBody *>(&function.body);
    body->directInitializations.front().type = lang::SemanticType::Int32;
    expect(!lang::verifyHirProgramPlans(frontend.semantics, corrupted).valid(),
           "a fresh-result witness cannot change its exact class type");
    break;
  }
  corrupted = frontend.hir;
  for (const auto &function : corrupted.functionInstances()) {
    if (function.body.directInitializations.empty())
      continue;
    auto *body = const_cast<lang::HirBody *>(&function.body);
    body->directInitializations.front().producer = nullptr;
    expect(!lang::verifyHirProgramPlans(frontend.semantics, corrupted).valid(),
           "a fresh-result witness cannot lose its source producer");
    break;
  }
}

void checkImmovableAndBorrowedResults() {
  const auto fixed = analyzeWithStandardLibrary("fixed-results.gti", R"(
class Fixed {
public:
  int value;
  Fixed(int value) : value(value) {}
  Fixed(Fixed&) = delete;
  Fixed(Fixed&&) = delete;
};
Fixed make() { return Fixed(4); }
Fixed relay() { return Fixed(make()); }
int main() {
  auto factory = []() -> Fixed { return Fixed(7); };
  Fixed result = Fixed((relay()));
  Fixed from_lambda = Fixed(factory());
  return result.value + from_lambda.value == 11 ? 0 : 1;
}
)");
  expect(fixed.canGenerateCode(),
         "initial creation and direct publication do not require movability");
  if (fixed.canGenerateCode())
    expect(!gti_test::emitCpp(fixed).contents.empty(),
           "an immovable factory result has a production representation");
  const auto borrow = analyzeWithStandardLibrary("borrowed-results.gti", R"(
class Lens {
  int& value;
public:
  Lens(int& value) : value(value) {}
  int read() { return this.value; }
};
Lens view(int& value) { return Lens(value); }
int main() {
  int owner = 4;
  Lens result = Lens(view(owner));
  return result.read() - 4;
}
)");
  expect(borrow.canGenerateCode(),
         "direct initialization retains a carrier's original external owner");

  for (
      const auto &source : {
          R"(class Fixed { public: Fixed() {} Fixed(Fixed&) = delete;
               Fixed(Fixed&&) = delete; };
               int main() { Fixed source{};
                 Fixed result = Fixed(std::move(source)); return 0; })",
          R"(class Lens { int& value; public: Lens(int& value) : value(value) {} };
               Lens bad() { int local = 4; return Lens(Lens(local)); }
               int main() { return 0; })"}) {
    const auto negative = lang::Frontend().analyze(
        "invalid-fresh-result.gti", source,
        {std::filesystem::path(__FILE__).parent_path().parent_path() /
         "stdlib/prelude.gti"});
    expect(!negative.semanticValid &&
               std::any_of(negative.diagnostics.begin(),
                           negative.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2018" ||
                                    diagnostic.code == "GTI-S2045";
                           }),
           "fresh initialization does not bypass a deleted real move or escape "
           "loan");
  }
}

void checkConcreteAndConstantResults() {
  const auto frontend = analyzeWithStandardLibrary("concrete-results.gti", R"(
class Box<T> {
public:
  T value;
  Box(T value) : value(value) {}
  Box(Box<T>&) = delete;
  Box(Box<T>&&) = delete;
};
Box<T> make<T>(T value) { return Box<T>(value); }
Box<T> relay<T>(T value) { return Box<T>((make<T>(value))); }
class Number {
public:
  int value;
  constexpr Number(int value) : value(value) {}
};
constexpr Number number() { return Number(Number(4)); }
constexpr Number constant = Number(number());
static_assert(constant.value == 4);
int main() {
  Box<int> result = Box<int>(relay<int>(7));
  return result.value == 7 && Number(number()).value == 4 ? 0 : 1;
}
)");
  expect(
      frontend.canGenerateCode(),
      "concrete generic reanalysis and constexpr share the fresh-result rule");
  if (frontend.canGenerateCode())
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "constant and concrete results retain production MIR execution");
}

int main() {
  checkFreshResults();
  checkImmovableAndBorrowedResults();
  checkConcreteAndConstantResults();
  return failures == 0 ? 0 : 1;
}
