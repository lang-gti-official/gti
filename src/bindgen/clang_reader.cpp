#include "gti/bindgen/header_reader.h"

#include <clang-c/Index.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// ClangCReader: the libclang-backed C17 declaration reader (ADR 021). One
// in-memory Clang translation unit provides preprocessing, parsing, and
// target integer widths; this file translates Clang facts into the GTI-owned
// NativeModel and no Clang type, cursor, or string crosses the reader
// interface. Reading stays fail-soft per declaration: anything outside the
// bounded surface becomes a warning diagnostic plus a recorded skip, never an
// abort, so one exotic declaration cannot cost the rest of the header.
//
// Includes are deliberately not resolved at this seam (`-nostdinc`): the
// model imports the given text alone, and every `#include` reports the same
// unresolved-include note the pipeline has always emitted. Include discovery
// and original-source provenance are milestone C-05.

namespace lang::bindgen {
namespace {

// Virtual absolute paths keep the in-memory translation unit self-contained:
// the `-include` prelude resolves by exact name, never by header search.
constexpr const char *kInputPath = "/gti-bindgen/input.h";
constexpr const char *kBuiltinsPath = "/gti-bindgen/builtins.h";

// <stdint.h>/<stddef.h> names resolve without their headers, exactly as the
// bounded reader resolved them through CTargetProfile. Clang's target macros
// keep every width a Clang fact for the selected data model; references to
// these typedefs resolve canonically, so they never surface as model aliases.
constexpr const char *kBuiltinsText = "typedef __INT8_TYPE__ int8_t;\n"
                                      "typedef __INT16_TYPE__ int16_t;\n"
                                      "typedef __INT32_TYPE__ int32_t;\n"
                                      "typedef __INT64_TYPE__ int64_t;\n"
                                      "typedef __UINT8_TYPE__ uint8_t;\n"
                                      "typedef __UINT16_TYPE__ uint16_t;\n"
                                      "typedef __UINT32_TYPE__ uint32_t;\n"
                                      "typedef __UINT64_TYPE__ uint64_t;\n"
                                      "typedef __SIZE_TYPE__ size_t;\n"
                                      "typedef __UINTPTR_TYPE__ uintptr_t;\n"
                                      "typedef __INTPTR_TYPE__ intptr_t;\n"
                                      "typedef __PTRDIFF_TYPE__ ptrdiff_t;\n"
                                      "typedef __PTRDIFF_TYPE__ ssize_t;\n";

// The reader receives a C data model, not a triple: parsing uses one
// canonical 64-bit target per data model so identical input produces
// identical Clang facts on every host. This is the bounded target seam until
// the pipeline forwards its real triple with original-source provenance
// (C-05/C-06).
[[nodiscard]] const char *canonicalTriple(const CTargetProfile &profile) {
  return profile.dataModel() == CDataModel::Llp64 ? "x86_64-pc-windows-msvc"
                                                  : "x86_64-unknown-linux-gnu";
}

[[nodiscard]] std::string owned(CXString value) {
  const char *text = clang_getCString(value);
  std::string result = text != nullptr ? text : std::string();
  clang_disposeString(value);
  return result;
}

[[nodiscard]] SourceOrigin originOf(CXSourceLocation location) {
  unsigned line = 0;
  unsigned column = 0;
  clang_getSpellingLocation(location, nullptr, &line, &column, nullptr);
  return {line, column};
}

[[nodiscard]] SourceOrigin originOf(CXCursor cursor) {
  return originOf(clang_getCursorLocation(cursor));
}

[[nodiscard]] bool inMainFile(CXCursor cursor) {
  return clang_Location_isFromMainFile(clang_getCursorLocation(cursor)) != 0;
}

[[nodiscard]] bool inBuiltinsFile(CXCursor cursor) {
  CXFile file = nullptr;
  clang_getSpellingLocation(clang_getCursorLocation(cursor), &file, nullptr,
                            nullptr, nullptr);
  return file != nullptr && owned(clang_getFileName(file)) == kBuiltinsPath;
}

[[nodiscard]] bool endsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size()) == suffix;
}

// Evaluates an object-like macro body as a bounded C integer constant
// expression: int64 arithmetic over integer literals, references to
// previously imported macro constants, parentheses, unary + - ~, and the
// binary operators | ^ & << >> + - * / % at C precedence. Anything else —
// strings, floats, casts, ternaries, unknown names, values outside int64 —
// makes the macro non-importable rather than approximated.
class MacroExpressionEvaluator {
public:
  MacroExpressionEvaluator(const std::vector<std::string> &tokens,
                           const std::map<std::string, std::int64_t> &known)
      : tokens_(tokens), known_(known) {}

  [[nodiscard]] std::optional<std::int64_t> evaluate() {
    const std::optional<std::int64_t> value = parseBitOr();
    return value.has_value() && position_ == tokens_.size() ? value
                                                            : std::nullopt;
  }

private:
  const std::vector<std::string> &tokens_;
  const std::map<std::string, std::int64_t> &known_;
  std::size_t position_ = 0;

  [[nodiscard]] bool accept(std::string_view token) {
    if (position_ < tokens_.size() && tokens_[position_] == token) {
      ++position_;
      return true;
    }
    return false;
  }

  // NOLINTNEXTLINE(misc-no-recursion)
  [[nodiscard]] std::optional<std::int64_t> parseBitOr() {
    std::optional<std::int64_t> left = parseBitXor();
    while (left.has_value() && accept("|")) {
      const std::optional<std::int64_t> right = parseBitXor();
      left = right.has_value() ? std::optional(*left | *right) : std::nullopt;
    }
    return left;
  }

  // NOLINTNEXTLINE(misc-no-recursion)
  [[nodiscard]] std::optional<std::int64_t> parseBitXor() {
    std::optional<std::int64_t> left = parseBitAnd();
    while (left.has_value() && accept("^")) {
      const std::optional<std::int64_t> right = parseBitAnd();
      left = right.has_value() ? std::optional(*left ^ *right) : std::nullopt;
    }
    return left;
  }

  // NOLINTNEXTLINE(misc-no-recursion)
  [[nodiscard]] std::optional<std::int64_t> parseBitAnd() {
    std::optional<std::int64_t> left = parseShift();
    while (left.has_value() && accept("&")) {
      const std::optional<std::int64_t> right = parseShift();
      left = right.has_value() ? std::optional(*left & *right) : std::nullopt;
    }
    return left;
  }

  // NOLINTNEXTLINE(misc-no-recursion)
  [[nodiscard]] std::optional<std::int64_t> parseShift() {
    std::optional<std::int64_t> left = parseAdditive();
    while (left.has_value()) {
      const bool shiftLeft = position_ < tokens_.size() && accept("<<");
      const bool shiftRight = !shiftLeft && accept(">>");
      if (!shiftLeft && !shiftRight) {
        break;
      }
      const std::optional<std::int64_t> right = parseAdditive();
      if (!right.has_value() || *right < 0 || *right > 62) {
        return std::nullopt;
      }
      if (shiftLeft) {
        // Shifting into or past the sign bit leaves the int64 contract.
        const std::int64_t limit = std::int64_t{1} << (62 - *right);
        if (*left < 0 || *left > limit) {
          return std::nullopt;
        }
        left = *left << *right;
      } else {
        left = *left >> *right;
      }
    }
    return left;
  }

  // NOLINTNEXTLINE(misc-no-recursion)
  [[nodiscard]] std::optional<std::int64_t> parseAdditive() {
    std::optional<std::int64_t> left = parseMultiplicative();
    while (left.has_value()) {
      const bool add = accept("+");
      const bool subtract = !add && accept("-");
      if (!add && !subtract) {
        break;
      }
      const std::optional<std::int64_t> right = parseMultiplicative();
      if (!right.has_value()) {
        return std::nullopt;
      }
      std::int64_t result = 0;
      if (add ? __builtin_add_overflow(*left, *right, &result)
              : __builtin_sub_overflow(*left, *right, &result)) {
        return std::nullopt;
      }
      left = result;
    }
    return left;
  }

  // NOLINTNEXTLINE(misc-no-recursion)
  [[nodiscard]] std::optional<std::int64_t> parseMultiplicative() {
    std::optional<std::int64_t> left = parseUnary();
    while (left.has_value()) {
      const bool multiply = accept("*");
      const bool divide = !multiply && accept("/");
      const bool remainder = !multiply && !divide && accept("%");
      if (!multiply && !divide && !remainder) {
        break;
      }
      const std::optional<std::int64_t> right = parseUnary();
      if (!right.has_value() || (!multiply && *right == 0)) {
        return std::nullopt;
      }
      if (multiply) {
        std::int64_t result = 0;
        if (__builtin_mul_overflow(*left, *right, &result)) {
          return std::nullopt;
        }
        left = result;
      } else if (*left == std::numeric_limits<std::int64_t>::min() &&
                 *right == -1) {
        return std::nullopt;
      } else {
        left = divide ? *left / *right : *left % *right;
      }
    }
    return left;
  }

  // NOLINTNEXTLINE(misc-no-recursion)
  [[nodiscard]] std::optional<std::int64_t> parseUnary() {
    if (accept("+")) {
      return parseUnary();
    }
    if (accept("-")) {
      const std::optional<std::int64_t> value = parseUnary();
      return value.has_value() &&
                     *value != std::numeric_limits<std::int64_t>::min()
                 ? std::optional(-*value)
                 : std::nullopt;
    }
    if (accept("~")) {
      const std::optional<std::int64_t> value = parseUnary();
      return value.has_value() ? std::optional(~*value) : std::nullopt;
    }
    return parsePrimary();
  }

  // NOLINTNEXTLINE(misc-no-recursion)
  [[nodiscard]] std::optional<std::int64_t> parsePrimary() {
    if (accept("(")) {
      const std::optional<std::int64_t> value = parseBitOr();
      return value.has_value() && accept(")") ? value : std::nullopt;
    }
    if (position_ >= tokens_.size()) {
      return std::nullopt;
    }
    const std::string &token = tokens_[position_];
    if (const auto known = known_.find(token); known != known_.end()) {
      ++position_;
      return known->second;
    }
    return parseIntegerLiteral();
  }

  [[nodiscard]] std::optional<std::int64_t> parseIntegerLiteral() {
    const std::string &token = tokens_[position_];
    if (token.empty() ||
        std::isdigit(static_cast<unsigned char>(token.front())) == 0) {
      return std::nullopt;
    }
    std::size_t digits = 0;
    unsigned long long value = 0;
    try {
      value = std::stoull(token, &digits, 0);
    } catch (...) {
      return std::nullopt;
    }
    // Only integer suffixes may follow the digits; anything else (a float's
    // '.' or exponent, a malformed literal) refuses the macro.
    for (std::size_t index = digits; index < token.size(); ++index) {
      const char suffix = static_cast<char>(
          std::tolower(static_cast<unsigned char>(token[index])));
      if (suffix != 'u' && suffix != 'l') {
        return std::nullopt;
      }
    }
    if (value > static_cast<unsigned long long>(
                    std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    ++position_;
    return static_cast<std::int64_t>(value);
  }
};

class ModelBuilder {
public:
  ModelBuilder(CXTranslationUnit unit, ReadResult &result)
      : unit_(unit), result_(result) {
    rootScope_ = result_.model.addScope(
        {NativeScopeKind::TranslationUnit, std::string(), 0});
  }

  // Translates Clang's diagnostics into reader diagnostics. Unresolved
  // includes keep the pipeline's canonical note; every other native error is
  // surfaced verbatim so a skipped declaration explains itself.
  void collectNativeDiagnostics() {
    const unsigned count = clang_getNumDiagnostics(unit_);
    for (unsigned index = 0; index < count; ++index) {
      CXDiagnostic diagnostic = clang_getDiagnostic(unit_, index);
      const CXDiagnosticSeverity severity =
          clang_getDiagnosticSeverity(diagnostic);
      if (severity >= CXDiagnostic_Error) {
        const SourceOrigin origin =
            originOf(clang_getDiagnosticLocation(diagnostic));
        const std::string text = owned(clang_getDiagnosticSpelling(diagnostic));
        if (endsWith(text, "file not found")) {
          warn(origin,
               "#include is not resolved; declarations from the included "
               "header are unavailable to this import");
        } else {
          warn(origin, "native C error: " + text);
          nativeErrorLines_.push_back(origin.line);
        }
      }
      clang_disposeDiagnostic(diagnostic);
    }
    std::sort(nativeErrorLines_.begin(), nativeErrorLines_.end());
  }

  void run() {
    clang_visitChildren(clang_getTranslationUnitCursor(unit_),
                        &ModelBuilder::topLevelVisitor, this);
    if (failed_) {
      return;
    }
    pruneAnonymousRecords();
  }

  [[nodiscard]] bool failed() const { return failed_; }

private:
  CXTranslationUnit unit_;
  ReadResult &result_;
  NativeScopeId rootScope_ = 0;
  bool failed_ = false;
  // USR-keyed identity: Clang cursors never become model identity, and these
  // maps are lookup indexes only — emitted order is explicit model data.
  std::map<std::string, NativeDeclarationId> declarationsByUsr_;
  // Typedefs that transparently name a record or enum (`typedef struct X X;`
  // and typedef-adopted anonymous definitions) resolve to the named
  // declaration instead of producing an alias declaration.
  std::map<std::string, NativeDeclarationId> transparentTypedefs_;
  // Imported macro-constant values by name, for later bodies that reference
  // them, and their declarations for redefinition updates.
  std::map<std::string, std::int64_t> macroValues_;
  std::map<std::string, NativeDeclarationId> macroDeclarations_;
  // Main-file lines carrying a native C error. Clang recovers from errors by
  // inventing types without always marking the declaration invalid, so any
  // declaration whose extent overlaps an error is refused wholesale —
  // importing recovery output would bind the wrong ABI.
  std::vector<std::uint32_t> nativeErrorLines_;

  [[nodiscard]] bool hasNativeErrorWithin(CXCursor cursor) const {
    const CXSourceRange extent = clang_getCursorExtent(cursor);
    const std::uint32_t first = originOf(clang_getRangeStart(extent)).line;
    const std::uint32_t last = originOf(clang_getRangeEnd(extent)).line;
    const auto begin = std::lower_bound(nativeErrorLines_.begin(),
                                        nativeErrorLines_.end(), first);
    return begin != nativeErrorLines_.end() && *begin <= last;
  }

  // Visitor callbacks cross a C API frame: no exception may unwind through
  // libclang, so each body converts failure into a break plus a flag.
  static CXChildVisitResult topLevelVisitor(CXCursor cursor, CXCursor,
                                            CXClientData data) {
    ModelBuilder *builder = static_cast<ModelBuilder *>(data);
    try {
      builder->visitTopLevel(cursor);
      return CXChildVisit_Continue;
    } catch (...) {
      builder->failed_ = true;
      return CXChildVisit_Break;
    }
  }

  struct ChildCollector {
    std::vector<CXCursor> children;
    bool failed = false;
  };

  static CXChildVisitResult collectChild(CXCursor cursor, CXCursor,
                                         CXClientData data) {
    ChildCollector *collector = static_cast<ChildCollector *>(data);
    try {
      collector->children.push_back(cursor);
      return CXChildVisit_Continue;
    } catch (...) {
      collector->failed = true;
      return CXChildVisit_Break;
    }
  }

  [[nodiscard]] std::vector<CXCursor> childrenOf(CXCursor cursor) {
    ChildCollector collector;
    clang_visitChildren(cursor, &ModelBuilder::collectChild, &collector);
    if (collector.failed) {
      failed_ = true;
    }
    return std::move(collector.children);
  }

  void warn(SourceOrigin origin, std::string message) {
    result_.diagnostics.push_back(
        {ReaderSeverity::Warning, origin, std::move(message)});
  }

  [[nodiscard]] std::string usrOf(CXCursor cursor) {
    std::string usr = owned(clang_getCursorUSR(cursor));
    if (usr.empty()) {
      // Some anonymous declarations carry no USR; a location key keeps two
      // distinct anonymous definitions from colliding.
      const SourceOrigin origin = originOf(cursor);
      usr = "gti-anonymous:" + std::to_string(origin.line) + ':' +
            std::to_string(origin.column);
    }
    return usr;
  }

  // -- Types ----------------------------------------------------------------

  [[nodiscard]] NativeTypeId makeInteger(std::uint32_t bits, bool isSigned) {
    NativeType type;
    type.kind = NativeTypeKind::Integer;
    type.bitWidth = bits;
    type.isSigned = isSigned;
    return result_.model.addType(std::move(type));
  }

  [[nodiscard]] NativeTypeId makeFloating(std::uint32_t bits) {
    NativeType type;
    type.kind = NativeTypeKind::Floating;
    type.bitWidth = bits;
    return result_.model.addType(std::move(type));
  }

  [[nodiscard]] NativeTypeId makeSimple(NativeTypeKind kind) {
    NativeType type;
    type.kind = kind;
    return result_.model.addType(std::move(type));
  }

  [[nodiscard]] NativeTypeId makeUnsupported(std::string spelling,
                                             std::string reason) {
    NativeType type;
    type.kind = NativeTypeKind::Unsupported;
    type.spelling = std::move(spelling);
    type.reason = std::move(reason);
    return result_.model.addType(std::move(type));
  }

  [[nodiscard]] NativeTypeId makeReference(NativeDeclarationId declaration) {
    const NativeDeclaration *target =
        result_.model.findDeclaration(declaration);
    NativeType type;
    type.kind =
        target->kind == NativeDeclarationKind::Record ? NativeTypeKind::Record
        : target->kind == NativeDeclarationKind::Enum ? NativeTypeKind::Enum
                                                      : NativeTypeKind::Alias;
    type.declaration = declaration;
    return result_.model.addType(std::move(type));
  }

  [[nodiscard]] std::uint32_t bitWidthOf(CXType type) {
    const long long size = clang_Type_getSizeOf(clang_getCanonicalType(type));
    return size > 0 ? static_cast<std::uint32_t>(size) * 8 : 0;
  }

  // Maps one Clang type use into the model. Named typedef sugar becomes an
  // Alias reference; builtin-prelude typedefs resolve canonically; `restrict`
  // and top-level qualifiers are invisible, and const survives only as a
  // pointee qualifier — the shapes the planner's policy is written against.
  [[nodiscard]] NativeTypeId mapType(CXType type) { // NOLINT(misc-no-recursion)
    switch (type.kind) {
    case CXType_Elaborated:
      return mapType(clang_Type_getNamedType(type));
    case CXType_Attributed:
      return mapType(clang_Type_getModifiedType(type));
    case CXType_Void:
      return makeSimple(NativeTypeKind::Void);
    case CXType_Bool:
      return makeSimple(NativeTypeKind::Bool);
    case CXType_Char_S:
    case CXType_Char_U:
      // Plain `char` stays distinct from the sized integers: the GTI ABI
      // admits `const char*` as c_string but bare `char` is not a scalar.
      return makeSimple(NativeTypeKind::Char);
    case CXType_SChar:
      return makeInteger(8, true);
    case CXType_UChar:
      return makeInteger(8, false);
    case CXType_Short:
    case CXType_Int:
    case CXType_Long:
    case CXType_LongLong:
      return makeInteger(bitWidthOf(type), true);
    case CXType_UShort:
    case CXType_UInt:
    case CXType_ULong:
    case CXType_ULongLong:
      return makeInteger(bitWidthOf(type), false);
    case CXType_Float:
      return makeFloating(32);
    case CXType_Double:
      return makeFloating(64);
    case CXType_LongDouble:
      return makeUnsupported(owned(clang_getTypeSpelling(type)),
                             "'long double' has no fixed-width GTI scalar");
    case CXType_Pointer: {
      const CXType pointee = clang_getPointeeType(type);
      const CXType pointeeCanonical = clang_getCanonicalType(pointee);
      if (pointeeCanonical.kind == CXType_FunctionNoProto) {
        return makeUnsupported(owned(clang_getTypeSpelling(type)),
                               "a function pointer without a prototype leaves "
                               "its parameters unspecified; declare them");
      }
      if (pointeeCanonical.kind == CXType_FunctionProto) {
        // The shape is a native fact the planner's callback policy consumes;
        // whether an inline function pointer is bindable is not decided here.
        NativeType node;
        node.kind = NativeTypeKind::Pointer;
        node.constPointee = clang_isConstQualifiedType(pointee) != 0;
        node.pointee = mapFunctionType(pointee);
        return result_.model.addType(std::move(node));
      }
      NativeType node;
      node.kind = NativeTypeKind::Pointer;
      node.constPointee = clang_isConstQualifiedType(pointee) != 0;
      node.pointee = mapType(pointee);
      return result_.model.addType(std::move(node));
    }
    case CXType_ConstantArray: {
      // Multi-extent arrays stay nested Array nodes, outermost first.
      NativeType node;
      node.kind = NativeTypeKind::Array;
      node.element = mapType(clang_getArrayElementType(type));
      node.count = static_cast<std::uint64_t>(clang_getArraySize(type));
      return result_.model.addType(std::move(node));
    }
    case CXType_IncompleteArray:
    case CXType_VariableArray:
    case CXType_DependentSizedArray:
      return makeUnsupported(owned(clang_getTypeSpelling(type)),
                             "an array without a positive integer literal "
                             "extent is outside the bounded surface");
    case CXType_Record: {
      CXCursor declaration = clang_getTypeDeclaration(type);
      if (clang_Cursor_isAnonymous(declaration) != 0) {
        // Only a typedef-adopted definition earns a name a binding can
        // spell; every other anonymous record use is inexpressible.
        const auto known = declarationsByUsr_.find(
            usrOf(clang_getCanonicalCursor(declaration)));
        if (known != declarationsByUsr_.end()) {
          const NativeDeclaration *adopted =
              result_.model.findDeclaration(known->second);
          if (adopted != nullptr && !adopted->name.empty()) {
            return makeReference(known->second);
          }
        }
        return makeUnsupported(owned(clang_getTypeSpelling(type)),
                               "an anonymous record type in this position is "
                               "outside the bounded surface");
      }
      return makeReference(ensureRecordDeclared(declaration));
    }
    case CXType_Enum: {
      CXCursor declaration = clang_getTypeDeclaration(type);
      const auto known =
          declarationsByUsr_.find(usrOf(clang_getCanonicalCursor(declaration)));
      if (known != declarationsByUsr_.end()) {
        return makeReference(known->second);
      }
      if (clang_Cursor_isAnonymous(declaration) != 0) {
        return makeUnsupported(owned(clang_getTypeSpelling(type)),
                               "an anonymous enum type in this position is "
                               "outside the bounded surface");
      }
      return makeUnsupported(owned(clang_getTypeSpelling(type)),
                             "enum '" +
                                 owned(clang_getCursorSpelling(declaration)) +
                                 "' is not declared");
    }
    case CXType_Typedef: {
      CXCursor declaration = clang_getTypeDeclaration(type);
      if (inBuiltinsFile(declaration)) {
        return mapType(clang_getCanonicalType(type));
      }
      const std::string usr = usrOf(declaration);
      if (const auto transparent = transparentTypedefs_.find(usr);
          transparent != transparentTypedefs_.end()) {
        return makeReference(transparent->second);
      }
      if (const auto known = declarationsByUsr_.find(usr);
          known != declarationsByUsr_.end()) {
        return makeReference(known->second);
      }
      return makeUnsupported(owned(clang_getTypeSpelling(type)),
                             "typedef '" +
                                 owned(clang_getCursorSpelling(declaration)) +
                                 "' is not part of this import");
    }
    case CXType_FunctionProto:
    case CXType_FunctionNoProto:
      return makeUnsupported(owned(clang_getTypeSpelling(type)),
                             "an unnamed function type is outside the bounded "
                             "surface; name it with a typedef");
    case CXType_Unexposed:
    default: {
      const CXType canonical = clang_getCanonicalType(type);
      if (clang_equalTypes(canonical, type) == 0) {
        // Sugar this reader does not name (paren types and friends) resolves
        // through its canonical form.
        return mapType(canonical);
      }
      const std::string spelling = owned(clang_getTypeSpelling(type));
      return makeUnsupported(spelling, "type '" + spelling +
                                           "' is outside the bounded surface");
    }
    }
  }

  // C parameter arrays decay to pointers; libclang reports the type as
  // written, so the decay — const pointee preserved — happens here. A
  // typedef of an array type decays identically, which the canonical check
  // covers.
  [[nodiscard]] NativeTypeId mapParameterType(CXType type) {
    CXType written = type;
    while (written.kind == CXType_Elaborated) {
      written = clang_Type_getNamedType(written);
    }
    const CXType canonical = clang_getCanonicalType(written);
    if (canonical.kind == CXType_ConstantArray ||
        canonical.kind == CXType_IncompleteArray ||
        canonical.kind == CXType_VariableArray) {
      const CXType element = clang_getArrayElementType(canonical);
      NativeType pointer;
      pointer.kind = NativeTypeKind::Pointer;
      // C applies an array qualifier to the element; the canonical form may
      // spell it on either node.
      pointer.constPointee = clang_isConstQualifiedType(element) != 0 ||
                             clang_isConstQualifiedType(canonical) != 0;
      pointer.pointee = mapType(element);
      return result_.model.addType(std::move(pointer));
    }
    return mapType(type);
  }

  [[nodiscard]] NativeTypeId
  mapFunctionType(CXType function) { // NOLINT(misc-no-recursion)
    CXType shape = function;
    if (clang_getNumArgTypes(shape) < 0) {
      shape = clang_getCanonicalType(function);
    }
    NativeType node;
    node.kind = NativeTypeKind::Function;
    node.result = mapType(clang_getResultType(shape));
    const int count = clang_getNumArgTypes(shape);
    for (int index = 0; index < count; ++index) {
      node.parameters.push_back(mapParameterType(
          clang_getArgType(shape, static_cast<unsigned>(index))));
    }
    node.variadic = clang_isFunctionTypeVariadic(shape) != 0;
    return result_.model.addType(std::move(node));
  }

  // -- Declarations ---------------------------------------------------------

  NativeDeclarationId ensureRecordDeclared(CXCursor cursor) {
    CXCursor canonical = clang_getCanonicalCursor(cursor);
    const std::string usr = usrOf(canonical);
    if (const auto known = declarationsByUsr_.find(usr);
        known != declarationsByUsr_.end()) {
      return known->second;
    }
    NativeDeclaration record;
    record.kind = NativeDeclarationKind::Record;
    // Unnamed definitions keep an empty model name (a typedef may adopt it);
    // newer libclang spells them "(unnamed at ...)" instead of "".
    record.name = clang_Cursor_isAnonymous(canonical) != 0
                      ? std::string()
                      : owned(clang_getCursorSpelling(canonical));
    record.scope = rootScope_;
    record.origin = originOf(canonical);
    record.recordIsUnion = clang_getCursorKind(canonical) == CXCursor_UnionDecl;
    const NativeDeclarationId id =
        result_.model.addDeclaration(std::move(record));
    result_.model.order.push_back(id);
    declarationsByUsr_.emplace(usr, id);
    return id;
  }

  void visitTopLevel(CXCursor cursor) {
    const CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_InclusionDirective) {
      // `-nostdinc` makes every include unresolvable; the native "file not
      // found" diagnostic already carries the canonical note, so a resolved
      // directive here would be a policy violation worth surfacing loudly —
      // but resolution is impossible without search paths, so nothing to do.
      return;
    }
    if (kind == CXCursor_MacroDefinition) {
      if (inMainFile(cursor)) {
        handleMacro(cursor);
      }
      return;
    }
    if (clang_isPreprocessing(kind) != 0) {
      return;
    }
    if (clang_isDeclaration(kind) == 0 || !inMainFile(cursor)) {
      return;
    }
    switch (kind) {
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
      handleRecord(cursor);
      return;
    case CXCursor_EnumDecl:
      handleEnum(cursor);
      return;
    case CXCursor_TypedefDecl:
      handleTypedef(cursor);
      return;
    case CXCursor_FunctionDecl:
      handleFunction(cursor);
      return;
    case CXCursor_VarDecl:
      warn(originOf(cursor),
           "native variable '" + owned(clang_getCursorSpelling(cursor)) +
               "' is not part of the GTI C ABI and was skipped");
      return;
    default:
      warn(originOf(cursor),
           "declaration is outside the bounded surface and was skipped");
      return;
    }
  }

  void handleRecord(CXCursor cursor) {
    const NativeDeclarationId id = ensureRecordDeclared(cursor);
    if (clang_isCursorDefinition(cursor) == 0) {
      return; // A forward declaration models as an incomplete record.
    }
    if (const NativeDeclaration *existing = result_.model.findDeclaration(id);
        existing == nullptr || existing->complete) {
      return;
    }
    std::vector<NativeRecordField> fields;
    std::string failureReason;
    const auto fail = [&failureReason](std::string reason) {
      if (failureReason.empty()) {
        failureReason = std::move(reason);
      }
    };
    if (hasNativeErrorWithin(cursor)) {
      fail("the record definition has native C errors");
    }
    for (const CXCursor child : childrenOf(cursor)) {
      const CXCursorKind childKind = clang_getCursorKind(child);
      if (clang_isAttribute(childKind) != 0) {
        // Packed/aligned attributes change the layout Clang computed; the
        // GTI [[c_abi]] surface has no spelling for them, so the record can
        // only survive as an opaque handle.
        fail("the record layout uses an attribute outside the bounded "
             "surface");
        continue;
      }
      if (childKind != CXCursor_FieldDecl) {
        continue; // Nested tag definitions surface through the field using
                  // them.
      }
      const SourceOrigin fieldOrigin = originOf(child);
      const std::string name = owned(clang_getCursorSpelling(child));
      if (clang_isInvalidDeclaration(child) != 0) {
        fail("field '" + name + "' has native C errors");
        continue;
      }
      if (clang_Cursor_isBitField(child) != 0) {
        fail("bit-field '" + name + "' has no GTI representation");
        continue;
      }
      if (name.empty()) {
        fail("field declarator at line " + std::to_string(fieldOrigin.line) +
             " is outside the bounded surface");
        continue;
      }
      bool fieldAttribute = false;
      for (const CXCursor fieldChild : childrenOf(child)) {
        fieldAttribute =
            fieldAttribute ||
            clang_isAttribute(clang_getCursorKind(fieldChild)) != 0;
      }
      if (fieldAttribute) {
        fail("field '" + name +
             "' uses an attribute outside the bounded surface");
        continue;
      }
      const NativeTypeId type = mapType(clang_getCursorType(child));
      fields.push_back({name, type, fieldOrigin});
    }
    // Re-find after mapping: type mapping may have grown the declaration
    // arena and moved the record.
    NativeDeclaration *record = result_.model.findDeclaration(id);
    record->complete = true;
    if (failureReason.empty()) {
      record->fields = std::move(fields);
    } else {
      // The planner degrades a complete-but-inexpressible record to an
      // opaque handle carrying this reason.
      record->fields.clear();
      record->layoutReason = std::move(failureReason);
    }
  }

  void handleEnum(CXCursor cursor) {
    if (clang_isCursorDefinition(cursor) == 0) {
      return; // A forward enum is invalid C; the native diagnostic reports it.
    }
    const std::string usr = usrOf(clang_getCanonicalCursor(cursor));
    if (declarationsByUsr_.find(usr) != declarationsByUsr_.end()) {
      return;
    }
    if (hasNativeErrorWithin(cursor)) {
      warn(originOf(cursor), "enum '" + owned(clang_getCursorSpelling(cursor)) +
                                 "' has native C errors and was skipped");
      return;
    }
    NativeDeclaration declaration;
    declaration.kind = NativeDeclarationKind::Enum;
    // An anonymous enum keeps its empty name: a following typedef adopts it,
    // and a bare one still plans as constants.
    declaration.name = clang_Cursor_isAnonymous(cursor) != 0
                           ? std::string()
                           : owned(clang_getCursorSpelling(cursor));
    declaration.scope = rootScope_;
    declaration.origin = originOf(cursor);
    for (const CXCursor child : childrenOf(cursor)) {
      if (clang_getCursorKind(child) != CXCursor_EnumConstantDecl) {
        continue;
      }
      declaration.enumerators.push_back(
          {owned(clang_getCursorSpelling(child)),
           static_cast<std::int64_t>(clang_getEnumConstantDeclValue(child))});
    }
    const NativeDeclarationId id =
        result_.model.addDeclaration(std::move(declaration));
    result_.model.order.push_back(id);
    declarationsByUsr_.emplace(usr, id);
  }

  void handleTypedef(CXCursor cursor) {
    const std::string name = owned(clang_getCursorSpelling(cursor));
    const std::string usr = usrOf(cursor);
    if (declarationsByUsr_.find(usr) != declarationsByUsr_.end() ||
        transparentTypedefs_.find(usr) != transparentTypedefs_.end()) {
      return; // A C11 typedef redeclaration restates one declaration.
    }
    const SourceOrigin origin = originOf(cursor);
    if (clang_isInvalidDeclaration(cursor) != 0 ||
        hasNativeErrorWithin(cursor)) {
      warn(origin,
           "typedef '" + name + "' has native C errors and was skipped");
      return;
    }
    const CXType underlying = clang_getTypedefDeclUnderlyingType(cursor);
    const CXType canonical = clang_getCanonicalType(underlying);
    if (canonical.kind == CXType_Record || canonical.kind == CXType_Enum) {
      CXCursor target = clang_getTypeDeclaration(canonical);
      NativeDeclarationId targetId = 0;
      if (canonical.kind == CXType_Record) {
        targetId = ensureRecordDeclared(target);
      } else if (const auto known = declarationsByUsr_.find(
                     usrOf(clang_getCanonicalCursor(target)));
                 known != declarationsByUsr_.end()) {
        targetId = known->second;
      }
      NativeDeclaration *named = result_.model.findDeclaration(targetId);
      if (named != nullptr && named->name.empty()) {
        // `typedef struct { ... } Name;` — the definition takes the typedef
        // name; the typedef itself is transparent.
        named->name = name;
        transparentTypedefs_.emplace(usr, targetId);
        return;
      }
      if (named != nullptr && named->name == name) {
        // `typedef struct Foo Foo;` — the bare name answers for the tag.
        transparentTypedefs_.emplace(usr, targetId);
        return;
      }
      if (named != nullptr) {
        addTypedefDeclaration(name, makeReference(targetId), origin, usr);
        return;
      }
    }
    // `typedef ret (*Name)(params);` stores the function shape directly —
    // the model convention the planner's callback policy expects.
    if (canonical.kind == CXType_Pointer) {
      const CXType pointee = clang_getPointeeType(canonical);
      const CXType pointeeCanonical = clang_getCanonicalType(pointee);
      if (pointeeCanonical.kind == CXType_FunctionProto ||
          pointeeCanonical.kind == CXType_FunctionNoProto) {
        CXType sugared = underlying;
        while (sugared.kind == CXType_Elaborated) {
          sugared = clang_Type_getNamedType(sugared);
        }
        const CXType shape = sugared.kind == CXType_Pointer
                                 ? clang_getPointeeType(sugared)
                                 : pointeeCanonical;
        addTypedefDeclaration(name, mapFunctionType(shape), origin, usr);
        return;
      }
    }
    if (canonical.kind == CXType_FunctionProto ||
        canonical.kind == CXType_FunctionNoProto) {
      addTypedefDeclaration(
          name,
          makeUnsupported(owned(clang_getTypeSpelling(underlying)),
                          "a function typedef without a pointer declarator "
                          "is outside the bounded surface"),
          origin, usr);
      return;
    }
    addTypedefDeclaration(name, mapType(underlying), origin, usr);
  }

  void addTypedefDeclaration(const std::string &name, NativeTypeId aliased,
                             SourceOrigin origin, const std::string &usr) {
    NativeDeclaration alias;
    alias.kind = NativeDeclarationKind::Typedef;
    alias.name = name;
    alias.scope = rootScope_;
    alias.origin = origin;
    alias.aliased = aliased;
    const NativeDeclarationId id =
        result_.model.addDeclaration(std::move(alias));
    result_.model.order.push_back(id);
    declarationsByUsr_.emplace(usr, id);
  }

  // Imports `#define NAME <integer constant expression>` as a constant
  // declaration (the C-06 macro slice). Function-like macros are code, an
  // empty body is an include guard, and neither is a constant, so both stay
  // silent; an object-like macro with a body that does not evaluate gets a
  // note so nothing plausible disappears without a trace. C redefinition
  // keeps the first declaration position with the latest value.
  void handleMacro(CXCursor cursor) {
    if (clang_Cursor_isMacroFunctionLike(cursor) != 0) {
      return;
    }
    const std::string name = owned(clang_getCursorSpelling(cursor));
    const std::vector<std::string> body = macroBodyTokens(cursor, name);
    if (body.empty()) {
      return;
    }
    const std::optional<std::int64_t> value =
        MacroExpressionEvaluator(body, macroValues_).evaluate();
    if (!value.has_value()) {
      warn(originOf(cursor),
           "macro '" + name +
               "' is not a bounded integer constant expression and was not "
               "imported");
      return;
    }
    macroValues_[name] = *value;
    if (const auto existing = macroDeclarations_.find(name);
        existing != macroDeclarations_.end()) {
      if (NativeDeclaration *declaration =
              result_.model.findDeclaration(existing->second)) {
        declaration->constantValue = *value;
      }
      return;
    }
    NativeDeclaration declaration;
    declaration.kind = NativeDeclarationKind::Constant;
    declaration.name = name;
    declaration.scope = rootScope_;
    declaration.origin = originOf(cursor);
    declaration.constantValue = *value;
    const NativeDeclarationId id =
        result_.model.addDeclaration(std::move(declaration));
    result_.model.order.push_back(id);
    macroDeclarations_.emplace(name, id);
  }

  // The macro definition's tokens after the name: `#define NAME body...`.
  [[nodiscard]] std::vector<std::string>
  macroBodyTokens(CXCursor cursor, const std::string &name) {
    CXToken *tokens = nullptr;
    unsigned count = 0;
    clang_tokenize(unit_, clang_getCursorExtent(cursor), &tokens, &count);
    std::vector<std::string> spellings;
    for (unsigned index = 0; index < count; ++index) {
      spellings.push_back(owned(clang_getTokenSpelling(unit_, tokens[index])));
    }
    clang_disposeTokens(unit_, tokens, count);
    if (spellings.empty() || spellings.front() != name) {
      return {};
    }
    spellings.erase(spellings.begin());
    return spellings;
  }

  void handleFunction(CXCursor cursor) {
    const std::string name = owned(clang_getCursorSpelling(cursor));
    const SourceOrigin origin = originOf(cursor);
    const std::string usr = usrOf(cursor);
    if (declarationsByUsr_.find(usr) != declarationsByUsr_.end()) {
      return; // C redeclarations restate one function.
    }
    bool invalid =
        clang_isInvalidDeclaration(cursor) != 0 || hasNativeErrorWithin(cursor);
    const int argumentCount = clang_Cursor_getNumArguments(cursor);
    for (int index = 0; !invalid && index < argumentCount; ++index) {
      invalid = clang_isInvalidDeclaration(clang_Cursor_getArgument(
                    cursor, static_cast<unsigned>(index))) != 0;
    }
    if (invalid) {
      // The native C error diagnostic collected from Clang names the cause;
      // importing Clang's recovery type would bind the wrong ABI.
      warn(origin,
           "declaration '" + name + "' has native C errors and was skipped");
      return;
    }
    if (clang_Cursor_getStorageClass(cursor) == CX_SC_Static) {
      warn(origin, "'static' declaration is not part of the linkable C ABI "
                   "surface and was skipped");
      return;
    }
    if (clang_Cursor_isFunctionInlined(cursor) != 0) {
      warn(origin, "'inline' declaration is not part of the linkable C ABI "
                   "surface and was skipped");
      return;
    }
    if (clang_isCursorDefinition(cursor) != 0) {
      warn(origin, "function definition '" + name +
                       "' is not importable and was skipped");
      return;
    }
    // An assembler label renames the link symbol; importing the C name
    // would bind a different function. C-06 may carry such facts as model
    // data; today they are outside the bounded surface.
    bool hasAsmLabel = false;
    for (const CXCursor child : childrenOf(cursor)) {
      hasAsmLabel =
          hasAsmLabel || clang_getCursorKind(child) == CXCursor_AsmLabelAttr;
    }
    if (hasAsmLabel) {
      warn(origin, "function '" + name +
                       "' carries an assembler label that renames its link "
                       "symbol and was skipped");
      return;
    }
    const CXType cursorType = clang_getCursorType(cursor);
    if (clang_getFunctionTypeCallingConv(cursorType) != CXCallingConv_C) {
      warn(origin, "function '" + name +
                       "' uses a non-C calling convention outside the "
                       "bounded surface and was skipped");
      return;
    }
    const NativeTypeId type = mapFunctionType(cursorType);
    NativeDeclaration declaration;
    declaration.kind = NativeDeclarationKind::Function;
    declaration.name = name;
    declaration.scope = rootScope_;
    declaration.origin = origin;
    declaration.type = type;
    const NativeType *shape = result_.model.findType(type);
    for (int index = 0; index < argumentCount; ++index) {
      const CXCursor argument =
          clang_Cursor_getArgument(cursor, static_cast<unsigned>(index));
      const NativeTypeId parameterType =
          shape != nullptr &&
                  static_cast<std::size_t>(index) < shape->parameters.size()
              ? shape->parameters[static_cast<std::size_t>(index)]
              : 0;
      declaration.parameterNames.push_back(
          {owned(clang_getCursorSpelling(argument)), parameterType});
    }
    const NativeDeclarationId id =
        result_.model.addDeclaration(std::move(declaration));
    result_.model.order.push_back(id);
    declarationsByUsr_.emplace(usr, id);
  }

  // A record definition no typedef ever named cannot be spelled from GTI;
  // it leaves emission order with its reason. Anonymous enums stay: their
  // enumerators still plan as constants.
  void pruneAnonymousRecords() {
    std::vector<NativeDeclarationId> kept;
    kept.reserve(result_.model.order.size());
    for (const NativeDeclarationId id : result_.model.order) {
      const NativeDeclaration *declaration = result_.model.findDeclaration(id);
      if (declaration != nullptr &&
          declaration->kind == NativeDeclarationKind::Record &&
          declaration->name.empty()) {
        warn(declaration->origin, "an anonymous record declaration is outside "
                                  "the bounded surface and was skipped");
        continue;
      }
      kept.push_back(id);
    }
    result_.model.order = std::move(kept);
  }
};

} // namespace

ClangCReader::ClangCReader(CTargetProfile profile) : profile_(profile) {}

ReadResult ClangCReader::read(std::string_view headerText) {
  ReadResult result;
  const std::string text(headerText);
  const std::string targetArgument =
      std::string("--target=") + canonicalTriple(profile_);
  const char *arguments[] = {
      "-x",
      "c",
      "-std=c17",
      "-w",
      // Reading is fail-soft per declaration: Clang's default error limit
      // would abandon a header after twenty native errors.
      "-ferror-limit=0",
      "-nostdinc",
      "-nobuiltininc",
      targetArgument.c_str(),
      "-include",
      kBuiltinsPath,
  };
  CXUnsavedFile files[2] = {
      {kInputPath, text.c_str(), static_cast<unsigned long>(text.size())},
      {kBuiltinsPath, kBuiltinsText,
       static_cast<unsigned long>(std::strlen(kBuiltinsText))},
  };
  CXIndex index = clang_createIndex(/*excludeDeclarationsFromPCH=*/0,
                                    /*displayDiagnostics=*/0);
  CXTranslationUnit unit = nullptr;
  const CXErrorCode code = clang_parseTranslationUnit2(
      index, kInputPath, arguments,
      static_cast<int>(sizeof(arguments) / sizeof(arguments[0])), files, 2,
      CXTranslationUnit_DetailedPreprocessingRecord |
          CXTranslationUnit_KeepGoing,
      &unit);
  if (code != CXError_Success || unit == nullptr) {
    result.diagnostics.push_back(
        {ReaderSeverity::Error,
         {},
         "the native C translation unit could not be created (libclang "
         "error " +
             std::to_string(static_cast<int>(code)) + ")"});
    if (unit != nullptr) {
      clang_disposeTranslationUnit(unit);
    }
    clang_disposeIndex(index);
    return result;
  }
  {
    ModelBuilder builder(unit, result);
    builder.collectNativeDiagnostics();
    builder.run();
    if (builder.failed()) {
      result.diagnostics.push_back(
          {ReaderSeverity::Error, {}, "native declaration import failed"});
    }
  }
  clang_disposeTranslationUnit(unit);
  clang_disposeIndex(index);
  // One deterministic presentation: every reader diagnostic reports in
  // source order regardless of which pass produced it.
  std::stable_sort(
      result.diagnostics.begin(), result.diagnostics.end(),
      [](const ReaderDiagnostic &left, const ReaderDiagnostic &right) {
        return left.origin.line != right.origin.line
                   ? left.origin.line < right.origin.line
                   : left.origin.column < right.origin.column;
      });
  return result;
}

} // namespace lang::bindgen
