#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace lang {
struct MirProgramTestAccess {
  static auto &functions(MirProgram &program) { return program.functions; }
};
} // namespace lang

int main() {
  int failures = 0;
  const auto expect = [&](bool condition, const std::string &message) {
    if (!condition) {
      std::cerr << message << '\n';
      ++failures;
    }
  };
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto analyze = [&](const std::string &source) {
    auto result = lang::Frontend().analyze(
        root / "tests" / "complete-borrow-summary.gti", source,
        {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
    if (!result.canGenerateCode())
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(result.canGenerateCode(), "result-summary fixture must lower");
    return result;
  };
  const auto functionId = [](const lang::FrontendResult &result,
                             const std::string &name) {
    for (const auto &function : result.hir.functionInstances())
      if (function.source && function.source->name().lexeme == name)
        return function.id;
    return lang::HirFunctionInstanceId{0};
  };
  const std::string token = "enum Token { empty, item(int value) };\n";
  const std::string leaf = "Token leaf() { return Token::empty; }\n";
  const std::string forwarding = "Token forward() { return leaf(); }\n";
  for (const auto &declarations : {leaf + forwarding, forwarding + leaf}) {
    const auto result =
        analyze(token + declarations +
                "int main() { auto value = forward(); return 0; }");
    if (!result.canGenerateCode())
      continue;
    const auto *forward =
        result.mir.findFunctionInstance(functionId(result, "forward"));
    expect(forward && forward->completeReturnBorrow &&
               forward->completeReturnBorrow->possibleAlternatives.size() == 1,
           "forward result evidence must be independent of declaration order");
    bool provedCall = false;
    if (forward)
      for (const auto &block : forward->body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::Call &&
              instruction.result)
            provedCall |= forward->body.findValue(*instruction.result)
                              ->borrowDependencies.has_value();
    expect(provedCall,
           "exact calls transport independently verified complete facts");
    auto missing = result.mir;
    auto &functions = lang::MirProgramTestAccess::functions(missing);
    functions[functionId(result, "leaf") - 1].completeReturnBorrow.reset();
    expect(!lang::verifyMirProgram(missing).valid(),
           "caller cannot rely on a missing callee result contract");
  }

  const auto joined = analyze(token + R"gti(
Token choose(bool first) {
  if (first) { return Token::empty; }
  return Token::item(7);
}
Token narrowed() {
  if (true) { return Token::empty; }
  return Token::item(8);
}
int main() { auto value = choose(true); return 0; }
)gti");
  if (joined.canGenerateCode()) {
    const auto id = functionId(joined, "choose");
    const auto *function = joined.mir.findFunctionInstance(id);
    expect(function && function->completeReturnBorrow &&
               function->completeReturnBorrow->possibleAlternatives.size() == 2,
           "all reachable return alternatives contribute to the contract");
    if (function && function->completeReturnBorrow) {
      for (int mutation = 0; mutation < 6; ++mutation) {
        auto malformed = joined.mir;
        auto &summary =
            *lang::MirProgramTestAccess::functions(malformed)[id - 1]
                 .completeReturnBorrow;
        if (mutation == 0)
          summary.possibleAlternatives.pop_back();
        if (mutation == 1)
          summary.possibleAlternatives.back() =
              summary.possibleAlternatives.front();
        if (mutation == 2)
          summary.possibleAlternatives.front().conditions.front().enumeration =
              lang::SemanticType::Int32;
        if (mutation == 3)
          summary.possibleAlternatives.front().conditions.front().variant =
              1000;
        if (mutation == 4)
          summary.possibleAlternatives.front().conditions.front().subobject = {
              1};
        if (mutation == 5)
          summary.dependencies.push_back({});
        expect(!lang::verifyMirProgram(malformed).valid(),
               "forged, missing or substituted result contract must fail: " +
                   std::to_string(mutation));
      }
    }
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = joined.hir, .mir = joined.mir, .level = level});
      expect(
          optimized.valid(),
          "optimizer narrowing preserves conservative declaration contracts");
    }
  }

  const auto single = analyze(token + leaf + "int main() { return 0; }");
  if (single.canGenerateCode()) {
    auto conservative = single.mir;
    auto &function = lang::MirProgramTestAccess::functions(
        conservative)[functionId(single, "leaf") - 1];
    if (function.completeReturnBorrow) {
      auto alternative =
          function.completeReturnBorrow->possibleAlternatives.front();
      alternative.conditions.front().variant = 1;
      function.completeReturnBorrow->possibleAlternatives.push_back(
          alternative);
      expect(lang::verifyMirProgram(conservative).valid(),
             "extra well-typed dependency-free alternative is conservative");
    }
  }

  for (const std::string &source :
       {"Token unknown(Token input) { return input; }",
        "Token unknown() { return unknown(); }",
        "Token other() { return unknown(); } Token unknown() { return other(); "
        "}",
        "Token unknown() { while (true) {} }",
        "void replace(mut Token& value) { value = Token::item(9); } Token "
        "unknown() { mut auto value = Token::empty; replace(value); return "
        "value; }"}) {
    const auto result = analyze(token + source + " int main() { return 0; }");
    if (!result.canGenerateCode())
      continue;
    const auto id = functionId(result, "unknown");
    const auto *function = result.mir.findFunctionInstance(id);
    expect(function && !function->completeReturnBorrow,
           "unknown/recursive/nonreturning/mutated result remains unproved: " +
               source);
    auto malformed = result.mir;
    for (auto &candidate : lang::MirProgramTestAccess::functions(malformed))
      if (candidate.returnType == function->returnType)
        candidate.completeReturnBorrow = lang::FunctionBorrowDependencySummary{
            .possibleAlternatives = {
                {.conditions = {
                     {.enumeration = candidate.returnType, .variant = 0}}}}};
    expect(!lang::verifyMirProgram(malformed).valid(),
           "forged cycle/no-return/unknown evidence cannot justify itself");
  }

  const auto generic = analyze(R"gti(
enum Box<T> { empty, item(T value) };
Box<T> factory<T>(T value) { return Box<T>::item(value); }
Box<int> integer() { return factory(4); }
Box<bool> boolean() { return factory(true); }
int main() { auto first = integer(); auto second = boolean(); return 0; }
)gti");
  if (generic.canGenerateCode()) {
    const auto integer = functionId(generic, "integer");
    const auto boolean = functionId(generic, "boolean");
    const auto *left = generic.mir.findFunctionInstance(integer);
    const auto *right = generic.mir.findFunctionInstance(boolean);
    expect(left && right && left->completeReturnBorrow &&
               right->completeReturnBorrow,
           "concrete generic calls compute exact result summaries");
    if (left && right && left->completeReturnBorrow &&
        right->completeReturnBorrow) {
      auto malformed = generic.mir;
      lang::MirProgramTestAccess::functions(malformed)[integer - 1]
          .completeReturnBorrow = right->completeReturnBorrow;
      expect(
          !lang::verifyMirProgram(malformed).valid(),
          "one concrete instance cannot substitute another's typed contract");
    }
  }
  // Retention is an optimization, not another language acceptance limit.
  // More distinct queries than the cache retains must still permit a later
  // query for the first concrete instance to rebuild the same exact summary.
  std::string evicted = token + R"gti(
Token factory<uint64_t N>() { return Token::item(int(N)); }
Token before() { return factory<0>(); }
)gti";
  for (unsigned index = 1; index != 72; ++index)
    evicted += "Token item" + std::to_string(index) +
               "() { return factory<" + std::to_string(index) + ">(); }\n";
  evicted += "Token after() { return factory<0>(); }\n"
             "int main() { auto first = before(); auto last = after(); return 0; }";
  const auto cache = analyze(evicted);
  if (cache.canGenerateCode()) {
    const auto *before = cache.mir.findFunctionInstance(functionId(cache, "before"));
    const auto *after = cache.mir.findFunctionInstance(functionId(cache, "after"));
    expect(before && after && before->completeReturnBorrow &&
               before->completeReturnBorrow == after->completeReturnBorrow,
           "eviction and requery preserve exact result contracts");
  }
  const auto chain = [&](unsigned depth) {
    return token + R"gti(
Token bounded<uint64_t N>() {
  if constexpr (N == 0) { return Token::empty; }
  else { return bounded<N - 1>(); }
}
int main() { auto value = bounded<)gti" + std::to_string(depth) +
           ">(); return 0; }";
  };
  const auto finite = analyze(chain(40));
  expect(finite.canGenerateCode(),
         "finite result-summary chain beyond the old private depth32 succeeds");
  const auto exhausted = lang::Frontend().analyze(
      root / "tests" / "complete-borrow-resource.gti", chain(66),
      {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
  expect(!exhausted.canGenerateCode() &&
             std::any_of(exhausted.diagnostics.begin(),
                         exhausted.diagnostics.end(), [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2105" &&
                                  diagnostic.primary.end > diagnostic.primary.start &&
                                  !diagnostic.primary.source.empty() &&
                                  !diagnostic.related.empty() &&
                                  !diagnostic.hints.empty() &&
                                  diagnostic.fixes.empty();
                         }),
         "resource exhaustion reports the established located diagnostic");
  return failures ? 1 : 0;
}
