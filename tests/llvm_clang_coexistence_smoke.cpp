#include "gti/bindgen/header_reader.h"
#include "gti/llvm_backend.h"

int main() {
  // Force both the complete native-backend archive closure and real libclang
  // into one consumer. Loading and orderly shutdown are part of this test.
  const lang::LlvmBackend backend;
  lang::bindgen::ClangCReader reader;
  const auto header = reader.read("int native_probe(int value);\n");
  return backend.name() == "llvm" && header.usable() ? 0 : 1;
}
