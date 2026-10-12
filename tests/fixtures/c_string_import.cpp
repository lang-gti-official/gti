#include <cstdint>

namespace {
char message[] = "GLFW error";
char bytes[256]{};
char large[65537]{};
} // namespace
extern "C" const char *foreign_message() { return message; }
extern "C" void foreign_rewrite() { message[0] = 'X'; }
extern "C" const char *foreign_empty() { return ""; }
extern "C" const char *foreign_embedded() { return "one\0two"; }
extern "C" const char *foreign_bytes() {
  for (unsigned i = 1; i < 256; ++i)
    bytes[i - 1] = static_cast<char>(i);
  return bytes;
}
extern "C" void foreign_callback(void (*callback)(std::int32_t, const char *)) {
  callback(7, "callback description");
}
extern "C" const char *foreign_large() {
  for (unsigned i = 0; i < 65536; ++i)
    large[i] = 'x';
  return large;
}
