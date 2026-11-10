#include "cpp_mir_body_emitter_test_fixture.h"

// Frozen C++ backend: whole-program C++ MIR body emitter admission and C++
// emission of fresh prvalue direct initialization under both C++ standards.
// The backend-neutral contract lives in prvalue_direct_initialization_tests.

using namespace gti_test::cpp_mir_body_emitter_fixture;

int main() {
  const auto frontend = analyzeWithStandardLibrary("fresh-results.gti", R"(
class Point {
public:
  int value;
  Point(int value) : value(value) {}
};
Point make() { return Point(4); }
Point relay() { return Point(Point((make()))); }
Point adjust(Point& source) { return Point(source.value + 1); }
int main() {
  Point a = Point(make());
  Point b{Point((relay()))};
  Point existing{7};
  Point copied = Point(Point(existing));
  Point moved = Point(Point((std::move(existing))));
  Point observed = adjust(Point(make()));
  return a.value + b.value + copied.value + moved.value == 22 &&
      observed.value == 5 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(), "fresh wrappers compose through HIR/MIR");
  if (!frontend.canGenerateCode())
    return 1;
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, standard));
    expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
           "the complete production MIR vocabulary admits the original ICE");
    expect(!gti_test::emitCpp(frontend, standard).contents.empty(),
           "fresh results use production emission, not an AST fallback");
  }
  return failures == 0 ? 0 : 1;
}
