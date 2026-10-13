#include "cpp_mir_body_emitter_test_fixture.h"

#include "gti/mir_printer.h"

int main() {
  const auto fixtures =
      std::filesystem::path(__FILE__).parent_path() / "fixtures";
  const auto path = fixtures / "structural_relocation.gti";
  std::ifstream input(path);
  expect(input.good(), "structural relocation fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "structural concepts and public relocation analyze");
  if (!frontend.canGenerateCode())
    return 1;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "relocation MIR verifies");
  using Kind = lang::SemanticType::Kind;
  for (const Kind kind :
       {Kind::Int8, Kind::Int16, Kind::Int32, Kind::Int64, Kind::UInt8,
        Kind::UInt16, Kind::UInt32, Kind::UInt64, Kind::Float, Kind::Double,
        Kind::Bool, Kind::Char, Kind::NullPtr, Kind::Enum, Kind::RawPointer,
        Kind::NativeFunction, Kind::FunctionItem, Kind::StringView,
        Kind::CString}) {
    const lang::SemanticType type(kind);
    expect(lang::semanticTraits(type).relocation ==
                   lang::RelocationKind::Bitwise &&
               lang::mirTypeRelocationKind(frontend.mir, type) ==
                   lang::RelocationKind::Bitwise,
           "only explicitly admitted passive kinds grant bitwise relocation");
  }
  // 63 is in the enum's representable range but has no declared enumerator.
  // Keep this probe undeclared when extending the type-kind inventory.
  static_assert(Kind::FunctionSignature < 63);
  for (const Kind kind :
       {Kind::Unknown, Kind::Void, Kind::TypeParameter, Kind::TypePack,
        Kind::TypeName, Kind::Function, Kind::FunctionSignature,
        Kind::Reference, Kind::Class, Kind::Lambda, static_cast<Kind>(63)}) {
    const lang::SemanticType type(kind);
    expect(lang::semanticTraits(type).relocation ==
                   lang::RelocationKind::Unavailable &&
               lang::mirTypeRelocationKind(frontend.mir, type) ==
                   lang::RelocationKind::Unavailable,
           "unknown, future, unresolved, and borrowed kinds fail closed");
    expect(lang::mirTypeRelocationKind(frontend.mir,
                                       lang::SemanticType::arrayOf(type, 2)) ==
               lang::RelocationKind::Unavailable,
           "an array cannot hide an unavailable relocation element");
  }
  const auto serialized = lang::MirPrinter().print(frontend.mir);
  expect(serialized.find('\0') == std::string::npos &&
             serialized.starts_with("mir-v103"),
         "relocation and conditional thread policies serialize as textual "
         "integers, not bytes");
  const lang::MirClassInstance *particle = nullptr;
  const lang::MirClassInstance *resource = nullptr;
  for (const auto &item : frontend.hir.classInstances()) {
    if (!item.source)
      continue;
    if (item.source->name().lexeme == "Particle")
      particle = frontend.mir.findClassInstance(item.id);
    if (item.source->name().lexeme == "Resource")
      resource = frontend.mir.findClassInstance(item.id);
  }
  expect(particle && resource,
         "unrelated user classes have concrete MIR identities");
  if (!particle || !resource)
    return 1;
  expect(particle->traits.relocation == lang::RelocationKind::Bitwise &&
             resource->traits.relocation == lang::RelocationKind::Structural,
         "semantic relocation distinction survives HIR and MIR");
  expect(
      lang::mirTypeRelocationKind(
          frontend.mir, lang::SemanticType::arrayOf(particle->type, 4)) ==
              lang::RelocationKind::Bitwise &&
          lang::mirTypeRelocationKind(
              frontend.mir, lang::SemanticType::arrayOf(resource->type, 4)) ==
              lang::RelocationKind::Structural,
      "fixed arrays inherit the exact element relocation rule");
  expect(lang::mirTypeRelocationKind(
             frontend.mir, lang::SemanticType::arrayOf(particle->type, 0)) ==
             lang::RelocationKind::Bitwise,
         "zero-length concrete arrays retain element relocation eligibility");
  const auto cpp = gti_test::emitCppText(frontend);
  bool rawBitwise = false;
  bool rawStructural = false;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::ObjectRelocate ||
            instruction.parameterTypes.empty() ||
            instruction.parameterTypes.front().arguments.size() != 1)
          continue;
        const auto kind = lang::mirTypeRelocationKind(
            frontend.mir, instruction.parameterTypes.front().arguments.front());
        rawBitwise |= kind == lang::RelocationKind::Bitwise;
        rawStructural |= kind == lang::RelocationKind::Structural;
      }
  expect(rawBitwise && rawStructural,
         "source sparse relocation retains both exact typed strategies");
  expect(cpp.find("mir_storage_relocate_v1") == std::string::npos &&
             cpp.find("::gti_internal::backend::mir_object_relocate_v1(") !=
                 std::string::npos &&
             cpp.find("mir_prefix_relocate_v1") == std::string::npos,
         "sparse and prefix source storage both use general typed relocation "
         "without a private prefix fallback");
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto malformed = frontend.mir;
    auto *passive = const_cast<lang::MirClassInstance *>(
        malformed.findClassInstance(particle->id));
    auto *owner = const_cast<lang::MirClassInstance *>(
        malformed.findClassInstance(resource->id));
    if (mutation == 0)
      owner->traits.relocation = lang::RelocationKind::Bitwise;
    if (mutation == 1)
      passive->moveConstructor = lang::SpecialMemberStatus::Deleted;
    if (mutation == 2)
      passive->declaredFields.front().type = resource->type;
    if (mutation == 3)
      passive->traits.relocation = lang::RelocationKind::Unavailable;
    if (mutation == 4)
      passive->polymorphic = true;
    expect(!lang::verifyMirProgram(malformed).valid(),
           "forged relocation facts cannot authorize the bulk path");
  }
  for (const auto fixture :
       {"stored_callable_clone.gti", "native_opaque_pointer_results.gti"}) {
    const auto compositionPath = fixtures / fixture;
    std::ifstream compositionInput(compositionPath);
    expect(compositionInput.good(), "relocation composition fixture exists");
    const std::string compositionSource{
        std::istreambuf_iterator<char>(compositionInput),
        std::istreambuf_iterator<char>()};
    const auto composition =
        analyzeWithStandardLibrary(compositionPath.string(), compositionSource);
    expect(composition.canGenerateCode() &&
               lang::verifyMirProgram(composition.mir).valid(),
           "interface bases, erased captures, and pointer-only native classes "
           "retain coherent relocation metadata");
    if (!composition.canGenerateCode())
      continue;
    expect(!gti_test::emitCppText(composition).empty(),
           "composed relocation metadata reaches the production backend");
    for (const auto &instance : composition.mir.classInstances()) {
      if (!instance.cOpaqueHandle)
        continue;
      expect(instance.traits.relocation == lang::RelocationKind::Unavailable,
             "opaque native pointees are not movable GTI values");
      auto malformed = composition.mir;
      auto *opaque = const_cast<lang::MirClassInstance *>(
          malformed.findClassInstance(instance.id));
      opaque->cOpaqueHandle = false;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "losing native-opaque identity invalidates relocation metadata");
    }
  }
  const auto requireRejected = [&](std::string text,
                                   std::string_view conceptName) {
    const auto result =
        analyzeWithStandardLibrary("relocation-denied.gti", text);
    const auto failure = std::find_if(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto &diagnostic) {
          return diagnostic.code == "GTI-S2029" &&
                 diagnostic.message.find(conceptName) != std::string::npos;
        });
    expect(!result.canGenerateCode() && failure != result.diagnostics.end(),
           "invalid structural relocation is rejected before lowering");
    if (failure != result.diagnostics.end()) {
      expect(!failure->hints.empty(),
             "constraint diagnostic explains eligibility");
      expect(!failure->related.empty(),
             "constraint diagnostic relates the declaration");
      expect(std::filesystem::path(failure->primary.source).filename() ==
                     "relocation-denied.gti" &&
                 failure->primary.start < failure->primary.end,
             "constraint diagnostic anchors the rejected source use");
    }
  };
  const std::string dependent = R"(
    struct Parcel<T> { T item; };
    enum Choice<T> { empty, item(T value) };
    void require_relocation<std::relocatable U>() {}
    void require_bitwise<std::bitwise_relocatable U>() {}
    void check<T>() requires std::relocatable<T> {
      require_relocation<Parcel<T>>();
      require_relocation<Choice<T>>();
      require_relocation<Parcel<Choice<T>>>();
    }
    void check_bits<std::bitwise_relocatable T>() {
      require_bitwise<Parcel<T>>();
    }
    class Resource {
    public:
      Resource() {}
      Resource(Resource& source) = delete;
      Resource(Resource&& source) = default;
      ~Resource() {}
    };
    int main() { check<Resource>(); check_bits<int>(); return 0; }
  )";
  const auto dependentResult =
      analyzeWithStandardLibrary("dependent-relocation.gti", dependent);
  expect(dependentResult.canGenerateCode(),
         "dependent fields preserve declared and active relocation guarantees");
  if (dependentResult.canGenerateCode())
    expect(!gti_test::emitCppText(dependentResult).empty(),
           "concrete dependent relocation remains verified through MIR");
  for (const std::string constraint :
       {"std::numeric", "std::signed_numeric", "std::integral",
        "std::signed_integral", "std::unsigned_integral",
        "std::floating_point"}) {
    const auto numeric = analyzeWithStandardLibrary(
        "numeric-storage.gti",
        "#include <std/memory>\n#include <std/vector>\n"
        "struct Parcel<T> { T item; }; "
        "void require_bits<std::bitwise_relocatable T>() {} "
        "void require_storage<T>() requires std::storage_element<T> {} "
        "void check<" +
            constraint +
            " T>() { "
            "require_bits<Parcel<T>>(); require_storage<T>(); "
            "mut std::vector<T> values{}; values.push_back(T(3)); "
            "} int main() { check<" +
            (constraint == "std::floating_point"      ? "double"
             : constraint == "std::unsigned_integral" ? "uint32_t"
                                                      : "int32_t") +
            ">(); return 0; }");
    expect(
        numeric.canGenerateCode(),
        "numeric contracts retain scalar relocation and storage guarantees: " +
            constraint);
    if (numeric.canGenerateCode())
      expect(
          !gti_test::emitCppText(numeric).empty(),
          "numeric vector and unrelated record uses reach verified emission");
  }
  for (const std::string requirement :
       {"std::copyable<T>", "std::relocatable<T>",
        "std::numeric<T> || std::copyable<T>"}) {
    requireRejected("#include <std/memory>\n"
                    "void need<T>() requires std::storage_element<T> {} "
                    "void check<T>() requires " +
                        requirement +
                        " { need<T>(); } int main() { return 0; }",
                    "storage_element");
  }
  for (const std::string constraint :
       {"std::move_constructible<T>",
        "std::relocatable<T> || std::move_constructible<T>"}) {
    requireRejected("struct Parcel<T> { T item; }; "
                    "void accept<std::relocatable U>() {} "
                    "void check<T>() requires " +
                        constraint +
                        " { accept<Parcel<T>>(); } int main() { return 0; }",
                    "relocatable");
  }
  requireRejected(R"(
    struct Parcel<T> { T item; };
    void accept<std::bitwise_relocatable U>() {}
    void check<T>() requires std::relocatable<T> { accept<Parcel<T>>(); }
    int main() { return 0; }
  )",
                  "bitwise_relocatable");
  requireRejected(R"(
    struct Parcel<T> {
      T item;
      int& loan;
      Parcel(T item, int& loan) : item(std::move(item)), loan(loan) {}
    };
    void accept<std::relocatable U>() {}
    void check<T>() requires std::relocatable<T> { accept<Parcel<T>>(); }
    int main() { return 0; }
  )",
                  "relocatable");
  requireRejected(R"(
    class Fixed { public: Fixed() {} Fixed(Fixed&& source) = delete; };
    void accept<std::relocatable T>() {}
    int main() { accept<Fixed>(); return 0; }
  )",
                  "relocatable");
  requireRejected(R"(
    class Resource { public: Resource() {} ~Resource() {} };
    void accept<std::bitwise_relocatable T>() {}
    int main() { accept<Resource>(); return 0; }
  )",
                  "bitwise_relocatable");
  requireRejected(R"(
    class View { int& value; public: View(int& value) : value(value) {} };
    void accept<std::relocatable T>() {}
    int main() { accept<View>(); return 0; }
  )",
                  "relocatable");
  for (const std::string replacement :
       {"source.relocate_to(source, std::size_t(0));",
        "Particle& loan = source.get(std::size_t(0)); "
        "source.relocate_to(destination, std::size_t(0)); [[discard]] "
        "loan.id();",
        "destination.construct(std::size_t(0), Particle(90, 9.0)); "
        "Particle& loan = destination.get(std::size_t(0)); "
        "source.relocate_to(destination, std::size_t(0)); [[discard]] "
        "loan.id();"}) {
    auto invalid = source;
    const std::string original =
        "source.relocate_to(destination, std::size_t(0));";
    invalid.replace(invalid.find(original), original.size(), replacement);
    const auto denied =
        analyzeWithStandardLibrary("relocation-loan-denied.gti", invalid);
    expect(
        !denied.canGenerateCode() &&
            std::none_of(denied.diagnostics.begin(), denied.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-B0001"; }),
        "self-aliasing and outstanding source/destination loans remain "
        "semantic errors");
  }
  return failures ? 1 : 0;
}
