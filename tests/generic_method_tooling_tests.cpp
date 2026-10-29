#include "gti/formatter.h"
#include "gti/lexer.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

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
} // namespace
int main() {
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
  std::cout << "generic method tooling checks passed\n";
}
