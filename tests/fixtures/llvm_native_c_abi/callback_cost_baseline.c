#include <stdint.h>
typedef uint64_t (*Operation)(uint64_t);
extern int32_t native_callback_cost(Operation operation);
static uint64_t transform(uint64_t value) { return value ^ UINT64_C(17); }
int main(void) { return native_callback_cost(transform); }
