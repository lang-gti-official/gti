#ifndef GTI_EXAMPLE_CALLBACK_SLOTS_H
#define GTI_EXAMPLE_CALLBACK_SLOTS_H

typedef int Operation(int value);
typedef Operation *Callback;
typedef const char *Text;
typedef unsigned short Row[3];
typedef Row Matrix[2];

struct HookTable {
  Callback *slot;
  Callback const *readonly_slot;
  Text label;
  Text const *labels;
  Row values;
  Matrix matrix;
};

Operation *obtain_operation(void);
void replace_callback(Callback *slot);
void apply_operation(Operation operation, int *result);
struct HookTable obtain_hooks(void);
int verify_hooks(struct HookTable table);
int text_length(Text text);
int row_sum(Row values);
unsigned long long hooks_size(void);
unsigned long long hooks_alignment(void);

#endif
