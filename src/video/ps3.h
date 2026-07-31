#ifndef VIDEO_PS3_H
#define VIDEO_PS3_H

#include <Limelight.h>

extern DECODER_RENDERER_CALLBACKS decoder_callbacks_ps3;

void ps3video_start();
void ps3video_stop();
int  ps3video_is_active();
void ps3video_draw();
int  ps3video_get_current_fps();
int  ps3video_get_decoded_fps();
int  ps3video_get_decode_latency();
int  ps3video_get_render_latency();
int  ps3video_get_net_latency();
void vdec_poll(void);  // Call from main thread to consume decoded frames

#endif
