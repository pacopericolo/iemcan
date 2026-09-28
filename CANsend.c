#include "m_pd.h"
#include "can_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CANSEND_MAXERRORS 8

static t_class *socketcansend_class = NULL;

typedef struct _socketcansend {
    t_object x_obj;
    t_outlet *x_infoout;
    t_outlet *x_stateout;
    t_can_backend *backend;
    size_t x_errcount;
} t_socketcansend;

static void socketcansend_disconnect(t_socketcansend *x) {
    if (x->backend) {
        can_backend_disconnect(x->backend);
    }
    x->x_errcount = 0;
    outlet_float(x->x_stateout, 0);
}

static void socketcansend_connect(t_socketcansend *x, t_symbol *s) {
    int res = can_backend_connect(x->backend, (s && s != gensym("")) ? s->s_name : "");
    x->x_errcount = 0;
    outlet_float(x->x_stateout, res ? 1 : 0);
}

static void socketcansend_dosend(t_socketcansend *x, t_symbol *s, int argc, t_atom *argv) {
    const char *canid_str = s->s_name;
    unsigned int can_id = 0;

    if (!canid_str || !*canid_str) return;

    // Hex-Parsing (z. B. "0x123") oder dezimal
    if ('0' == canid_str[0] && ('x' == canid_str[1] || 'X' == canid_str[1])) {
        if (sscanf(canid_str + 2, "%x", &can_id) != 1) {
            pd_error(x, "CANsend: Ungültige Hex-CAN-ID '%s'", canid_str);
            return;
        }
    } else {
        can_id = (unsigned int)strtoul(canid_str, NULL, 10);
    }

    // Nutzdaten sammeln
    unsigned char data[8];
    int dlc = argc > 8 ? 8 : argc;

    for (int i = 0; i < dlc; i++) {
        int v = (int)atom_getfloat(argv + i);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        data[i] = (unsigned char)v;
    }

    // Über Backend versenden
    if (!can_backend_send(x->backend, can_id, dlc, data)) {
        x->x_errcount++;
        pd_error(x, "CANsend: Fehler beim Senden der Nachricht 0x%X", can_id);
        
        if (x->x_errcount >= CANSEND_MAXERRORS) {
            pd_error(x, "CANsend: Zu viele Fehler hintereinander (%u)... Trenne Verbindung.", 
                     (unsigned int)x->x_errcount);
            socketcansend_disconnect(x);
        }
        return;
    }

    x->x_errcount = 0;
}

static void socketcansend_send(t_socketcansend *x, t_symbol *s, int argc, t_atom *argv) {
    if (argc < 1) {
        pd_error(x, "Verwendung: %s <can_id> [byte0 byte1 ...]", s->s_name);
        return;
    }
    socketcansend_dosend(x, atom_getsymbol(argv), argc - 1, argv + 1);
}

static void socketcansend_any(t_socketcansend *x, t_symbol *s, int argc, t_atom *argv) {
    const char *sel = s->s_name;
    if (sel && '0' == sel[0] && ('x' == sel[1] || 'X' == sel[1])) {
        socketcansend_dosend(x, s, argc, argv);
    } else {
        pd_error(x, "CANsend: Ungültiges Nachrichtenformat '%s'", sel);
    }
}

static void *socketcansend_new(t_symbol *s) {
    t_socketcansend *x = (t_socketcansend *)pd_new(socketcansend_class);
    x->x_infoout = outlet_new(&x->x_obj, &s_list);
    x->x_stateout = outlet_new(&x->x_obj, &s_float);
    x->x_errcount = 0;

    // Backend-Instanz initialisieren
    x->backend = can_backend_init(x, NULL, NULL);

    if (s && s != gensym("")) {
        socketcansend_connect(x, s);
    }
    return x;
}

static void socketcansend_free(t_socketcansend *x) {
    if (x->backend) {
        can_backend_free(x->backend);
        x->backend = NULL;
    }
}

void CANsend_setup(void) {
    socketcansend_class = class_new(gensym("CANsend"),
        (t_newmethod)(void *)socketcansend_new,
        (t_method)socketcansend_free,
        sizeof(t_socketcansend), 0,
        A_DEFSYM, 0);

    class_addmethod(socketcansend_class, (t_method)socketcansend_connect, gensym("connect"), A_DEFSYM, 0);
    class_addmethod(socketcansend_class, (t_method)socketcansend_disconnect, gensym("disconnect"), 0);
    class_addlist(socketcansend_class, (t_method)socketcansend_send);
    class_addmethod(socketcansend_class, (t_method)socketcansend_send, gensym("send"), A_GIMME, 0);
    class_addanything(socketcansend_class, (t_method)socketcansend_any);
}