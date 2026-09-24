#pragma once
#include <sqlite3.h>
// SQLITE_TRANSIENT is a function-pointer sentinel, not an ordinary callback.
// This adapter requests a copy; callers never fabricate that sentinel in GTI.
int gti_sqlite_bind_text_copy(sqlite3_stmt *statement, int index,
                              const char *text);
const char *gti_sqlite_column_text(sqlite3_stmt *statement, int column);
