extern "C" int *iterator_sanitizer_control_allocate(int count) {
  return new int[count];
}
