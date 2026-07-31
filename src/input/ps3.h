#ifndef INPUT_PS3_H
#define INPUT_PS3_H

#include <io/pad.h>

typedef struct {
    int buttons_down;    // Buttons currently pressed this frame
    int buttons_pressed; // Buttons that just transitioned to pressed
    short ly, lx, ry, rx;
} ps3_pad_state_t;

void ps3input_start();
void ps3input_stop();
void ps3input_get_data(ps3_pad_state_t *state);

#endif
