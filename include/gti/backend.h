#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace lang {

class LoweredProgram;

enum class BackendArtifactKind {
  Source,
  Object,
};

struct BackendArtifact {
  BackendArtifactKind kind = BackendArtifactKind::Source;
  std::string contents;
  std::string extension;
};

// A verified GTI program can reach a backend capability that is deliberately
// not implemented yet. This is distinct from an invariant failure: drivers
// must publish it as a stable limitation diagnostic rather than an ICE.
class BackendUnsupportedError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
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
