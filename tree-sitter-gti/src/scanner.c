// External scanner for GTI raw string literals, `R"delim( ... )delim"`.
// A closing sequence must repeat the opening delimiter, which a regular
// expression cannot express. The scanner mirrors the compiler lexer's rule:
// a delimiter has at most 16 printable ASCII characters other than space,
// parentheses, backslash and the double quote; the body ends at the first
// `)delim"` and may span lines.
#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>

enum TokenType {
  RAW_STRING_LITERAL,
};

enum { RAW_STRING_DELIMITER_LIMIT = 16 };

void *tree_sitter_gti_external_scanner_create(void) { return NULL; }

void tree_sitter_gti_external_scanner_destroy(void *payload) { (void)payload; }

unsigned tree_sitter_gti_external_scanner_serialize(void *payload,
                                                    char *buffer) {
  (void)payload;
  (void)buffer;
  return 0;
}

void tree_sitter_gti_external_scanner_deserialize(void *payload,
                                                  const char *buffer,
                                                  unsigned length) {
  (void)payload;
  (void)buffer;
  (void)length;
}

static bool delimiter_character(int32_t value) {
  return value > 0x20 && value < 0x7F && value != '(' && value != ')' &&
         value != '\\' && value != '"';
}

bool tree_sitter_gti_external_scanner_scan(void *payload, TSLexer *lexer,
                                           const bool *valid_symbols) {
  (void)payload;
  if (!valid_symbols[RAW_STRING_LITERAL]) {
    return false;
  }
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t' ||
         lexer->lookahead == '\r' || lexer->lookahead == '\n') {
    lexer->advance(lexer, true);
  }
  if (lexer->lookahead != 'R') {
    return false;
  }
  lexer->advance(lexer, false);
  if (lexer->lookahead != '"') {
    return false;
  }
  lexer->advance(lexer, false);

  int32_t delimiter[RAW_STRING_DELIMITER_LIMIT];
  unsigned length = 0;
  while (lexer->lookahead != '(') {
    if (length == RAW_STRING_DELIMITER_LIMIT ||
        !delimiter_character(lexer->lookahead)) {
      return false;
    }
    delimiter[length++] = lexer->lookahead;
    lexer->advance(lexer, false);
  }
  lexer->advance(lexer, false);

  for (;;) {
    if (lexer->eof(lexer)) {
      return false;
    }
    if (lexer->lookahead != ')') {
      lexer->advance(lexer, false);
      continue;
    }
    lexer->advance(lexer, false);
    // Match `delim"` after this `)`. On a mismatch, resume scanning at the
    // current character: it may itself begin the real closing sequence.
    unsigned matched = 0;
    while (matched < length && lexer->lookahead == delimiter[matched]) {
      lexer->advance(lexer, false);
      ++matched;
    }
    if (matched == length && lexer->lookahead == '"') {
      lexer->advance(lexer, false);
      lexer->result_symbol = RAW_STRING_LITERAL;
      return true;
    }
  }
}
