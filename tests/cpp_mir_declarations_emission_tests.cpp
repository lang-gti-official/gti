// Frozen C++-backend representation checks moved out of the declarations
// compiler suite. Each case asserts a C++ spelling (declaration keywords,
// access labels, special-member syntax, include selection, mangled names)
// that has no backend-neutral meaning. The language behaviour these programs
// exercise is asserted on MIR in compiler_declarations_tests.cpp.

#include "compiler_test_fixture.h"
#include "cpp_backend_test_support.h"
#include "native_symbol_spelling.h"
#include "temporary_directory.h"

namespace {

[[maybe_unused]] std::string
emitAnalyzedSource(std::string_view source,
                   lang::TargetInfo target = lang::TargetInfo::host(),
                   lang::CppStandard standard = lang::CppStandard::Cpp23) {
  const lang::FrontendResult frontend =
      lang::Frontend({.target = target})
          .analyze("compiler-emission-fixture.gti", std::string(source));
  expect(frontend.canGenerateCode(),
         "a semantically valid emission fixture should complete MIR "
         "lowering");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected emission-fixture diagnostic: "
                << diagnostic.message << '\n';
    }
    return {};
  }
  return gti_test::emitCppText(frontend, standard, std::move(target));
}

void testClassesStructsAndAccessCppEmission() {
  const std::string validSource = R"(
class Vault {
  int secret = 7;

public:
  int reveal() { return this.secret; }
  int reveal_other(Vault other) { return other.secret; }
public:
  Vault() = default;
};

struct Reading {
  int value = 1;

private:
  int hidden = 2;

public:
  int total() { return this.value + this.hidden; }
public:
  Reading() = default;
};

int open(mut Vault vault) { return vault.reveal(); }
int read(Reading reading) { return reading.value; }
int main() {
  Vault vault{};
  Reading reading{};
  return vault.reveal() + reading.total() - 10;
}
)";
  const lang::FrontendResult classFrontend = lang::Frontend().analyze(
      "classes-and-access.gti", validSource, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(classFrontend.canGenerateCode(),
         "access-controlled declaration defaults should retain verified MIR");
  if (!classFrontend.canGenerateCode())
    return;
  const std::string generated = gti_test::emitCppText(classFrontend);
  expect(generated.find("class Vault;") != std::string::npos &&
             generated.find("struct Reading;") != std::string::npos &&
             generated.find("class Vault {") != std::string::npos &&
             generated.find("struct Reading {") != std::string::npos &&
             generated.find("std::int32_t __gti_fn_") != std::string::npos &&
             generated.find("_reveal() const;") != std::string::npos &&
             generated.find("private:\n    std::int32_t hidden;") !=
                 std::string::npos &&
             generated.find("= (*this).hidden;") != std::string::npos,
         "emitter should preserve declaration kinds, access labels, and "
         "frontend-owned field immutability");
}

void testConstructorsAndReceiverMutabilityCppEmission() {
  const std::string validSource = R"(
class Counter {
  mut int value;
  int step = 1;

public:
  Counter() : value(0) {}
  Counter(int initial) : value(initial) {}
  Counter(bool reset) : value(0) {}
  int read() { return this.value; }
  int advance(int amount) mut {
    this.value += amount;
    return this.value;
  }
};

struct Origin {
  int x = 0;
  Origin(int initial) : x(initial) {}
public:
  Origin() = default;
};

class LifecycleValue {
  int value = 0;

public:
  LifecycleValue(LifecycleValue& other) = default;
  LifecycleValue(LifecycleValue&& other) = default;
public:
  LifecycleValue() = default;
};

class MoveOnlyPolicy {
  int value = 0;

public:
  MoveOnlyPolicy(MoveOnlyPolicy& other) = delete;
  MoveOnlyPolicy(MoveOnlyPolicy&& other) = default;
public:
  MoveOnlyPolicy() = default;
};

int inspect(Counter counter) { return counter.read(); }
int main() {
  Counter zero = Counter();
  Counter fixed = Counter(1);
  mut int observed = fixed.read();
  mut Counter moving = Counter(observed);
  observed = moving.advance(2);
  Origin origin = Origin();
  Origin shifted = Origin(4);
  Counter reset = Counter(true);
  LifecycleValue source{};
  LifecycleValue copied = LifecycleValue(source);
  mut LifecycleValue movable{};
  LifecycleValue moved = LifecycleValue(std::move(movable));
  mut MoveOnlyPolicy owner{};
  MoveOnlyPolicy transferred = MoveOnlyPolicy(std::move(owner));
  return observed + origin.x;
}
)";
  const lang::FrontendResult validFrontend = lang::Frontend().analyze(
      "constructors-and-receivers.gti", validSource, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  expect(validFrontend.canGenerateCode(),
         "explicit construction and mutable receiver calls should validate");
  if (!validFrontend.canGenerateCode())
    return;
  const lang::BackendArtifact generatedArtifact =
      gti_test::emitCpp(validFrontend, validFrontend.mir, validFrontend.mir,
                        lang::OptimizationPipeline().run(
                            validFrontend.hir, lang::OptimizationLevel::O0));
  const std::string &generated = generatedArtifact.contents;
  const std::string &lifecycleGenerated = generatedArtifact.contents;
  expect(generated.find("explicit Counter(const std::int32_t initial);") !=
                 std::string::npos &&
             generated.find("Counter::Counter(std::int32_t __gti_mir_arg_0) : "
                            "value(__gti_mir_arg_0)") != std::string::npos,
         "constructor overloads should lower explicitly with field "
         "initialization");
  expect(generated.find("std::int32_t __gti_fn_") != std::string::npos &&
             generated.find("_read() const") != std::string::npos,
         "methods should lower as read-only by default");
  expect(generated.find("_advance(const std::int32_t amount) const") ==
                 std::string::npos &&
             generated.find("_advance(const std::int32_t amount)") !=
                 std::string::npos,
         "mutable receiver methods should lower without C++ const");
  const std::string originSlotPrefix =
      "mir_lifetime_slot<::__gti_program::Origin> ";
  const std::size_t originSlotAt = generated.find(originSlotPrefix);
  const std::size_t originSlotNameAt =
      originSlotAt == std::string::npos
          ? std::string::npos
          : originSlotAt + originSlotPrefix.size();
  const std::size_t originSlotNameEnd =
      originSlotNameAt == std::string::npos
          ? std::string::npos
          : generated.find(';', originSlotNameAt);
  const std::string originSlot =
      originSlotNameEnd == std::string::npos
          ? std::string{}
          : generated.substr(originSlotNameAt,
                             originSlotNameEnd - originSlotNameAt);
  expect(!originSlot.empty() &&
             generated.find(originSlot + ".construct();", originSlotNameEnd) !=
                 std::string::npos,
         "generated default construction should remain explicit at the call "
         "site");
  expect(
      lifecycleGenerated.find("Origin();") != std::string::npos &&
          lifecycleGenerated.find("Origin::Origin()") != std::string::npos &&
          lifecycleGenerated.find("Counter(const Counter &other) = default;") !=
              std::string::npos &&
          lifecycleGenerated.find("Counter(Counter &&) = default;") !=
              std::string::npos &&
          lifecycleGenerated.find(
              "Counter &operator=(const Counter &other) = default;") !=
              std::string::npos &&
          lifecycleGenerated.find(
              "Counter &operator=(Counter &&) = default;") !=
              std::string::npos &&
          lifecycleGenerated.find("~Counter() noexcept = default;") !=
              std::string::npos &&
          lifecycleGenerated.find(
              "LifecycleValue(const LifecycleValue &other) = default;") !=
              std::string::npos &&
          lifecycleGenerated.find(
              "LifecycleValue(LifecycleValue &&) = default;") !=
              std::string::npos &&
          lifecycleGenerated.find(
              "MoveOnlyPolicy(const MoveOnlyPolicy &other) = delete;") !=
              std::string::npos,
      "the backend should explicitly emit compiler-generated special "
      "members");
}

void testInheritanceAndInterfacesCppEmission() {
  const std::string source = R"(
interface Renderable {
  int render(int frame);
};

interface Named {
  int name_id();
};

class Entity {
public:
  Entity(int id) : id(id) {}
  virtual int tick(int frame) { return frame + this.id; }
  virtual int tick(float frame) { return 10; }

private:
  int id;
};

class Sprite : public Entity, public Renderable, public Named {
public:
  Sprite(int id) : Entity(id) {}
  int tick(int frame) override { return frame + 2; }
  int inherited_tick() { return tick(1.5); }
  int render(int frame) override { return this.tick(frame); }
  int name_id() override { return 7; }
};

interface Reader<T> {
  T read();
};

class Box<T> : public Reader<T> {
  T value;

public:
  Box(T initial) : value(initial) {}
  T read() override { return this.value; }
};

class ClassContract {
public:
  virtual int class_only() = 0;
public:
  ClassContract() = default;
};

class ClassImplementation : public ClassContract {
public:
  int class_only() override { return 1; }
public:
  ClassImplementation() = default;
};

int invoke(Renderable& renderable) {
  return renderable.render(3);
}

int invoke_direct(Sprite& sprite) {
  return sprite.render(3);
}

int main() {
  Sprite sprite{4};
  Box<int> box{3};
  Reader<int>& reader = box;
  int inherited_overload = sprite.tick(1.5);
  return invoke(sprite) + invoke_direct(sprite) + inherited_overload +
         sprite.inherited_tick() + reader.read() - 33;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("inheritance.gti", source);
  expect(frontend.canGenerateCode(),
         "public state, interface, and generic inheritance should pass the "
         "frontend");
  if (!frontend.canGenerateCode())
    return;
  const lang::HirClassInstance *spriteHir = nullptr;
  for (const lang::HirClassInstance &owner : frontend.hir.classInstances())
    if (owner.source != nullptr && owner.source->name().lexeme == "Sprite")
      spriteHir = &owner;
  const lang::MirClassInstance *entityMir = nullptr;
  for (const lang::HirClassInstance &owner : frontend.hir.classInstances())
    if (owner.source != nullptr && owner.source->name().lexeme == "Entity")
      entityMir = frontend.mir.findClassInstance(owner.id);
  const lang::MirConstructorInstance *spriteConstructor = nullptr;
  const lang::MirConstructorInstance *entityConstructor = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (spriteHir != nullptr && constructor.owner == spriteHir->id)
      spriteConstructor = &constructor;
    if (entityMir != nullptr && constructor.owner == entityMir->id)
      entityConstructor = &constructor;
  }
  const lang::BackendArtifact generatedArtifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir,
                        lang::OptimizationPipeline().run(
                            frontend.hir, lang::OptimizationLevel::O0));
  const std::string &generated = generatedArtifact.contents;
  constexpr std::string_view nameIdDefinition =
      "std::int32_t Sprite::name_id() const {";
  const std::size_t firstNameIdDefinition = generated.find(nameIdDefinition);
  // The verified base stage now uses the status-result constructor overload,
  // not a plain C++ base call. Derive both tags from selected MIR identities.
  const std::string selectedBaseDefinition =
      spriteConstructor != nullptr && entityConstructor != nullptr
          ? "Sprite::Sprite(::gti_internal::backend::mir_failure_constructor_"
            "tag_v1<" +
                std::to_string(spriteConstructor->id) +
                ">, std::int32_t __gti_mir_arg_0, bool "
                "*__gti_mir_constructor_success, ::gti_failure_record_v2 "
                "*__gti_mir_failure_record) : ::__gti_program::Entity("
                "::gti_internal::backend::mir_failure_constructor_tag_v1<" +
                std::to_string(entityConstructor->id) +
                ">{}, __gti_mir_arg_0, __gti_mir_constructor_success, "
                "__gti_mir_failure_record)"
          : std::string{};
  expect(
      generated.find(
          "class Sprite : public ::__gti_program::Entity, public "
          "::__gti_program::Renderable, public ::__gti_program::Named") !=
              std::string::npos &&
          generated.find("virtual std::int32_t render(") != std::string::npos &&
          generated.find("render(const std::int32_t frame) const override") !=
              std::string::npos &&
          generated.find("virtual std::int32_t class_only() const = 0;") !=
              std::string::npos &&
          generated.find("class_only() const override") != std::string::npos &&
          generated.find(" = 0;") != std::string::npos &&
          generated.find("virtual ~Renderable() noexcept = default;") !=
              std::string::npos &&
          generated.find("explicit Sprite(const std::int32_t id);") !=
              std::string::npos &&
          !selectedBaseDefinition.empty() &&
          generated.find(selectedBaseDefinition) != std::string::npos &&
          generated.find("virtual bool read__gti_mir_failure(") !=
              std::string::npos &&
          generated.find("read__gti_mir_failure(std::int32_t "
                         "*__gti_mir_out_result") != std::string::npos &&
          generated.find("_Box::read(std::add_pointer_t<std::int32_t> "
                         "__gti_mir_out_result) const") != std::string::npos &&
          firstNameIdDefinition != std::string::npos &&
          generated.find(nameIdDefinition,
                         firstNameIdDefinition + nameIdDefinition.size()) ==
              std::string::npos &&
          generated.find("static_cast<const ::__gti_program::Entity &>") !=
              std::string::npos,
      "the C++ backend should emit inheritance, pure contracts, overrides, "
      "base construction, polymorphic destruction, and GTI-selected virtual "
      "overloads");
}

void testDirectBraceConstructionCppEmission() {
  const std::string source = R"(
class Box<T> {
  T value;

public:
  Box(T initial) : value(initial) {}
  T read() { return this.value; }
};

using IntBox = Box<int>;

class Holder {
  Box<int> box{3};

public:
  int read() { return this.box.read(); }
public:
  Holder() = default;
};

class Pair {
  int left;
  int right;

public:
  Pair(int left, int right) : left(left), right(right) {}
  int sum() { return this.left + this.right; }
};

struct Point {
  int x = 4;
public:
  Point() = default;
};

int main() {
  Box<int> boxed{1};
  mut Box<int> changing{2};
  IntBox aliased{3,};
  Holder holder{};
  Point point{};
  Pair pair{5, 6,};
  return boxed.read() + changing.read() + aliased.read() + holder.read() +
         point.x + pair.sum();
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("direct-initialization.gti", source);
  expect(frontend.canGenerateCode(),
         "class direct initialization should pass the shared frontend");
  if (!frontend.canGenerateCode())
    return;
  const std::string generated = gti_test::emitCppText(frontend);
  expect(generated.find("mir_lifetime_slot<::__gti_program::__gti_type_") !=
                 std::string::npos &&
             generated.find(".construct(__gti_mir_v_") != std::string::npos &&
             generated.find("mir_lifetime_slot<::__gti_program::Holder>") !=
                 std::string::npos &&
             generated.find(".construct();") != std::string::npos &&
             generated.find("mir_lifetime_slot<::__gti_program::Pair>") !=
                 std::string::npos &&
             generated.find("Pair::Pair(std::int32_t __gti_mir_arg_0, "
                            "std::int32_t __gti_mir_arg_1)") !=
                 std::string::npos,
         "the backend should lower braces through explicit constructor calls");
}

void testNamedGenericsCppEmission() {
  const std::string source = R"(
class Box<T> {
  mut T value;

public:
  Box(T value) : value(value) {}
  T get() { return this.value; }
  U echo<U>(U replacement) { return replacement; }
  void set(T replacement) mut { this.value = replacement; }
};

T identity<T>(T value) { return value; }
T unbox<T>(Box<T> box) { return box.get(); }
U relay<T, U>(Box<T> box, U value) { return box.echo<U>(value); }

int main() {
  mut Box<int> box = Box<int>(identity(7));
  box.set(identity<int>(9));
  int value = unbox(box);
  int relayed = relay(box, box.echo<int>(value));
  std::string_view text = identity<std::string_view>("generic");
  return relayed;
}

)";
  const lang::FrontendResult validFrontend = lang::Frontend().analyze(
      "named-generics.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(validFrontend.canGenerateCode(),
         "generic substitution and exact inference should validate");
  if (!validFrontend.canGenerateCode())
    return;
  const std::string generated = gti_test::emitCppText(validFrontend);
  expect(generated.find("_Box;") != std::string::npos &&
             generated.find("_Box {") != std::string::npos &&
             generated.find("template <typename T>\n  class Box") ==
                 std::string::npos,
         "a selected nameable generic class should lower with matching "
         "GTI-owned concrete forward and full declarations");
  expect(generated.find("__gti_inst_") != std::string::npos &&
             containsMangledFunction(generated, "", "identity",
                                     "(std::int32_t __gti_mir_arg_0)") &&
             containsMangledFunction(generated, "", "identity",
                                     "(std::string_view __gti_mir_arg_0)") &&
             containsMangledFunction(generated, "", "unbox", ""),
         "nameable generic functions should lower as GTI-selected concrete "
         "functions");
  expect(
      generated.find("mir_lifetime_slot<::__gti_program::__gti_type_") !=
              std::string::npos &&
          generated.find("_identity(__gti_mir_v_") != std::string::npos &&
          generated.find("_Box::__gti_member_") != std::string::npos &&
          generated.find("_set(") != std::string::npos &&
          containsMangledFunction(generated, "", "identity", "(__gti_mir_v_") &&
          generated.find("_set(__gti_mir_v_") != std::string::npos,
      "applied types and explicit generic calls should lower recursively");
  const auto sourceNamespace = generated.find("namespace __gti_program {");
  expect(sourceNamespace != std::string::npos,
         "concrete source declarations retain their GTI program namespace");
  expect(generated.find(".template __gti_fn_") == std::string::npos &&
             generated.find("_Box::__gti_member_") != std::string::npos &&
             generated.find("_echo(__gti_mir_v_") != std::string::npos &&
             sourceNamespace != std::string::npos &&
             generated.find("template <> ", sourceNamespace) ==
                 std::string::npos,
         "the backend should emit the concrete selected generic method "
         "without reopening C++ dependent-template lookup");
}

void testNativeGenericCarrierClosureCppEmission() {
  const std::string source = R"(
class Leaf<T> {
  T value;

public:
  Leaf(T value) : value(value) {}
  T get() { return this.value; }
};

class DormantCarrier<T> {
  Leaf<T> value;

public:
  DormantCarrier(Leaf<T> value) : value(value) {}
};

int main() {
  Leaf<int> value = Leaf<int>(7);
  return value.get();
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("native-generic-carrier-closure.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected carrier-closure diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "a dormant dependent generic carrier should validate");
  if (!frontend.canGenerateCode())
    return;
  const std::string generated = gti_test::emitCppText(frontend);
  expect(
      concreteBackendSymbols(generated, "__gti_type_", "Leaf").size() == 1 &&
          generated.find("template <typename T>\n  class Leaf;") !=
              std::string::npos &&
          generated.find("template <typename T>\n  class Leaf {") !=
              std::string::npos &&
          generated.find("template <typename T>\n  class DormantCarrier {") !=
              std::string::npos,
      "a retained native carrier should close over every dependent generic "
      "family while concrete calls keep their direct representation");
}

void testUninstantiatedGenericSignatureClosureCppEmission() {
  const auto frontend =
      lang::Frontend().analyze("generic-signature-closure.gti", R"(
struct Result<T> { T value; };
class Dormant<T> {
  Result<char> field = {.value = 'x'};
public:
  Dormant(Result<int8_t> input) {}
  Result<int> result();
  void accept(Result<bool> input);
  Result<T> dependent(Result<T> input) { return input; }
public:
  Dormant() = default;
};
int main() { return 0; }
)");
  expect(frontend.canGenerateCode(),
         "unused generic signatures should reach verified MIR");
  if (!frontend.canGenerateCode())
    return;
  const auto generated = gti_test::emitCppText(frontend);
  expect(concreteBackendSymbols(generated, "__gti_type_", "Result").size() == 4,
         "each concrete declaration signature type must have a representation "
         "identity");
}

void testDependentCarrierRepresentationClosureCppEmission() {
  const auto frontend = analyzeTrustedPreludeFixture("carrier-closure.gti", R"(
struct Leaf<T> { T value; Leaf(T value) : value(std::move(value)) {} };
struct Wrapper<T> { Leaf<T> leaf; Wrapper(T value) : leaf(Leaf<T>(std::move(value))) {} };
struct Independent<T> { T value; };
class Owner<T> {
  using value_type = T;
public:
  Owner() {}
  Wrapper<T> result(T value) { return Wrapper<T>(std::move(value)); }
  Independent<int> concrete();
};
int main() { Owner<int> owner{}; auto value = owner.result(42); return value.leaf.value - 42; }
)");
  expect(frontend.canGenerateCode(), "dependent carrier closure must verify");
  if (!frontend.canGenerateCode())
    return;
  const auto generated = gti_test::emitCppText(frontend);
  expect(
      concreteBackendSymbols(generated, "__gti_type_", "Owner").empty() &&
          concreteBackendSymbols(generated, "__gti_type_", "Wrapper").empty() &&
          concreteBackendSymbols(generated, "__gti_type_", "Leaf").empty(),
      "dependent owner result and transitive field must share native "
      "template representation with concrete MIR signatures");
  expect(
      concreteBackendSymbols(generated, "__gti_type_", "Independent").size() ==
          1,
      "fully concrete signature references must retain direct representation");
}

void testExactQualifiedClassSpecializationsCppEmission() {
  const std::string source = R"(
class demo::Value<int> {
public:
  int read() { return 2; }
public:
  Value() = default;
};

namespace demo {
class Value<T> {
public:
  int read() { return 1; }
public:
  Value() = default;
};

class Count<uint64_t N> {
public:
  int read() { return 1; }
public:
  Count() = default;
};
}

int read_exact(demo::Value<int>& value) { return value.read(); }

class demo::Count<7> {
public:
  int read() { return 7; }
public:
  Count() = default;
};

int read_through_generic<T>(demo::Value<T>& value) {
  return value.read();
}

int main() {
  demo::Value<int> exact{};
  demo::Value<bool> primary{};
  demo::Count<7> exact_value{};
  demo::Count<8> primary_value{};
  return read_exact(exact) + read_through_generic<int>(exact) +
         primary.read() + exact_value.read() + primary_value.read() - 13;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("exact-qualified-specialization.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected exact-specialization diagnostic: "
                << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "a qualified exact class specialization should replace its primary "
         "for the matching canonical application");
  if (frontend.canGenerateCode()) {
    const std::string generated = gti_test::emitCppText(frontend);
    const std::size_t firstValue = generated.find("_Value {");
    const std::size_t secondValue =
        firstValue == std::string::npos
            ? std::string::npos
            : generated.find("_Value {", firstValue + 1);
    const std::size_t firstCount = generated.find("_Count {");
    const std::size_t secondCount =
        firstCount == std::string::npos
            ? std::string::npos
            : generated.find("_Count {", firstCount + 1);
    expect(generated.find("_Value;") != std::string::npos &&
               firstValue != std::string::npos &&
               secondValue != std::string::npos &&
               firstCount != std::string::npos &&
               secondCount != std::string::npos &&
               generated.find("Value<std::") == std::string::npos &&
               generated.find("Count<") == std::string::npos,
           "the C++ representation should emit ordinary concrete classes for "
           "both primary applications and selected exact specializations");
  }
}

void testConstrainedGenericsCppEmission() {
  const std::string source = R"(
T minimum<std::ordered T>(T left, T right) {
  if (left < right) { return left; }
  return right;
}

T multiply<std::numeric T>(T left, T right) {
  return T(left * right);
}

T remainder<std::integral T>(T left, T right) {
  return T(left % right);
}

bool is_zero<std::integral T>(T value) {
  return value == (0) and (0) == value;
}

T increment<std::integral T>(T value) {
  return T(value + 1);
}

T low_bits<std::integral T>(T value) {
  return T(value & 3);
}

T negate<std::signed_numeric T>(T value) { return T(-value); }
T signed_integer<std::signed_integral T>(T value) { return value; }
T unsigned_integer<std::unsigned_integral T>(T value) { return value; }
T floating_value<std::floating_point T>(T value) { return value; }

T square_integral<std::integral T>(T value) {
  return multiply(value, value);
}

void consume_integrals<std::integral Values...>(Values... values) {}

class NumericBox<std::numeric T> {
  T value;

public:
  NumericBox(T value) : value(value) {}
  T doubled() { return T(this.value + this.value); }
};

int main() {
  int low = minimum(2, 5);
  int product = multiply(3, 4);
  int rest = remainder(7, 4);
  bool zero = is_zero(uint64_t(0));
  uint64_t incremented = increment(uint64_t(41));
  uint8_t bits = low_bits(uint8_t(7));
  int negative = negate(2);
  int signed_value = signed_integer(2);
  uint64_t unsigned_value = unsigned_integer(uint64_t(2));
  float decimal = floating_value(2.5);
  int square = square_integral(5);
  consume_integrals(1, uint64_t(2));
  NumericBox<int> box = NumericBox<int>(6);
  if (low == 2 and product == 12 and rest == 3 and zero and
      incremented == 42 and bits == 3 and negative == -2 and
      signed_value == 2 and unsigned_value == uint64_t(2) and
      decimal == 2.5 and square == 25 and box.doubled() == 12) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult valid = lang::Frontend().analyze(
      "constrained-generics.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected constrained generic diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "standard numeric constraints should validate generic operations, "
         "propagation, packs, and classes");
  if (!valid.canGenerateCode())
    return;
  const std::string generated = gti_test::emitCppText(valid);
  expect(
      generated.find("template <typename T>\n  T __gti_fn_") ==
              std::string::npos &&
          containsMangledFunction(generated, "", "minimum",
                                  "(std::int32_t __gti_mir_arg_0, std::int32_t "
                                  "__gti_mir_arg_1)") &&
          generated.find("_minimum<std::int32_t>(") == std::string::npos &&
          generated.find("mir_checked_multiply_v1<std::int32_t>") !=
              std::string::npos &&
          generated.find("requires __gti_std::") == std::string::npos,
      "constraints should remain frontend metadata while generic numeric "
      "conversions lower through checked backend casts");
  expect(generated.find("static_cast<std::uint64_t>(0)") != std::string::npos &&
             generated.find("static_cast<std::uint8_t>(3)") !=
                 std::string::npos,
         "the C++ backend should explicitly preserve contextual generic "
         "integer literal types");
}

void testConstrainedGenericCapabilitiesCppEmission() {
  lang::FrontendResult capabilities = lang::Frontend().analyze(
      "exact-generic-capabilities.gti", R"(
class Rank {
  int value;

public:
  Rank() : value(0) {}
  Rank(int value) : value(value) {}
  int get() { return this.value; }

  bool operator==(Rank& other) { return this.value == other.value; }
  bool operator!=(Rank& other) { return this.value != other.value; }
  bool operator<(Rank& other) { return this.value < other.value; }
  bool operator<=(Rank& other) { return this.value <= other.value; }
  bool operator>(Rank& other) { return this.value > other.value; }
  bool operator>=(Rank& other) { return this.value >= other.value; }
};

class MoveOnly {
  int value;

public:
  MoveOnly() : value(0) {}
  MoveOnly(int value) : value(value) {}
  MoveOnly(MoveOnly& other) = delete;
  MoveOnly(MoveOnly&& other) = default;
};

bool equivalent<std::equality_comparable T>(T left, T right) {
  return left == right;
}

T earlier<std::totally_ordered T>(T left, T right) {
  if (left < right) { return left; }
  return right;
}

T legacy_earlier<std::ordered T>(T left, T right) {
  return earlier(left, right);
}

T copy_value<std::copyable T>(T value) { return value; }
T move_value<std::movable T>(T value) { return std::move(value); }
T default_value<std::default_initializable T>() { return T(); }
bool ordered_equal<std::totally_ordered T>(T left, T right) {
  return equivalent(left, right);
}
T numeric_copy<std::numeric T>(T value) { return copy_value(value); }
T copy_move<std::copyable T>(T value) {
  return move_value(std::move(value));
}

int main() {
  Rank first = Rank(1);
  Rank second = Rank(2);
  bool same = equivalent(first, first);
  Rank lower = earlier(first, second);
  Rank legacy = legacy_earlier(first, second);
  Rank copied = copy_value(first);
  Rank moved_copy = copy_move(first);
  Rank zero = default_value<Rank>();
  int copied_number = numeric_copy(4);
  MoveOnly source = MoveOnly(3);
  MoveOnly moved = move_value(std::move(source));
  if (same and ordered_equal(first, first) and lower.get() == 1 and
      legacy.get() == 1 and copied.get() == 1 and moved_copy.get() == 1 and
      zero.get() == 0 and copied_number == 4) {
    return 0;
  }
  return 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!capabilities.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : capabilities.diagnostics) {
      std::cerr << "Unexpected exact capability diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(capabilities.canGenerateCode(),
         "exact lifecycle and comparison capabilities should accept "
         "matching primitives and nominal classes");
  if (!capabilities.canGenerateCode())
    return;
  lang::HirConstructorInstanceId defaultRankConstructor = 0;
  for (const lang::HirConstructorInstance &instance :
       capabilities.hir.constructorInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "Rank" &&
        instance.parameterTypes.empty()) {
      defaultRankConstructor = instance.id;
      break;
    }
  }
  const std::string capabilityCpp = gti_test::emitCppText(capabilities);
  // The failure-free default_value<Rank> publishes its exact default
  // construction as its plain return expression.
  expect(capabilityCpp.find("operator<(") != std::string::npos &&
             capabilityCpp.find("operator>=(") != std::string::npos &&
             capabilityCpp.find("return ::__gti_program::Rank();") !=
                 std::string::npos &&
             capabilityCpp.find("constructor-instance " +
                                std::to_string(defaultRankConstructor)) !=
                 std::string::npos &&
             capabilityCpp.find("concept ") == std::string::npos,
         "the C++ backend should emit selected relational methods and generic "
         "default construction without C++ concepts");
}

void testConcurrencyTypeCapabilitiesCppEmission() {
  const std::string source = R"(
import std.cstdio;
import std.memory;

struct Scalar {
  int value = 0;
public:
  Scalar() = default;
};

class Box<T> {
  T value;
public:
  Box(T input) : value(input) {}
};

class Node {
  int value = 0;
  mut std::unique_ptr<Node> next = std::unique_ptr<Node>();
public:
  Node() = default;
};

class RawBox {
  int* address = nullptr;
public:
  RawBox() = default;
};

[[no_transfer, unsafe_share]] class PolicyValue {
  int value = 0;
public:
  PolicyValue() = default;
};

[[unsafe_transfer]] class AuditedCleanup {
  int* address = nullptr;
public:
  ~AuditedCleanup() {}
public:
  AuditedCleanup() = default;
};

[[transfer, share]] interface ConcurrentValue {
  int read();
};

class ConcurrentScalar : public ConcurrentValue {
  int value = 0;
public:
  int read() override { return this.value; }
public:
  ConcurrentScalar() = default;
};

T transfer_value<std::transferable T>(T value) {
  return std::move(value);
}

void share_value<std::shareable T>(T& value) {}
int inspect_transfer<std::transferable T>(T value) { return value(); }
T relay_transfer<std::transferable T>(T value) {
  return transfer_value(std::move(value));
}
void relay_share<std::shareable T>(T& value) { share_value(value); }

int main() {
  Scalar scalar = transfer_value(Scalar());
  Node node = relay_transfer(Node());
  std::unique_ptr<Scalar> owner = std::make_unique<Scalar>();
  std::unique_ptr<Scalar> moved_owner = transfer_value(std::move(owner));
  share_value(scalar);
  relay_share(node);
  share_value(moved_owner);
  Box<int> integer_box = Box<int>(1);
  Box<std::string_view> view_box = Box<std::string_view>("view");
  expected<Scalar, int> result = Scalar();
  int values[2] = {};
  int* addresses[2] = {nullptr, nullptr};
  auto callable = [scalar]() -> int { return 1; };
  int callable_result = inspect_transfer(callable);
  ConcurrentScalar concurrent = ConcurrentScalar();
  share_value(concurrent);
  AuditedCleanup cleanup = transfer_value(AuditedCleanup());
  return callable_result - 1;
}
)";

  lang::FrontendResult valid = lang::Frontend().analyze(
      "concurrency-type-capabilities.gti", source, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected concurrency capability diagnostic: "
                << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "structural transfer/share capabilities should accept scalars, "
         "recursive owners, reviewed cleanup, interfaces, and safe captures");
  if (!valid.canGenerateCode())
    return;
  const std::string generated = gti_test::emitCppText(valid);
  const std::size_t emittedProgram =
      generated.find("namespace __gti_program {");
  const std::string_view generatedProgram =
      emittedProgram == std::string::npos
          ? std::string_view(generated)
          : std::string_view(generated).substr(emittedProgram);
  const std::size_t constrainedCallableBody =
      generatedProgram.find("// GTI verified-MIR body: deduced-callable");
  const std::size_t constrainedCallable =
      constrainedCallableBody == std::string_view::npos
          ? std::string_view::npos
          : generatedProgram.rfind("inspect_transfer", constrainedCallableBody);
  expect(constrainedCallableBody != std::string_view::npos &&
             constrainedCallable != std::string_view::npos &&
             constrainedCallableBody - constrainedCallable < 512,
         "frontend-checked generic constraints should retain a verified-MIR "
         "source-template body for an unnameable callable instance");
  // Source-defined reflection methods such as is_transferable/is_shareable
  // are ordinary declarations. Policy attributes and source concept spellings
  // must not become backend traits or constraints.
  expect(generatedProgram.find("unsafe_transfer") == std::string_view::npos &&
             generatedProgram.find("no_transfer") == std::string_view::npos &&
             generatedProgram.find("std::transferable<") ==
                 std::string_view::npos &&
             generatedProgram.find("std::shareable<") == std::string_view::npos,
         "capability policy and constraints should remain frontend facts, not "
         "emitted C++ traits or spellings");
}

void testSourceDefinedConceptsCppEmission() {
  const std::string source = R"(
concept arithmetic_value<T> = std::numeric<T> && std::copyable<T>;
concept signed_arithmetic<T> =
    arithmetic_value<T> and std::signed_numeric<T>;

T numeric_identity<std::numeric T>(T value) { return value; }

T twice<signed_arithmetic T>(T value) {
  return T(value + value);
}

T forward<signed_arithmetic T>(T value) {
  return numeric_identity(value);
}

int main() {
  int result = forward(twice(3));
  if (result == 6) { return 0; }
  return 1;
}
)";
  lang::FrontendResult valid = lang::Frontend().analyze(
      "source-concepts.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected source concept diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "source-defined concept aliases and conjunctions should propagate "
         "their exact capabilities");
  if (!valid.canGenerateCode())
    return;
  const std::string generated = gti_test::emitCppText(valid);
  expect(generated.find("concept arithmetic_value") == std::string::npos &&
             generated.find("concept signed_arithmetic") == std::string::npos &&
             generated.find("template <typename T>") != std::string::npos,
         "concepts should remain frontend validity metadata and not delegate "
         "constraint checking to the C++ backend");
}

void testStructuralRequiresAndAccumulateCppEmission() {
  const std::string source = R"(
import std.iterator;
import std.numeric;
import std.vector;

concept compatible_sentinel<Sentinel, Iterator> =
    std::sentinel_for<Sentinel, Iterator>;

T local_accumulate<Iterator, Sentinel, std::numeric T>(
    mut Iterator first, Sentinel last, mut T init)
  requires std::input_iterator<Iterator> &&
           compatible_sentinel<Sentinel, Iterator> &&
           std::accumulates_into<Iterator, T> {
  for (; first != last; ++first) {
    init = T(std::move(init) + *first);
  }
  return init;
}

T forward_accumulate<Iterator, Sentinel, std::numeric T>(
    mut Iterator first, Sentinel last, mut T init)
  requires std::input_iterator<Iterator> &&
           compatible_sentinel<Sentinel, Iterator> &&
           std::accumulates_into<Iterator, T> {
  return std::accumulate(std::move(first), std::move(last), std::move(init));
}

class NumberSentinel {
  int limit;

public:
  NumberSentinel(int value) : limit(value) {}
  int position() { return this.limit; }
};

class NumberIterator {
  mut int current;

public:
  NumberIterator(int value) : current(value) {}
  int& operator*() { return this.current; }
  void operator++() mut { this.current++; }
  bool operator!=(NumberSentinel& sentinel) {
    return this.current != sentinel.position();
  }
};

int main() {
  int local = local_accumulate(NumberIterator(1), NumberSentinel(5), 0);
  int standard = std::accumulate(NumberIterator(1), NumberSentinel(5), 0);
  int forwarded =
      forward_accumulate(NumberIterator(1), NumberSentinel(5), 0);
  mut std::vector<int> values{};
  values.push_back(1);
  values.push_back(2);
  values.push_back(3);
  int owned = std::accumulate(values.begin(), values.end(), 0);
  values.push_back(4);
  mut std::vector<int8_t> narrowValues{};
  narrowValues.push_back(int8_t(1));
  narrowValues.push_back(int8_t(2));
  int8_t narrow = std::accumulate(
      narrowValues.begin(), narrowValues.end(), int8_t(0));
  return local + standard + forwarded + owned + narrow - 39;
}
)";

  lang::FrontendResult valid = lang::Frontend().analyze(
      "structural-requires.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected structural requires diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "trailing structural requirements should admit an exact iterator, "
         "distinct sentinel, owner-tied std iterator, and numeric "
         "accumulator");
  if (!valid.canGenerateCode())
    return;
  const std::string generated = gti_test::emitCppText(valid);
  expect(
      generated.find("concept compatible_sentinel") == std::string::npos &&
          generated.find("requires __gti_std::") == std::string::npos &&
          generated.find("__gti_operator_dereference") != std::string::npos &&
          generated.find("__gti_operator_pre_increment") != std::string::npos &&
          generated.find("__gti_operator_not_equal") != std::string::npos &&
          generated.find("// GTI verified-MIR body:") != std::string::npos,
      "the C++ backend should emit already-validated operator calls, not "
      "GTI concepts or trailing requires clauses");
}

void testExactSentinelBridgesCppEmission() {
  const lang::FrontendResult exactSentinelBridges = lang::Frontend().analyze(
      "exact-sentinel-bridges.gti", R"(
import std.numeric;
import std.iterator;

class BaseEnd {
public:
  BaseEnd() = default;
};

class DerivedEnd : public BaseEnd {
  int limit;
public:
  DerivedEnd(int value) : limit(value) {}
  int position() { return this.limit; }
};

class OtherEnd { public: OtherEnd() = default; };

class GenericComparable<T, uint64_t N> {
public:
  bool operator!=(GenericComparable<T, N>& other) { return true; }
  bool differs(GenericComparable<T, N>& other) { return this != other; }
public:
  GenericComparable() = default;
};

interface IteratorContract {
  int& operator*();
  void operator++() mut;
  bool operator!=(DerivedEnd& end);
};

class DynamicIterator : public IteratorContract {
  mut int current;
public:
  DynamicIterator(int value) : current(value) {}
  int& operator*() override { return this.current; }
  void operator++() mut override { this.current++; }
  bool operator!=(DerivedEnd& end) override {
    return this.current != end.position();
  }
};

int dynamic_step<Iterator, Sentinel>(mut Iterator& first, Sentinel& last)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> {
  ++first;
  if (first != last) { return *first; }
  return 0;
}

class BaseIterator {
  mut int current;
public:
  BaseIterator(int value) : current(value) {}
  int& operator*() { return this.current; }
  void operator++() mut { this.current++; }
  bool operator!=(DerivedEnd& end) {
    return this.current != end.position();
  }
};

class DerivedIterator : public BaseIterator {
public:
  DerivedIterator(int value) : BaseIterator(value) {}
  bool operator!=(BaseEnd& end) { return false; }
  bool operator!=(mut OtherEnd& end) { return true; }
};

int main() {
  mut DerivedIterator iterator = DerivedIterator(1);
  DerivedEnd end = DerivedEnd(4);
  bool direct = iterator != end;
  if (!direct) { return 1; }
  GenericComparable<int, 4> generic_left = GenericComparable<int, 4>();
  GenericComparable<int, 4> generic_right = GenericComparable<int, 4>();
  if (!generic_left.differs(generic_right)) { return 3; }
  mut DynamicIterator dynamic = DynamicIterator(1);
  mut IteratorContract& view = dynamic;
  if (dynamic_step(view, end) != 2) { return 2; }
  return std::accumulate(DerivedIterator(1), DerivedEnd(4), 0) - 6;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!exactSentinelBridges.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic :
         exactSentinelBridges.diagnostics) {
      std::cerr << "Unexpected exact sentinel bridge diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(exactSentinelBridges.canGenerateCode(),
         "an inherited exact sentinel operation should remain valid beside a "
         "derived convertible overload and mutable-sentinel overload");
  if (!exactSentinelBridges.canGenerateCode())
    return;
  const std::string exactBridgeCpp =
      gti_test::emitCppText(exactSentinelBridges);
  expect(
      exactBridgeCpp.find("::gti_internal::backend::exact_type<const "
                          "::__gti_program::DerivedEnd &>") !=
              std::string::npos &&
          exactBridgeCpp.find("::gti_internal::backend::exact_type<const "
                              "::__gti_program::BaseEnd &>") !=
              std::string::npos &&
          exactBridgeCpp.find("::gti_internal::backend::exact_type<"
                              "::__gti_program::OtherEnd &>") ==
              std::string::npos &&
          exactBridgeCpp.find(
              "friend const std::int32_t & __gti_operator_dereference("
              "const IteratorContract &__gti_receiver)") != std::string::npos &&
          exactBridgeCpp.find(
              "_dynamic_step__gti_mir_failure("
              "::__gti_program::IteratorContract & __gti_mir_arg_0, "
              "const ::__gti_program::DerivedEnd & __gti_mir_arg_1") !=
              std::string_view::npos &&
          exactBridgeCpp.find(
              "exact_type<const ::__gti_program::__gti_type_") !=
              std::string::npos &&
          exactBridgeCpp.find("_GenericComparable &>") != std::string::npos &&
          exactBridgeCpp.find("_GenericComparable & __gti_argument_0") !=
              std::string::npos &&
          exactBridgeCpp.find("GenericComparable<void, 0>") ==
              std::string::npos,
      "sentinel bridges should tag exact read-only parameter types and "
      "must not expose mutable-sentinel overloads");
}

void testConstructorLocalTypeGenericsCppEmission() {
  const std::string source = R"(
import std.string;
import std.memory;

class Capture {
public:
  Capture<T>(T value) {}
public:
  Capture() = default;
};

void discard_constructor_pack<Args...>(Args... args) {}
class PackedCapture {
public:
  PackedCapture<Args...>(int fixed, Args... args) {
    discard_constructor_pack(args...);
  }
public:
  PackedCapture() = default;
};

class Tagged<Stored> {
  Stored stored;

public:
  Tagged<Tag>(Stored value, Tag tag) : stored(value) {}
  Stored read() { return this.stored; }
};

int main() {
  Capture integer = Capture(1);
  Capture boolean = Capture(true);
  PackedCapture empty_pack = PackedCapture(0);
  PackedCapture two_pack = PackedCapture(0, true, 2);
  auto allocated_pack = std::make_unique<PackedCapture>(0, 'a', false);
  Tagged<int> tagged = Tagged<int>(4, true);
  mut std::string source = std::string("hello");
  mut std::string copy = std::string(source.begin(), source.end());
  source.push_back('!');
  if (copy == "hello" and source == "hello!" and tagged.read() == 4) {
    return 0;
  }
  return 1;
}
)";

  const std::filesystem::path entry = gti_test::temporaryDirectory() /
                                      "gti-constructor-local-generics/main.gti";
  const lang::FrontendResult valid = lang::Frontend().analyze(
      entry, source, {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected constructor-local generic diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && valid.mir.valid() &&
             lang::verifyMirProgram(valid.mir).valid(),
         "constructor-local type arguments should infer exactly, satisfy "
         "declared requirements, and lower through verified MIR");
  if (!valid.canGenerateCode())
    return;
  const std::string generated = gti_test::emitCppText(valid);
  // Private backend support may use C++ representation constraints. The
  // source constructor contract itself must remain resolved by GTI.
  const std::size_t emittedProgram =
      generated.find("namespace __gti_program {");
  expect(
      generated.find("explicit Capture(std::int32_t __gti_mir_arg_0);") !=
              std::string::npos &&
          generated.find("explicit Capture(bool __gti_mir_arg_0);") !=
              std::string::npos &&
          concreteBackendSymbols(generated, "__gti_type_", "Tagged").size() ==
              1 &&
          generated.find("Capture(T value)") == std::string::npos &&
          generated.find("string(Iterator first, Sentinel last)") ==
              std::string::npos &&
          emittedProgram != std::string::npos &&
          generated.find("requires std::", emittedProgram) == std::string::npos,
      "the backend should emit each selected constructor as an ordinary "
      "concrete overload without delegating GTI inference or constraints to "
      "C++");
}

void testValueGenericsCppEmission() {
  const std::string source = R"(
constexpr uint64_t default_array_base = 2;
constexpr uint64_t default_array_size = default_array_base + 1;

class StaticArray<T, uint64_t N> {
  T values[N] = {};

public:
  uint64_t size() { return N; }
  T first() { return this.values[0]; }
public:
  StaticArray() = default;
};

class WrappedArray<T, uint64_t N> {
  StaticArray<T, N> value = StaticArray<T, N>();

public:
  uint64_t size() { return this.value.size(); }
public:
  WrappedArray() = default;
};

class DefaultArray<T = int, uint64_t N = default_array_size> {
  T values[N] = {};

public:
  uint64_t size() { return N; }
public:
  DefaultArray() = default;
};

int main() {
  StaticArray<int, 4> four = StaticArray<int, 4>();
  StaticArray<int, 2 + 2> arithmetic = StaticArray<int, 2 + 2>();
  StaticArray<int, 8> eight = StaticArray<int, 8>();
  WrappedArray<int, 4> wrapped = WrappedArray<int, 4>();
  DefaultArray defaulted = DefaultArray();
  if (four.size() == 4 and arithmetic.size() == 4 and eight.size() == 8 and
      wrapped.size() == 4 and four.first() == 0 and defaulted.size() == 3) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult valid =
      lang::Frontend().analyze("value-generics.gti", source);
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected value generic diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "uint64_t value parameters should support class identity and array "
         "extents");
  if (!valid.canGenerateCode())
    return;
  const std::string generated = gti_test::emitCppText(valid);
  expect(
      concreteBackendSymbols(generated, "__gti_type_", "StaticArray").size() ==
              2 &&
          concreteBackendSymbols(generated, "__gti_type_", "WrappedArray")
                  .size() == 1 &&
          concreteBackendSymbols(generated, "__gti_type_", "DefaultArray")
                  .size() == 1 &&
          generated.find("std::array<std::int32_t, 4> values") !=
              std::string::npos &&
          generated.find("std::array<std::int32_t, 8> values") !=
              std::string::npos &&
          generated.find(
              "template <typename T, std::size_t N>\n  class StaticArray") ==
              std::string::npos &&
          generated.find("typename T =") == std::string::npos &&
          generated.find("template <typename T, std::size_t N =") ==
              std::string::npos,
      "the C++ backend should give each GTI-selected mixed type/value "
      "application an ordinary concrete class and should not rely on "
      "native template selection or defaults");
}

void testContextualArrayArgumentsCppEmission() {
  const std::string source = R"(
int total<uint64_t N>(int values[N]) {
  mut int result = 0;
  for (mut uint64_t index = 0; index < N; index++) {
    result += values[index];
  }
  return result;
}

T first<T, uint64_t N>(T fallback, T values[N]) { return values[0]; }

constexpr uint64_t exact_extent<uint64_t N>() { return N; }

int relay_total<uint64_t N>(int values[N]) { return total<N>(values); }

int choose(int values[2]) { return 2; }
int choose(int values[3]) { return 3; }

class Total {
  int stored;

public:
  Total<uint64_t N>(int values[N]) : stored(total(values)) {}
  int read() { return this.stored; }
  int add<uint64_t N>(int values[N]) { return this.stored + total(values); }
};

int main() {
  int summed = total({1, 2, 3});
  int selected = first(0, {4, 5});
  int extent = choose({6, 7, 8});
  Total object = Total({9, 10});
  int method = object.add({2, 3});
  int named_values[2] = {11, 12};
  int named = total(named_values);
  int explicit_total = total<3>({3, 4, 5});
  int explicit_mixed = first<int, 2>(0, {6, 7});
  int relayed = relay_total<2>({8, 9});
  uint64_t explicit_extent = exact_extent<4>();
  int explicit_method = object.add<2>({1, 1});
  return summed + selected + extent + object.read() + method + named +
         explicit_total + explicit_mixed + relayed + int(explicit_extent) +
         explicit_method - 139;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("contextual-arrays.gti", source);
  if (!frontend.canGenerateCode())
    return;
  const std::string generated = gti_test::emitCppText(frontend);
  expect(generated.find("std::initializer_list") == std::string::npos &&
             generated.find("template <std::size_t N>\n  bool __gti_fn_") ==
                 std::string::npos &&
             generated.find("std::array<std::int32_t, 3>{__gti_mir_v_") !=
                 std::string::npos &&
             generated.find("std::array<std::int32_t, 2>{__gti_mir_v_") !=
                 std::string::npos &&
             containsMangledFunction(
                 generated, "", "total",
                 "__gti_mir_failure(std::array<std::int32_t, 3>"),
         "the backend should materialize an explicit std::array value and "
         "never delegate list semantics to C++ overload resolution");
}

void testVariadicGenericsCppEmission() {
  const std::string source = R"(
class Packed<T...> { public: Packed() = default; };
void consume<Args...>(Args... values) {}

void route<Rest...>(int first, Rest... rest) { consume(rest...); }
void route<Rest...>(bool first, Rest... rest) { consume(rest...); }

void relay<Args...>(Args... values) {
  consume(values...);
  consume(0, values...);
  route(1, values...);
}

T first<T, Rest...>(T value, Rest... rest) {
  relay(rest...);
  return value;
}

class Forwarder {
public:
  void send<Args...>(Args... values) {
    relay(values...);
  }
public:
  Forwarder() = default;
};

int main() {
  Packed<int, bool> metadata{};
  consume();
  relay(1, true, "gti");
  int inferred = first(7, false, "tail");
  int explicit_types = first<int, std::string_view>(9, "tail");
  Forwarder forwarder = Forwarder();
  forwarder.send(uint64_t(3), "method");
  if (inferred == 7 and explicit_types == 9) { return 0; }
  return 1;
}
)";

  lang::FrontendResult valid = lang::Frontend().analyze(
      "variadic-generics.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected variadic diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "final function type packs should infer, forward, and preserve exact "
         "concrete MIR parameter roles across equivalent pack groupings");
  if (!valid.canGenerateCode())
    return;
  // None of these forwarding bodies can fail, so each concrete instance
  // lowers in its plain form.
  const std::string generated = gti_test::emitCppText(valid);
  expect(containsMangledFunction(
             generated, "", "consume",
             "(std::int32_t __gti_mir_arg_0, bool __gti_mir_arg_1, "
             "std::string_view __gti_mir_arg_2"),
         "variadic declarations and forwarding should lower explicitly");
  expect(containsMangledFunction(
             generated, "", "consume",
             "(std::int32_t __gti_mir_arg_0, std::int32_t __gti_mir_arg_1, "
             "bool __gti_mir_arg_2, std::string_view __gti_mir_arg_3"),
         "a final symbolic pack should compose with concrete pack elements");
  expect(generated.find("Rest... __gti_mir_arg_1") == std::string::npos &&
             containsMangledFunction(
                 generated, "", "first",
                 "(std::int32_t __gti_mir_arg_0, bool __gti_mir_arg_1, "
                 "std::string_view __gti_mir_arg_2"),
         "fixed generic arguments and explicit pack elements should lower in "
         "source order");
}

void testPackFoldCppEmission() {
  const std::string foldSource = R"(
void touch<T>(mut int& count, T& value) { count++; }
void other<T>(mut int& count, T& value) { count += 10; }

int visit_all<Values...>(Values... values) {
  mut int count = 0;
  (touch(count, values), ...);
  return count;
}

int main() {
  mut int count = 0;
  int value = 1;
  other(count, value);
  return visit_all(1, true, uint64_t(3)) - 3;
}
)";
  const lang::FrontendResult foldFrontend =
      lang::Frontend().analyze("pack-fold.gti", foldSource);
  if (!foldFrontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : foldFrontend.diagnostics) {
      std::cerr << "Unexpected pack-fold diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(foldFrontend.canGenerateCode() &&
             lang::verifyMirProgram(foldFrontend.mir).valid(),
         "a bounded call pack fold should select and lower every concrete "
         "element in source order");
  if (!foldFrontend.canGenerateCode())
    return;
  const lang::FunctionDecl *visitAll =
      findTopLevelFunction(foldFrontend.program, "visit_all");
  const auto *foldStatement =
      visitAll == nullptr ? nullptr
                          : dynamic_cast<const lang::ExpressionStmt *>(
                                visitAll->body()->statements().at(1).get());
  const auto *fold = foldStatement == nullptr
                         ? nullptr
                         : dynamic_cast<const lang::PackFold *>(
                               foldStatement->expression().get());
  const lang::ResolvedPackFoldInfo *resolvedFold =
      fold == nullptr ? nullptr : foldFrontend.semantics.findPackFold(*fold);
  const std::string foldGenerated = gti_test::emitCppText(foldFrontend);
  expect(resolvedFold != nullptr &&
             containsMangledFunction(foldGenerated, "", "touch",
                                     "__gti_mir_failure(__gti_mir_p_") &&
             foldGenerated.find("_touch(count, values), ...") ==
                 std::string::npos,
         "the C++ backend should emit the one compiler-selected fold target "
         "instead of asking native overload resolution to choose per element");
}

void testExactFunctionOverloadsAndConversionsCppEmission() {
  const std::string source = R"(
namespace math {
uint64_t pow(uint64_t base, uint64_t exponent) { return base * exponent; }
float pow(float base, float exponent) { return base * exponent; }
}

struct Selector<T, U> {
  T apply(T value) { return value; }
  U apply(U value) { return value; }
public:
  Selector() = default;
};

struct Receiver {
  mut int value = 0;
  int inspect() { return this.value; }
  int inspect() mut {
    this.value += 1;
    return this.value;
  }
public:
  Receiver() = default;
};

int main() {
  uint64_t base = uint64_t(2);
  uint64_t exponent = uint64_t(8);
  uint64_t whole = math::pow(base, exponent);
  float decimal = math::pow(2.0, 8.0);
  Selector<int, float> selector = Selector<int, float>();
  int selected = selector.apply(2);
  float selected_decimal = selector.apply(2.0);
  Receiver read_only = Receiver();
  mut Receiver writable = Receiver();
  int read_result = read_only.inspect();
  int write_result = writable.inspect();
  return int(whole) + selected + read_result + write_result;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("overloads.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected overload diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "exact namespace and method overloads should validate");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  // The checked integer pow keeps its failure form; unchecked float
  // multiplication cannot fail, so the float overload is plain.
  expect(
      artifact.contents.find(
          "::__gti_program::math::__gti_fn_1_pow__gti_mir_failure(") !=
              std::string::npos &&
          artifact.contents.find("::__gti_program::math::__gti_fn_2_pow(") !=
              std::string::npos &&
          concreteBackendSymbols(artifact.contents, "__gti_type_", "Selector")
                  .size() == 1 &&
          concreteBackendSymbols(artifact.contents, "__gti_member_", "apply")
                  .size() == 2,
      "the C++ backend should emit the function identity selected by GTI");
  expect(artifact.contents.find("mir_checked_convert_v1<std::uint64_t>(") !=
                 std::string::npos &&
             artifact.contents.find("mir_checked_convert_v1<std::int32_t>(") !=
                 std::string::npos &&
             artifact.contents.find(
                 "std::bit_cast<float>(std::uint32_t{0x40000000U})") !=
                 std::string::npos,
         "explicit conversions and float literals should lower to matching C++ "
         "types");
}

void testFixedArraysCppEmission() {
  const std::string source = R"(
uint32_t video[64 * 32] = {};

int extent(int values[2]) { return 2; }
int extent(int values[3]) { return 3; }
int first(int values[3]) { return values[0]; }

struct Buffers {
public:
  mut int samples[3] = {1, 2, 3};

  void bump() mut { this.samples[1] += 4; }
  uint64_t count() { return this.samples.size(); }
public:
  Buffers() = default;
};

int main() {
  mut int buffer[2 + 3] = {1, 2, 3, 4, 5};
  buffer[2] = 10;
  buffer[1]++;
  int copy[5] = buffer;
  int matrix[0x2][0b11] = {{1, 2, 3}, {4, 5, 6}};
  mut Buffers buffers = Buffers();
  buffers.bump();
  uint64_t count = buffer.size();
  if (count == 5 and first(matrix[1]) == 4 and extent(matrix[0]) == 3 and
      buffers.count() == 3 and copy[4] == 5) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("arrays.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected fixed array diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "fixed arrays should support value semantics and checked indexing");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(
      artifact.contents.find("const std::array<std::uint32_t, 2048> video = "
                             "std::array<std::uint32_t, 2048>{}") !=
              std::string::npos &&
          artifact.contents.find("std::array<std::int32_t, 5> __gti_mir_p_") !=
              std::string::npos &&
          artifact.contents.find("std::array<std::array<std::int32_t, 3>, 2> "
                                 "__gti_mir_p_") != std::string::npos &&
          artifact.contents.find("mir_checked_array_read_v1") !=
              std::string::npos &&
          artifact.contents.find("mir_checked_array_write_v1") !=
              std::string::npos &&
          artifact.contents.find("mir_checked_add_v1<std::int32_t>") !=
              std::string::npos &&
          artifact.contents.find("static_cast<std::uint64_t>") !=
              std::string::npos,
      "the C++ backend should preserve array values and checked operations");
}

void testFixedArrayDynamicWriteFailureEdgeCppEmission() {
  const std::string source = R"(
int main() {
  mut int32_t cells[4] = {};
  mut uint64_t index = uint64_t(0);
  while (index < uint64_t(9)) {
    index++;
  }
  cells[index] = 7;
  return int(cells[uint64_t(0)]);
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("dynamic-array-write.gti", source);
  expect(frontend.canGenerateCode(),
         "the dynamic array-write fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::size_t status = artifact.contents.find(
      " = ::gti_internal::backend::mir_checked_array_write_v1(");
  const std::size_t variable =
      status == std::string::npos
          ? std::string::npos
          : artifact.contents.rfind("__gti_mir_failure_status_", status);
  const std::string consumer =
      variable == std::string::npos
          ? std::string()
          : "if (" + artifact.contents.substr(variable, status - variable) +
                ".code == GTI_FAILURE_CODE_NONE_V1)";
  expect(status != std::string::npos && variable != std::string::npos &&
             !consumer.empty() &&
             artifact.contents.find(consumer, status) != std::string::npos,
         "the emitted checked array write must branch on its failure status "
         "instead of discarding it");
}

// The C++ emitter used to admit a nonterminal index only when it was a
// compile-time in-bounds constant and declined the body otherwise. The chained
// text now owns a bound per dimension: the failure form guards each dynamic
// nonterminal index before the terminal checked helper runs.
void testFixedArrayChainedDynamicIndexCppEmission() {
  const std::string source = R"(
int main() {
  mut int32_t matrix[2][3] = {};
  mut uint64_t row = uint64_t(0);
  while (row < uint64_t(9)) {
    row = row + uint64_t(1);
  }
  uint64_t column = uint64_t(1);
  matrix[row][column] = 7;
  return int(matrix[row][column]);
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("chained-array-index.gti", source);
  expect(frontend.canGenerateCode(),
         "the chained array-index fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  // The emitter reports a declined body by throwing, which is what raises
  // GTI-B0001 through the driver. Catching it keeps the regression a readable
  // failure instead of an abort.
  lang::BackendArtifact artifact;
  std::string declined;
  try {
    artifact =
        gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  } catch (const std::exception &error) {
    declined = error.what();
  }
  expect(declined.empty(),
         "a chained dynamic element access should stay inside the complete "
         "C++ text vocabulary");

  const std::size_t entry =
      artifact.contents.rfind("bool __gti_entry__gti_mir_failure(");
  const std::string body = entry == std::string::npos
                               ? std::string()
                               : artifact.contents.substr(entry);
  const auto occurrences = [&](const std::string &needle) {
    std::size_t count = 0;
    for (std::size_t at = body.find(needle); at != std::string::npos;
         at = body.find(needle, at + needle.size())) {
      ++count;
    }
    return count;
  };
  const std::string guard =
      " = ::gti_internal::backend::mir_checked_array_bound_v1(";
  const std::string read =
      " = ::gti_internal::backend::mir_checked_array_read_v1(";
  const std::string write =
      " = ::gti_internal::backend::mir_checked_array_write_v1(";
  // Each row is checked before evaluating its column. The terminal access
  // still guards its complete projected place; its retained row guard is not
  // a substitute for the earlier, independently routed selection check.
  expect(!body.empty() && occurrences(guard) == 4 && occurrences(read) == 1 &&
             occurrences(write) == 1,
         "read and write each retain an early row preflight plus the terminal "
         "access's complete chained guards");

  const std::size_t guardAt = body.find(guard);
  const std::size_t statusAt =
      guardAt == std::string::npos
          ? std::string::npos
          : body.rfind("__gti_mir_failure_status_", guardAt);
  const std::string status = statusAt == std::string::npos
                                 ? std::string()
                                 : body.substr(statusAt, guardAt - statusAt);
  const std::size_t writeAt = body.find(write);
  expect(!status.empty() && writeAt != std::string::npos && guardAt < writeAt &&
             body.find("if (" + status + ".code == GTI_FAILURE_CODE_NONE_V1) {",
                       guardAt) < writeAt,
         "the guard should open the success branch the terminal checked "
         "helper runs inside");
}

void testLocalTypeInferenceCppEmission() {
  const std::string source = R"(
class Box {
  int value = 0;

public:
  Box(int value) : value(value) {}
  int read() { return this.value; }
public:
  Box() = default;
};

T preserve<T>(T value) {
  auto inferred = value;
  return inferred;
}

int main() {
  auto integer = 1;
  mut auto running = preserve(integer);
  running += 2;
  auto truth = running == 3;
  auto text = "gti";
  auto box = Box(running);
  int values[2] = {running, 4};
  auto copied_values = values;
  auto empty = nullptr;
  for (mut auto index = 0; index < 2; index++) {
    running += copied_values[index];
  }
  if (truth and text == "gti" and empty == nullptr and box.read() == 3 and
      running == 10) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("auto-inference.gti", source);
  expect(frontend.canGenerateCode(),
         "initialized local auto bindings should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("bool __gti_entry__gti_mir_failure(") !=
                 std::string::npos &&
             artifact.contents.find("std::int32_t __gti_mir_p_") !=
                 std::string::npos &&
             artifact.contents.find("std::string_view __gti_mir_p_") !=
                 std::string::npos &&
             artifact.contents.find("std::array<std::int32_t, 2> "
                                    "__gti_mir_p_") != std::string::npos,
         "the C++ backend should lower inferred locals to their concrete MIR "
         "representations");
}

void testStructuredBindingsCppEmission() {
  const std::string source = R"(
struct Pair<T, U> {
  T first;
  U second;

  Pair(T first, U second) : first(first), second(second) {}
  ~Pair() {}
};

int main() {
  int values[2] = {2, 3};
  auto [left, right] = values;
  auto [number, enabled] = Pair<int, bool>(4, true);
  mut int result = 1;
  if (left == 2 && right == 3 && number == 4 && enabled) {
    result = 0;
  }
  return result;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("structured-bindings.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected structured-binding diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "fixed arrays and public direct aggregate fields should support "
         "structured bindings");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::unordered_set<std::string> pairTypes =
      concreteBackendSymbols(artifact.contents, "__gti_type_", "Pair");
  const std::string pairLifetime =
      pairTypes.size() == 1
          ? "mir_lifetime_slot<::__gti_program::" + *pairTypes.begin() + ">"
          : std::string();
  expect(artifact.contents.find("bool __gti_entry__gti_mir_failure(") !=
                 std::string::npos &&
             artifact.contents.find("std::array<std::int32_t, 2> "
                                    "__gti_mir_p_") != std::string::npos &&
             !pairLifetime.empty() &&
             artifact.contents.find(pairLifetime) != std::string::npos &&
             artifact.contents.find(".get().first") != std::string::npos &&
             artifact.contents.find(".get().second") != std::string::npos &&
             artifact.contents.find("const auto [left, right]") ==
                 std::string::npos,
         "the C++ backend should represent validated structured bindings as "
         "MIR projections without relying on C++ decomposition declarations");
}

void testTypeAliasesCppEmission() {
  const std::string source = R"(
using LaterSize = Size;
using Size = uint64_t;
using Triple = int[3];

class Box<T> {
  using stored_type = T;
  T value;
public:
  using value_type = stored_type;
  Box(T value) : value(value) {}
  value_type get() { return this.value; }
};

class Traits {
public:
  using value_type = hidden_type;
private:
  using hidden_type = int;
public:
  value_type identity(value_type value) { return value; }
public:
  Traits() = default;
};

using IntBox = Box<int>;
using TraitValue = Traits::value_type;
using BoxValue = IntBox::value_type;
using Completion = Box<bool>;
using Result = int;

Completion complete() { return Completion(true); }

Result main() {
  LaterSize count = Size(3);
  Triple values = {1, 2, 3};
  IntBox box = IntBox(values[1]);
  Traits traits{};
  BoxValue input = 4;
  TraitValue trait_value = traits.identity(input);
  Completion completion = complete();
  if (count == 3 and box.get() == 2 and trait_value == 4 and
      completion.get()) { return 0; }
  return 1;
}
)";
  lang::Lexer lexer;
  auto tokens = lexer.scan(source);
  expect(!lexer.hadError(), "type aliases should lex as declarations");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "namespace-scoped type aliases should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "type aliases should canonicalize before semantic checks");
  const std::string generated = emitAnalyzedSource(source);
  expect(
      generated.find("using LaterSize = std::uint64_t;") != std::string::npos &&
          generated.find("using Triple = std::array<std::int32_t, 3>;") !=
              std::string::npos &&
          generated.find("using IntBox = "
                         "::__gti_program::Box<std::int32_t>;") !=
              std::string::npos &&
          generated.find("using value_type = T;") != std::string::npos &&
          generated.find("using TraitValue = std::int32_t;") !=
              std::string::npos &&
          generated.find("static_cast<std::uint64_t>(3)") !=
              std::string::npos &&
          generated.find("using Completion = "
                         "::__gti_program::Box<bool>;") != std::string::npos &&
          generated.find("int main()") != std::string::npos,
      "the C++ backend should emit canonical, declaration-order-independent "
      "aliases");
}

void testNamespacesAndAliasesCppEmission() {
  const std::string source = R"(
namespace engine {
namespace graphics {
class Renderer { public: Renderer() = default; };
void render() {}
void renderTwice() {
  render();
  render();
}
}
}

namespace gfx = engine::graphics;
gfx::Renderer createRenderer();

int main() {
  engine::graphics::render();
  gfx::renderTwice();
  return 0;
}
)";
  lang::Lexer lexer;
  auto tokens = lexer.scan(source);
  expect(!lexer.hadError(), "namespace source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "nested namespaces and aliases should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "qualified calls and namespace aliases should resolve");
  const std::string generated = emitAnalyzedSource(source);
  expect(generated.find("namespace engine {") != std::string::npos &&
             generated.find("namespace graphics {") != std::string::npos,
         "emitter should preserve nested namespaces");
  expect(generated.find("namespace gfx = engine::graphics;") !=
             std::string::npos,
         "emitter should preserve namespace aliases");
  expect(containsMangledFunction(generated,
                                 "::__gti_program::engine::graphics::Renderer ",
                                 "createRenderer", "();") &&
             generated.find("namespace gfx = engine::graphics;") !=
                 std::string::npos,
         "qualified types should parse and emit through namespace aliases");
  expect(containsMangledFunction(generated, "engine::graphics::", "render",
                                 "();") &&
             containsMangledFunction(
                 generated, "engine::graphics::", "renderTwice", "();"),
         "emitter should preserve qualified calls");
}

void testExternCInteropCppEmission() {
  const std::string source = R"(
extern "C" {
  int32_t socket(int32_t domain, int32_t type, int32_t protocol);
  int32_t close(int32_t descriptor);
}

int main() {
  int32_t descriptor = socket(2, 1, 0);
  if (descriptor >= 0) {
    [[discard]] close(descriptor);
  }
  return 0;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("extern-c.gti", source);
  expect(frontend.canGenerateCode(),
         "fixed-width extern C declarations and calls should compile");
  if (!frontend.canGenerateCode())
    return;
  const std::string generated = gti_test::emitCppText(frontend);
  const auto socketName = gti_test::nativeSpelling(generated, "socket");
  const auto closeName = gti_test::nativeSpelling(generated, "close");
  expect(
      generated.find("extern \"C\" {") != std::string::npos &&
          socketName != "socket" && closeName != "close" &&
          generated.find("std::int32_t " + socketName +
                         "(std::int32_t domain, std::int32_t type, "
                         "std::int32_t protocol)") != std::string::npos &&
          generated.find("std::int32_t " + closeName +
                         "(std::int32_t descriptor)") != std::string::npos,
      "private C prototypes retain exact native symbols and scalar ABI types");
  expect(generated.find(socketName + "(__gti_mir_v_") != std::string::npos &&
             generated.find(closeName + "(__gti_mir_v_") != std::string::npos &&
             generated.find("__gti_fn_") == std::string::npos,
         "resolved C calls use the exact prototype identities bound to native "
         "symbols");
}

void testExternCLinkNameCppEmission() {
  const lang::FrontendResult linkedName =
      lang::Frontend().analyze("extern-c-link-name.gti", R"(
namespace cli11 {
extern "C" {
  [[link_name("gti_cli11_app_create")]]
  int32_t app_create(int32_t value);
}
}
int main() { return cli11::app_create(42) - 42; }
)");
  expect(linkedName.canGenerateCode() && linkedName.diagnostics.empty(),
         "link_name should separate GTI source lookup from a portable C "
         "symbol");
  if (!linkedName.canGenerateCode())
    return;
  const std::string linkedCpp = gti_test::emitCppText(linkedName);
  const auto linkedSymbol =
      gti_test::nativeSpelling(linkedCpp, "gti_cli11_app_create");
  expect(linkedSymbol != "gti_cli11_app_create" &&
             linkedCpp.find("std::int32_t " + linkedSymbol +
                            "(std::int32_t value)") != std::string::npos &&
             linkedCpp.find("::__gti_program::cli11::" + linkedSymbol + "(") !=
                 std::string::npos &&
             linkedCpp.find(" app_create(") == std::string::npos,
         "the backend should declare and call only the selected native link "
         "symbol");
}

void testLateExternCCppEmission() {
  const lang::FrontendResult lateDeclaration =
      lang::Frontend().analyze("late-extern-c.gti", R"(
namespace platform {
int32_t invoke_close(int32_t descriptor) { return close(descriptor); }
extern "C" { int32_t close(int32_t descriptor); }
}
int main() { return platform::invoke_close(-1); }
)");
  expect(lateDeclaration.canGenerateCode(),
         "namespace-scoped C declarations should resolve before their source "
         "position");
  if (!lateDeclaration.canGenerateCode())
    return;
  const std::string lateCpp = gti_test::emitCppText(lateDeclaration);
  const std::size_t latePrototype = lateCpp.find("extern \"C\" {");
  const auto lateClose = gti_test::nativeSpelling(lateCpp, "close");
  const std::size_t lateCall =
      lateCpp.find("::__gti_program::platform::" + lateClose + "(__gti_mir_v_");
  expect(latePrototype != std::string::npos && lateCall != std::string::npos &&
             lateClose != "close" && latePrototype < lateCall,
         "the backend should forward namespace-scoped C prototypes before "
         "earlier callers");
}

void testOrdinaryPrototypeCppEmission() {
  const lang::FrontendResult ordinaryPrototype =
      lang::Frontend().analyze("ordinary-prototype.gti", R"(
int32_t native_like(int32_t value);
int main() { return native_like(7); }
)");
  expect(ordinaryPrototype.canGenerateCode(),
         "ordinary GTI bodyless functions should remain valid declarations");
  if (!ordinaryPrototype.canGenerateCode())
    return;
  const std::string ordinaryCpp = gti_test::emitCppText(ordinaryPrototype);
  expect(
      ordinaryCpp.find("extern \"C\"") == std::string::npos &&
          containsMangledFunction(ordinaryCpp, "std::int32_t ", "native_like",
                                  "(const std::int32_t value);") &&
          containsMangledFunction(ordinaryCpp, "", "native_like",
                                  "(__gti_mir_v_") &&
          ordinaryCpp.find("native_like__gti_mir_failure") == std::string::npos,
      "ordinary prototypes should retain GTI mangling unless linkage is "
      "explicitly C");
}

void testRuntimeCAbiTextCppEmission() {
  const lang::FrontendResult runtime =
      analyzeTrustedStandalonePreludeFixture("extern-runtime.gti", R"(
namespace gti_internal {
class text_view {};
namespace runtime {
using native_text = gti_internal::text_view;
extern "C" {
int32_t gti_rt_write_stdout(native_text value);
int64_t gti_rt_open_file_read(native_text path);
}

void write_stdout(gti_internal::text_view value) {
  [[discard]] gti_rt_write_stdout(value);
}

int64_t open_file_read(gti_internal::text_view path) {
  return gti_rt_open_file_read(path);
}
}
}

int main() {
  gti_internal::runtime::write_stdout("hello");
  [[discard]] gti_internal::runtime::open_file_read("input");
  return 0;
}
)");
  expect(runtime.canGenerateCode(),
         "counted text buffers should support runtime C ABI migration");
  if (!runtime.canGenerateCode())
    return;
  const std::string runtimeCpp = gti_test::emitCppText(runtime);
  const auto writeSymbol =
      gti_test::nativeSpelling(runtimeCpp, "gti_rt_write_stdout");
  expect(
      runtimeCpp.find("#include <gti/c_abi.h>") != std::string::npos &&
          writeSymbol != "gti_rt_write_stdout" &&
          runtimeCpp.find("#include <gti/runtime.hpp>") == std::string::npos &&
          runtimeCpp.find("std::int32_t " + writeSymbol +
                          "(::gti_c_string_view value)") != std::string::npos &&
          runtimeCpp.find("::gti_internal::backend::to_c_string_view("
                          "__gti_mir_v_") != std::string::npos,
      "text views should cross C linkage through the explicit public "
      "counted-buffer ABI");
}

void testShadowedCRecordCppEmission() {
  const lang::FrontendResult shadowedRecord =
      analyzeTrustedStandalonePreludeFixture("shadowed-c-record.gti", R"(
namespace gti_internal { class text_view {}; }
namespace native {
struct gti_c_string_view { public: gti_c_string_view() = default; };
extern "C" {
int32_t measure_text(gti_internal::text_view value);
}
int32_t measure() { return measure_text("x"); }
}
int main() { return native::measure() - 1; }
)");
  expect(shadowedRecord.canGenerateCode(),
         "a GTI namespace should be allowed to reuse the public record's "
         "unqualified spelling");
  if (!shadowedRecord.canGenerateCode())
    return;
  const std::string shadowedRecordCpp = gti_test::emitCppText(shadowedRecord);
  const auto measureSymbol =
      gti_test::nativeSpelling(shadowedRecordCpp, "measure_text");
  expect(measureSymbol != "measure_text" &&
             shadowedRecordCpp.find(
                 "inline ::gti_c_string_view to_c_string_view") !=
                 std::string::npos &&
             shadowedRecordCpp.find("std::int32_t " + measureSymbol +
                                    "(::gti_c_string_view value)") !=
                 std::string::npos,
         "C ABI text lowering should name the global public record even when "
         "a source namespace shadows it");
}

void testLegacyRuntimeBindingCppEmission() {
  const lang::FrontendResult legacyRuntime =
      analyzeTrustedStandalonePreludeFixture("legacy-runtime.gti", R"(
namespace gti_internal {
class text_view {};
namespace runtime {
@runtime("stdout.write")
void write_stdout(gti_internal::text_view value);
}
}
int main() {
  gti_internal::runtime::write_stdout("compatibility");
  return 0;
}
)");
  expect(legacyRuntime.canGenerateCode(),
         "the closed compiler-owned runtime attribute should remain a "
         "compatible internal declaration surface");
  if (!legacyRuntime.canGenerateCode())
    return;
  const std::string legacyRuntimeCpp = gti_test::emitCppText(legacyRuntime);
  expect(legacyRuntimeCpp.find("#include <gti/runtime.hpp>") !=
                 std::string::npos &&
             legacyRuntimeCpp.find("#include <gti/c_abi.h>") ==
                 std::string::npos,
         "legacy runtime bindings should stay separate from public C ABI "
         "lowering");
}

void testScopedEnumsCppEmission() {
  const std::string source = R"(
namespace engine {
enum class State : uint8_t {
  Idle,
  Running = 4,
  Stopped,
};
enum class Mode { editor, game };
}

using StateAlias = engine::State;

StateAlias next(StateAlias state) {
  if (state == StateAlias::Idle) {
    return engine::State::Running;
  }
  return StateAlias::Stopped;
}

int main() {
  auto state = next(StateAlias::Idle);
  if (state == engine::State::Running) {
    return 0;
  }
  return 1;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("scoped-enums.gti", source);
  expect(frontend.canGenerateCode(),
         "scoped enum declarations and qualified enumerators should compile");
  if (!frontend.canGenerateCode())
    return;
  const std::string generated = gti_test::emitCppText(frontend);
  expect(generated.find("enum class State : std::uint8_t;") !=
                 std::string::npos &&
             generated.find("enum class State : std::uint8_t {") !=
                 std::string::npos &&
             generated.find("enum class Mode : std::int32_t {") !=
                 std::string::npos &&
             generated.find("Running = 4") != std::string::npos,
         "the C++ backend should emit fixed-backing scoped enums");
}

const NamedTest kTests[] = {
    {"testClassesStructsAndAccessCppEmission",
     testClassesStructsAndAccessCppEmission},
    {"testConstructorsAndReceiverMutabilityCppEmission",
     testConstructorsAndReceiverMutabilityCppEmission},
    {"testInheritanceAndInterfacesCppEmission",
     testInheritanceAndInterfacesCppEmission},
    {"testDirectBraceConstructionCppEmission",
     testDirectBraceConstructionCppEmission},
    {"testNamedGenericsCppEmission", testNamedGenericsCppEmission},
    {"testNativeGenericCarrierClosureCppEmission",
     testNativeGenericCarrierClosureCppEmission},
    {"testUninstantiatedGenericSignatureClosureCppEmission",
     testUninstantiatedGenericSignatureClosureCppEmission},
    {"testDependentCarrierRepresentationClosureCppEmission",
     testDependentCarrierRepresentationClosureCppEmission},
    {"testExactQualifiedClassSpecializationsCppEmission",
     testExactQualifiedClassSpecializationsCppEmission},
    {"testConstrainedGenericsCppEmission", testConstrainedGenericsCppEmission},
    {"testConstrainedGenericCapabilitiesCppEmission",
     testConstrainedGenericCapabilitiesCppEmission},
    {"testConcurrencyTypeCapabilitiesCppEmission",
     testConcurrencyTypeCapabilitiesCppEmission},
    {"testSourceDefinedConceptsCppEmission",
     testSourceDefinedConceptsCppEmission},
    {"testStructuralRequiresAndAccumulateCppEmission",
     testStructuralRequiresAndAccumulateCppEmission},
    {"testExactSentinelBridgesCppEmission",
     testExactSentinelBridgesCppEmission},
    {"testConstructorLocalTypeGenericsCppEmission",
     testConstructorLocalTypeGenericsCppEmission},
    {"testValueGenericsCppEmission", testValueGenericsCppEmission},
    {"testContextualArrayArgumentsCppEmission",
     testContextualArrayArgumentsCppEmission},
    {"testVariadicGenericsCppEmission", testVariadicGenericsCppEmission},
    {"testPackFoldCppEmission", testPackFoldCppEmission},
    {"testExactFunctionOverloadsAndConversionsCppEmission",
     testExactFunctionOverloadsAndConversionsCppEmission},
    {"testFixedArraysCppEmission", testFixedArraysCppEmission},
    {"testFixedArrayDynamicWriteFailureEdgeCppEmission",
     testFixedArrayDynamicWriteFailureEdgeCppEmission},
    {"testFixedArrayChainedDynamicIndexCppEmission",
     testFixedArrayChainedDynamicIndexCppEmission},
    {"testLocalTypeInferenceCppEmission", testLocalTypeInferenceCppEmission},
    {"testStructuredBindingsCppEmission", testStructuredBindingsCppEmission},
    {"testTypeAliasesCppEmission", testTypeAliasesCppEmission},
    {"testNamespacesAndAliasesCppEmission",
     testNamespacesAndAliasesCppEmission},
    {"testExternCInteropCppEmission", testExternCInteropCppEmission},
    {"testExternCLinkNameCppEmission", testExternCLinkNameCppEmission},
    {"testLateExternCCppEmission", testLateExternCCppEmission},
    {"testOrdinaryPrototypeCppEmission", testOrdinaryPrototypeCppEmission},
    {"testRuntimeCAbiTextCppEmission", testRuntimeCAbiTextCppEmission},
    {"testShadowedCRecordCppEmission", testShadowedCRecordCppEmission},
    {"testLegacyRuntimeBindingCppEmission",
     testLegacyRuntimeBindingCppEmission},
    {"testScopedEnumsCppEmission", testScopedEnumsCppEmission},
};

} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "cpp_mir_declarations_emission");
}
