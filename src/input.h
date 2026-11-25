#ifndef INPUT_H
#define INPUT_H

#include <io/pad.h>

/* Maximum number of controllers reported to Sunshine/Moonlight.
 * Standard XInput / GameStream local co-op supports up to 4 gamepads (bits 0..3 -> 0x0F).
 * PSL1GHT ioPadInit(7) supports up to 7 Bluetooth/USB controllers on PS3, and Sunshine
 * supports up to 16 controllers; raise to 7 if using DSU/SDL >4-player co-op on Sunshine. */
#define PS3_MAX_CONTROLLERS 4

typedef struct {
    int buttons_down;    // Buttons currently pressed this frame
    int buttons_pressed; // Buttons that just transitioned to pressed
    short ly, lx, ry, rx;
} ps3_pad_state_t;

void ps3input_start();
void ps3input_stop();
void ps3input_get_data(ps3_pad_state_t *state);
int ps3input_get_connected_mask(void);
void ps3input_set_rumble(unsigned short controller_num, unsigned short low_freq_motor, unsigned short high_freq_motor);

#endif
