#include "../src/compiler/cpp_mir_representation_snapshot.h"
#include "../src/compiler/cpp_representation.h"
#include "cpp_backend_test_support.h"
#include "gti/lowered_program_printer.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <variant>

namespace lang {
struct MirProgramTestAccess {
  static std::vector<MirPayloadEnum> &enums(MirProgram &program) {
    return program.payloadEnums_;
  }
  static std::vector<MirFunctionInstance> &functions(MirProgram &program) {
    return program.functions;
  }
};
struct LoweredProgramTestAccess {
  static std::vector<LoweredDeclaration> &
  declarations(LoweredProgram &program) {
    return program.declarations_;
  }
};
} // namespace lang

int main() {
  int failures = 0;
  const auto expect = [&](bool condition, const char *message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  };
  const auto frontend = lang::Frontend().analyze("generic-enum-backend.gti", R"(
struct Box<T> { mut T value; };
enum Message<T> { empty, item(T value), boxed(Box<T> value) };
Message<int> make_message() { return Message<int>::item(7); }
int read(Message<int>& message) {
  switch (message) {
    case Message<int>::empty: return 0;
    case Message<int>::item(value): return value;
    case Message<int>::boxed(auto& value): return value.value;
  }
}
int read(Message<bool>& message) {
  switch (message) {
    case Message<bool>::empty: return 0;
    case Message<bool>::item(value): return value ? 1 : 0;
    case Message<bool>::boxed(auto& value): return value.value ? 1 : 0;
  }
}
int main() {
  auto left = make_message();
  auto right = Message<bool>::item(true);
  return read(left) + read(right) - 8;
}
)");
  expect(frontend.canGenerateCode(),
         "generic enum fixture reaches verified MIR");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  expect(frontend.hir.enumDeclarations().size() == 2 &&
             frontend.mir.payloadEnums().size() == 2,
         "two exact instances coexist without a symbolic primary");
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto forged = frontend.mir;
    bool changed = false;
    for (auto &function : lang::MirProgramTestAccess::functions(forged)) {
      for (auto &block : function.body.blocks) {
        for (auto &instruction : block.instructions) {
          if (changed ||
              instruction.operation != lang::MirOperation::PayloadExtract)
            continue;
          expect(instruction.payloadExtractionKind.has_value(),
                 "payload extraction preserves the resolved pattern mode");
          if (mutation == 0)
            instruction.payloadExtractionKind.reset();
          else if (mutation == 1)
            instruction.payloadExtractionKind =
                lang::PayloadExtractionKind::TransferReference;
          else
            instruction.payloadExtractionKind =
                instruction.info.category == lang::ValueCategory::Place
                    ? lang::PayloadExtractionKind::CopyValue
                    : lang::PayloadExtractionKind::BorrowValue;
          changed = true;
        }
      }
    }
    expect(changed && !lang::verifyMirProgram(forged).valid(),
           "missing or substituted extraction modes cannot change storage "
           "borrowing into reference transfer or value copying");
  }
  if (frontend.mir.payloadEnums().size() != 2)
    return 1;
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto forged = frontend.mir;
    bool changed = false;
    for (auto &function : lang::MirProgramTestAccess::functions(forged)) {
      for (auto &loan : function.body.loans) {
        if (changed || loan.kind != lang::MirLoanKind::Local)
          continue;
        for (const auto &value : function.body.values) {
          if (changed || value.info.category != lang::ValueCategory::Place)
            continue;
          lang::SemanticType reference{lang::SemanticType::Reference};
          reference.arguments.push_back(value.info.type);
          loan.referenceSelection = lang::MirReferenceSelection{
              .value = mutation == 0 ? 0 : value.id,
              .carrier = mutation == 1 ? 0 : loan.source,
              .fields = {1},
              .reference = reference};
          changed = true;
        }
      }
    }
    expect(changed && !lang::verifyMirProgram(forged).valid(),
           "a missing or unrelated selected-reference producer cannot "
           "reinterpret an ordinary storage borrow");
  }
  const auto &left = frontend.mir.payloadEnums()[0];
  const auto &right = frontend.mir.payloadEnums()[1];
  expect(left.declaration == right.declaration && left.type != right.type &&
             left.variants[1].payloadTypes != right.variants[1].payloadTypes,
         "primary declaration identity does not collapse concrete payloads");
  expect(frontend.mir.findPayloadEnum(left.type) == &left &&
             frontend.mir.findPayloadEnum(right.type) == &right &&
             frontend.mir.findPayloadEnum(left.declaration) == nullptr,
         "catalog lookup requires the exact concrete type");
  for (const auto &enumeration : frontend.mir.payloadEnums()) {
    const auto &unusedClass = enumeration.variants[2].payloadTypes[0];
    expect(std::any_of(frontend.mir.classInstances().begin(),
                       frontend.mir.classInstances().end(),
                       [&](const auto &instance) {
                         return instance.type == unusedClass;
                       }),
           "unused alternatives still discover their concrete payload layout");
  }
  auto badMir = frontend.mir;
  lang::MirProgramTestAccess::enums(badMir)[1].type = left.type;
  expect(!lang::verifyMirProgram(badMir).valid(),
         "MIR rejects duplicated or cross-instance enum metadata");
  auto cyclicMir = frontend.mir;
  auto &cyclicEnum = lang::MirProgramTestAccess::enums(cyclicMir).front();
  cyclicEnum.variants[1].payloadTypes[0] = cyclicEnum.type;
  const auto cyclicVerification = lang::verifyMirProgram(cyclicMir);
  expect(!cyclicVerification.valid() &&
             std::any_of(cyclicVerification.errors.begin(),
                         cyclicVerification.errors.end(),
                         [](const auto &error) {
                           return error.message.find("inline layout") !=
                                  std::string::npos;
                         }),
         "MIR rejects forged recursive enum layout before deriving cleanup");
  const auto lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  const auto leftName = lang::cppConcreteEnumName(lowered, left.type);
  const auto rightName = lang::cppConcreteEnumName(lowered, right.type);
  expect(leftName && rightName && leftName != rightName,
         "native representation names preserve exact generic identity");
  const auto serialized = lang::LoweredProgramPrinter().print(lowered);
  expect(serialized.starts_with("lowered-program-v20") &&
             lang::MirPrinter().print(frontend.mir).starts_with("mir-v110"),
         "snapshot versions retain the concrete enum identity contract");
  auto badLowered = lowered;
  for (auto &row : lang::LoweredProgramTestAccess::declarations(badLowered)) {
    if (auto *enumeration =
            std::get_if<lang::LoweredEnumDeclaration>(&row.payload);
        enumeration && enumeration->type == right.type) {
      enumeration->type = left.type;
      break;
    }
  }
  expect(!lang::verifyLoweredProgram(badLowered).empty(),
         "sealed declarations reject a forged concrete enum identity");
  for (const bool changeCleanup : {false, true}) {
    auto changedCarrier = lowered;
    for (auto &row :
         lang::LoweredProgramTestAccess::declarations(changedCarrier)) {
      if (auto *enumeration =
              std::get_if<lang::LoweredEnumDeclaration>(&row.payload);
          enumeration && enumeration->type == right.type) {
        if (changeCleanup)
          enumeration->requiresActiveCleanup =
              !enumeration->requiresActiveCleanup;
        else
          enumeration->traits.copyable = !enumeration->traits.copyable;
        break;
      }
    }
    expect(!lang::verifyLoweredProgram(changedCarrier).empty(),
           "tagged carrier selection cannot forge cleanup or native operation "
           "availability independently of sealed MIR traits");
  }
  auto rows =
      lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23);
  expect(rows.enums.size() == 2 && rows.enums[0].type != rows.enums[1].type,
         "detached representation rows preserve both enum instances");
  for (auto &row : rows.enums) {
    if (row.type == right.type)
      row.type = left.type;
  }
  // The emitter keeps a reference to its map, so the map must outlive it.
  const lang::CppMirBodyEmissionMap rejectedMap(rows);
  const lang::CppMirBodyEmitter rejected(frontend.mir, rejectedMap);
  expect(
      !rejected
           .analyze({lang::MirBodyKind::Function,
                     frontend.mir.functionInstances().front().id})
           .ready(),
      "production emission rejects cross-instance copied representation rows");
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const auto artifact = gti_test::emitCpp(frontend, standard);
    expect(leftName && rightName &&
               artifact.contents.find("struct " + *leftName) !=
                   std::string::npos &&
               artifact.contents.find("struct " + *rightName) !=
                   std::string::npos,
           "production verified-MIR emission defines both concrete layouts");
  }
  const auto compositionLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto composed = lang::Frontend().analyze(
      "composed-payload-move.gti", R"(
class Resource {
  int id;
public:
  Resource(int value) : id(value) {}
  Resource(Resource& other) = delete;
  Resource(Resource&& other) = default;
  ~Resource() {}
  int read() { return this.id; }
};
enum Packet<T> { empty, item(T value) };
int main() {
  Resource[2] values = {Resource(3), Resource(4)};
  auto packet = Packet<Resource[2]>::item(std::move(values));
  switch (std::move(packet)) {
  case Packet<Resource[2]>::empty: return 1;
  case Packet<Resource[2]>::item(items):
    return items[0].read() + items[1].read() - 7;
  }
}
)",
      {compositionLibrary / "prelude.gti"}, {}, {compositionLibrary});
  expect(composed.canGenerateCode(),
         "an owning array payload retains verified move/drop identities");
  if (!composed.canGenerateCode())
    for (const auto &diagnostic : composed.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  if (composed.canGenerateCode()) {
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto invalid = composed.mir;
      bool changed = false;
      for (auto &function : lang::MirProgramTestAccess::functions(invalid)) {
        auto &body = function.body;
        for (const auto &block : body.blocks) {
          for (const auto &instruction : block.instructions) {
            if (changed || instruction.kind != lang::MirInstructionKind::Move ||
                instruction.info.type.kind != lang::SemanticType::Array ||
                instruction.lifecycle.size() != 1)
              continue;
            const auto target = instruction.lifecycle.front().target;
            for (auto &drop : body.dropObligations) {
              if (drop.id != target)
                continue;
              if (mutation == 0)
                drop.value = 0;
              else if (mutation == 1)
                drop.value = instruction.hirValue + 10000;
              else if (mutation == 2)
                drop.generatedValue = 10000;
              else
                drop.place = instruction.operands.front().place;
              changed = true;
              break;
            }
          }
        }
      }
      expect(changed && !lang::verifyMirProgram(invalid).valid(),
             "array payload move rejects missing/substituted drop identity or "
             "slot");
    }
  }
  const auto cleanupForwarding = lang::Frontend().analyze(
      "single-action-cleanup.gti", R"(
class Resource {
public:
  Resource() {}
  Resource(Resource& other) = delete;
  Resource(Resource&& other) = default;
  ~Resource() {}
};
class Single {
  Resource child;
public:
  Single() : child(Resource()) {}
};
class Multiple {
  Resource first;
  Resource second;
public:
  Multiple() : first(Resource()), second(Resource()) {}
};
class WithDestructor {
  Resource child;
public:
  WithDestructor() : child(Resource()) {}
  ~WithDestructor() {}
};
enum Choice { none, left(Resource value), right(Resource value) };
enum Pair { none, both(Resource first, Resource second) };
int main() {
  Single single{};
  Multiple multiple{};
  WithDestructor destructor{};
  auto choice = Choice::right(Resource());
  auto pair = Pair::both(Resource(), Resource());
  return 0;
}
)",
      {compositionLibrary / "prelude.gti"}, {}, {compositionLibrary});
  expect(
      cleanupForwarding.canGenerateCode(),
      "unrelated single/multiple-action cleanup wrappers reach verified MIR");
  if (!cleanupForwarding.canGenerateCode())
    for (const auto &diagnostic : cleanupForwarding.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  if (cleanupForwarding.canGenerateCode()) {
    const auto text = gti_test::emitCpp(cleanupForwarding).contents;
    for (const auto &instance : cleanupForwarding.hir.classInstances()) {
      const auto name = instance.source->name().lexeme;
      if (name != "Single" && name != "Multiple" && name != "WithDestructor")
        continue;
      const auto start = text.find("constexpr (__gti_cleanup_instance == " +
                                   std::to_string(instance.id) + ")");
      const auto end = text.find("else { static_assert", start);
      expect(start != std::string::npos && end != std::string::npos,
             "each exact class cleanup descriptor has an emitted adapter");
      if (start == std::string::npos || end == std::string::npos)
        continue;
      const auto adapter = text.substr(start, end - start);
      expect((adapter.find("mir_cleanup_sequence_v1 sequence") ==
              std::string::npos) == (name == "Single"),
             "only a destructor-free singleton class recipe tail-forwards");
      if (name == "Single")
        expect(
            adapter.find("(record, unwinding);") != std::string::npos,
            "singleton cleanup forwards the original record and unwind mode");
    }
    for (const auto &enumeration : cleanupForwarding.mir.payloadEnums()) {
      const auto cleanup =
          lang::mirUniqueCleanupType(cleanupForwarding.mir, enumeration.type);
      expect(cleanup.has_value(),
             "an owned enum has a complete cleanup recipe");
      if (!cleanup)
        continue;
      const auto expression = lang::cppMirCleanupValueExpression(
          cleanupForwarding.mir, *cleanup, "value", "record", "unwinding");
      const bool singletonAlternatives = enumeration.variants.size() == 3;
      expect((expression.find("mir_cleanup_sequence_v1") ==
              std::string::npos) == singletonAlternatives,
             "distinct singleton alternatives forward, but two simultaneous "
             "children retain ordered secondary-failure handling");
      if (singletonAlternatives)
        expect(
            expression.find("(record, unwinding)") != std::string::npos &&
                expression.find("return true;") != std::string::npos,
            "enum forwarding retains incoming failure state and empty success");
    }
  }
  const auto projected =
      lang::Frontend().analyze("projected-enum-borrow.gti", R"(
enum Event<T> { item(T value) };
int main() {
  Event<int> event = Event<int>::item(7);
  int unrelated = 3;
  Event<int>& view = event;
  switch (view) {
    case Event<int>::item(auto& value): return value - 7;
  }
}
)");
  expect(projected.canGenerateCode(),
         "borrowed patterns through local references use checked shared "
         "reborrows");
  if (projected.canGenerateCode()) {
    auto unrelated = projected.mir;
    bool changed = false;
    for (auto &function : lang::MirProgramTestAccess::functions(unrelated)) {
      auto &body = function.body;
      for (auto &loan : body.loans) {
        if (loan.parent == 0 || loan.kind != lang::MirLoanKind::Local)
          continue;
        const auto *source = body.findPlace(loan.source);
        const auto other = std::find_if(
            body.places.begin(), body.places.end(), [&](const auto &place) {
              return place.root == lang::MirPlaceRootKind::Binding &&
                     place.type == source->type && place.projections.empty();
            });
        if (other != body.places.end()) {
          loan.source = other->id;
          changed = true;
          break;
        }
      }
    }
    expect(changed && !lang::verifyMirProgram(unrelated).valid(),
           "a shared projected child cannot borrow unrelated storage");
    auto mutableChild = projected.mir;
    changed = false;
    for (auto &function : lang::MirProgramTestAccess::functions(mutableChild)) {
      for (auto &loan : function.body.loans) {
        if (loan.parent != 0 && loan.kind == lang::MirLoanKind::Local) {
          loan.access = lang::AccessMode::Mutable;
          changed = true;
          break;
        }
      }
    }
    expect(changed && !lang::verifyMirProgram(mutableChild).valid(),
           "a shared projected child cannot escalate its parent access");
    auto deadParent = projected.mir;
    changed = false;
    for (auto &function : lang::MirProgramTestAccess::functions(deadParent)) {
      auto &body = function.body;
      for (const auto &loan : body.loans) {
        if (loan.parent == 0 || loan.kind != lang::MirLoanKind::Local)
          continue;
        lang::MirBlockId childBlock = 0;
        lang::MirInstructionId childInstruction = 0;
        for (const auto &block : body.blocks)
          for (const auto &instruction : block.instructions)
            if (instruction.kind == lang::MirInstructionKind::Borrow &&
                instruction.loan == loan.id) {
              childBlock = block.id;
              childInstruction = instruction.id;
            }
        std::optional<lang::MirInstruction> retire;
        for (auto &block : body.blocks) {
          const auto end =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const auto &instruction) {
                             return instruction.kind ==
                                        lang::MirInstructionKind::EndBorrow &&
                                    instruction.loan == loan.parent;
                           });
          if (end != block.instructions.end()) {
            retire = *end;
            block.instructions.erase(end);
            break;
          }
        }
        auto *block = body.findBlock(childBlock);
        if (retire && block != nullptr) {
          const auto before = std::find_if(
              block->instructions.begin(), block->instructions.end(),
              [&](const auto &instruction) {
                return instruction.id == childInstruction;
              });
          block->instructions.insert(before, *retire);
          changed = true;
        }
        break;
      }
      if (changed)
        break;
    }
    expect(changed && !lang::verifyMirProgram(deadParent).valid(),
           "a projected child cannot be created after its parent has ended");
    auto forwardingCycle = projected.mir;
    changed = false;
    bool cyclicIdentityRejected = false;
    for (auto &function :
         lang::MirProgramTestAccess::functions(forwardingCycle)) {
      auto &body = function.body;
      for (const auto &loan : body.loans) {
        if (loan.parent == 0 || loan.kind != lang::MirLoanKind::Local)
          continue;
        // The child's projected SSA source already forwards through its
        // parent's carrier. Redirecting that parent back to the child forms
        // a storage-forwarding cycle without changing the SSA producer.
        body.loans[loan.parent - 1].source = loan.source;
        cyclicIdentityRejected =
            !lang::mirCanonicalPlaceContains(body, loan.source, loan.source);
        changed = true;
        break;
      }
      if (changed)
        break;
    }
    expect(changed && cyclicIdentityRejected &&
               !lang::verifyMirProgram(forwardingCycle).valid(),
           "cyclic value/carrier forwarding cannot fabricate storage identity");
  } else {
    for (const auto &diagnostic : projected.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  const auto nested = lang::Frontend().analyze("returned-enum-argument.gti", R"(
class Resource {
  int value;
public:
  Resource(int value) : value(value) {}
  Resource(Resource&& other) = default;
  ~Resource() {}
};
enum Event<T> { idle, item(T value) };
Event<Resource> make_event() { return Event<Resource>::item(Resource(7)); }
void consume<T>(T value) {}
int main() {
  consume(make_event());
  return 0;
}
)");
  expect(nested.canGenerateCode(),
         "returned owning enum arguments retain exact MIR cleanup identity");
  if (nested.canGenerateCode()) {
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      const auto artifact = gti_test::emitCpp(nested, standard);
      expect(
          !artifact.contents.empty(),
          "a returned owning enum is transported directly into another call");
    }
  } else {
    for (const auto &diagnostic : nested.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  const auto rawPointee =
      lang::Frontend().analyze("raw-pointee-carrier-boundary.gti", R"(
struct Counts { mut int value = 0; };
class Counter {
  int& source;
  Counts* state = nullptr;
public:
  Counter(int& source, Counts* state) : source(source), state(state) {}
  unsafe void increment() { unsafe { this.state->value++; } }
  int count() { unsafe { return this.state->value; } }
};
int main() {
  int owner = 7;
  mut Counts counts{};
  Counter counter{owner, &counts};
  unsafe { counter.increment(); }
  return counter.count() - 1;
}
)");
  expect(
      rawPointee.canGenerateCode(),
      "an unsafe mutable pointee is not the readonly borrowed carrier shell");
  if (rawPointee.canGenerateCode()) {
    bool distinct = false;
    for (const auto &function : rawPointee.mir.functionInstances()) {
      const auto &body = function.body;
      for (const auto &place : body.places) {
        if (place.root != lang::MirPlaceRootKind::Value ||
            std::none_of(place.projections.begin(), place.projections.end(),
                         [](const auto &projection) {
                           return projection.kind ==
                                  lang::MirProjectionKind::RawDereference;
                         }))
          continue;
        for (const auto &loan : body.loans)
          if (loan.kind == lang::MirLoanKind::Stored && loan.entry)
            distinct = distinct || !lang::mirCanonicalPlaceContains(
                                       body, loan.source, place.id);
      }
    }
    expect(distinct,
           "raw pointee identity stays separate from a checked carrier origin");
    expect(!gti_test::emitCpp(rawPointee).contents.empty(),
           "the raw pointer boundary retains verified-MIR production emission");
  } else {
    for (const auto &diagnostic : rawPointee.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  for (const std::string declaration :
       {"enum Shape { rect(int width), owned(Res resource) };",
        "enum ShapeFamily<T> { rect(int width), owned(T resource) }; "
        "using Shape = ShapeFamily<Res>;"}) {
    const auto field =
        lang::Frontend().analyze("owning-enum-field-switch.gti", std::string(R"(
class Res {
public:
  Res() {}
  ~Res() {}
  int id() { return 7; }
};
)") + declaration + R"(
class Slot {
public:
  Shape shape;
  Slot(Shape shape) : shape(std::move(shape)) {}
};
int main() {
  Slot slot = Slot(Shape::rect(3));
  switch (slot.shape) {
    case Shape::rect(width): return width - 3;
    case Shape::owned(auto& resource): return resource.id();
  }
}
)",
                                 {library / "prelude.gti"}, {}, {library});
    expect(field.canGenerateCode(),
           "generic and nongeneric owning enum fields retain verified MIR");
    if (!field.canGenerateCode()) {
      for (const auto &diagnostic : field.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      expect(!gti_test::emitCpp(field, standard).contents.empty(),
             "owning enum field discriminants and payload reads use exact "
             "checked place emission");
    }
  }
  for (const std::string declaration :
       {"enum E { item(P value), empty };",
        "enum Family<T> { item(T value), empty }; using E = Family<P>;"}) {
    const auto passive = lang::Frontend().analyze(
        "passive-class-enum-publications.gti", std::string(R"(
class P {
  mut int number = 0;
public:
  P(int number) : number(number) {}
  int get() { return this.number; }
};
)") + declaration + R"(
int main() {
  E fresh = E::item(P(7));
  P source = P(9);
  E copied = E::item(source);
  switch (fresh) {
    case E::item(value): if (value.get() != 7) { return 1; } break;
    case E::empty: return 2;
  }
  switch (copied) {
    case E::item(auto& value): return value.get() - 9;
    case E::empty: return 3;
  }
}
)");
    expect(passive.canGenerateCode(),
           "passive class enum construction, copy and copied extraction "
           "retain exact MIR identities");
    if (!passive.canGenerateCode()) {
      for (const auto &diagnostic : passive.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      expect(!gti_test::emitCpp(passive, standard).contents.empty(),
             "fused passive class publications do not invent SSA storage");
    }
  }
  const auto sibling =
      lang::Frontend().analyze("same-typed-payload-siblings.gti", R"(
enum Event<T> { item(T value) };
class Inner {
public:
  Event<int> data;
  Inner(Event<int> data) : data(data) {}
};
enum Pair<T> { items(T first, T second), other(T first, T second) };
int main() {
  Inner left = Inner(Event<int>::item(1));
  Inner right = Inner(Event<int>::item(2));
  Pair<Inner> pair = Pair<Inner>::items(left, right);
  switch (pair) {
    case Pair<Inner>::items(auto& first, second):
      switch (first.data) {
        case Event<int>::item(auto& value):
          if (value != 1) { return 1; }
          break;
      }
      switch (second.data) {
        case Event<int>::item(value): return value - 2;
      }
    case Pair<Inner>::other: return 3;
  }
}
)");
  expect(sibling.canGenerateCode(),
         "same-typed payload siblings retain nested exact selector identity");
  if (!sibling.canGenerateCode()) {
    for (const auto &diagnostic : sibling.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  } else {
    bool selectorProof = false;
    bool variantRejected = false;
    bool typeRejected = false;
    for (const auto &function : sibling.mir.functionInstances()) {
      const auto &original = function.body;
      const lang::MirInstruction *first = nullptr;
      const lang::MirInstruction *second = nullptr;
      const lang::MirInstruction *nested = nullptr;
      for (const auto &block : original.blocks)
        for (const auto &instruction : block.instructions) {
          if (instruction.operation != lang::MirOperation::PayloadExtract)
            continue;
          if (instruction.info.type.kind == lang::SemanticType::Class) {
            if (instruction.info.category == lang::ValueCategory::Place)
              first = &instruction;
            else
              second = &instruction;
          } else if (instruction.info.category == lang::ValueCategory::Place)
            nested = &instruction;
        }
      if (!first || !second || !nested)
        continue;
      const auto firstPlace =
          std::find_if(original.places.begin(), original.places.end(),
                       [&](const auto &place) {
                         return place.root == lang::MirPlaceRootKind::Value &&
                                place.value == first->result &&
                                place.projections.empty();
                       });
      if (firstPlace == original.places.end())
        continue;
      const auto firstLoan = std::find_if(
          original.loans.begin(), original.loans.end(),
          [&](const auto &loan) { return loan.source == firstPlace->id; });
      if (firstLoan == original.loans.end())
        continue;
      auto body = original;
      body.values[*second->result - 1].info.category =
          lang::ValueCategory::Place;
      auto findInstruction =
          [&](lang::MirInstructionId id) -> lang::MirInstruction * {
        for (auto &block : body.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.id == id)
              return &instruction;
        return nullptr;
      };
      auto *secondExtract = findInstruction(second->id);
      secondExtract->info.category = lang::ValueCategory::Place;
      auto secondPlace = *firstPlace;
      secondPlace.id = body.places.size() + 1;
      secondPlace.value = *second->result;
      secondPlace.sourceValue = second->hirValue;
      body.places.push_back(secondPlace);
      auto loan = *firstLoan;
      loan.id = body.loans.size() + 1;
      loan.source = secondPlace.id;
      loan.carriers.clear();
      body.loans.push_back(loan);
      auto secondSubject = *body.findPlace(nested->operands.front().place);
      secondSubject.id = body.places.size() + 1;
      secondSubject.root = lang::MirPlaceRootKind::Loan;
      secondSubject.binding = 0;
      secondSubject.symbol = 0;
      secondSubject.value = 0;
      secondSubject.loan = loan.id;
      if (!secondSubject.projections.empty() &&
          secondSubject.projections.front().kind ==
              lang::MirProjectionKind::Dereference)
        secondSubject.projections.erase(secondSubject.projections.begin());
      body.places.push_back(secondSubject);
      auto firstSubjectAlias = secondSubject;
      firstSubjectAlias.id = body.places.size() + 1;
      firstSubjectAlias.loan = firstLoan->id;
      body.places.push_back(firstSubjectAlias);
      const auto subject = nested->operands.front().place;
      selectorProof =
          lang::mirCanonicalPlaceContains(body, subject,
                                          firstSubjectAlias.id) &&
          lang::mirCanonicalPlaceContains(body, firstSubjectAlias.id,
                                          subject) &&
          !lang::mirCanonicalPlaceContains(body, firstPlace->id,
                                           secondPlace.id) &&
          !lang::mirCanonicalPlaceContains(body, subject, secondSubject.id);
      secondExtract->payloadIndex = 0;
      secondExtract->enumVariant = 1;
      selectorProof = selectorProof &&
                      !lang::mirCanonicalPlaceContains(body, firstPlace->id,
                                                       secondPlace.id);
      secondExtract->enumVariant = 0;
      const auto wrongType = lang::SemanticType::enumType(
          *first->enumOwner, {lang::SemanticType::Int32});
      secondExtract->operands.front().type = wrongType;
      selectorProof = selectorProof &&
                      !lang::mirCanonicalPlaceContains(body, firstPlace->id,
                                                       secondPlace.id);
      auto forgedVariant = sibling.mir;
      auto forgedType = sibling.mir;
      auto forgedSubject = sibling.mir;
      for (auto &target : lang::MirProgramTestAccess::functions(forgedVariant))
        if (target.id == function.id)
          for (auto &block : target.body.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.id == first->id)
                instruction.enumVariant = 1;
      for (auto &target : lang::MirProgramTestAccess::functions(forgedType))
        if (target.id == function.id)
          for (auto &block : target.body.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.id == first->id)
                instruction.operands.front().type = wrongType;
      // The matching switch still guards first.data. Substituting an equal-
      // typed sibling subject must fail even though its enum tag could match.
      secondExtract->operands.front().type = second->operands.front().type;
      secondExtract->payloadIndex = 1;
      findInstruction(nested->id)->operands.front().place = secondSubject.id;
      for (auto &target : lang::MirProgramTestAccess::functions(forgedSubject))
        if (target.id == function.id)
          target.body = body;
      const auto subjectValidation = lang::verifyMirProgram(forgedSubject);
      selectorProof =
          selectorProof && !subjectValidation.valid() &&
          std::any_of(subjectValidation.errors.begin(),
                      subjectValidation.errors.end(), [&](const auto &error) {
                        return error.owner == function.id &&
                               error.instruction == nested->id &&
                               error.message.find("payload-enum instruction") !=
                                   std::string::npos;
                      });
      variantRejected = !lang::verifyMirProgram(forgedVariant).valid();
      typeRejected = !lang::verifyMirProgram(forgedType).valid();
      break;
    }
    expect(selectorProof && variantRejected && typeRejected,
           "exact selectors survive Binding/Loan aliases and reject forged "
           "same-typed siblings, alternatives and concrete enum arguments");
  }
  const auto finite =
      lang::Frontend().analyze("finite-enum-family-nesting.gti", R"(
enum Event<T> { item(T value) };
struct Leaf { int value; };
struct B { Event<Leaf> value; };
struct A { Event<B> value; };
int main() { return 0; }
)");
  expect(finite.canGenerateCode(),
         "finite nesting of distinct enum instances is not a layout cycle");
  if (finite.canGenerateCode()) {
    expect(finite.mir.payloadEnums().size() == 2,
           "both nested exact instances are discovered even without execution");
    for (const auto &enumeration : finite.mir.payloadEnums()) {
      expect(
          lang::mirTypeRelocationKind(finite.mir, enumeration.type) ==
                  lang::RelocationKind::Structural &&
              enumeration.traits.relocation == lang::RelocationKind::Structural,
          "relocation recursion is keyed by exact enum type, not primary ID");
    }
    auto recursive = finite.mir;
    auto &cycle = lang::MirProgramTestAccess::enums(recursive).front();
    cycle.variants.front().payloadTypes.front() = cycle.type;
    expect(lang::mirTypeRelocationKind(recursive, cycle.type) ==
                   lang::RelocationKind::Unavailable &&
               !lang::verifyMirProgram(recursive).valid(),
           "an actual repeated exact enum instance remains a rejected layout "
           "cycle");
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      expect(!gti_test::emitCpp(finite, standard).contents.empty(),
             "finite alternating class/enum layouts reach verified emission");
    }
  } else {
    for (const auto &diagnostic : finite.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  const auto aliases =
      lang::Frontend().analyze("enum-alias-layout-authority.gti", R"(
struct Phantom<T> { T value; };
struct Actual<T> { T value; };
enum Metadata<T> { absent, number(int32_t value) };
enum Payload<T> { item(Actual<T> value) };
using ClassOnly = Phantom<int32_t>;
using EnumOnly = Metadata<Phantom<int32_t>>;
using ActualPayload = Payload<int32_t>;
using NestedEnum = Phantom<Metadata<bool>>;
int main() { return 0; }
)");
  expect(aliases.canGenerateCode(),
         "type-only aliases retain valid exact enum identities");
  if (aliases.canGenerateCode()) {
    expect(aliases.hir.classInstances().size() == 1 &&
               aliases.hir.classInstances().front().source->name().lexeme ==
                   "Actual" &&
               aliases.mir.classInstances().size() == 1 &&
               aliases.mir.payloadEnums().size() == 3,
           "alias arguments do not instantiate phantom class layouts, while "
           "actual enum alternatives retain their concrete class layout");
    const auto aliasLowered =
        gti_test::lowerProgram(aliases, aliases.mir, aliases.mir);
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      const auto text = gti_test::emitCpp(aliases, standard).contents;
      bool classForwardOnly = false;
      for (const auto &row : aliasLowered.declarations()) {
        const auto *alias =
            std::get_if<lang::LoweredTypeAliasDeclaration>(&row.payload);
        if (alias == nullptr)
          continue;
        if (alias->type.kind == lang::SemanticType::Enum) {
          const auto name =
              lang::cppConcreteEnumName(aliasLowered, alias->type);
          expect(name &&
                     text.find("struct " + *name + " {") != std::string::npos,
                 "enum-only aliases publish their exact concrete enum rows");
        } else if (alias->type.kind == lang::SemanticType::Class) {
          const auto name =
              lang::cppConcreteClassName(aliasLowered, alias->type);
          expect(name &&
                     text.find("struct " + *name + ";") != std::string::npos &&
                     text.find("struct " + *name + " {") == std::string::npos,
                 "class aliases publish forward identities, never layouts");
          classForwardOnly = true;
        }
      }
      expect(classForwardOnly,
             "the alias fixture checks a phantom concrete class forward");
    }
  } else {
    for (const auto &diagnostic : aliases.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  // A generic call can name an enum instance only in its type arguments. No
  // declaration seeds that instance, yet MIR verifies its exact catalog row.
  for (const bool reachableOnly : {false, true}) {
    const auto identity = lang::Frontend({.lowerReachableOnly = reachableOnly})
                              .analyze("enum-type-argument-identity.gti", R"(
struct Phantom<T> { T value; };
enum Metadata<T> { absent, number(int32_t value) };
int tag<T>() { return 7; }
int main() { return tag<Metadata<Phantom<int32_t>>>() - 7; }
)");
    if (!identity.canGenerateCode())
      for (const auto &diagnostic : identity.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    const auto &functions = identity.mir.functionInstances();
    const auto tag = std::find_if(functions.begin(), functions.end(),
                                  [](const auto &function) {
                                    return function.typeArguments.size() == 1;
                                  });
    expect(identity.canGenerateCode() && tag != functions.end() &&
               identity.mir.payloadEnums().size() == 1 &&
               identity.mir.findPayloadEnum(tag->typeArguments.front()) ==
                   &identity.mir.payloadEnums().front() &&
               (reachableOnly || identity.mir.classInstances().empty()),
           "an enum named only by a type argument publishes its exact row "
           "without requesting a phantom class layout in complete lowering");
  }
  const auto packs = lang::Frontend().analyze("enum-pack-traits.gti", R"(
enum Signal<T> { None, Value(T value) };
enum Tag { Zero, One };
struct Packet<T> {
  T value;
  Packet(T item) : value(item) {}
};
T construct<T, Args...>(Args... args) { return T(args...); }
int main() {
  auto empty = construct<Packet<Signal<int>>>(Signal<int>::None);
  auto number = construct<Packet<Signal<int>>>(Signal<int>::Value(7));
  auto flag = construct<Packet<Signal<bool>>>(Signal<bool>::Value(true));
  auto tag = construct<Packet<Tag>>(Tag::One);
  return 0;
}
)");
  expect(packs.canGenerateCode() && lang::verifyMirProgram(packs.mir).valid(),
         "concrete enum packs preserve exact nominal traits through source "
         "construction without library-specific rules");
  std::size_t enumElements = 0;
  if (packs.canGenerateCode()) {
    expect(!gti_test::emitCppText(packs).empty(),
           "enum packs execute through verified MIR");
    for (const auto &function : packs.mir.functionInstances()) {
      for (const auto &place : function.body.places) {
        if (place.type.kind != lang::SemanticType::Enum ||
            place.projections.size() != 1 ||
            place.projections.front().kind !=
                lang::MirProjectionKind::PackElement)
          continue;
        ++enumElements;
        const auto *enumeration = packs.mir.findPayloadEnum(place.type);
        expect(place.traits == (enumeration ? enumeration->traits
                                            : lang::semanticTraits(place.type)),
               "pack traits use the exact concrete enum catalogue row");
        auto malformed = packs.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &forged = changed->body.places[place.id - 1];
        forged.traits.copyable = !forged.traits.copyable;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "program verification rejects forged enum pack traits");
        if (enumeration) {
          malformed = packs.mir;
          auto &catalogue = lang::MirProgramTestAccess::enums(malformed);
          std::erase_if(catalogue, [&](const auto &row) {
            return row.type == place.type;
          });
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "an enum pack cannot fall back to scalar traits when its "
                 "exact concrete catalogue row is missing");
        }
      }
    }
  } else {
    for (const auto &diagnostic : packs.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(enumElements >= 3,
         "the fixture exercises integral and distinct payload enum pack types");
  const auto owningPack = lang::Frontend().analyze("owning-enum-pack.gti", R"(
struct Resource {
  int value;
  Resource(int initial) : value(initial) {}
  Resource(Resource& other) = delete;
  Resource(Resource&& other) = default;
  ~Resource() {}
};
enum Message { Value(Resource value) };
void discard<Args...>(Args... args) {}
int main() { discard(Message::Value(Resource(1))); return 0; }
)");
  expect(!owningPack.canGenerateCode() &&
             std::any_of(
                 owningPack.diagnostics.begin(), owningPack.diagnostics.end(),
                 [](const auto &item) { return item.code == "GTI-S2067"; }) &&
             std::none_of(
                 owningPack.diagnostics.begin(), owningPack.diagnostics.end(),
                 [](const auto &item) { return item.code == "GTI-B0001"; }),
         "unsupported owning-enum packs retain their source diagnostic");
  const auto referenceEntry = lang::Frontend().analyze(
      "enum-reference-entry.gti", R"gti(
enum Reading { empty, value(int& owner) };
bool is_ready(Reading& reading) {
  switch (reading) {
  case Reading::empty: return false;
  case Reading::value: return true;
  }
}
int main() {
  int owner = 7;
  auto reading = Reading::value(owner);
  return is_ready(reading) ? 0 : 1;
}
)gti",
      {compositionLibrary / "prelude.gti"}, {}, {compositionLibrary});
  expect(referenceEntry.canGenerateCode(),
         "reference enum formal has independently verified guarded inventory");
  if (referenceEntry.canGenerateCode()) {
    for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
      expect(!gti_test::emitCpp(referenceEntry, standard).contents.empty(),
             "abstract enum reference inventory does not require native class "
             "fields");
    const auto referenceLowered = gti_test::lowerProgram(
        referenceEntry, referenceEntry.mir, referenceEntry.mir);
    const lang::CppMirBodyEmissionMap referenceMap(
        lang::buildCppMirBodyEmissionMapRows(referenceLowered,
                                             lang::CppStandard::Cpp23));
    for (const bool entryAddress : {false, true}) {
      auto addressed = referenceEntry.mir;
      bool tested = false;
      for (auto &function : lang::MirProgramTestAccess::functions(addressed)) {
        for (const auto &place : function.body.places) {
          if (tested || place.projections.size() != 3 ||
              place.projections[1].kind != lang::MirProjectionKind::Field)
            continue;
          tested = true;
          lang::MirLoan addressLoan;
          addressLoan.id = function.body.loans.size() + 1;
          addressLoan.kind = entryAddress ? lang::MirLoanKind::Parameter
                                          : lang::MirLoanKind::Local;
          addressLoan.source = place.id;
          addressLoan.entry = entryAddress;
          function.body.loans.push_back(addressLoan);
          if (!entryAddress) {
            lang::MirInstruction borrow;
            borrow.id = 10000;
            borrow.kind = lang::MirInstructionKind::Borrow;
            borrow.loan = addressLoan.id;
            borrow.info.type = place.type;
            borrow.operands.push_back({.kind = lang::MirOperandKind::Loan,
                                       .loan = 2,
                                       .type = place.type});
            function.body.blocks.front().instructions.insert(
                function.body.blocks.front().instructions.begin(), borrow);
          }
          const lang::CppMirBodyEmitter rejected(addressed, referenceMap);
          expect(!rejected.supportsBodyText(
                     {lang::MirBodyKind::Function, function.id}),
                 "an abstract enum path cannot hide a runtime Borrow or entry "
                 "address");
        }
      }
      expect(tested,
             "runtime address rejection fixture finds its abstract path");
    }
    auto forged = referenceEntry.mir;
    bool changed = false;
    for (auto &function : lang::MirProgramTestAccess::functions(forged))
      for (auto &place : function.body.places)
        if (!changed && place.sourceValue == 0 &&
            place.root == lang::MirPlaceRootKind::Binding &&
            place.projections.size() == 3 &&
            place.projections[1].kind == lang::MirProjectionKind::Field) {
          place.projections[1].field = 0;
          changed = true;
        }
    expect(changed && !lang::verifyMirProgram(forged).valid(),
           "provenance-only enum path still requires an exact typed payload "
           "field");
  }
  const auto publishedCall = lang::Frontend().analyze(
      "owning-enum-call-publication.gti", R"gti(
#include <std/string>
enum Packet { number(int value), text(std::string value) };
Packet produce(bool text) {
  if (text) return Packet::text(std::string("owned publication"));
  return Packet::number(7);
}
int main() {
  auto packet = produce(false);
  switch (packet) {
    case Packet::number(value): return value - 7;
    case Packet::text(auto& value): return int(value.size());
  }
}
)gti",
      {compositionLibrary / "prelude.gti"}, {}, {compositionLibrary});
  expect(publishedCall.canGenerateCode(),
         "fallible enum result has a checked immediate binding publication");
  if (publishedCall.canGenerateCode()) {
    for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      const auto text = gti_test::emitCpp(publishedCall, standard).contents;
      expect(text.find("payload call published in its binding") !=
                 std::string::npos,
             "enum call constructs directly in its sole binding destination");
    }
    auto delayed = publishedCall.mir;
    bool delayedPublication = false;
    for (auto &function : lang::MirProgramTestAccess::functions(delayed)) {
      lang::MirInstruction literal;
      lang::MirValue literalValue;
      lang::MirInstructionId nextInstruction = 1;
      lang::MirValueId nextValue = 1;
      for (const auto &value : function.body.values)
        nextValue = std::max(nextValue, value.id + 1);
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          nextInstruction = std::max(nextInstruction, instruction.id + 1);
          if (instruction.operation == lang::MirOperation::Literal &&
              instruction.result) {
            literal = instruction;
            literalValue = *function.body.findValue(*instruction.result);
          }
        }
      if (!literal.result)
        continue;
      for (auto &block : function.body.blocks) {
        if (delayedPublication || block.instructions.empty())
          continue;
        const auto &first = block.instructions.front();
        if (first.kind != lang::MirInstructionKind::Initialize ||
            first.operands.size() != 1 ||
            first.operands.front().type.kind != lang::SemanticType::Enum)
          continue;
        literal.id = nextInstruction;
        literal.result = nextValue;
        literalValue.id = nextValue;
        literalValue.definition = nextInstruction;
        literalValue.definitionBlock = block.id;
        function.body.values.push_back(literalValue);
        block.instructions.insert(block.instructions.begin(), literal);
        delayedPublication = true;
      }
      expect(lang::rebuildMirValueUses(function.body),
             "delayed publication rebuilds exact value use index");
    }
    const auto delayedProof = lang::verifyMirProgram(delayed);
    const bool validDelayed = delayedPublication && delayedProof.valid();
    if (!validDelayed)
      for (const auto &error : delayedProof.errors)
        std::cerr << "delayed: " << error.message << '\n';
    expect(validDelayed,
           "a pure intervening value keeps delayed publication legal");
    if (validDelayed) {
      const auto text =
          gti_test::emitCpp(publishedCall, delayed, delayed).contents;
      expect(text.find("payload call published in its binding") ==
                     std::string::npos &&
                 text.find("end payload-enum value representation") !=
                     std::string::npos,
             "non-immediate publication retains the ordinary representation "
             "fallback");
    }
    for (int mutation = 0; mutation < 3; ++mutation) {
      auto malformed = publishedCall.mir;
      bool changed = false;
      for (auto &function : lang::MirProgramTestAccess::functions(malformed))
        for (auto &block : function.body.blocks)
          for (auto &instruction : block.instructions) {
            if (changed ||
                instruction.kind != lang::MirInstructionKind::Initialize ||
                instruction.operands.size() != 1 ||
                instruction.operands.front().type.kind !=
                    lang::SemanticType::Enum ||
                instruction.lifecycle.size() != 1)
              continue;
            auto &transfer = instruction.lifecycle.front();
            if (transfer.kind != lang::MirLifecycleEventKind::Reparent)
              continue;
            changed = true;
            if (mutation == 0)
              transfer.source = transfer.target;
            else if (mutation == 1)
              transfer.conditional = true;
            else {
              auto *destination =
                  function.body.findPlace(*instruction.destination);
              for (auto &drop : function.body.dropObligations)
                if (drop.place == destination->id)
                  drop.initiallyActive = true;
            }
          }
      expect(
          changed && !lang::verifyMirProgram(malformed).valid(),
          "direct publication does not waive source obligation, unconditional "
          "transfer, or inactive destination proofs");
    }
  }
  const auto movedPublication = lang::Frontend().analyze(
      "owning-enum-move-publication.gti", R"gti(
#include <std/string>
enum Packet { number(int value), text(std::string value) };
int main() {
  auto packet = Packet::text(std::string("owned publication"));
  switch (std::move(packet)) {
    case Packet::number(value): return value;
    case Packet::text(value): return int(value.size()) - 17;
  }
}
)gti",
      {compositionLibrary / "prelude.gti"}, {}, {compositionLibrary});
  expect(movedPublication.canGenerateCode(),
         "consuming switch uses independently checked enum move publication");
  if (movedPublication.canGenerateCode()) {
    for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
      expect(gti_test::emitCpp(movedPublication, standard)
                     .contents.find("payload move published in its binding") !=
                 std::string::npos,
             "enum move keeps its source shell but directly constructs its "
             "adjacent binding destination");
    for (int mutation = 0; mutation < 3; ++mutation) {
      auto malformed = movedPublication.mir;
      bool changed = false;
      for (auto &function : lang::MirProgramTestAccess::functions(malformed))
        for (auto &block : function.body.blocks)
          for (auto &instruction : block.instructions) {
            if (changed || instruction.kind != lang::MirInstructionKind::Move ||
                instruction.info.type.kind != lang::SemanticType::Enum ||
                instruction.lifecycle.size() != 1)
              continue;
            changed = true;
            auto &transfer = instruction.lifecycle.front();
            if (mutation == 0)
              transfer.source = transfer.target;
            else if (mutation == 1)
              transfer.target = transfer.source;
            else {
              // A duplicated publication cannot create a second drop authority.
              for (auto &candidate : block.instructions)
                if (candidate.kind == lang::MirInstructionKind::Initialize &&
                    candidate.operands.size() == 1 &&
                    candidate.operands.front().value == *instruction.result)
                  candidate.lifecycle.front().source = transfer.source;
            }
          }
      expect(changed && !lang::verifyMirProgram(malformed).valid(),
             "move publication preserves distinct source/value/binding "
             "obligations and cannot duplicate source cleanup authority");
    }
  }
  return failures == 0 ? 0 : 1;
}
