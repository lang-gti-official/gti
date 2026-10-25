#define _DEFAULT_SOURCE 1
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

// Independently compiled C owns these definitions, calls and expectations.
#define ONE(X) X(a)
#define TWO(X) X(a) X(b)
#define THREE(X) X(a) X(b) X(c)
#define FOUR(X) X(a) X(b) X(c) X(d)
#define INC(field) value.field += (#field[0] - 'a' + 1);
#define SAME(field)                                                            \
  if (actual.field != expected.field)                                          \
    return __LINE__;
#define RECORD(Name, Fields, Members)                                          \
  typedef struct Name {                                                        \
    Fields                                                                     \
  } Name;                                                                      \
  Name c_##Name(Name value) { Members(INC) return value; }                     \
  extern Name llvm_##Name(Name value);                                         \
  extern void import_##Name(const Name *, Name *);                             \
  extern uint64_t layout_##Name(unsigned);                                     \
  static int check_##Name(Name input) {                                        \
    Name expected = c_##Name(input);                                           \
    Name actual = llvm_##Name(input);                                          \
    Members(SAME) import_##Name(&input, &actual);                              \
    Members(SAME) if (layout_##Name(0) != sizeof(Name) ||                      \
                      layout_##Name(1) != _Alignof(Name)) return __LINE__;     \
    return 0;                                                                  \
  }

RECORD(Byte, uint8_t a;, ONE)
RECORD(Tiny, uint8_t a; uint8_t b; uint8_t c;, THREE)
RECORD(Shorts, int16_t a; uint8_t b;, TWO)
RECORD(Words, uint32_t a; uint32_t b;, TWO)
RECORD(TripleWords, uint32_t a; uint32_t b; uint32_t c;, THREE)
RECORD(Tail, uint64_t a; uint8_t b;, TWO)
RECORD(Pair, uint64_t a; uint64_t b;, TWO)
RECORD(Pointer, uint8_t *a; uint32_t b;, TWO)
RECORD(Float1, float a;, ONE)
RECORD(Float2, float a; float b;, TWO)
RECORD(Float3, float a; float b; float c;, THREE)
RECORD(Float4, float a; float b; float c; float d;, FOUR)
RECORD(Double1, double a;, ONE)
RECORD(Double2, double a; double b;, TWO)
RECORD(IntDouble, int64_t a; double b;, TWO)
RECORD(DoubleInt, double a; int64_t b;, TWO)
RECORD(FloatInt, float a; int32_t b;, TWO)
RECORD(FloatDouble, float a; double b;, TWO)

// Exhaust one register class, or leave only one GPR for a two-GPR record.
#define GPRS uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4
#define GPSUM (p0 + p1 + p2 + p3 + p4)
#define SSES                                                                   \
  double p0, double p1, double p2, double p3, double p4, double p5, double p6, \
      double p7
#define SSESum (p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7)
Pair c_gpr(GPRS, Pair value, uint64_t tail) {
  value.a += GPSUM + tail;
  value.b += 1;
  return value;
}
IntDouble c_mixed(GPRS, uint64_t p5, IntDouble value, double tail) {
  value.a += GPSUM + p5;
  value.b += tail;
  return value;
}
Double2 c_sse(SSES, Double2 value, uint64_t tail) {
  value.a += SSESum;
  value.b += tail;
  return value;
}
extern Pair llvm_gpr(GPRS, Pair, uint64_t);
extern IntDouble llvm_mixed(GPRS, uint64_t, IntDouble, double);
extern Double2 llvm_sse(SSES, Double2, uint64_t);
extern void import_gpr(const Pair *, Pair *);
extern void import_mixed(const IntDouble *, IntDouble *);
extern void import_sse(const Double2 *, Double2 *);

#define CHECK(Name, ...)                                                       \
  do {                                                                         \
    int line = check_##Name((Name){__VA_ARGS__});                              \
    if (line) {                                                                \
      fprintf(stderr, #Name " failed at %d\n", line);                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)
#define OFFSET(Name, Field, Index)                                             \
  do {                                                                         \
    if (layout_##Name(Index + 2) != offsetof(Name, Field))                     \
      return __LINE__;                                                         \
  } while (0)

int main(void) {
  uint8_t bytes[8] = {0};
  CHECK(Byte, 254);
  CHECK(Tiny, 1, 127, 254);
  CHECK(Shorts, -251, 250);
  CHECK(Words, 0x7ffffffe, 0x81234567);
  CHECK(TripleWords, 0x12345678, 0x81234567, 0x76543210);
  CHECK(Tail, UINT64_C(0xabcdef1287654321), 253);
  CHECK(Pair, UINT64_C(0x123456789abcdef0), UINT64_C(0xfedcba9876543210));
  CHECK(Pointer, bytes, 0x81726354);
  CHECK(Float1, -1.5f);
  CHECK(Float2, -2.5f, 19.25f);
  CHECK(Float3, 11.5f, -3.25f, 7.75f);
  CHECK(Float4, 1.25f, -2.75f, 4.5f, 99.25f);
  CHECK(Double1, -17.125);
  CHECK(Double2, -11.25, 123.5);
  CHECK(IntDouble, -2147483651LL, 25.75);
  CHECK(DoubleInt, -13.5, 2147483651LL);
  CHECK(FloatInt, -3.25f, -17123);
  CHECK(FloatDouble, -9.5f, 17.125);
  OFFSET(Tiny, c, 2);
  OFFSET(Shorts, b, 1);
  OFFSET(Tail, b, 1);
  OFFSET(Pointer, b, 1);
  OFFSET(Float3, c, 2);
  OFFSET(Float4, d, 3);
  OFFSET(Double2, b, 1);
  OFFSET(IntDouble, b, 1);
  OFFSET(DoubleInt, b, 1);
  OFFSET(FloatDouble, b, 1);

  Pair pair = {91, 73}, pairOut;
  pairOut = llvm_gpr(1, 2, 3, 4, 5, pair, 17);
  if (pairOut.a != 123 || pairOut.b != 74)
    return __LINE__;
  import_gpr(&pair, &pairOut);
  if (pairOut.a != 123 || pairOut.b != 74)
    return __LINE__;
  IntDouble mixed = {-91, 3.5}, mixedOut;
  mixedOut = llvm_mixed(1, 2, 3, 4, 5, 6, mixed, 17.25);
  if (mixedOut.a != -70 || mixedOut.b != 20.75)
    return __LINE__;
  import_mixed(&mixed, &mixedOut);
  if (mixedOut.a != -70 || mixedOut.b != 20.75)
    return __LINE__;
  Double2 sse = {1.25, -7.75}, sseOut;
  sseOut = llvm_sse(1, 2, 3, 4, 5, 6, 7, 8, sse, 17);
  if (sseOut.a != 37.25 || sseOut.b != 9.25)
    return __LINE__;
  import_sse(&sse, &sseOut);
  if (sseOut.a != 37.25 || sseOut.b != 9.25)
    return __LINE__;

  if (pair.a != 91 || pair.b != 73 || mixed.a != -91 || mixed.b != 3.5 ||
      sse.a != 1.25 || sse.b != -7.75)
    return __LINE__;

  // Widened coercions must not access beyond 3-byte or 12-byte source objects.
  long page = sysconf(_SC_PAGESIZE);
  if (page <= 0)
    return __LINE__;
  void *pages = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
  if (pages == MAP_FAILED)
    return __LINE__;
  if (mprotect((char *)pages + page, (size_t)page, PROT_NONE))
    return __LINE__;
  Tiny *edge = (Tiny *)((char *)pages + page - sizeof(Tiny));
  *edge = (Tiny){1, 2, 3};
  Tiny edgeOut;
  import_Tiny(edge, &edgeOut);
  if (edgeOut.a != 2 || edgeOut.b != 4 || edgeOut.c != 6)
    return __LINE__;
  import_Tiny(&edgeOut, edge);
  if (edge->a != 3 || edge->b != 6 || edge->c != 9)
    return __LINE__;
  TripleWords *edgeWords =
      (TripleWords *)((char *)pages + page - sizeof(TripleWords));
  *edgeWords = (TripleWords){11, 13, 17};
  TripleWords wordsOut;
  import_TripleWords(edgeWords, &wordsOut);
  if (wordsOut.a != 12 || wordsOut.b != 15 || wordsOut.c != 20)
    return __LINE__;
  import_TripleWords(&wordsOut, edgeWords);
  if (edgeWords->a != 13 || edgeWords->b != 17 || edgeWords->c != 23)
    return __LINE__;
  if (munmap(pages, (size_t)page * 2))
    return __LINE__;
  puts("native C ABI: 18 records, 3 register-pressure signatures, both "
       "directions");
  return 0;
}
