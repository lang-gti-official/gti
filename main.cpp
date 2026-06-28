#include "lexer.h"

int main(int argc, char *argv[]) {
  lang::Lexer l;
  l.consume("test_lang.cpp");
  return 0;
}
