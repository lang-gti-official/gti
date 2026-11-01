#include "gti/compiled_scalar_unit.h"
#include "gti/persistent_declaration.h"
#include <iostream>
#include <stdexcept>

using namespace lang;
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
  throw std::runtime_error("accepted malformed compiled borrow contract");
}
} // namespace
int main() {
  try {
    const auto read = SemanticType::referenceTo(SemanticType::Int32);
    const auto write =
        SemanticType::referenceTo(SemanticType::Int32, AccessMode::Mutable);
    const std::vector<SemanticType> parameters{read, read};
    const CompiledBorrowContract second{BorrowOriginKind::Argument, 1,
                                        AccessMode::ReadOnly};
    validateCompiledBorrowContract(read, parameters, second);
    rejects([&] { validateCompiledBorrowContract(read, parameters, {}); });
    rejects([&] {
      validateCompiledBorrowContract(
          read, parameters,
          {BorrowOriginKind::Argument, 2, AccessMode::ReadOnly});
    });
    rejects([&] {
      validateCompiledBorrowContract(
          read, parameters,
          {BorrowOriginKind::Global, 0, AccessMode::ReadOnly});
    });
    rejects([&] {
      validateCompiledBorrowContract(
          write, parameters,
          {BorrowOriginKind::Argument, 1, AccessMode::Mutable});
    });
    rejects([&] {
      validateCompiledBorrowContract(
          read, std::vector{SemanticType::referenceTo(SemanticType::Int64)},
          {BorrowOriginKind::Argument, 0, AccessMode::ReadOnly});
    });
    rejects([&] {
      validateCompiledBorrowContract(SemanticType::Int32, parameters, second);
    });
    const auto invalidAccess = static_cast<AccessMode>(255);
    const auto invalidReference =
        SemanticType::referenceTo(SemanticType::Int32, invalidAccess);
    rejects([&] {
      validateCompiledBorrowContract(
          invalidReference, std::vector{invalidReference},
          {BorrowOriginKind::Argument, 0, invalidAccess});
    });
    rejects([&] {
      validateCompiledBorrowContract(
          read, std::vector{invalidReference},
          {BorrowOriginKind::Argument, 0, AccessMode::ReadOnly});
    });
    const PersistentTypeRegistry registry;
    PersistentFunctionKey key{
        {"borrow-test", "api", "api", ""}, "choose", false, parameters};
    const auto encoded = writePersistentFunctionKey(key, registry);
    expect(readPersistentFunctionKey(encoded, registry).parameters ==
               parameters,
           "reference key lost parameter access or types");
    auto voidParameter = key;
    voidParameter.parameters = {SemanticType::Void};
    rejects([&] { (void)writePersistentFunctionKey(voidParameter, registry); });
    CompiledScalarSignature signature{
        encoded, read, {}, writePersistentType(read, registry), second};
    const auto symbol = compiledScalarSymbol(signature);
    expect(compiledInterfaceCallableSource(signature).find(
               "[[borrowed_from(arg1)]]") != std::string::npos,
           "facade lost selected argument dependency");
    signature.borrow.parameter = 0;
    expect(compiledScalarSymbol(signature) != symbol,
           "different borrow origin reused the native symbol");
    signature.borrow.access = AccessMode::Mutable;
    rejects([&] { (void)compiledScalarSymbol(signature); });
    key.parameters = {write};
    signature = {writePersistentFunctionKey(key, registry),
                 write,
                 {},
                 writePersistentType(write, registry),
                 {BorrowOriginKind::Argument, 0, AccessMode::Mutable}};
    expect(compiledInterfaceCallableSource(signature).find("mut int32_t&") !=
               std::string::npos,
           "facade lost mutable-reference access");
    std::cout << "compiled borrow identity, access and bounds passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
