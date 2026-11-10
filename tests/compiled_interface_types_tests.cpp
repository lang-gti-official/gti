#include "gti/compiled_interface_names.h"
#include "gti/compiled_interface_types.h"
#include "gti/compiled_scalar_unit.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include <iostream>
#include <stdexcept>
using namespace lang;
namespace lang {
struct LoweredProgramTestAccess {
  static auto &declarations(LoweredProgram &p) { return p.declarations_; }
};
} // namespace lang
namespace {
void expect(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class F> void rejects(F action) {
  try {
    action();
  } catch (const std::invalid_argument &) {
    return;
  }
  throw std::runtime_error("accepted malformed nominal contract");
}
LoweredProgram lower(std::string source, std::string dependency = {}) {
  const auto root = std::filesystem::current_path();
  std::unordered_map<std::string, std::string> overlays;
  if (!dependency.empty())
    overlays.emplace((root / "type-dependency.gti").string(),
                     std::move(dependency));
  auto frontend =
      Frontend().analyze(root / "interface-types.gti", source, {}, overlays);
  if (!frontend.canGenerateCode()) {
    for (const auto &d : frontend.diagnostics)
      std::cerr << d.message << '\n';
    throw std::runtime_error("nominal fixture did not lower:\n" + source);
  }
  auto result = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  expect(result.valid(), "fixture must satisfy lowered contract");
  return std::move(*result.program);
}
std::pair<SemanticType, SourceUnitId>
valueType(const LoweredProgram &program, std::string_view name = "Value") {
  for (const auto &d : program.declarations())
    if (const auto *c = std::get_if<LoweredClassDeclaration>(&d.payload);
        c && c->qualifiedName == name)
      return {SemanticType::classType(c->id), c->sourceUnit};
  throw std::runtime_error("missing Value declaration");
}
void namespacedTypes() {
  const std::string source = R"(export module namespaced;
export namespace one {
  struct Value { int number = 7; Value() = default; };
  enum class State : uint8_t { ready = 3 };
  Value echo(Value v) { return v; }
}
namespace two {
  export struct Value { one::Value inner; one::State state = one::State::ready; Value() = default; };
  export Value echo(Value v) { return v; }
  int hidden(int v) { return v; }
}
)";
  auto program = lower(source);
  const auto [type, unit] = valueType(program, "two::Value");
  const PersistentDeclarationOwner owner{"names", "api", "namespaced", {}};
  const std::vector<std::string> names{"one::echo", "two::echo"};
  const auto signatures =
      CompiledScalarUnitBuilder().selectExports(program, unit, owner, names);
  expect(signatures.size() == 2, "inherited namespace export was lost");
  const auto metadata = signatures.front().typeMetadata;
  const auto facade = compiledInterfaceTypeSource(metadata);
  expect(facade.find("namespace one") != std::string::npos &&
             facade.find("namespace two") != std::string::npos,
         "qualified declarations lost namespace scopes");
  auto other =
      lower("export module namespaced;\n" + facade +
            "namespace one { export Value echo(Value v) { return v; } }\n"
            "namespace two { export Value echo(Value v) { return v; } }\n");
  const auto [otherType, otherUnit] = valueType(other, "two::Value");
  expect(projectCompiledInterfaceTypes(other, otherUnit, owner,
                                       std::span(&otherType, 1)) == metadata,
         "namespace projection changed layout or inherited visibility");
  const auto registry = compiledInterfaceTypeRegistry(program, unit, metadata);
  const auto otherRegistry =
      compiledInterfaceTypeRegistry(other, otherUnit, metadata);
  expect(readPersistentType(writePersistentType(type, registry),
                            otherRegistry) == otherType,
         "qualified nominal identity did not remap");
  rejects([&] {
    (void)CompiledScalarUnitBuilder().selectExports(
        program, unit, owner, std::vector<std::string>{"two::hidden"});
  });
  for (const auto name :
       {"::one", "one::", "one:::two", "one::2bad", "one.two", "one::bad;"}) {
    expect(!isCompiledInterfaceName(name), "malformed qualified name accepted");
    rejects([&] { (void)compiledInterfaceNamespaceSource(name, "int x;\n"); });
  }
  std::string deep = "name";
  for (unsigned i = 1; i < 64; ++i)
    deep += "::name";
  expect(isCompiledInterfaceName(deep) &&
             !isCompiledInterfaceName(deep + "::name") &&
             isCompiledInterfaceName(std::string(1024, 'a')) &&
             !isCompiledInterfaceName(std::string(1025, 'a')),
         "qualified name resource bounds changed");
  auto malformed = metadata;
  malformed.replace(malformed.find("one::Value"), 10, "one:::alue");
  rejects([&] { (void)compiledInterfaceSyntheticRegistry(malformed); });
}
void shadowedNamespaceTypes() {
  auto program = lower(R"(export module shadows;
namespace origin {
  export struct Value { int number = 7; Value() = default; };
  export enum class State : uint8_t { ready = 3 };
}
using RootValue = origin::Value;
using RootState = origin::State;
namespace client {
  namespace origin {
    export struct Value { bool flag = true; Value() = default; };
    export enum class State : uint8_t { ready = 9 };
  }
  export struct Value {
    RootValue external; origin::Value local;
    RootState state = RootState::ready;
    origin::State localState = origin::State::ready;
    Value() = default;
  };
  export Value echo(Value value) { return value; }
}
namespace gtilib_type_0_ { export int count(int value) { return value; } }
)");
  const auto [type, unit] = valueType(program, "client::Value");
  const PersistentDeclarationOwner owner{"shadows", "api", "shadows", {}};
  const auto signatures = CompiledScalarUnitBuilder().selectExports(
      program, unit, owner,
      std::vector<std::string>{"client::echo", "gtilib_type_0_::count"});
  const auto source = compiledInterfaceSource(signatures);
  expect(source.find("using gtilib_type_1_") != std::string::npos,
         "private aliases collided with an interface namespace");
  auto roundTrip = lower("export module shadows;\n" + source);
  const auto [other, otherUnit] = valueType(roundTrip, "client::Value");
  const auto &metadata = signatures.front().typeMetadata;
  auto captured =
      lower("export module shadows;\n" + compiledInterfaceTypeSource(metadata));
  const auto capturedUnit = valueType(captured, "client::Value").second;
  rejects([&] {
    (void)compiledInterfaceTypeRegistry(captured, capturedUnit, metadata);
  });
  expect(projectCompiledInterfaceTypes(roundTrip, otherUnit, owner,
                                       std::span(&other, 1)) == metadata,
         "relative namespace lookup captured a canonical nominal type");
  (void)compiledInterfaceTypeRegistry(roundTrip, otherUnit, metadata);
  auto nested = lower(R"(export module nested;
export struct Outer { struct Value { int number = 7; }; };
)");
  const auto [nestedType, nestedUnit] = valueType(nested, "Outer::Value");
  rejects([&] {
    (void)projectCompiledInterfaceTypes(nested, nestedUnit, owner,
                                        std::span(&nestedType, 1));
  });
}
void dependencyOwnedTypes() {
  const std::string dependency = R"(export module dependency;
namespace model {
  struct Hidden { int secret = 11; Hidden() = default; };
  export enum class Tag : uint8_t { ready = 7 };
  export struct Point { mut int x = 3; private: Hidden hidden; public: Point() = default; };
  export Point echo(Point value) { return value; }
  export Tag tag(Tag value) { return value; }
}
)";
  const PersistentDeclarationOwner baseOwner{"test", "model", "dependency", {}};
  const PersistentDeclarationOwner derivedOwner{
      "test", "derived", "derived", {}};
  auto base = lower(dependency);
  const auto [baseType, baseUnit] = valueType(base, "model::Point");
  const auto baseSignatures = CompiledScalarUnitBuilder().selectExports(
      base, baseUnit, baseOwner,
      std::vector<std::string>{"model::echo", "model::tag"});
  auto derived = lower(R"(export module derived;
import "type-dependency.gti";
namespace derived {
  export struct Packet { model::Point point; model::Tag tag = model::Tag::ready; Packet() = default; };
  export Packet echo(Packet value) { return value; }
  export model::Point forward(model::Point value) { return value; }
}
)",
                       dependency);
  const auto [packet, unit] = valueType(derived, "derived::Packet");
  const auto importedUnit = valueType(derived, "model::Point").second;
  std::vector<CompiledScalarImport> imports;
  for (const auto &signature : baseSignatures)
    imports.push_back({signature, importedUnit});
  const std::vector<std::string> names{"derived::echo", "derived::forward"};
  rejects([&] {
    (void)CompiledScalarUnitBuilder().selectExports(derived, unit, derivedOwner,
                                                    names);
  });
  const auto signatures = CompiledScalarUnitBuilder().selectExports(
      derived, unit, derivedOwner, names, imports);
  const auto metadata = signatures.front().typeMetadata;
  expect(compiledInterfaceDependencies(metadata) == std::vector{baseOwner},
         "dependency nominal ownership was lost");
  auto projected = lower(
      "export module derived; import \"type-dependency.gti\";\n" +
          compiledInterfaceSource(signatures),
      "export module dependency;\n" + compiledInterfaceSource(baseSignatures));
  const auto [other, otherUnit] = valueType(projected, "derived::Packet");
  const auto modelUnit = valueType(projected, "model::Point").second;
  std::vector<CompiledInterfaceUnit> units{{otherUnit, derivedOwner},
                                           {modelUnit, baseOwner}};
  const auto registry =
      compiledInterfaceTypeRegistry(projected, otherUnit, metadata, units);
  expect(projectCompiledInterfaceTypes(projected, otherUnit, derivedOwner,
                                       std::span(&other, 1), units) == metadata,
         "forwarded layout or owner changed during import");
  rejects([&] {
    (void)compiledInterfaceTypeRegistry(projected, otherUnit, metadata);
  });
  units.back().owner.target = "impostor";
  rejects([&] {
    (void)compiledInterfaceTypeRegistry(projected, otherUnit, metadata, units);
  });
  units.back() = units.front();
  rejects([&] { validateCompiledInterfaceUnits(units); });
}
} // namespace
int main() {
  try {
    namespacedTypes();
    shadowedNamespaceTypes();
    dependencyOwnedTypes();
    const std::string source = R"(export module types;
export enum class Tag : uint8_t { idle = 0, ready = 7 };
export struct Inner { mut int number = 0; Inner() = default; };
export struct Value { mut Inner inner; mut Tag tag = Tag::idle; private: int hidden = 3; };
export Value identity(Value value) { return value; }
)";
    auto program = lower(source);
    auto [type, unit] = valueType(program);
    const PersistentDeclarationOwner owner{"types", "types", "types", {}};
    const auto bytes = projectCompiledInterfaceTypes(program, unit, owner,
                                                     std::span(&type, 1));
    expect(!bytes.empty(), "nominal closure was lost");
    const auto registry = compiledInterfaceTypeRegistry(program, unit, bytes);
    const auto recipe = writePersistentType(type, registry);
    const auto facade = compiledInterfaceTypeSource(bytes);
    expect(facade.find("private:") != std::string::npos,
           "private field access was lost");
    expect(facade.find("identity") == std::string::npos,
           "implementation leaked into metadata");
    auto roundTrip =
        lower("export module types;\n" + facade +
              "export Value identity(Value value) { return value; }\n");
    auto [roundType, roundUnit] = valueType(roundTrip);
    expect(projectCompiledInterfaceTypes(roundTrip, roundUnit, owner,
                                         std::span(&roundType, 1)) == bytes,
           "source projection changed nominal construction or layout");
    auto changedOrder =
        lower("export module types; struct Before { int unused = 0; };\n" +
              source.substr(source.find('\n') + 1));
    auto [other, otherUnit] = valueType(changedOrder);
    expect(other.classId != type.classId, "fixture must remap nominal IDs");
    const auto mapped =
        compiledInterfaceTypeRegistry(changedOrder, otherUnit, bytes);
    expect(readPersistentType(recipe, mapped) == other,
           "persistent recipe failed ID remapping");
    for (auto malformed : {std::string("unknown"),
                           bytes.substr(0, bytes.size() - 1), bytes + "0:"})
      rejects([&] { (void)compiledInterfaceSyntheticRegistry(malformed); });
    auto changed = program;
    for (auto &d : LoweredProgramTestAccess::declarations(changed))
      if (auto *f = std::get_if<LoweredStorageDeclaration>(&d.payload);
          f && f->ownerClass == type.classId) {
        f->type = SemanticType::Int64;
        break;
      }
    rejects([&] { (void)compiledInterfaceTypeRegistry(changed, unit, bytes); });
    changed = program;
    for (auto &d : LoweredProgramTestAccess::declarations(changed))
      if (auto *f = std::get_if<LoweredStorageDeclaration>(&d.payload);
          f && f->ownerClass == type.classId && f->initializerConstant) {
        f->initializerConstant = ConstantValue{true};
        break;
      }
    rejects([&] { (void)compiledInterfaceTypeRegistry(changed, unit, bytes); });
    expect(!verifyLoweredProgram(changed).empty(),
           "unsealed initializer change accepted");
    auto unsupported = source;
    unsupported.replace(
        unsupported.find("int hidden = 3;"),
        std::string("int hidden = 3;").size(),
        "int hidden = 3; public: int get() { return this.hidden; }");
    auto withMethod = lower(unsupported);
    auto [methodType, methodUnit] = valueType(withMethod);
    rejects([&] {
      (void)projectCompiledInterfaceTypes(withMethod, methodUnit, owner,
                                          std::span(&methodType, 1));
    });
    changed = program;
    for (auto &d : LoweredProgramTestAccess::declarations(changed))
      if (auto *c = std::get_if<LoweredClassDeclaration>(&d.payload);
          c && c->id == type.classId)
        c->explicitlyExported = false;
    rejects([&] { (void)compiledInterfaceTypeRegistry(changed, unit, bytes); });
    expect(!verifyLoweredProgram(changed).empty(),
           "unsealed export visibility change accepted");
    std::cout
        << "nominal closure, remapping, privacy and mutation checks passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
