#ifndef GTI_EXAMPLE_FOREIGN_VARIABLES_H
#define GTI_EXAMPLE_FOREIGN_VARIABLES_H

typedef int (*Operation)(int value);
struct Snapshot {
  int value;
  int *counter;
  Operation operation;
};

union Payload {
  int number;
  struct Coordinates { int x; int y; } position;
};
struct Packet { int tag; union Payload data; };

extern union Payload payload;
extern union Payload second_payload;
extern const union Payload frozen_payload;
extern struct Packet packet;

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
struct Snapshot make_snapshot(int value);
union Payload make_payload(int value);
struct Coordinates make_coordinates(int x, int y);
void switch_payload(void);
int observe_position(void);
int observe_payload(union Payload value);
int observe_packet(struct Packet value);

#endif
