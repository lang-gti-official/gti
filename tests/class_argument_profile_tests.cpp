#include "compiler_test_fixture.h"

namespace {
const std::string owned = R"(
class Packet<T, E> {
  T payload;
public:
  Packet(T value) : payload(std::move(value)) {}
  T& value() { return this.payload; }
  int kind() { return 1; }
};
)";
const std::string empty = R"(
class Packet<void, E> {
public:
  Packet() {}
  void value() {}
  int kind() { return 2; }
};
)";
const std::string borrowed = R"(
class Packet<T&, E> {
  T& payload;
public:
  Packet(T& value) : payload(value) {}
  T& value() { return this.payload; }
  int kind() { return 3; }
};
)";

lang::FrontendResult analyze(std::string source) {
  return lang::Frontend().analyze("class-argument-profile.gti",
                                  std::move(source), {standardLibraryPrelude()},
                                  {}, {standardLibraryRoot()});
}

void accepted(std::string source) {
  const auto result = analyze(std::move(source));
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode(), "argument profile is a valid source class");
  if (result.canGenerateCode()) {
    expect(lang::verifyMirProgram(result.mir).valid(),
           "profile preserves verified MIR");
    expect(!gti_test::emitCpp(result).contents.empty(),
           "profile reaches native emission");
  }
}

void rejected(std::string source, std::string_view code,
              std::string_view text) {
  const auto result = analyze(std::move(source));
  expect(!result.canGenerateCode(),
         "invalid profile or borrow has no code generation");
  const auto found =
      std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
                   [&](const auto &diagnostic) {
                     return diagnostic.code == code &&
                            diagnostic.message.find(text) != std::string::npos;
                   });
  if (found == result.diagnostics.end())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(found != result.diagnostics.end(),
         "profile reports the owning semantic rule");
}
} // namespace

int main() {
  const auto emptyConstruction = analyze(R"(
struct Completion {
  int value;
  implicit Completion(int value = 42) : value(value) {}
};
Completion complete() { return; }
int main() { return complete().value - 42; }
)");
  expect(emptyConstruction.canGenerateCode(),
         "empty return has a complete checked construction");
  if (emptyConstruction.canGenerateCode()) {
    expect(lang::verifyHirProgramPlans(emptyConstruction.semantics,
                                       emptyConstruction.hir)
               .valid(),
           "empty return semantic proof validates independently");
    for (int mutation = 0; mutation < 11; ++mutation) {
      auto changed = emptyConstruction.hir;
      bool found = false;
      for (const auto &function : changed.functionInstances()) {
        if (function.body.emptyReturnConstructions.empty())
          continue;
        found = true;
        auto &body = const_cast<lang::HirBody &>(function.body);
        auto &entry = body.emptyReturnConstructions.front();
        auto *value = const_cast<lang::HirValue *>(body.findValue(entry.value));
        switch (mutation) {
        case 0:
          entry.constructionProof.reset();
          break;
        case 1:
          entry.source = nullptr;
          break;
        case 2:
          value->constructorTarget.reset();
          break;
        case 3:
          value->defaultArguments.clear();
          break;
        case 4:
          value->callPlan->arguments.front().defaultArgument = false;
          break;
        case 5:
          value->fullExpression = 0;
          break;
        case 6:
          body.emptyReturnConstructions.push_back(entry);
          break;
        case 7:
          body.emptyReturnConstructions.clear();
          break;
        case 8:
          value->unsafeOperation =
              lang::UnsafeOperationKind::DeclaredUnsafeConstruction;
          break;
        case 9:
          value->info.type = lang::SemanticType::Bool;
          break;
        case 10:
          value->borrowDependencies = lang::SemanticBorrowDependencySet{};
          break;
        }
        break;
      }
      expect(found,
             "empty return fixture contains a source construction proof");
      expect(
          !lang::verifyHirProgramPlans(emptyConstruction.semantics, changed)
               .valid(),
          "missing, altered and duplicated empty-return proofs are rejected");
    }
  }
  accepted(R"(
class Done<E> {
public:
  int value;
  implicit Done(int initial = 42) : value(initial) {}
};
Done<int> finish() { return; }
T finish_generic<T>() { return; }
int main() { return finish().value + finish_generic<Done<int>>().value - 84; }
)");
  rejected(R"(
struct Explicit { Explicit() {} };
Explicit finish() { return; }
int main() { return 0; }
)",
           "GTI-S2012", "constructor");
  rejected(R"(
class Hidden { implicit Hidden() {} };
Hidden finish() { return; }
int main() { return 0; }
)",
           "GTI-S2007", "private");
  rejected(R"(
struct Ambiguous {
  implicit Ambiguous(int value = 1) {}
  implicit Ambiguous(bool value = true) {}
};
Ambiguous finish() { return; }
int main() { return 0; }
)",
           "GTI-S2013", "ambiguous");
  accepted(R"(
import std.meta;
static_assert(std::meta::type<void>().is_void());
static_assert(!std::meta::type<int>().is_void());
static_assert(std::meta::type<int&>().is_reference());
static_assert(!std::meta::type<int>().is_reference());
int main() { return 0; }
)");
  accepted(R"(
import std.meta;
class Reply<T, E> {
public:
  Reply() {}
  int kind() { return 1; }
};
class Reply<void, E> {
public:
  Reply() {}
  int kind() { return 2; }
};
Reply<std::meta::callable_return_type<F>, int> invoke<F>(F& callback) {
  callback();
  return Reply<std::meta::callable_return_type<F>, int>();
}
int main() {
  auto empty = []() -> void {};
  return invoke(empty).kind() - 2;
}
)");
  const std::string client = R"(
int inspect<T, E>(Packet<T, E>& packet) { return packet.kind(); }
int main() {
  Packet<int, int> value{42};
  Packet<void, int> empty{};
  int owner = 7;
  Packet<int&, int> reference{owner};
  empty.value();
  return inspect(value) + inspect(empty) + inspect(reference) - 6;
}
)";
  for (const auto &definitions :
       {owned + empty + borrowed, empty + borrowed + owned,
        borrowed + owned + empty})
    accepted(definitions + client);

  accepted(owned + empty + borrowed + R"(
concept Profile<P> {
  typename payload_type;
  int inspect(P& packet);
};
for <T, E> concept Profile<Packet<T, E>> {
  using payload_type = T;
  int inspect(Packet<T, E>& packet) { return packet.kind(); }
};
int inspect_profile<P>(P& packet) requires Profile<P> {
  return Profile<P>::inspect(packet);
}
int main() {
  Packet<int, int> owned{1};
  Packet<void, int> empty{};
  int owner = 7;
  Packet<int&, int> borrowed{owner};
  return inspect_profile(owned) + inspect_profile(empty) +
      inspect_profile(borrowed) - 6;
}
)");

  // Raw-pointer profiles distinguish pointee access, as reference profiles
  // distinguish referent access; the binder names the pointee.
  const std::string pointers = R"(
class Packet<T*, E> {
  T* payload = nullptr;
public:
  Packet(T* value) : payload(value) {}
  T* value() { return this.payload; }
  int kind() { return 4; }
public:
  Packet() = default;
};
class Packet<const T*, E> {
  const T* payload = nullptr;
public:
  Packet(const T* value) : payload(value) {}
  const T* value() { return this.payload; }
  int kind() { return 5; }
public:
  Packet() = default;
};
)";
  accepted(owned + pointers + R"(
int main() {
  mut int number = 7;
  Packet<int*, bool> writable{&number};
  Packet<const int*, bool> readable{&number};
  Packet<int**, bool> nested{nullptr};
  Packet<int, bool> value{3};
  int* address = writable.value();
  unsafe { *address = 8; }
  return writable.kind() + readable.kind() + nested.kind() + value.kind() -
      14;
}
)");

  rejected(owned + owned, "GTI-S2006", "Duplicate declaration");
  rejected(owned + empty + empty, "GTI-S2006", "Duplicate declaration");
  rejected("class Packet<T&, E> { public: Packet() = default; }; class "
           "Packet<T, E&> {};",
           "GTI-S2006", "Duplicate declaration");
  rejected(owned + borrowed + R"(
Packet<int&, int> bad() {
  int local = 42;
  return Packet<int&, int>(local);
}
)",
           "GTI-S2045", "outlives the result");
  rejected(owned + borrowed + R"(
int main() {
  mut int owner = 42;
  Packet<int&, int> reference{owner};
  owner = 7;
  return reference.value();
}
)",
           "GTI-S2017", "borrow");
  return failures == 0 ? 0 : 1;
}
