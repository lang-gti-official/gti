#include "events.h"
#include <stddef.h>
_Static_assert(sizeof(EventData) == 16, "union maximum member size");
_Static_assert(_Alignof(EventData) == 8, "union maximum member alignment");
_Static_assert(offsetof(EventData, number) == 0 && offsetof(EventData, position) == 0 && offsetof(EventData, bytes) == 0, "overlap");
_Static_assert(sizeof(Event) == 24 && offsetof(Event, data) == 8, "nested layout");
Event make_event(void) { Event value = {0}; value.kind = 1; value.data.number = 42; return value; }
EventData roundtrip_data(EventData value) { return value; }
int check_event(Event value) { return value.kind == 2 && value.data.position.x == 3.0f && value.data.position.y == 4.0f; }
void update_event(Event *value) { value->kind = 3; value->data.bytes[15] = 99; }
UserData make_userdata(void) { UserData value; value.pointer = 0; return value; }
NestedData make_nested(void) { NestedData value = {0}; value.data[1].number = 42; return value; }
uint64_t event_size(void) { return sizeof(Event); }
uint64_t data_alignment(void) { return _Alignof(EventData); }
