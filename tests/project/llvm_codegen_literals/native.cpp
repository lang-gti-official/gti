namespace {
const char *previous;
}
extern "C" int record_address(const char *value) {
  previous = value;
  return 0;
}
extern "C" int compare_address(const char *value) {
  return value == previous ? 0 : 1;
}
