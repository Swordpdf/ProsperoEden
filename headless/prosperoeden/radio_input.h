#ifndef RADIO_INPUT_H
#define RADIO_INPUT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADIO_INPUT_CROSS,
    RADIO_INPUT_CIRCLE,
    RADIO_INPUT_SQUARE,
    RADIO_INPUT_TRIANGLE,
    RADIO_INPUT_OPTIONS,
    RADIO_INPUT_L1,
    RADIO_INPUT_R1,
    RADIO_INPUT_UP,
    RADIO_INPUT_DOWN,
    RADIO_INPUT_LEFT,
    RADIO_INPUT_RIGHT,
    RADIO_INPUT_COUNT
} radio_input_key_t;

typedef struct {
    radio_input_key_t key;
    bool pressed;
} radio_input_event_t;

bool radio_input_init(void);
void radio_input_poll(void);
#ifdef EDEN_DEV_ROM_ID
void radio_input_development_sample(const void * sample);
#endif
bool radio_input_next(radio_input_event_t * event);
bool radio_input_pressed(radio_input_key_t key);
/* The players whose controller is connected (bit 0: the one driving the menu; bits 1-3: the
   other signed-in users), as of the last poll. */
unsigned radio_input_controllers(void);
/* A short vibration of the controller driving the menu, at `percent` (1-100) of full strength:
   what a strength setting feels like. It ends by itself (radio_input_poll), and a new one
   replaces the one that runs. */
void radio_input_pulse(unsigned percent);
void radio_input_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
