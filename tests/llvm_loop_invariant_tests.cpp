#include "../src/compiler/llvm/loop_invariants.h"
#include <iostream>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <stdexcept>
#include <string>

namespace {
void replace(std::string &text, const std::string &from,
             const std::string &to) {
  const auto offset = text.find(from);
  if (offset == std::string::npos)
    throw std::runtime_error("fixture token missing");
  text.replace(offset, from.size(), to);
}
void check(const std::string &name, const std::string &initial,
           const std::string &fast, const std::string &next,
           const std::string &qualifier, unsigned expected,
           const std::string &shape = "") {
  std::string source = R"IR(target triple = "arm64-apple-macosx"
@first = internal constant [3 x i64] [i64 0, i64 0, i64 7]
@second = internal constant [3 x i64] [i64 0, i64 0, i64 9]
@mutable = internal global [3 x i64] [i64 0, i64 0, i64 7]
@external = external constant [3 x i64]
declare ptr @opaque(ptr)
define i64 @loop(i64 %limit, ptr %input) {
entry:
 br label %header
header:
 %state = phi ptr [ INITIAL, %entry ], [ %next, %latch ]
 %index = phi i64 [ 0, %entry ], [ %incremented, %latch ]
 %address = getelementptr [3 x i64], ptr %state, i64 0, i64 2
 %tag = load VOLATILE i64, ptr %address, align 8
 %match = icmp eq i64 %tag, 7
 br i1 %match, label %fast, label %slow
fast:
 FAST
 br label %latch
slow:
 %fallback = call ptr @opaque(ptr %state)
 br label %latch
latch:
 %next = phi ptr [ FASTVALUE, %fast ], [ %fallback, %slow ]
 %incremented = add i64 %index, 1
 %again = icmp ult i64 %incremented, %limit
 br i1 %again, label %header, label %exit
exit:
 ret i64 %tag
}
)IR";
  replace(source, "INITIAL", initial);
  replace(source, "FASTVALUE", next);
  replace(source, "FAST", fast);
  replace(source, "VOLATILE", qualifier);
  if (qualifier == "atomic")
    replace(source, "ptr %address, align 8", "ptr %address monotonic, align 8");
  if (shape == "undefined-slot")
    replace(source, "i64 7]", "i64 undef]");
  if (shape == "external-initializer")
    replace(source, "internal constant",
            "internal externally_initialized constant");
  if (shape == "same-entries" || shape == "mixed-entries") {
    replace(source, "entry:\n br label %header", R"IR(entry:
 %entry_test = icmp eq ptr %input, null
 br i1 %entry_test, label %entry_a, label %entry_b
entry_a:
 br label %header
entry_b:
 br label %header)IR");
    replace(source, "[ @first, %entry ]",
            shape == "same-entries"
                ? "[ @first, %entry_a ], [ @first, %entry_b ]"
                : "[ @first, %entry_a ], [ @second, %entry_b ]");
    replace(source, "[ 0, %entry ]", "[ 0, %entry_a ], [ 0, %entry_b ]");
  }
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(source, diagnostic, context);
  if (!module) {
    diagnostic.print("loop invariant fixture", llvm::errs());
    throw std::runtime_error("invalid " + name);
  }
  module->setDataLayout("e-p:64:64");
  if (llvm::verifyModule(*module, &llvm::errs()))
    throw std::runtime_error("unverified " + name);
  const auto changed = lang::native_llvm::foldInductivePointerLoops(*module);
  if (changed != expected || llvm::verifyModule(*module, &llvm::errs()))
    throw std::runtime_error("wrong induction result: " + name);
  if (lang::native_llvm::foldInductivePointerLoops(*module))
    throw std::runtime_error("non-idempotent small fixture: " + name);
}
} // namespace
int main() {
  check("constant", "@first", "", "%state", "", 1);
  check("mutable table", "@mutable", "", "%state", "", 0);
  check("unknown table", "@external", "", "%state", "", 0);
  check("unknown entry", "%input", "", "%state", "", 0);
  check("reachable replacement", "@first", "", "@second", "", 0);
  check("reachable callback", "@first",
        "%replacement = call ptr @opaque(ptr %state)", "%replacement", "", 0);
  check("volatile", "@first", "", "%state", "volatile", 0);
  check("unknown select", "@first",
        "%predicate = icmp ne ptr %input, null\n %replacement = select i1 "
        "%predicate, ptr %state, ptr @second",
        "%replacement", "", 0);
  check("same select", "@first",
        "%predicate = icmp eq i64 7, 7\n %replacement = select i1 %predicate, "
        "ptr %state, ptr @second",
        "%replacement", "", 1);
  check("null entry", "null", "", "%state", "", 0);
  check("poison entry", "poison", "", "%state", "", 0);
  check("undefined entry", "undef", "", "%state", "", 0);
  check("atomic", "@first", "", "%state", "atomic", 0);
  check("undefined slot", "@first", "", "%state", "", 0, "undefined-slot");
  check("external initialization", "@first", "", "%state", "", 0,
        "external-initializer");
  check("same entry constants", "@first", "", "%state", "", 1, "same-entries");
  check("mixed entry constants", "@first", "", "%state", "", 0,
        "mixed-entries");
  std::cout << "17 inductive pointer controls passed\n";
}
