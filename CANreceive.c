#include "m_pd.h"
#include "can_backend.h"

// Globale Klassen-Variable für Pd ganz oben deklarieren:
static t_class *socketcanrecv_class;


typedef struct _socketcanrecv {
    t_object x_obj;
    t_outlet *x_msgout, *x_errout;
    t_can_backend *backend;
} t_socketcanrecv;

static void socketcanrecv_filter(t_socketcanrecv *x, t_symbol *s, int argc, t_atom *argv) {
    (void)s;
    if (x && x->backend) {
        can_backend_set_filter(x->backend, argc, argv);
    }
}

static void socketcanrecv_connect(t_socketcanrecv *x, t_symbol *s) {
    can_backend_connect(x->backend, s->s_name);
}

static void socketcanrecv_disconnect(t_socketcanrecv *x) {
    can_backend_disconnect(x->backend);
}

static void *socketcanrecv_new(t_symbol *s) {
    t_socketcanrecv *x = (t_socketcanrecv *)pd_new(socketcanrecv_class);
    x->x_msgout = outlet_new(&x->x_obj, &s_list);
    x->x_errout = outlet_new(&x->x_obj, &s_list);
    
    // Backend initialisieren
    x->backend = can_backend_init(x, x->x_msgout, x->x_errout);
    return x;
}

static void socketcanrecv_free(t_socketcanrecv *x) {
    can_backend_free(x->backend);
}

void CANreceive_setup(void) {
    socketcanrecv_class = class_new(gensym("CANreceive"),
        (t_newmethod)(void *)socketcanrecv_new,
        (t_method)socketcanrecv_free,
        sizeof(t_socketcanrecv),
        CLASS_DEFAULT,
        A_DEFSYM, 0);

    class_addmethod(socketcanrecv_class, (t_method)socketcanrecv_connect, gensym("connect"), A_DEFSYM, 0);
    class_addmethod(socketcanrecv_class, (t_method)socketcanrecv_disconnect, gensym("disconnect"), 0);
	class_addmethod(socketcanrecv_class, (t_method)socketcanrecv_filter, gensym("filter"), A_GIMME, 0);
}
