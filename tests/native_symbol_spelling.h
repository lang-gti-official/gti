#pragma once

#include <string>
#include <string_view>

namespace gti_test {

// GTI declares each extern "C" function under a private C++ identity and
// binds its link symbol through an assembler label or a COFF alternate name,
// so the symbol never collides with a system header prototype. Returns the
// identity generated C++ uses for `symbol`, or `symbol` itself when unbound.
inline std::string nativeSpelling(std::string_view generated,
                                  std::string_view symbol) {
  const std::string identity = "__gti_c_fn_";
  for (const std::string &label :
       {"__asm__(\"_" + std::string(symbol) + "\")",
        "__asm__(\"" + std::string(symbol) + "\")"}) {
    const std::size_t at = generated.find(label);
    if (at == std::string_view::npos)
      continue;
    const std::size_t begin = generated.rfind(identity, at);
    const std::size_t end = generated.find('(', begin);
    if (begin != std::string_view::npos && end != std::string_view::npos)
      return std::string(generated.substr(begin, end - begin));
  }
  const std::string directive = "=" + std::string(symbol) + "\")";
  const std::size_t at = generated.find(directive);
  if (at == std::string_view::npos)
    return std::string(symbol);
  const std::size_t begin = generated.rfind(':', at);
  return std::string(generated.substr(begin + 1, at - begin - 1));
}

} // namespace gti_test
