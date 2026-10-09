#include "hooks.h"

static int increment(int value) { return value + 1; }
static int double_value(int value) { return value * 2; }
static Callback stored = increment;
static Text const labels[] = {"first", "second"};

Operation *obtain_operation(void) { return increment; }
void replace_callback(Callback *slot) { *slot = double_value; }
void apply_operation(Operation operation, int *result) {
  *result = operation(21);
}
struct HookTable obtain_hooks(void) {
  struct HookTable table = {&stored, &stored,   "ready",
                            labels,  {1, 2, 3}, {{1, 2, 3}, {4, 5, 6}}};
  return table;
}
int verify_hooks(struct HookTable table) {
  return table.slot == &stored && table.readonly_slot == &stored &&
         table.label[0] == 'r' && table.labels[1][0] == 's' &&
         table.values[2] == 3 && table.matrix[1][2] == 6;
}
int text_length(Text text) {
  int length = 0;
  while (text[length] != '\0') {
    ++length;
  }
  return length;
}
int row_sum(Row values) { return values[0] + values[1] + values[2]; }
unsigned long long hooks_size(void) { return sizeof(struct HookTable); }
unsigned long long hooks_alignment(void) { return _Alignof(struct HookTable); }
