#ifndef CAN_BACKEND_H
#define CAN_BACKEND_H

#include "m_pd.h"

// Das opaque/plattformspezifische Backend-Handle
typedef struct _can_backend t_can_backend;

// Einheitliche Funktions-Schnittstellen
t_can_backend* can_backend_init(void *pd_obj, t_outlet *msgout, t_outlet *errout);
int  can_backend_connect(t_can_backend *backend, const char *device_or_channel);
void can_backend_disconnect(t_can_backend *backend);
void can_backend_free(t_can_backend *backend);

void can_backend_set_filter(t_can_backend *b, int argc, t_atom *argv);
int  can_backend_send(t_can_backend *b, unsigned int can_id, int dlc, const unsigned char *data);

#endif