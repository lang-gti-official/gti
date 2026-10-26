#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

typedef void (*Wait)(const void *, uint64_t);
typedef void (*Notify)(const void *);
typedef void (*Store)(void *, uint64_t);
typedef uint64_t (*Load)(const void *);

#define DECLARE(Bits) \
  extern void gti_test_wait_u##Bits##_wait_0(const void *, uint64_t); \
  extern void gti_test_wait_u##Bits##_wait_1(const void *, uint64_t); \
  extern void gti_test_wait_u##Bits##_wait_4(const void *, uint64_t); \
  extern void gti_test_wait_u##Bits##_one(const void *); \
  extern void gti_test_wait_u##Bits##_all(const void *); \
  extern void gti_test_llvm_store##Bits(void *, uint64_t); \
  extern uint64_t gti_test_llvm_load##Bits(const void *);
DECLARE(8)
DECLARE(16)
DECLARE(32)
DECLARE(64)

struct Case {
  Wait wait;
  Notify notify;
  Store store;
  Load load;
  unsigned width;
};
#define CASE(Bits, Order, Notification) \
  {gti_test_wait_u##Bits##_wait_##Order, gti_test_wait_u##Bits##_##Notification, \
   gti_test_llvm_store##Bits, gti_test_llvm_load##Bits, Bits / 8}
#define ORDERS(Bits) \
  CASE(Bits, 0, one), CASE(Bits, 1, one), CASE(Bits, 4, one), \
  CASE(Bits, 0, all), CASE(Bits, 1, all), CASE(Bits, 4, all)
static const struct Case cases[] = {ORDERS(8), ORDERS(16), ORDERS(32), ORDERS(64)};

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static unsigned entered;
static int published;
static _Alignas(8) unsigned char lane[8];

static void *consume(void *argument) {
  const struct Case *selected = argument;
  assert(pthread_mutex_lock(&gate) == 0);
  ++entered;
  assert(pthread_cond_signal(&ready) == 0);
  assert(pthread_mutex_unlock(&gate) == 0);
  selected->wait(lane, 0);
  // The acquire load independently checks publication for the relaxed wait.
  assert(selected->load(lane) == 1);
  assert(published == 42);
  return 0;
}

int main(void) {
  for (unsigned test = 0; test < sizeof(cases) / sizeof(cases[0]); ++test) {
    const struct Case *selected = &cases[test];
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
      selected->store(lane, 0);
      published = 0;
      entered = 0;
      pthread_t worker;
      assert(pthread_create(&worker, 0, consume, (void *)selected) == 0);
      assert(pthread_mutex_lock(&gate) == 0);
      while (entered == 0)
        assert(pthread_cond_wait(&ready, &gate) == 0);
      assert(pthread_mutex_unlock(&gate) == 0);
      published = 42;
      selected->store(lane, 1);
      selected->notify(lane);
      assert(pthread_join(worker, 0) == 0);
    }
  }
  puts("generated wait/notify: four widths, three orders, one/all publication passed");
  return 0;
}
