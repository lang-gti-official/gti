// Strict host IEEE scalar reference, not a definition of GTI semantics.
#ifdef __cplusplus
extern "C" {
#endif
float oracle_f(int operation, float a, float b) {
  switch (operation) {
  case 0:
    return a + b;
  case 1:
    return a - b;
  case 2:
    return a * b;
  case 3:
    return a / b;
  case 4:
    return a == b;
  case 5:
    return a < b;
  }
  return 0;
}
double oracle_d(int operation, double a, double b) {
  switch (operation) {
  case 0:
    return a + b;
  case 1:
    return a - b;
  case 2:
    return a * b;
  case 3:
    return a / b;
  case 4:
    return a == b;
  case 5:
    return a < b;
  }
  return 0;
}
#ifdef __cplusplus
}
#endif
