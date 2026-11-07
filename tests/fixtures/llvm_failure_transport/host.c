// A real separately compiled, non-unwind-permitted C callback consumer.
int transport_c_host(int (*callback)(void *), void *context, int *returned) {
  int result = callback(context);
  *returned = 1;
  return result;
}
