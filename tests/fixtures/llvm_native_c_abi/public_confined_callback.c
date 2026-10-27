#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t (*Operation)(int32_t);
static Operation transferred;
static const char *mode;

static void terminal(int signal_number) {
  (void)signal_number;
  _Exit(86);
}

static void *foreign_entry(void *unused) {
  (void)unused;
  fputs("foreign-enter\n", stderr);
  (void)transferred(25);
  _Exit(87);
}

int32_t native_confined_cleanup_divisor(void) {
  fputs("gti-cleanup\n", stderr);
  return strcmp(mode, "double-failure") == 0 ? 0 : 1;
}

int32_t native_confined_invoke(Operation operation) {
  mode = getenv("GTI_CONFINED_MODE");
  if (!mode || signal(SIGABRT, terminal) == SIG_ERR)
    return 89;
  if (strcmp(mode, "foreign") == 0) {
    transferred = operation;
    pthread_t thread;
    if (pthread_create(&thread, NULL, foreign_entry, NULL) != 0)
      return 89;
    if (pthread_join(thread, NULL) != 0)
      return 89;
    return 87;
  }
  const int32_t result = operation(strcmp(mode, "success") == 0 ? 25 : 0);
  fputs("native-returned\n", stderr);
  return result == 4 ? 0 : 88;
}
