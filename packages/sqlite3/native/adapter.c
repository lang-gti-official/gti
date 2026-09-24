#include "bindings.h"
int gti_sqlite_bind_text_copy(sqlite3_stmt *statement, int index,
                              const char *text) {
  return sqlite3_bind_text(statement, index, text, -1, SQLITE_TRANSIENT);
}
const char *gti_sqlite_column_text(sqlite3_stmt *statement, int column) {
  return (const char *)sqlite3_column_text(statement, column);
}
