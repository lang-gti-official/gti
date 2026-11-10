#include "gti/frontend.h"
#include "gti/persistent_declaration.h"
#include <cstdlib>
#include <functional>
#include <iostream>
using namespace lang;
namespace {
void expect(bool condition, const char *why) {
  if (!condition) {
    std::cerr << why << '\n';
    std::exit(1);
  }
}
void rejects(const std::function<void()> &f) {
  try {
    f();
  } catch (const std::invalid_argument &) {
    return;
  }
  expect(false, "invalid identity accepted");
}
std::string analyzed(std::string source, const std::string &path) {
  FrontendOptions options;
  options.stopAfter = FrontendPhase::Semantics;
  options.suggestIncludes = false;
  auto result = Frontend(options).analyze(path, source);
  for (const auto &d : result.diagnostics)
    std::cerr << d.message << '\n';
  expect(result.semanticValid, "identity fixture must analyze");
  for (const auto &statement : result.program.declarations()) {
    const auto *declaration =
        dynamic_cast<const FunctionDecl *>(statement.get());
    if (!declaration)
      continue;
    const auto *function = result.semantics.findFunction(*declaration);
    if (function && function->qualifiedName == "answer")
      return persistentFunctionKey(*function,
                                   {"example/pkg", "core", "api", ""});
  }
  expect(false, "missing analyzed function");
  return {};
}

std::string nominalAnalyzed(bool moved) {
  FrontendOptions options;
  options.stopAfter = FrontendPhase::Semantics;
  options.suggestIncludes = false;
  const std::string source =
      std::string(moved ? "struct Before { int n; }; " : "") +
      "struct Item { int value; }; enum class Choice { A, B };";
  auto result = Frontend(options).analyze(
      moved ? "/tmp/other/types.gti" : "/tmp/types.gti", source);
  expect(result.semanticValid, "nominal fixture must analyze");
  std::string bytes;
  for (const auto &statement : result.program.declarations()) {
    if (auto *decl = dynamic_cast<const ClassDecl *>(statement.get())) {
      auto *info = result.semantics.findClassType(*decl);
      if (info && info->qualifiedName == "Item") {
        PersistentTypeRegistry registry;
        registry.bind(*info, {"pkg", "core", "api", ""});
        bytes += writePersistentType(
            SemanticType::referenceTo(SemanticType::classType(info->id)),
            registry);
      }
    }
    if (auto *decl = dynamic_cast<const EnumDecl *>(statement.get())) {
      auto *info = result.semantics.findEnumType(*decl);
      if (info && info->qualifiedName == "Choice") {
        PersistentTypeRegistry registry;
        registry.bind(*info, {"pkg", "core", "api", ""});
        SemanticType type(SemanticType::Enum);
        type.enumId = info->id;
        bytes += writePersistentType(type, registry);
      }
    }
  }
  expect(!bytes.empty(), "missing nominal declarations");
  return bytes;
}
void nominalTests() {
  PersistentDeclarationOwner owner{"pkg", "core", "api", ""};
  ClassTypeInfo producer;
  producer.id = 1;
  producer.qualifiedName = "Item";
  ClassTypeInfo consumer = producer;
  consumer.id = 91;
  consumer.sourceUnit = 55;
  PersistentTypeRegistry from, to;
  from.bind(producer, owner);
  to.bind(consumer, owner);
  auto source = SemanticType::arrayOf(
      SemanticType::rawPointerTo(SemanticType::classType(1),
                                 AccessMode::ReadOnly),
      7);
  auto expected = SemanticType::arrayOf(
      SemanticType::rawPointerTo(SemanticType::classType(91),
                                 AccessMode::ReadOnly),
      7);
  auto bytes = writePersistentType(source, from);
  expect(readPersistentType(bytes, to) == expected,
         "nominal reference remaps local IDs");
  expect(writePersistentType(expected, to) == bytes,
         "local ID absent from wire");
  expect(nominalAnalyzed(false) == nominalAnalyzed(true),
         "real nominal identity survives relocation and declaration reorder");
  EnumTypeInfo e;
  e.id = 1;
  e.qualifiedName = "Item";
  from.bind(e, owner);
  e.id = 91;
  to.bind(e, owner);
  SemanticType enumeration(SemanticType::Enum);
  enumeration.enumId = 1;
  auto enumBytes = writePersistentType(enumeration, from);
  expect(enumBytes != writePersistentType(SemanticType::classType(1), from),
         "class and enum references differ");
  expect(
      writePersistentType(SemanticType::referenceTo(SemanticType::classType(1),
                                                    AccessMode::ReadOnly),
                          from) !=
          writePersistentType(
              SemanticType::referenceTo(SemanticType::classType(1),
                                        AccessMode::Mutable),
              from),
      "access mode is preserved");
  auto otherOwner = owner;
  otherOwner.module = "other";
  auto other = producer;
  other.id = 2;
  from.bind(other, otherOwner);
  expect(from.key(SemanticType::classType(1)) !=
             from.key(SemanticType::classType(2)),
         "same spelling in distinct modules remains distinct");
  enumeration.enumId = 91;
  expect(readPersistentType(enumBytes, to) == enumeration,
         "enum remaps independently of class ID");
  expect(persistentClassKey(producer, owner) != persistentEnumKey(e, owner),
         "nominal kinds distinguish identical names");
  from.bind(producer, owner);
  auto duplicate = producer;
  duplicate.id = 2;
  rejects([&] { from.bind(duplicate, owner); });
  duplicate = producer;
  duplicate.qualifiedName = "Other";
  rejects([&] { from.bind(duplicate, owner); });
  expect(from.key(SemanticType::classType(1)) ==
             persistentClassKey(producer, owner),
         "failed binding leaves registry intact");
  PersistentTypeRegistry empty;
  rejects([&] { (void)readPersistentType(bytes, empty); });
  rejects([&] { (void)writePersistentType(SemanticType::classType(3), from); });
  duplicate = producer;
  duplicate.genericParameters.emplace_back();
  duplicate.genericParameters.back().typeArgumentPath = {0};
  rejects([&] { from.bind(duplicate, owner); });
  duplicate = producer;
  duplicate.exactSpecializationPrimary = 1;
  rejects([&] { from.bind(duplicate, owner); });
  duplicate = producer;
  duplicate.enclosingClass = 1;
  rejects([&] { from.bind(duplicate, owner); });
  duplicate = producer;
  duplicate.compilerPrivate = true;
  rejects([&] { from.bind(duplicate, owner); });
  duplicate = producer;
  duplicate.id = 0;
  rejects([&] { from.bind(duplicate, owner); });
  rejects([&] {
    (void)writePersistentType(SemanticType::classType(1, {SemanticType::Int32}),
                              from);
  });
  auto decorated = source;
  decorated.arrayLengthParameterId = 7;
  rejects([&] { (void)writePersistentType(decorated, from); });
  auto deep = SemanticType(SemanticType::Int32);
  for (unsigned i = 0; i < 34; ++i)
    deep = SemanticType::rawPointerTo(std::move(deep));
  rejects([&] { (void)writePersistentType(deep, from); });
  for (std::size_t i = 0; i < bytes.size(); ++i)
    rejects([&] {
      (void)readPersistentType(std::string_view(bytes).substr(0, i), to);
    });
  rejects([&] { (void)readPersistentType(bytes + "x", to); });
  rejects([&] { (void)readPersistentType("gti.type.3:3:i32", to); });
  for (std::size_t i = 0; i < bytes.size(); ++i)
    for (unsigned c = 0; c < 256; ++c) {
      auto mutated = bytes;
      mutated[i] = static_cast<char>(c);
      try {
        auto decoded = readPersistentType(mutated, to);
        expect(writePersistentType(decoded, to) == mutated,
               "all accepted mutations are canonical");
      } catch (const std::invalid_argument &) {
      }
    }
}

std::string genericAnalyzed(bool moved) {
  FrontendOptions options;
  options.stopAfter = FrontendPhase::Semantics;
  options.suggestIncludes = false;
  std::string source = std::string(moved ? "struct Before { int n; }; " : "") +
                       "struct Box<T, uint64_t N> { T value; }; enum Event<T> "
                       "{ item(T value) }; "
                       "struct Use { Box<int, 3> box; Event<int> event; };";
  auto result = Frontend(options).analyze(
      moved ? "/tmp/other/generic.gti" : "/tmp/generic.gti", source);
  for (const auto &d : result.diagnostics)
    std::cerr << d.message << '\n';
  expect(result.semanticValid, "generic fixture must analyze");
  PersistentTypeRegistry registry;
  const ClassTypeInfo *use = nullptr;
  for (const auto &statement : result.program.declarations()) {
    if (auto *decl = dynamic_cast<const ClassDecl *>(statement.get())) {
      auto *info = result.semantics.findClassType(*decl);
      expect(info, "missing class info");
      registry.bind(*info, {"pkg", "core", "api", ""});
      if (info->qualifiedName == "Use")
        use = info;
    }
    if (auto *decl = dynamic_cast<const EnumDecl *>(statement.get())) {
      auto *info = result.semantics.findEnumType(*decl);
      expect(info, "missing enum info");
      registry.bind(*info, {"pkg", "core", "api", ""});
    }
  }
  expect(use && use->fields.size() == 2, "generic fields resolved");
  std::string bytes;
  for (const auto &field : use->fields) {
    auto encoded = writePersistentType(field.type, registry);
    expect(readPersistentType(encoded, registry) == field.type,
           "real resolved generic type round trip");
    bytes += encoded;
  }
  return bytes;
}
void genericTests() {
  PersistentDeclarationOwner owner{"pkg", "core", "api", ""};
  ClassTypeInfo c;
  c.id = 1;
  c.qualifiedName = "Generic";
  GenericParameterInfo t, v, p;
  t.id = 10;
  v.id = 11;
  v.value = true;
  p.id = 12;
  p.pack = true;
  c.genericParameters = {t, v, p};
  expect(persistentClassKey(c, owner) ==
             "gti.nominal-key.2:5:class3:pkg4:core3:api7:Generic3:tvp",
         "generic nominal schema golden key");
  auto other = c;
  other.id = 99;
  for (auto &parameter : other.genericParameters)
    parameter.id += 100;
  PersistentTypeRegistry producer, consumer;
  producer.bind(c, owner);
  consumer.bind(other, owner);
  auto input = SemanticType::classType(
      1, {SemanticType::Int32, SemanticType::Bool},
      {CompileTimeValue::uint64(18446744073709551615ULL)});
  auto expected = input;
  expected.classId = 99;
  auto bytes = writePersistentType(input, producer);
  expect(readPersistentType(bytes, consumer) == expected,
         "generic instances remap owner IDs and preserve substitutions");
  expect(writePersistentType(expected, consumer) == bytes,
         "generic binder IDs never persist");
  auto changed = input;
  changed.valueArguments[0] = CompileTimeValue::uint64(0);
  expect(writePersistentType(changed, producer) != bytes,
         "value arguments distinguish instances");
  changed = input;
  changed.arguments.pop_back();
  expect(readPersistentType(writePersistentType(changed, producer), consumer)
                 .arguments.size() == 1,
         "empty final pack");
  changed.arguments.clear();
  rejects([&] { (void)writePersistentType(changed, producer); });
  changed = input;
  changed.valueArguments.clear();
  rejects([&] { (void)writePersistentType(changed, producer); });
  changed = input;
  changed.valueArguments[0] = CompileTimeValue::parameter(11);
  rejects([&] { (void)writePersistentType(changed, producer); });
  changed = input;
  changed.arguments[0] = SemanticType::typeParameter(10);
  rejects([&] { (void)writePersistentType(changed, producer); });
  auto bad = c;
  bad.genericParameters = {p, t};
  rejects([&] {
    PersistentTypeRegistry r;
    r.bind(bad, owner);
  });
  bad = c;
  bad.genericParameters.back().value = true;
  rejects([&] {
    PersistentTypeRegistry r;
    r.bind(bad, owner);
  });
  bad = c;
  bad.exactSpecializationPrimary = 8;
  rejects([&] {
    PersistentTypeRegistry r;
    r.bind(bad, owner);
  });
  bad = c;
  bad.genericParameters = {v, t, p};
  expect(persistentClassKey(bad, owner) != persistentClassKey(c, owner),
         "declaration parameter ordering retained");
  expect(genericAnalyzed(false) == genericAnalyzed(true),
         "real generic owners stable across declaration reorder");
  for (std::size_t n = 0; n < bytes.size(); ++n)
    rejects([&] {
      (void)readPersistentType(std::string_view(bytes).substr(0, n), consumer);
    });
  rejects([&] { (void)readPersistentType("gti.type.1:3:i32", consumer); });
  auto huge = input;
  huge.arguments.resize(257, SemanticType::Int32);
  rejects([&] { (void)writePersistentType(huge, producer); });
}
} // namespace
int main(int argc, char **argv) {
  if (argc == 2) {
    const bool relocated = std::string_view(argv[1]) == "--relocated";
    std::cout << analyzed(
        relocated
            ? "int first() { return 0; } int answer(int x) { return x+2; }"
            : "int answer(int x) { return x; }",
        relocated ? "/tmp/other/library.gti" : "/tmp/library.gti");
    std::cout << "\n"
              << nominalAnalyzed(relocated) << "\n"
              << genericAnalyzed(relocated);
    return 0;
  }
  nominalTests();
  genericTests();
  FunctionInfo f;
  f.qualifiedName = "answer";
  f.parameterTypes = {SemanticType::Int32};
  PersistentDeclarationOwner owner{"example/pkg", "core", "api", ""};
  const auto key = persistentFunctionKey(f, owner);
  expect(key == "gti.function-key.1:11:example/"
                "pkg4:core3:api0:6:answer6:module1:13:i32",
         "golden identity grammar");
  expect(writePersistentFunctionKey(readPersistentFunctionKey(key)) == key,
         "round trip");
  expect(analyzed("int answer(int x) { return x; }", "/tmp/identity-a.gti") ==
             key,
         "semantic projection");
  expect(
      analyzed("int before() { return 0; } int answer(int x) { return x + 1; }",
               "/tmp/elsewhere/identity-b.gti") == key,
      "relocation, order and body independence");
  auto changed = f;
  changed.id = 999;
  changed.sourceUnit = 42;
  changed.returnType = SemanticType::Bool;
  expect(persistentFunctionKey(changed, owner) == key,
         "transient IDs and return type are not overload identity");
  for (unsigned field = 0; field < 3; ++field) {
    auto other = owner;
    std::string *fields[] = {&other.package, &other.target, &other.module};
    *fields[field] += "other";
    expect(persistentFunctionKey(f, other) != key, "logical owner distinction");
  }
  changed = f;
  changed.parameterTypes = {SemanticType::UInt32};
  expect(persistentFunctionKey(changed, owner) != key, "overload distinction");
  changed = f;
  changed.internalLinkage = true;
  rejects([&] { (void)persistentFunctionKey(changed, owner); });
  auto local = owner;
  local.unit = "src/a.gti";
  auto localKey = persistentFunctionKey(changed, local);
  local.unit = "src/b.gti";
  expect(persistentFunctionKey(changed, local) != localKey,
         "source-local owner distinction");
  for (auto path : {"/a.gti", "../a.gti", "src//a.gti", "src/./a.gti", "src/",
                    "C:a.gti", "src\\a.gti"}) {
    local.unit = path;
    rejects([&] { (void)persistentFunctionKey(changed, local); });
  }
  rejects([&] { (void)persistentFunctionKey(f, local); });
  for (auto kind :
       {SemanticType::Unknown, SemanticType::Class, SemanticType::Reference,
        SemanticType::TypeParameter, SemanticType::Void}) {
    changed = f;
    changed.parameterTypes = {kind};
    rejects([&] { (void)persistentFunctionKey(changed, owner); });
  }
  changed = f;
  changed.parameterTypes[0].classId = 1;
  rejects([&] { (void)persistentFunctionKey(changed, owner); });
  changed = f;
  changed.ownerClass = 1;
  rejects([&] { (void)persistentFunctionKey(changed, owner); });
  changed = f;
  changed.parameterPack = true;
  rejects([&] { (void)persistentFunctionKey(changed, owner); });
  changed = f;
  changed.genericParameters.emplace_back();
  rejects([&] { (void)persistentFunctionKey(changed, owner); });
  changed = f;
  changed.compilerPrivate = true;
  rejects([&] { (void)persistentFunctionKey(changed, owner); });
  changed = f;
  changed.linkage = LanguageLinkage::C;
  rejects([&] { (void)persistentFunctionKey(changed, owner); });
  for (std::size_t n = 0; n < key.size(); ++n)
    rejects([&] {
      (void)readPersistentFunctionKey(std::string_view(key).substr(0, n));
    });
  for (auto bad :
       {key + "x", std::string("gti.function-key.2:") + key.substr(19),
        std::string(16385, 'x')})
    rejects([&] { (void)readPersistentFunctionKey(bad); });
  auto bad = key;
  bad.insert(bad.find("11:"), "0");
  rejects([&] { (void)readPersistentFunctionKey(bad); });
  auto record = readPersistentFunctionKey(key);
  record.parameters.resize(257, SemanticType::Int32);
  rejects([&] { (void)writePersistentFunctionKey(record); });
  record = readPersistentFunctionKey(key);
  record.owner.package = std::string(1025, 'a');
  rejects([&] { (void)writePersistentFunctionKey(record); });
  std::cout << "persistent declaration identity checks passed\n";
}
