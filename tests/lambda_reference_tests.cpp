#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(std::string_view label, std::string source,
                             bool library = false) {
  const auto standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return library
             ? lang::Frontend().analyze(std::string(label), std::move(source),
                                        {standardLibrary / "prelude.gti"}, {},
                                        {standardLibrary})
             : lang::Frontend().analyze(std::string(label), std::move(source));
}

void checkAccepted(std::string_view label, std::string source,
                   lang::AccessMode access, std::size_t parameter = 0) {
  const auto result = analyze(label, std::move(source));
  expect(result.canGenerateCode(), label);
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  expect(lang::verifyMirProgram(result.mir).valid(),
         "accepted lambda reference returns retain verified MIR");
  bool found = false;
  for (const auto &lambda : result.mir.lambdaInstances()) {
    if (lambda.returnType.kind != lang::SemanticType::Reference ||
        lambda.returnType.referenceAccess != access)
      continue;
    found = true;
    expect(lambda.returnBorrowOrigin == lang::BorrowOriginKind::Argument &&
               lambda.returnBorrowParameter == parameter &&
               lambda.returnBorrowAccess == access,
           "lambda result retains its selected parameter and exact access");
  }
  expect(found, "the concrete MIR contains the tested reference lambda");
}

void checkRejected(std::string_view label, std::string source,
                   std::string_view message, bool library = false) {
  const auto result = analyze(label, std::move(source), library);
  expect(!result.semanticValid && !result.canGenerateCode(), label);
  expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const auto &diagnostic) {
                       return diagnostic.phase ==
                                  lang::DiagnosticPhase::Semantics &&
                              diagnostic.message.find(message) !=
                                  std::string::npos;
                     }),
         "rejection identifies the semantic ownership rule");
  expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code == "GTI-B0001";
                      }),
         "unsupported or invalid reference returns fail before MIR/backend");
}

void checkMutableResult() {
  checkAccepted("mutable-lambda-result.gti", R"gti(
int main() {
  auto increment = [](mut int& value) -> mut int& {
    value += 1;
    return value;
  };
  mut int number = 10;
  mut int& result = increment(number);
  result += 5;
  return number - 16;
}
)gti",
                lang::AccessMode::Mutable);
}

void checkReadOnlyReborrow() {
  checkAccepted("readonly-lambda-later-argument.gti", R"gti(
int main() {
  auto observe = [](int amount, mut int& value) -> int& {
    value += amount;
    return value;
  };
  mut int number = 10;
  int& result = observe(2, number);
  return result - 12;
}
)gti",
                lang::AccessMode::ReadOnly, 1);
  checkAccepted("readonly-lambda-result.gti", R"gti(
int main() {
  auto observe = [](mut int& value) -> int& {
    value += 1;
    return value;
  };
  mut int number = 10;
  int& result = observe(number);
  int snapshot = result;
  number += 1;
  return snapshot + number - 23;
}
)gti",
                lang::AccessMode::ReadOnly);

  checkAccepted("readonly-lambda-record-result.gti", R"gti(
class Gauge {
public:
  mut int reading;
  Gauge(int reading) : reading(reading) {}
};
int main() {
  auto observe = [](mut Gauge& source) -> Gauge& {
    source.reading += 1;
    return source;
  };
  auto reading = [](mut Gauge& source) -> int& {
    source.reading += 1;
    return source.reading;
  };
  mut Gauge gauge = Gauge(5);
  Gauge& view = observe(gauge);
  int first = view.reading;
  int& second = reading(gauge);
  return first + second - 13;
}
)gti",
                lang::AccessMode::ReadOnly);
}

void checkAliasesAndGenerics() {
  checkAccepted("lambda-local-reborrow.gti", R"gti(
int main() {
  auto relay = [](mut int& value) -> mut int& {
    mut int& parent = value;
    mut int& child = parent;
    return child;
  };
  mut int number = 4;
  mut int& result = relay(number);
  result += 3;
  return number - 7;
}
)gti",
                lang::AccessMode::Mutable);

  checkAccepted("lambda-shared-local-reborrow.gti", R"gti(
[[borrowed_from(value)]]
int& invoke<F>(F& callback, mut int& value) { return callback(value); }
[[borrowed_from(value)]]
int& invoke_owned<F>(F callback, mut int& value) { return callback(value); }
void observe_generic<T>(mut T& value) {
  auto observe = [](mut T& source) -> T& { return source; };
  T& result = observe(value);
}
int main() {
  auto observe = [](mut int& value) -> int& {
    mut int& parent = value;
    int& child = parent;
    return child;
  };
  mut int number = 8;
  int& result = invoke(observe, number);
  int first = result;
  int& owned_result = invoke_owned(observe, number);
  int second = owned_result;
  observe_generic(number);
  return first + second - 16;
}
)gti",
                lang::AccessMode::ReadOnly);

  checkAccepted("lambda-generic-mutable-result.gti", R"gti(
[[borrowed_from(value)]]
mut int& invoke<F>(F& callback, mut int& value) { return callback(value); }
int main() {
  auto relay = [](mut int& value) -> mut int& { return value; };
  mut int number = 9;
  mut int& result = invoke(relay, number);
  result += 1;
  return number - 10;
}
)gti",
                lang::AccessMode::Mutable);
}

void checkRejections() {
  checkRejected("named-inferred-reborrow-boundary.gti", R"gti(
int& observe(mut int& value) { return value; }
int main() { return 0; }
)gti",
                "borrowed return cannot derive from a mutable parameter");
  checkRejected("lambda-reference-parameter-remains-suspended.gti", R"gti(
int read(mut int& value) {
  auto observe = [](mut int& input) -> int& { return input; };
  int& result = observe(value);
  value += 1;
  return result;
}
int main() { return 0; }
)gti",
                "borrow");
  for (const auto returnType : {"mut int", "mut int*"}) {
    const auto invalid =
        analyze("lambda-mutable-value-result.gti",
                "int main() { auto invalid = []() -> " +
                    std::string(returnType) + " { return 0; }; return 0; }");
    expect(!invalid.semanticValid &&
               std::any_of(
                   invalid.diagnostics.begin(), invalid.diagnostics.end(),
                   [](const auto &d) {
                     return d.code == "GTI-S2030" &&
                            d.message.find("use 'mut T&'") != std::string::npos;
                   }),
           "mutable lambda result spelling requires a checked reference");
  }
  const std::string prefix = R"gti(
int main() {
  auto observe = [](mut int& value) -> int& { value += 1; return value; };
  mut int number = 1;
)gti";
  checkRejected("lambda-live-readonly-result.gti",
                prefix + "int& result = observe(number); number += 1; "
                         "return result; }",
                "borrow");
  checkRejected("lambda-result-no-upgrade.gti",
                prefix + "mut int& result = observe(number); "
                         "return result; }",
                "mutable");
  checkRejected("lambda-repeated-mutation.gti",
                prefix +
                    "int& first = observe(number); "
                    "int& second = observe(number); return first + second; }",
                "borrow");
  checkRejected("lambda-local-owner-escape.gti", R"gti(
int main() {
  auto bad = [](mut int& source) -> int& { int local = 1; return local; };
  return 0;
}
)gti",
                "selected parameter");
  checkRejected("lambda-capture-escape.gti", R"gti(
int main() {
  int captured = 1;
  auto bad = [&captured](mut int& source) -> int& { return captured; };
  return 0;
}
)gti",
                "selected parameter");
  checkRejected("lambda-no-mutable-origin.gti", R"gti(
int main() {
  auto bad = [](int& source) -> mut int& { return source; };
  return 0;
}
)gti",
                "requires exactly one mutable reference parameter");
  checkRejected("lambda-local-copy-mutable-escape.gti", R"gti(
int main() {
  auto bad = [](mut int& source) -> mut int& {
    mut int copy = source;
    return copy;
  };
  return 0;
}
)gti",
                "selected parameter");

  const std::string ambiguous = R"gti(
int main() {
  auto bad = [](int& left, mut int& right) -> int& { return left; };
  return 0;
}
)gti";
  const auto result = analyze("lambda-ambiguous-result.gti", ambiguous);
  const auto found = std::find_if(
      result.diagnostics.begin(), result.diagnostics.end(), [](const auto &d) {
        return d.message.find("borrowed return dependency is ambiguous") !=
               std::string::npos;
      });
  expect(!result.semanticValid && found != result.diagnostics.end(),
         "both shared and exclusive inputs compete for a shared result");
  if (found != result.diagnostics.end()) {
    const auto start = ambiguous.find("int&", ambiguous.find("->"));
    const auto expectedSource =
        std::filesystem::absolute("lambda-ambiguous-result.gti")
            .lexically_normal()
            .string();
    if (found->primary.source != expectedSource ||
        found->primary.start != start || found->primary.end != start + 3) {
      std::cerr << "Ambiguous result span: " << found->primary.source << ':'
                << found->primary.start << '-' << found->primary.end
                << "; expected " << start << '-' << start + 3 << '\n';
    }
    expect(
        found->code == "GTI-S2017" &&
            found->phase == lang::DiagnosticPhase::Semantics &&
            found->primary.source == expectedSource &&
            found->primary.start == start && found->primary.end == start + 3 &&
            !found->hints.empty(),
        "ambiguous result diagnostic retains code, exact type span and hint");
  }
}

void checkErasureBoundary() {
  checkRejected("lambda-readonly-reborrow-erasure.gti", R"gti(
import std.functional;
int main() {
  auto observe = [](mut int& source) -> int& { return source; };
  std::function_ptr<int&(mut int&)> erased{observe};
  return 0;
}
)gti",
                "exactly one read-only reference parameter", true);
  checkRejected("lambda-readonly-reborrow-view-erasure.gti", R"gti(
import std.functional;
int main() {
  auto observe = [](mut int& source) -> int& { return source; };
  std::function_ref<int&(mut int&)> erased{observe};
  return 0;
}
)gti",
                "signature's origin parameter", true);
}
} // namespace

int main() {
  checkMutableResult();
  checkReadOnlyReborrow();
  checkAliasesAndGenerics();
  checkRejections();
  checkErasureBoundary();
  return failures == 0 ? 0 : 1;
}
