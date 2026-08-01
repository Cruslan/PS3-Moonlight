#pragma once

#include <stdbool.h>
#include <Limelight.h>

enum {
  LI_DISCONNECTED,
  LI_READY,
  LI_PAIRED,
  LI_CONNECTED,
  LI_MINIMIZED
};

extern CONNECTION_LISTENER_CALLBACKS connection_callbacks;
extern int connection_stage;

void connection_callbacks_init(void);
bool connection_is_ready(void);
bool connection_is_connected(void);
int connection_get_status(void);
