#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace lang {
struct MirProgramTestAccess {
  static std::vector<MirFunctionInstance> &functions(MirProgram &program) {
    return program.functions;
  }
};
} // namespace lang

// Prefix `?` through `std::tryable`: an operand other than built-in
// `expected` propagates through the selected conformance block. Every rule is
// exercised through unrelated user types.
int main() {
  int failures = 0;
  const auto expect = [&](bool condition, const std::string &message) {
    if (!condition) {
      std::cerr << message << '\n';
      ++failures;
    }
  };
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto analyze = [&](const std::string &source) {
    return lang::Frontend().analyze(root / "tests" / "tryable-propagation.gti",
                                    source, {root / "stdlib" / "prelude.gti"},
                                    {}, {root / "stdlib"});
  };
  const auto dump = [](const lang::FrontendResult &result) {
    for (const auto &d : result.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  };
  const auto accepted = [&](const std::string &source,
                            const std::string &message,
                            std::initializer_list<std::string> fragments = {}) {
    const auto result = analyze(source);
    expect(result.canGenerateCode(), message);
    if (!result.canGenerateCode()) {
      dump(result);
      return;
    }
    const std::string cpp = gti_test::emitCpp(result).contents;
    for (const std::string &fragment : fragments) {
      expect(cpp.find(fragment) != std::string::npos,
             message + " (emitted C++ lacks '" + fragment + "')");
    }
  };
  const auto rejected = [&](const std::string &source, const char *code,
                            const std::string &fragment) {
    const auto result = analyze(source);
    const bool located = std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto &d) {
          const bool hinted = std::any_of(
              d.hints.begin(), d.hints.end(), [&](const std::string &hint) {
                return hint.find(fragment) != std::string::npos;
              });
          return d.code == code && !d.primary.source.empty() &&
                 d.primary.end > d.primary.start &&
                 (d.message.find(fragment) != std::string::npos || hinted);
        });
    if (!located)
      dump(result);
    expect(!result.canGenerateCode(),
           "invalid source must not reach codegen: " + fragment);
    expect(located,
           std::string("missing ") + code + " with '" + fragment + "'");
  };

  const std::string nativePayloads = R"gti(
[[c_abi]] struct NativeAddress { mut const int* address; mut int stamp; };
[[c_abi]] struct NativeBatch { NativeAddress records[2]; };
[[c_abi]] union NativeChoice { int integer; const int* address; };
extern "C" {
  NativeAddress native_address();
  NativeBatch native_batch();
  NativeChoice native_choice();
}
enum NativeEvent {
  address(NativeAddress value), batch(NativeBatch value),
  choice(NativeChoice value), empty
};
int main() {
  unsafe {
    NativeAddress address = native_address();
    NativeBatch batch = native_batch();
    NativeChoice choice = native_choice();
    NativeEvent first = NativeEvent::address(address);
    NativeEvent second = NativeEvent::batch(batch);
    NativeEvent third = NativeEvent::choice(choice);
    switch (first) {
      case NativeEvent::address(value): return value.stamp;
      case NativeEvent::batch: return 1;
      case NativeEvent::choice: return 2;
      case NativeEvent::empty: return 3;
    }
  }
}
)gti";
  accepted(nativePayloads,
           "validated native layouts must establish complete empty payload "
           "dependencies, including nested records, arrays and native unions");
  const auto nativePayloadResult = analyze(nativePayloads);
  if (nativePayloadResult.canGenerateCode()) {
    auto missingNativeEvidence = nativePayloadResult.mir;
    bool changed = false;
    for (auto &function : lang::MirProgramTestAccess::functions(
             missingNativeEvidence)) {
      for (auto &value : function.body.values) {
        if (value.borrowDependencies &&
            value.info.type.kind == lang::SemanticType::Enum) {
          value.borrowDependencies.reset();
          changed = true;
          break;
        }
      }
      if (changed)
        break;
    }
    expect(changed, "native payload fixture must publish complete evidence");
    expect(!lang::verifyMirProgram(missingNativeEvidence).valid(),
           "native-layout payloads must not bypass required complete evidence");
  }
  rejected(R"gti(
[[c_abi]] struct InvalidNative { int& owner; };
enum InvalidEvent { value(InvalidNative native), empty };
int main() { return 0; }
)gti", "GTI-S2064", "outside the bounded C ABI record field set");

  const std::string types = R"gti(
struct Rejected {
  int code;
  Rejected(int value) : code(value) {}
};
class Checked<T> {
public:
  T value;
  int code;
  Checked(T payload, int failure) : value(payload), code(failure) {}
  Checked(Checked<T>& other) = delete;
  Checked(Checked<T>&& other) = default;
};
for <std::copyable T> concept std::tryable<Checked<T>> {
  using value_type = T;
  using residual_type = Rejected;
  bool has_value(Checked<T>& result) { return result.code == 0; }
  [[borrowed_from(result)]] T take_value(Checked<T> result) { return result.value; }
  Rejected take_residual(Checked<T> result) { return Rejected(result.code); }
};
class Status {
public:
  int code;
  Status(int value) : code(value) {}
  implicit Status(Rejected rejected) : code(rejected.code) {}
};
Checked<int> make(int value, int failure) {
  return Checked<int>(value, failure);
}
)gti";

  accepted(types + R"gti(
Status total(int failure) {
  Checked<int> held = make(2, 0);
  int value = ?make(1, failure) + ?std::move(held);
  return Status(value);
}
int measure<V>(V value) { return 4; }
Status forward<T>(T result) requires std::tryable<T> {
  std::tryable<T>::value_type value = ?std::move(result);
  return Status(measure(value));
}
int main() {
  return total(0).code + forward(make(4, 0)).code - 7;
}
)gti",
           "temporary, moved and generic operands should propagate through "
           "their block",
           {"__gti_conformance_"});

  rejected(types + R"gti(
struct Plain {
  int value;
};
Status plain() {
  Plain value{1};
  int copy = ?value;
  return Status(copy);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "no 'for concept std::tryable<Plain>' block");
  rejected(types + R"gti(
Status named() {
  Checked<int> held = make(1, 0);
  int value = ?held;
  return Status(value);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "std::move(name)");
  rejected(types + R"gti(
int scalar() {
  int value = ?make(1, 0);
  return value;
}
int main() { return 0; }
)gti",
           "GTI-S2086", "cannot be initialized from it");
  rejected(types + R"gti(
class Other {
public:
  int code;
  Other(int value) : code(value) {}
};
Other other() {
  int value = ?make(1, 0);
  return Other(value);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "declares no implicit constructor");
  rejected(types + R"gti(
void nothing() {
  int value = ?make(1, 0);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "declared return type accepts it");
  rejected(types + R"gti(
Status unconstrained<T>(T result) {
  int value = ?std::move(result);
  return Status(value);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "requires 'requires std::tryable<T>'");
  rejected(types + R"gti(
constexpr Status constant() {
  int value = ?make(1, 0);
  return Status(value);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "constexpr function");
  const std::string enumProtocol = types + R"gti(
enum class Parse {
  ok(int value),
  failed(int code),
};
for concept std::tryable<Parse> {
  using value_type = int;
  using residual_type = Rejected;
  bool has_value(Parse& parse) {
    switch (parse) {
      case Parse::ok: return true;
      case Parse::failed: return false;
    }
  }
  int take_value(Parse parse) {
    switch (parse) {
      case Parse::ok(value): return value;
      case Parse::failed: return 0;
    }
  }
  Rejected take_residual(Parse parse) {
    switch (parse) {
      case Parse::ok: return Rejected(0);
      case Parse::failed(code): return Rejected(code);
    }
  }
};
Parse parse(int text) { return Parse::ok(text); }
Status enumerated(int text) {
  Parse held = parse(text);
  int value = ?Parse::ok(1) + ?parse(text) + ?held + ?std::move(held);
  return Status(value);
}
int main() { return enumerated(2).code - 7; }
)gti";
  accepted(enumProtocol, "a payload enum can implement propagation directly");
  const std::string passiveEnum = R"gti(
enum Token { item(int value), empty };
int inspect(Token& token) {
  switch (token) {
    case Token::item(value): return value;
    case Token::empty: return 0;
  }
}
)gti";
  accepted(passiveEnum + "int main() { return inspect(Token::item(7)) - 7; }",
           "ordinary readonly calls can observe passive enum temporaries");
  rejected(passiveEnum + R"gti(
void change(mut Token& token) {}
int main() { change(Token::item(7)); return 0; }
)gti",
           "GTI-S2017", "mutable reference to a temporary");
  rejected(passiveEnum + R"gti(
[[borrowed_from(token)]] Token& escape(Token& token) { return token; }
int main() { Token& value = escape(Token::item(7)); return inspect(value); }
)gti",
           "GTI-S2017", "may return a reference");
  const auto localSets = analyze(passiveEnum + R"gti(
Token select(bool flag) {
  mut auto value = Token::empty;
  if (flag) { value = Token::item(7); }
  else { value = Token::empty; }
  auto copied = value;
  return std::move(copied);
}
Token snapshot() {
  mut auto value = Token::empty;
  auto copied = value;
  value = Token::item(9);
  return copied;
}
int main() {
  auto first = select(true);
  auto second = snapshot();
  return inspect(first) + inspect(second) - 7;
}
)gti");
  expect(localSets.canGenerateCode(),
         "complete sets survive binding, branch union, copy and move");
  if (!localSets.canGenerateCode())
    dump(localSets);
  else {
    bool sawUnion = false;
    bool sawMove = false;
    for (const auto &function : localSets.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          const auto *value = instruction.result
                                  ? function.body.findValue(*instruction.result)
                                  : nullptr;
          if (!value || !value->borrowDependencies)
            continue;
          sawUnion |=
              value->borrowDependencies->possibleAlternatives.size() == 2;
          sawMove |= instruction.kind == lang::MirInstructionKind::Move;
        }
    expect(sawUnion && sawMove, "transport evidence includes both branch "
                                "alternatives and a moved result");
    for (int corruption = 0; corruption != 3; ++corruption) {
      auto malformed = localSets.mir;
      bool changed = false;
      for (auto &function : lang::MirProgramTestAccess::functions(malformed))
        for (auto &block : function.body.blocks)
          for (auto &instruction : block.instructions) {
            auto *value = instruction.result
                              ? &function.body.values[*instruction.result - 1]
                              : nullptr;
            if (changed || instruction.kind != lang::MirInstructionKind::Load ||
                !value || !value->borrowDependencies ||
                value->borrowDependencies->possibleAlternatives.size() != 2)
              continue;
            auto &alternatives =
                value->borrowDependencies->possibleAlternatives;
            if (corruption == 0)
              alternatives.pop_back();
            if (corruption == 1)
              alternatives.front().conditions.front().subobject = {1};
            if (corruption == 2)
              alternatives.back() = alternatives.front();
            changed = true;
          }
      expect(changed && !lang::verifyMirProgram(malformed).valid(),
             "MIR rejects omitted, substituted and duplicated joined "
             "alternatives");
    }
  }
  for (const std::string source :
       {"Token unknown(Token input) { return input; }",
        "void replace(mut Token& input) { input = Token::item(9); } "
        "Token unknown() { mut auto value = Token::empty; replace(value); "
        "return value; }"}) {
    const auto unknownSet =
        analyze(passiveEnum + source + " int main() { return 0; }");
    expect(
        unknownSet.canGenerateCode(),
        "unknown or mutably invalidated alternatives retain no complete proof");
    if (!unknownSet.canGenerateCode()) {
      dump(unknownSet);
      continue;
    }
    auto malformed = unknownSet.mir;
    bool changed = false;
    for (auto &function : lang::MirProgramTestAccess::functions(malformed))
      for (auto &block : function.body.blocks)
        for (auto &instruction : block.instructions) {
          auto *value = instruction.result
                            ? &function.body.values[*instruction.result - 1]
                            : nullptr;
          if (changed || instruction.kind != lang::MirInstructionKind::Load ||
              !value || value->info.type.kind != lang::SemanticType::Enum ||
              value->borrowDependencies)
            continue;
          value->borrowDependencies = lang::MirBorrowDependencySet{
              .possibleAlternatives = {
                  {.conditions = {
                       {.enumeration = value->info.type, .variant = 1}}}}};
          changed = true;
        }
    expect(changed,
           "unknown-storage mutation must select a transport in: " + source);
    expect(!changed || !lang::verifyMirProgram(malformed).valid(),
           "MIR cannot invent an empty-set proof for unknown or mutated "
           "storage in: " +
               source);
  }
  accepted(passiveEnum + R"gti(
Token repeated(int count) {
  mut auto value = Token::empty;
  for (mut int index = 0; index < count; ++index) {
    auto previous = value;
    [[discard]] inspect(previous);
    value = Token::item(index);
  }
  return value;
}
int main() { auto value = repeated(3); return inspect(value) - 2; }
)gti",
           "loop-carried alternatives do not reuse a first-iteration proof");
  const auto ownedSets = analyze(R"gti(
#include <std/string>
enum Owned { item(std::string text), empty };
Owned move_result() {
  auto original = Owned::item(std::string("error"));
  auto relocated = std::move(original);
  return relocated;
}
int main() { auto value = move_result(); return 0; }
)gti");
  expect(ownedSets.canGenerateCode(), "dependency-free owning alternatives "
                                      "preserve explicit and implicit moves");
  if (!ownedSets.canGenerateCode())
    dump(ownedSets);
  else {
    std::size_t ownedMoves = 0;
    for (const auto &function : ownedSets.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          const auto *value = instruction.result
                                  ? function.body.findValue(*instruction.result)
                                  : nullptr;
          if (instruction.kind == lang::MirInstructionKind::Move && value &&
              value->info.type.kind == lang::SemanticType::Enum &&
              value->borrowDependencies)
            ++ownedMoves;
        }
    expect(
        ownedMoves >= 2,
        "actual owning moves, including synthesized return, retain exact sets");
  }
  const auto enumProof = analyze(enumProtocol);
  if (enumProof.canGenerateCode()) {
    for (int corruption = 0; corruption != 6; ++corruption) {
      auto malformed = enumProof.mir;
      bool changed = false;
      for (auto &function : lang::MirProgramTestAccess::functions(malformed))
        for (auto &value : function.body.values) {
          if (changed || !value.borrowDependencies ||
              value.borrowDependencies->possibleAlternatives.empty())
            continue;
          auto &set = *value.borrowDependencies;
          if (corruption == 0)
            value.borrowDependencies.reset();
          if (corruption == 1)
            ++set.possibleAlternatives.front().conditions.front().variant;
          if (corruption == 2)
            set.possibleAlternatives.front().conditions.front().enumeration =
                lang::SemanticType::Unknown;
          if (corruption == 3)
            set.possibleAlternatives.push_back(
                set.possibleAlternatives.front());
          if (corruption == 4)
            set.dependencies.push_back({});
          if (corruption == 5)
            set.possibleAlternatives.front().conditions.front().subobject = {1};
          changed = true;
        }
      expect(changed, "empty-set mutation must select a proven enum producer");
      expect(!changed || !lang::verifyMirProgram(malformed).valid(),
             "MIR rejects missing empty evidence, wrong typed alternatives, "
             "invented dependencies and substituted nested paths");
    }
    for (int corruption = 0; corruption != 9; ++corruption) {
      auto malformed = enumProof.mir;
      bool changed = false;
      for (auto &function : lang::MirProgramTestAccess::functions(malformed))
        for (auto &block : function.body.blocks)
          for (auto &instruction : block.instructions) {
            if (changed ||
                instruction.kind != lang::MirInstructionKind::CallInput ||
                instruction.callInputKind !=
                    lang::HirCallInputKind::ReadTemporaryBorrow ||
                !instruction.loan || instruction.operands.size() != 1)
              continue;
            auto &loan = function.body.loans[*instruction.loan - 1];
            auto &place = function.body.places[loan.source - 1];
            if (place.type.kind != lang::SemanticType::Enum)
              continue;
            if (corruption == 0)
              place.sourceValue = 0;
            if (corruption == 1)
              loan.producedBy = 0;
            if (corruption == 2)
              loan.escapes = true;
            if (corruption == 3)
              loan.access = lang::AccessMode::Mutable;
            if (corruption == 4)
              instruction.operands.front().place = 0;
            if (corruption == 5)
              instruction.loan.reset();
            if (corruption >= 6) {
              auto obligation =
                  std::find_if(function.body.dropObligations.begin(),
                               function.body.dropObligations.end(),
                               [&](const lang::MirDropObligation &candidate) {
                                 return candidate.place == place.id;
                               });
              if (obligation == function.body.dropObligations.end())
                continue;
              if (corruption == 6)
                obligation->fullExpression = 0;
              if (corruption == 7)
                obligation->value = 0;
              if (corruption == 8)
                obligation->dropType.requiresActiveCleanup = true;
            }
            changed = true;
          }
      expect(changed,
             "enum temporary mutation must select its materialization");
      expect(!changed || !lang::verifyMirProgram(malformed).valid(),
             "MIR must reject a missing, mismatched, escaping or upgraded "
             "enum temporary argument loan or lifetime obligation");
    }
  }
  const std::string borrowed = R"gti(
struct Rejected {
  int code;
  Rejected(int value) : code(value) {}
};
class Reading {
public:
  int& external;
  int code;
  Reading(int& owner, int failure) : external(owner), code(failure) {}
};
for concept std::tryable<Reading> {
  using value_type = int&;
  using residual_type = Rejected;
  bool has_value(Reading& value) { return value.code == 0; }
  [[borrowed_from(value)]] int& take_value(Reading value) {
    return value.external;
  }
  Rejected take_residual(Reading value) { return Rejected(value.code); }
};
class Status {
public:
  int code;
  Status(int value) : code(value) {}
  implicit Status(Rejected error) : code(error.code) {}
};
)gti";
  accepted(borrowed + R"gti(
Status read(int& owner, int failure) {
  Reading source{owner, failure};
  int& value = ?std::move(source);
  return Status(value);
}
int main() {
  int owner = 7;
  return read(owner, 0).code - 7;
}
)gti",
           "propagation keeps a by-value carrier's external reference");

  const std::string mutableBorrowed = R"gti(
struct Rejected { int code; Rejected(int value) : code(value) {} };
class Editing {
public:
  mut int& external;
  int code;
  Editing(mut int& owner, int failure) : external(owner), code(failure) {}
};
for concept std::tryable<Editing> {
  using value_type = mut int&;
  using residual_type = Rejected;
  bool has_value(Editing& value) { return value.code == 0; }
  [[borrowed_from(value)]] mut int& take_value(mut Editing value) {
    return value.external;
  }
  Rejected take_residual(Editing value) { return Rejected(value.code); }
};
struct Status {
  int code;
  Status(int value) : code(value) {}
  implicit Status(Rejected error) : code(error.code) {}
};
)gti";
  accepted(mutableBorrowed + R"gti(
Status edit(mut int& owner) {
  Editing source{owner, 0};
  mut int& value = ?std::move(source);
  value = 8;
  return Status(value);
}
int main() { mut int owner = 7; return edit(owner).code - 8; }
)gti",
           "propagation transfers exclusive external reference authority");
  rejected(mutableBorrowed + R"gti(
Status edit(mut int& owner) {
  Editing source{owner, 0};
  mut int& value = ?std::move(source);
  owner = 9;
  return Status(value);
}
int main() { mut int owner = 7; return edit(owner).code; }
)gti",
           "GTI-S2017", "borrow");

  const auto proof = analyze(borrowed + R"gti(
Status read(int& owner) {
  Reading source{owner, 0};
  int& value = ?std::move(source);
  return Status(value);
}
int main() { int owner = 7; return read(owner).code - 7; }
)gti");
  const std::string guarded = R"gti(
class Guard {
public:
  int& owner;
  Guard(int& value) : owner(value) {}
  ~Guard() {}
};
class Envelope {
public:
  Guard guard;
  Envelope(Guard value) : guard(std::move(value)) {}
};
)gti";
  rejected(guarded + R"gti(
[[borrowed_from(value)]] int& escape(Guard value) { return value.owner; }
int main() { return 0; }
)gti",
           "GTI-S2017", "cleanup-bound carrier cannot escape");
  rejected(guarded + R"gti(
[[borrowed_from(value)]] int& escape(Envelope value) {
  return value.guard.owner;
}
int main() { return 0; }
)gti",
           "GTI-S2017", "cleanup-bound carrier cannot escape");
  const auto guardProof = analyze(guarded + R"gti(
int main() {
  int owner = 7;
  Guard guard{owner};
  return guard.owner - 7;
}
)gti");
  expect(guardProof.canGenerateCode(),
         "a source destructor retains its reference until ordinary cleanup");
  if (guardProof.canGenerateCode()) {
    auto malformed = guardProof.mir;
    bool changed = false;
    for (auto &function : lang::MirProgramTestAccess::functions(malformed))
      for (auto &loan : function.body.loans) {
        if (loan.lifecycleBound) {
          loan.lifecycleBound = false;
          changed = true;
        }
      }
    expect(changed, "guard mutation must select a lifecycle-bound dependency");
    expect(!changed || !lang::verifyMirProgram(malformed).valid(),
           "MIR must reject omitted destructor-bound dependency retention");
  }
  if (proof.canGenerateCode()) {
    for (bool primary : {true, false}) {
      auto malformed = proof.mir;
      bool changed = false;
      for (auto &function : lang::MirProgramTestAccess::functions(malformed))
        for (auto &loan : function.body.loans) {
          if (changed || loan.kind != lang::MirLoanKind::Stored)
            continue;
          const lang::BorrowDependencyCondition forged{
              .enumeration = lang::SemanticType::enumType(1), .variant = 0};
          if (primary)
            loan.conditions.push_back(forged);
          else
            loan.additionalDependencies.push_back({.source = loan.source,
                                                   .access = loan.access,
                                                   .conditions = {forged}});
          changed = true;
        }
      expect(changed, "conditional mutation must select a stored dependency");
      expect(!changed || !lang::verifyMirProgram(malformed).valid(),
             "primary and additional dependencies both require exact "
             "active-alternative proofs");
    }
    for (int corruption = 0; corruption < 4; ++corruption) {
      auto malformed = proof.mir;
      bool changed = false;
      for (auto &function : lang::MirProgramTestAccess::functions(malformed)) {
        for (auto &block : function.body.blocks) {
          for (auto &instruction : block.instructions) {
            if (changed || instruction.kind != lang::MirInstructionKind::Call ||
                instruction.borrowOrigin != lang::BorrowOriginKind::Argument ||
                instruction.info.category != lang::ValueCategory::Place)
              continue;
            if (corruption == 0)
              instruction.borrowOrigin = lang::BorrowOriginKind::None;
            else if (corruption == 1)
              instruction.borrowArgument = 1;
            else if (corruption == 2)
              instruction.borrowAccess = lang::AccessMode::Mutable;
            else if (instruction.loan)
              function.body.loans[*instruction.loan - 1].source = 0;
            else
              continue;
            changed = true;
          }
        }
      }
      expect(changed, "borrowed propagation mutation must find its call proof");
      expect(!changed || !lang::verifyMirProgram(malformed).valid(),
             "MIR must reject missing, substituted, upgraded or sourceless "
             "borrowed protocol result authority");
    }
  }

  // An enum template conforms through one block, and its residual may be
  // the enum itself.
  accepted(R"gti(
#include <std/panic>
enum class Result<T, E> {
  ok(T value),
  failed(E error),
};
for <std::copyable T, std::copyable E> concept std::tryable<Result<T, E>> {
  using value_type = T;
  using residual_type = Result<T, E>;
  bool has_value(Result<T, E>& result) {
    switch (result) {
    case Result<T, E>::ok:
      return true;
    case Result<T, E>::failed:
      return false;
    }
  }
  T take_value(Result<T, E> result) {
    switch (result) {
    case Result<T, E>::ok(value):
      return value;
    case Result<T, E>::failed(error):
      std::panic("take_value on a failed result");
    }
  }
  Result<T, E> take_residual(Result<T, E> result) { return result; }
};
Result<int, int> twice(int value) {
  int half = ?Result<int, int>::ok(value);
  return Result<int, int>::ok(half * 2);
}
int main() {
  switch (twice(2)) {
  case Result<int, int>::ok(value):
    return value - 4;
  case Result<int, int>::failed(error):
    return 1;
  }
}
)gti",
           "an enum template's block should serve '?' on its instances");
  rejected(types + R"gti(
enum class Code {
  ok,
  failed,
};
for concept std::tryable<Code> {
  using value_type = void;
  using residual_type = Rejected;
  bool has_value(Code& code) { return code == Code::ok; }
  void take_value(Code code) {}
  Rejected take_residual(Code code) { return Rejected(1); }
};
Status coded() {
  ?Code::failed;
  return Status(0);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "class, struct or payload-enum operand");
  // Built-in expected keeps its own rule.
  rejected(types + R"gti(
expected<int, int> load() { return 1; }
Status mixed() {
  int value = ?load();
  return Status(value);
}
int main() { return 0; }
)gti",
           "GTI-S2086", "returning 'expected<U, E>'");

  if (failures != 0) {
    std::cerr << failures << " tryable propagation checks failed\n";
    return 1;
  }
  std::cout << "tryable propagation tests passed\n";
  return 0;
}
