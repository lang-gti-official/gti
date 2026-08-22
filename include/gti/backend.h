#pragma once

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

class Backend {
public:
  Backend() = default;
  Backend(const Backend &) = delete;
  Backend &operator=(const Backend &) = delete;
  virtual ~Backend() = default;

  [[nodiscard]] virtual std::string_view name() const = 0;
  [[nodiscard]] virtual BackendArtifact
  generate(const LoweredProgram &program) = 0;
};

} // namespace lang
