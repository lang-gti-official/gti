#pragma once

#include "gti/diagnostic.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lang {

class LoweredProgram;

enum class BackendKind {
  Cpp,
  Llvm,
};

[[nodiscard]] std::string_view backendKindName(BackendKind backend);
[[nodiscard]] std::optional<BackendKind>
parseBackendKind(std::string_view name);

enum class BackendArtifactKind {
  Source,
  Object,
};

struct EmbeddingCompanion {
  std::string header;
  std::vector<std::string> publicSymbols;
};

struct BackendArtifact {
  BackendArtifactKind kind = BackendArtifactKind::Source;
  std::string contents;
  std::string extension;
  std::optional<EmbeddingCompanion> embedding;
};

// A verified GTI program can reach a backend capability that is deliberately
// not implemented yet. This is distinct from an invariant failure: drivers
// must publish it as a stable limitation diagnostic rather than an ICE.
class BackendUnsupportedError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;

  BackendUnsupportedError(std::string message, SourceSpan source)
      : std::runtime_error(std::move(message)), source(std::move(source)) {}

  [[nodiscard]] const std::optional<SourceSpan> &location() const {
    return source;
  }

private:
  std::optional<SourceSpan> source;
};

class Backend {
public:
  Backend() = default;
  Backend(const Backend &) = delete;
  Backend &operator=(const Backend &) = delete;
  virtual ~Backend() = default;

  [[nodiscard]] virtual std::string_view name() const = 0;
  // Consumes a detached value. Drivers hand off their finished program without
  // copying it; a caller retaining its input may deliberately pass an lvalue.
  [[nodiscard]] virtual BackendArtifact generate(LoweredProgram program) = 0;
};

} // namespace lang
