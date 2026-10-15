#ifndef GTI_EXAMPLE_FOREIGN_VARIABLES_H
#define GTI_EXAMPLE_FOREIGN_VARIABLES_H

typedef int (*Operation)(int value);
struct Snapshot {
  int value;
  int *counter;
  Operation operation;
};

extern int counter;
extern const int limit;
extern int *value_pointer;
extern const int *readonly_pointer;
extern Operation operation;
extern Operation const readonly_operation;
extern struct Snapshot snapshot;
extern int foreign_epoch;

int *counter_address(void);
const int *limit_address(void);
void mutate_counter(int value);
int observe_snapshot(void);

#endif
