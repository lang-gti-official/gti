#include "compiler_test_fixture.h"
#include "cpp_backend_test_support.h"

// C++ representation of owning callable adapters: the ADL adapter is declared
// in its owner and defined after the result layout. The backend-neutral
// contract lives in owning_function_tests.cpp.
int main() {
  const auto nominalPath = std::filesystem::path(__FILE__).parent_path() /
                           "fixtures/nested_nominal_owned_callback.gti";
  std::ifstream nominalInput(nominalPath);
  expect(nominalInput.good(), "owning nominal-result callback fixture exists");
  const std::string nominalSource{std::istreambuf_iterator<char>(nominalInput),
                                  {}};
  const auto nominal =
      analyzeTrustedPreludeFixture(nominalPath.string(), nominalSource);
  expect(nominal.canGenerateCode(),
         "a stored owning target may return a class value");
  if (nominal.canGenerateCode()) {
    const auto text = gti_test::emitCppText(nominal);
    const auto declaration =
        text.find("friend ::__gti_program::Packet __gti_invoke(const Target &");
    const auto definition =
        text.find("inline ::__gti_program::Packet __gti_invoke(const Target &");
    const auto layout = text.find("struct Packet {");
    expect(declaration != std::string::npos &&
               definition != std::string::npos && layout != std::string::npos &&
               layout < definition && declaration < definition,
           "the ADL adapter is declared in its owner and defined after the "
           "result layout");
    if (declaration != std::string::npos)
      expect(text.find(';', declaration) < text.find('{', declaration),
             "the in-class friend must not instantiate an incomplete class "
             "result");
  }
  const auto orderPath =
      nominalPath.parent_path() / "callable_adapter_declaration_order.gti";
  std::ifstream orderInput(orderPath);
  expect(orderInput.good(), "cyclic adapter/layout dependency fixture exists");
  const std::string orderSource{std::istreambuf_iterator<char>(orderInput), {}};
  const auto ordered =
      analyzeTrustedPreludeFixture(orderPath.string(), orderSource);
  expect(ordered.canGenerateCode(),
         "a callable may return a class containing that callable by value");
  if (ordered.canGenerateCode()) {
    const auto text = gti_test::emitCppText(ordered);
    const auto declaration =
        text.find("friend ::__gti_program::api::Packet __gti_invoke(");
    const auto layout = text.find("struct Packet {");
    const auto definition =
        text.find("inline ::__gti_program::api::Packet __gti_invoke(");
    expect(declaration != std::string::npos && layout != std::string::npos &&
               definition != std::string::npos && declaration < layout &&
               layout < definition,
           "adapter code must not create a false recursive layout dependency");
  }
  return failures == 0 ? 0 : 1;
}
