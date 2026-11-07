#include "gti/formatter.h"
#include "gti/lexer.h"
#include "gti/language_queries.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace lang {
struct SemanticModelTestAccess {
  static void record(SemanticModel &model, GenericParameterInfo parameter) {
    model.recordGenericParameter(std::move(parameter));
  }
};
} // namespace lang

namespace {
void expect(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
void check(std::string_view source, std::string_view signature) {
  const auto output = lang::Formatter().format(source);
  if (output.find(signature) == std::string::npos)
    std::cerr << output;
  expect(output.find(signature) != std::string::npos,
         "generic qualified signatures retain type spacing");
  expect(lang::Formatter().format(output) == output,
         "generic owner formatting is idempotent");
  lang::Lexer originalLexer, formattedLexer;
  const auto original = originalLexer.scan(std::string(source));
  const auto formatted = formattedLexer.scan(output);
  expect(!originalLexer.hadError() && !formattedLexer.hadError() &&
             original.size() == formatted.size() &&
             std::equal(original.begin(), original.end(), formatted.begin(),
                        [](const auto &a, const auto &b) {
                          // EOF is a synthetic sentinel, not a source token.
                          // Compare every actual source token exactly.
                          return a.kind == b.kind &&
                                 (a.kind == lang::TokenKind::END_OF_FILE ||
                                  a.lexeme == b.lexeme);
                        }),
         "generic owner formatting preserves source tokens");
}
void selectedLifetimeIdentity() {
  lang::SemanticModel model;
  const lang::GenericParameterInfo hidden{
      .id = 10, .name = {lang::TokenKind::IDENTIFIER, "$lifetime10", {}, 0, 1},
      .lifetime = true, .implicit = true};
  const lang::GenericParameterInfo written{
      .id = 20, .name = {lang::TokenKind::IDENTIFIER, "operation", {}, 0, 1},
      .lifetime = true};
  const lang::GenericParameterInfo actual{
      .id = 30, .name = {lang::TokenKind::IDENTIFIER, "caller", {}, 0, 1},
      .lifetime = true};
  for (const auto &parameter : {hidden, written, actual})
    lang::SemanticModelTestAccess::record(model, parameter);
  const lang::SignaturePrinter printer(model);
  lang::FunctionInfo function{.qualifiedName = "choose", .returnType = lang::SemanticType::Int32,
                              .genericParameters = {hidden, written}};
  lang::ResolvedCallInfo selected{.returnType = lang::SemanticType::Int32,
      .lifetimeArguments = {lang::Lifetime::staticLifetime(), lang::Lifetime::parameter(actual.id)},
      .lifetimeApplication = {.bindings = {{lang::Lifetime::parameter(hidden.id), lang::Lifetime::staticLifetime()},
                                           {lang::Lifetime::parameter(written.id), lang::Lifetime::parameter(actual.id)}}}};
  expect(printer.function(function, &selected) == "int32_t choose<caller>()",
         "selected operation lifetime follows its binder ID after an implicit conformance head");
  expect(printer.function(function) == "int32_t choose<lifetime operation>()",
         "implicit lifetime names do not appear in source declaration signatures");
  selected.lifetimeApplication.bindings.pop_back();
  expect(printer.function(function, &selected) == "int32_t choose<_>()",
         "missing selected binder evidence cannot guess from the hidden presentation prefix");
  selected.lifetimeApplication.bindings.push_back({lang::Lifetime::parameter(written.id), lang::Lifetime::staticLifetime()});
  selected.lifetimeApplication.bindings.push_back({lang::Lifetime::parameter(written.id), lang::Lifetime::parameter(actual.id)});
  expect(printer.function(function, &selected) == "int32_t choose<_>()",
         "conflicting selected binder facts cannot choose the first lifetime");
  function.genericParameters = {hidden};
  expect(printer.function(function, &selected) == "int32_t choose()",
         "an entirely implicit head adds no empty source generic list");
  function.genericParameters = {written};
  selected.lifetimeApplication.bindings.clear();
  selected.lifetimeArguments = {lang::Lifetime::staticLifetime()};
  expect(printer.function(function, &selected) == "int32_t choose<static>()",
         "the documented source-only explicit lifetime prefix remains a display fallback");
  lang::GenericParameterInfo projected = written;
  projected.id = 40;
  projected.name.lexeme = "R";
  projected.lifetime = false;
  lang::ClassTypeInfo patterned{.id = 1, .qualifiedName = "Callback",
      .genericParameters = {projected, hidden, written},
      .genericArgumentPatterns = {lang::SemanticType::functionSignature(
          lang::SemanticType::Int32, {lang::SemanticType::Int32})}};
  expect(printer.classType(patterned) == "class Callback<int32_t(int32_t), lifetime operation>",
         "patterned nominal hover retains its explicit lifetime suffix without repeating projected binders");
}
} // namespace
int main() {
  selectedLifetimeIdentity();
  check("T Box < T >::read(){return this.value;}", "T Box<T>::read()");
  check("U n::Box < T >::convert < U >(U value){return value;}",
        "U n::Box<T>::convert<U>(U value)");
  check("Parcel < T > n::Box < T >::make(){return Parcel<T>();}",
        "Parcel<T> n::Box<T>::make()");
  check(
      "Parcel < U > n::Box < T >::convert < U >(U value){return Parcel<U>();}",
      "Parcel<U> n::Box<T>::convert<U>(U value)");
  check("Parcel < T > n:: /* owner */ Box < T >::make(){}",
        "Parcel<T> n:: /* owner */ Box<T>::make()");
  check("bool compare(){return left<middle>limits::value<int>();}",
        "return left < middle > limits::value<int>();");
  check("class Callback <R&<result>(Args...),lifetime environment>{int&<environment> value;};",
        "class Callback<R&<result>(Args...), lifetime environment>");
  check("struct Reference<T&<borrowed>,lifetime owner>{};",
        "struct Reference<T&<borrowed>, lifetime owner>");
  std::cout << "generic method tooling checks passed\n";
}
