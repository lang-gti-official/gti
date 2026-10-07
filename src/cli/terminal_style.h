#pragma once

#include <iosfwd>
#include <string>
#include <string_view>

namespace lang::cli {

enum class ColorMode { Auto, Always, Never };
enum class Style { Reset, Error, Warning, Note, Success };

// Presentation only: never put these escapes in Diagnostic values or artifacts.
class TerminalStyle {
public:
  bool setMode(std::string_view value);
  [[nodiscard]] bool enabled(const std::ostream &stream) const;
  [[nodiscard]] bool interactive(const std::ostream &stream) const;
  [[nodiscard]] unsigned columns(const std::ostream &stream) const;
  [[nodiscard]] std::string_view code(const std::ostream &stream,
                                      Style style) const;

private:
  ColorMode mode_ = ColorMode::Auto;
};

// Used only for captured GTI build children when their destination is plain.
[[nodiscard]] std::string withoutStyles(std::string_view text);

} // namespace lang::cli
