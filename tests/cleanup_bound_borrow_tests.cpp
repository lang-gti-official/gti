#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <utility>

namespace {
int failures = 0;
void expect(bool condition, const std::string &name) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}
const std::string prefix = R"gti(
#include <std/utility>
mut int observed = 0;
struct Resource {
  mut int value = 7;
  void reset() mut { this.value = 9; }
};
class Guard {
  Resource& resource;
public:
  Guard(Resource& resource) : resource(resource) {}
  ~Guard() { observed += this.resource.value; }
};
class Box<T> {
  T item;
public:
  Box(T item) : item(std::move(item)) {}
};
)gti";
lang::FrontendResult analyze(const std::string &source) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  return lang::Frontend().analyze(
      root / "cleanup-bound-test.gti", prefix + source,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
}
void show(const lang::FrontendResult &result) {
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  if (std::getenv("GTI_TEST_DUMP_MIR") != nullptr) {
    std::set<std::pair<lang::MirBodyKind, std::size_t>> printed;
    for (const auto &error : lang::verifyMirProgram(result.mir).errors) {
      std::cerr << "MIR body " << static_cast<int>(error.bodyKind) << ':'
                << error.owner << " instruction " << error.instruction << ": "
                << error.message << '\n';
      if (!printed.emplace(error.bodyKind, error.owner).second)
        continue;
      if (error.bodyKind == lang::MirBodyKind::Function) {
        if (const auto *function = result.hir.findFunctionInstance(error.owner);
            function != nullptr && function->source != nullptr)
          std::cerr << "function " << error.owner << " "
                    << function->source->name().lexeme << '\n';
        if (const auto *function = result.mir.findFunctionInstance(error.owner))
          std::cerr << lang::MirPrinter().print(function->body);
      } else if (error.bodyKind == lang::MirBodyKind::Constructor) {
        if (const auto *constructor =
                result.mir.findConstructorInstance(error.owner))
          std::cerr << lang::MirPrinter().print(constructor->body);
      }
    }
  }
}
void positive(const std::string &name, const std::string &source) {
  const auto result = analyze(source);
  if (!result.canGenerateCode())
    show(result);
  expect(result.canGenerateCode(), name);
  if (result.canGenerateCode())
    expect(lang::verifyMirProgram(result.mir).valid(), name + " verifies");
}
void negative(const std::string &name, const std::string &source,
              const std::string &expectedCode = "GTI-S2017") {
  const auto result = analyze(source);
  if (std::getenv("GTI_TEST_DUMP_DIAGNOSTICS") != nullptr) {
    std::cerr << name << '\n';
    show(result);
  }
  const bool semanticError = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &d) {
        return d.severity == lang::DiagnosticSeverity::Error &&
               d.code == expectedCode;
      });
  if (result.canGenerateCode() || !semanticError)
    show(result);
  expect(!result.canGenerateCode() && semanticError, name);
}
} // namespace

int main() {
  {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/borrowing_composition.gti";
    std::ifstream input(fixture);
    positive(
        "forwarded external fields do not retain a temporary carrier shell",
        std::string{std::istreambuf_iterator<char>(input), {}});
  }
  const std::string projectedGuard = R"gti(
class MutGuard {
  mut int& target;
public:
  MutGuard(mut int& target) : target(target) {}
  ~MutGuard() { this.target += 1; }
};
class Parent {
  mut int& target;
public:
  Parent(mut int& target) : target(target) {}
  void apply() mut { MutGuard child{this.target}; }
  void conflict() mut { MutGuard child{this.target}; this.target += 1; }
};
)gti";
  // Keep the invalid method out of the positive program: bodies are checked
  // independently of whether a particular caller selects them.
  const auto conflict = projectedGuard.find("  void conflict()");
  auto validGuard = projectedGuard;
  validGuard.erase(conflict, validGuard.find('\n', conflict) - conflict);
  positive("cleanup-owning child reborrows an incoming receiver field",
           validGuard + R"gti(
void apply(mut Parent& parent) { parent.apply(); }
int main() { mut int value = 7;
  { mut Parent parent{value}; apply(parent); }
  return value - 8;
}
)gti");
  negative("child cleanup retains its projected parent dependency",
           projectedGuard + "int main() { return 0; }");

  positive("by-value guard moves retain their formal provenance", R"gti(
void consume(Guard guard) {
  auto first = std::move(guard);
  auto second = std::move(first);
}
int main() {
  Resource resource{};
  consume(Guard(resource));
  return observed - 7;
}
)gti");

  positive("entry owner movement through exclusive branches", R"gti(
void consume(Guard guard, bool branch) {
  if (branch) { auto left = std::move(guard); }
  else { auto right = std::move(guard); }
}
int main() {
  Resource resource{};
  consume(Guard(resource), true);
  consume(Guard(resource), false);
  return observed - 14;
}
)gti");

  {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/nested_borrow_fields.gti";
    std::ifstream input(fixture);
    positive("nested borrowed members preserve receiver and by-value origins",
             std::string{std::istreambuf_iterator<char>(input), {}});
  }
  const std::string mixedOwner = R"gti(
class Mixed {
  Resource& resource;
  mut int local;
public:
  Mixed(Resource& resource) : resource(resource), local(7) {}
  int& get() { return this.local; }
  Resource& external() { return this.resource; }
  void reset() mut { this.local = 9; }
  ~Mixed() {}
};
)gti";
  negative("owned field accessor protects physical carrier storage",
           mixedOwner + R"gti(
int main() { Resource resource{}; mut Mixed mixed{resource};
  int& element = mixed.get(); mixed.reset(); return element; }
)gti");
  negative("owned field accessor prevents carrier movement", mixedOwner + R"gti(
int main() { Resource resource{}; mut Mixed mixed{resource};
  int& element = mixed.get(); auto moved = std::move(mixed); return element; }
)gti");
  positive("owned field accessor allows mutation after its loan ends",
           mixedOwner + R"gti(
int main() { Resource resource{}; mut Mixed mixed{resource};
  int& element = mixed.get(); int value = element;
  mixed.reset(); return value - 7; }
)gti");
  positive("stored reference accessor retains the external owner",
           mixedOwner + R"gti(
int main() { Resource resource{}; mut Mixed mixed{resource};
  Resource& external = mixed.external(); mixed.reset(); return external.value - 7; }
)gti");
  positive("references before repeated carrier movement keep exact ancestry",
           mixedOwner + R"gti(
int main() { Resource resource{}; Mixed first{resource};
  int& a = first.get(); int sum = a;
  auto second = std::move(first);
  int& b = second.get(); int sum2 = sum + b;
  auto third = std::move(second);
  return sum2 + third.get() - 21;
}
)gti");
  const std::string nestedView = R"gti(
class View {
  Resource& owner;
public:
  View(Resource& owner) : owner(owner) {}
  Resource& get() { return this.owner; }
};
class Wrapper {
  View view;
  mut int scratch;
public:
  Wrapper(Resource& owner) : view(View(owner)), scratch(0) {}
  View binding() { return this.view; }
  void touch() mut { this.scratch++; }
  int inspect() mut {
    auto copy = this.binding();
    this.touch();
    return copy.get().value;
  }
};
)gti";
  positive("copied nested view protects the referent rather than wrapper shell",
           nestedView + R"gti(
int main() { Resource resource{}; mut Wrapper wrapper{resource};
  auto view = wrapper.binding(); wrapper.touch();
  return view.get().value + wrapper.inspect() - 14;
}
)gti");
  negative("copied nested view keeps its actual resource borrowed",
           nestedView + R"gti(
int main() { mut Resource resource{}; Wrapper wrapper{resource};
  auto view = wrapper.binding(); resource.reset(); return view.get().value;
}
)gti");
  positive("shared independent guards and whole movement", R"gti(
int main() {
  mut Resource owner{};
  { Guard first{owner}; Guard second{owner}; auto moved = std::move(first); }
  owner.reset(); return observed - 14;
})gti");
  const std::string copiedViewOwner = R"gti(
struct PlainView {
  Resource& resource;
  PlainView(Resource& resource) : resource(resource) {}
};
class ViewOwner {
  PlainView view;
public:
  ViewOwner(PlainView view) : view(view) {}
  ~ViewOwner() { observed += this.view.resource.value; }
};
)gti";
  positive(
      "copying a passive view into an owner retains a distinct cleanup loan",
      copiedViewOwner + R"gti(
int main() { mut Resource resource{};
  { PlainView view{resource}; ViewOwner first{view}; ViewOwner second{view}; }
  resource.reset(); return observed - 14;
}
)gti");
  negative("copied passive view remains protected through the owning wrapper "
           "cleanup",
           copiedViewOwner + R"gti(
int main() { mut Resource resource{}; PlainView view{resource};
  ViewOwner owner{view}; resource.reset(); return 0;
}
)gti");
  positive("guard factory", R"gti(
Guard acquire(Resource& owner) { return Guard(owner); }
int main() { Resource owner{}; auto guard = acquire(owner); return 0; }
)gti");
  positive("consumed guard parameter", R"gti(
void consume(Guard guard) {}
int main() { mut Resource owner{}; consume(Guard(owner)); owner.reset(); return 0; }
)gti");
  positive("nested generic owning guard", R"gti(
int main() { mut Resource owner{}; { Box<Guard> box{Guard(owner)}; }
  owner.reset(); return observed - 7; }
)gti");
  positive("explicit nested constructor composes a cleanup dependency", R"gti(
class Inner<R> {
  R& resource;
public:
  Inner(R& resource) : resource(resource) {}
  ~Inner() {}
};
class Outer<R> {
  Inner<R> inner;
public:
  Outer(R& resource) : inner(Inner<R>(resource)) {}
};
class OuterAgain<R> {
  Outer<R> outer;
public:
  OuterAgain(R& resource) : outer(Outer<R>(resource)) {}
};
int main() { mut Resource owner{}; { OuterAgain<Resource> box{owner};
  auto moved = std::move(box); } owner.reset(); return 0; }
)gti");
  const std::string composed = R"gti(
class PairGuard {
  Resource& left; Resource& right;
public:
  PairGuard(Resource& a, Resource& b) : left(a), right(b) {}
  ~PairGuard() { observed += this.left.value + this.right.value; }
};
class Reversed {
  PairGuard pair;
public:
  Reversed(Resource& a, Resource& b) : pair(PairGuard(b, a)) {}
};
)gti";
  positive("composition preserves both reordered origins", composed + R"gti(
int main() { mut Resource a{}; mut Resource b{};
  { Reversed pair{a,b}; } a.reset(); b.reset(); return observed - 14; }
)gti");
  for (const auto &owner : {std::string("a"), std::string("b")})
    negative("composition retains reordered owner " + owner, composed + R"gti(
int main() { mut Resource a{}; mut Resource b{}; Reversed pair{a,b};
)gti" + owner + ".reset(); return 0; }");
  negative("composed local resource cannot escape", R"gti(
class Outer {
  Guard guard;
public:
  Outer(Resource& resource) : guard(Guard(resource)) {}
};
Outer bad() { Resource local{}; return Outer(local); }
int main() { auto escaped = bad(); return 0; }
)gti",
           "GTI-S2045");
  negative("nested construction cannot retain a by-value owned parameter",
           R"gti(
class Outer {
  Guard guard;
public:
  Outer(Resource resource) : guard(Guard(resource)) {}
};
int main() { Resource owner{}; Outer bad{owner}; return 0; }
)gti",
           "GTI-S2045");
  negative("arbitrary factory calls do not fabricate constructor summaries",
           R"gti(
Guard acquire(Resource& resource) { return Guard(resource); }
class Outer {
  Guard guard;
public:
  Outer(Resource& resource) : guard(acquire(resource)) {}
};
int main() { Resource owner{}; Outer bad{owner}; return 0; }
)gti",
           "GTI-S2045");
  negative("nested construction cannot duplicate one exclusive dependency",
           R"gti(
class Exclusive {
  mut Resource& resource;
public:
  Exclusive(mut Resource& resource) : resource(resource) {}
};
class Twice {
  Exclusive first; Exclusive second;
public:
  Twice(mut Resource& resource)
      : first(Exclusive(resource)), second(Exclusive(resource)) {}
};
int main() { mut Resource owner{}; Twice bad{owner}; return 0; }
)gti",
           "GTI-S2045");
  positive("independent dependency set", R"gti(
class Both {
  Resource& first; Resource& second;
public:
  Both(Resource& first, Resource& second) : first(first), second(second) {}
  ~Both() { observed += this.first.value + this.second.value; }
};
int main() { Resource a{}; Resource b{}; Both both{a, b}; return 0; }
)gti");
  positive("cleanup-bound buffering retains an adapter shell and its owner",
           R"gti(
#include <std/io>
#include <std/string>
int main() {
  std::byte_buffer bytes = {uint8_t(97)};
  auto view = bytes.as_span();
  mut std::io::memory_reader source{view};
  mut std::io::buffered_reader<std::io::memory_reader> input{source, std::size_t(2)};
  mut std::string line{};
  auto result = std::io::getline(input, line);
  return result ? 0 : 1;
}
)gti");
  positive("resource is available after the complete cleanup dependency scope",
           R"gti(
class View {
  Resource& resource;
public:
  View(Resource& resource) : resource(resource) {}
  int read() { return this.resource.value; }
};
class Adapter {
  View& view;
public:
  Adapter(View& view) : view(view) {}
  ~Adapter() { observed += this.view.read(); }
};
int main() {
  mut Resource resource{};
  { View view{resource}; Adapter adapter{view}; }
  resource.reset();
  return observed - 7;
}
)gti");
  negative("cleanup transitively retains the borrowed carrier's owner", R"gti(
class View {
  Resource& resource;
public:
  View(Resource& resource) : resource(resource) {}
  int read() { return this.resource.value; }
};
class Adapter {
  View& view;
public:
  Adapter(View& view) : view(view) {}
  ~Adapter() { observed += this.view.read(); }
};
int main() {
  mut Resource resource{};
  View view{resource};
  Adapter adapter{view};
  resource.reset();
  return 0;
}
)gti");
  negative("last use cannot release cleanup dependency", R"gti(
int main() { mut Resource owner{}; Guard guard{owner}; owner.reset(); return 0; }
)gti");

  positive("unrelated owning error does not retain an extracted success",
           R"gti(
struct Error {
  ~Error() { observed += 1; }
};
expected<Resource&, Error> lookup(Resource& owner) { return owner; }
int main() {
  mut Resource owner{};
  auto carrier = lookup(owner);
  Resource& extracted = std::move(carrier).expect("resource");
  int result = extracted.value;
  owner.reset();
  return result - 7;
}
)gti");
  negative("movement cannot release cleanup dependency", R"gti(
int main() { mut Resource owner{}; Guard guard{owner}; auto moved = std::move(guard);
  owner.reset(); return 0; }
)gti");
  negative("nested guard cannot release cleanup dependency", R"gti(
int main() { mut Resource owner{}; Box<Guard> box{Guard(owner)};
  owner.reset(); return 0; }
)gti");
  negative("local resource cannot escape", R"gti(
Guard bad() { Resource owner{}; return Guard(owner); }
int main() { auto guard = bad(); return 0; }
)gti",
           "GTI-S2045");
  negative("cleanup cannot upgrade a shared reference", R"gti(
class Bad {
  Resource& resource;
public:
  Bad(Resource& resource) : resource(resource) {}
  ~Bad() { this.resource.reset(); }
};
int main() { Resource owner{}; Bad guard{owner}; return 0; }
)gti",
           "GTI-S2000");
  // A method of a by-reference capture's referent or of an accessor's result
  // produces a cleanup-bound carrier exactly as a reference formal's does.
  const std::string station = R"gti(
class Station {
  Resource resource;
public:
  Station() {}
  Guard watch() { return Guard(this.resource); }
};
class Depot {
  mut Station station;
public:
  Depot() {}
  mut Station& get() mut { return this.station; }
};
)gti";
  {
    const auto captured = analyze(station + R"gti(
int main() {
  Station station{};
  auto observe = [&station]() -> int {
    auto guard = station.watch();
    return 1;
  };
  return observe() + observe() - 2;
}
)gti");
    if (!captured.canGenerateCode())
      show(captured);
    expect(captured.canGenerateCode() &&
               lang::verifyMirProgram(captured.mir).valid(),
           "cleanup-bound carrier from a by-reference capture's referent");
    // The carrier's loan is a child of the capture's entry loan and borrows
    // the dereferenced capture, not the capture slot itself.
    bool exact = false;
    for (const auto &lambda : captured.mir.lambdaInstances())
      for (const auto &loan : lambda.body.loans) {
        const auto *parent = lambda.body.findLoan(loan.parent);
        const auto *source = lambda.body.findPlace(loan.source);
        const auto *slot =
            parent != nullptr ? lambda.body.findPlace(parent->source) : nullptr;
        exact = exact ||
                (loan.kind == lang::MirLoanKind::Stored && loan.lifecycleBound &&
                 parent != nullptr && parent->entry &&
                 parent->kind == lang::MirLoanKind::Parameter && source &&
                 slot && source->root == lang::MirPlaceRootKind::Symbol &&
                 source->capture != 0 && source->capture == slot->capture &&
                 !source->projections.empty() &&
                 source->projections.front().kind ==
                     lang::MirProjectionKind::Dereference);
      }
    expect(exact, "captured carrier keeps the capture entry loan as parent");
  }
  positive("cleanup-bound carrier from an accessor receiver of a mutable formal",
           station + R"gti(
int run(mut Depot& depot) {
  auto guard = depot.get().watch();
  return 0;
}
int main() { mut Depot depot{}; return run(depot); }
)gti");
  return failures == 0 ? 0 : 1;
}
