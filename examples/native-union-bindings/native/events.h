#ifndef GTI_EXAMPLE_EVENTS_H
#define GTI_EXAMPLE_EVENTS_H
#include <stdint.h>
typedef struct Position { float x; float y; } Position;
typedef union EventData { uint64_t number; Position position; uint8_t bytes[16]; } EventData;
typedef struct Event { uint32_t kind; EventData data; } Event;
typedef union UserData { void *pointer; uint64_t number; } UserData;
typedef union NestedData { EventData data[2]; double real; } NestedData;
Event make_event(void);
EventData roundtrip_data(EventData value);
int check_event(Event value);
void update_event(Event *value);
UserData make_userdata(void);
NestedData make_nested(void);
uint64_t event_size(void);
uint64_t data_alignment(void);
#endif
