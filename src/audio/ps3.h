#ifndef AUDIO_PS3_H
#define AUDIO_PS3_H

#include <Limelight.h>

extern AUDIO_RENDERER_CALLBACKS audio_callbacks_ps3;

void ps3audio_start();
void ps3audio_stop();
unsigned int ps3audio_get_decoded_packets();

#endif
