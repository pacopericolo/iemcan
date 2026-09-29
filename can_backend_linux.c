#include "can_backend.h"
#include <m_pd.h>
#include <s_stuff.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_FILTERS 64

struct _can_backend {
    void *pd_obj;
    t_outlet *msgout;
    t_outlet *errout;
    int sockfd;
    int is_connected;

    // Cache für Filterregeln
    struct can_filter filters[MAX_FILTERS];
    int filter_count;
};

// Polling-Callback für den Pd-Hauptthread
static void linux_can_poll_callback(t_can_backend *b, int fd) {
    if (!b || !b->msgout || fd < 0) return;

    struct can_frame frame;
    ssize_t nbytes = read(fd, &frame, sizeof(struct can_frame));

    if (nbytes == sizeof(struct can_frame)) {
        unsigned int can_id = frame.can_id & CAN_EFF_MASK;

        char idbuf[32];
        snprintf(idbuf, sizeof(idbuf), "0x%X", can_id);
        t_symbol *s_id = gensym(idbuf);

        int dlc = frame.can_dlc > 8 ? 8 : frame.can_dlc;
        t_atom argv[8];
        for (int i = 0; i < dlc; i++) {
            SETFLOAT(&argv[i], frame.data[i]);
        }

        outlet_anything(b->msgout, s_id, dlc, argv);
    }
}

// Hilfsfunktion: Überträgt die geparsten Filter im Kernel-Socket
static void apply_filters_to_socket(t_can_backend *b) {
    if (!b || b->sockfd < 0) return;

    if (b->filter_count > 0) {
        if (setsockopt(b->sockfd, SOL_CAN_RAW, CAN_RAW_FILTER, b->filters, b->filter_count * sizeof(struct can_filter)) < 0) {
            pd_error(b->pd_obj, "iemcan (Linux): Fehler beim Setzen der Kernel-Filter!");
        } else {
            post("iemcan (Linux): %d Kernel-Filterregeln angewendet.", b->filter_count);
        }
    } else {
        // Keinerlei Filter gesetzt -> Alle Nachrichten akzeptieren
        struct can_filter default_filter;
        default_filter.can_id = 0;
        default_filter.can_mask = 0;
        setsockopt(b->sockfd, SOL_CAN_RAW, CAN_RAW_FILTER, &default_filter, sizeof(default_filter));
    }
}

t_can_backend* can_backend_init(void *pd_obj, t_outlet *msgout, t_outlet *errout) {
    t_can_backend *b = (t_can_backend *)calloc(1, sizeof(t_can_backend));
    if (!b) return NULL;

    b->pd_obj = pd_obj;
    b->msgout = msgout;
    b->errout = errout;
    b->sockfd = -1;
    b->is_connected = 0;
    b->filter_count = 0;

    return b;
}

void can_backend_set_filter(t_can_backend *b, int argc, t_atom *argv) {
    if (!b) return;

    b->filter_count = 0;

    if (argc >= 1) {
        int start_idx = 0;
        // Ignoriere ein führendes "||" oder "&&" (Original-Syntax)
        if (argv[0].a_type == A_SYMBOL) {
            const char *op = atom_getsymbol(&argv[0])->s_name;
            if (strcmp(op, "||") == 0 || strcmp(op, "&&") == 0) {
                start_idx = 1;
            }
        }

        for (int i = start_idx; i < argc && b->filter_count < MAX_FILTERS; i++) {
            if (argv[i].a_type == A_SYMBOL) {
                const char *ptr = atom_getsymbol(&argv[i])->s_name;
                unsigned int id = 0, mask = 0;

                // Syntax "0x2BC:7FF"
                if (sscanf(ptr, "0x%x:%x", &id, &mask) == 2) {
                    b->filters[b->filter_count].can_id = id;
                    b->filters[b->filter_count].can_mask = mask & ~CAN_ERR_FLAG;
                    b->filter_count++;
                } 
                // Einfache Syntax "0x2BC"
                else if (sscanf(ptr, "0x%x", &id) == 1) {
                    b->filters[b->filter_count].can_id = id;
                    b->filters[b->filter_count].can_mask = (id > 0x7FF) ? CAN_EFF_MASK : CAN_SFF_MASK;
                    b->filter_count++;
                }
            } else if (argv[i].a_type == A_FLOAT) {
                unsigned int id = (unsigned int)atom_getfloat(&argv[i]);
                b->filters[b->filter_count].can_id = id;
                b->filters[b->filter_count].can_mask = (id > 0x7FF) ? CAN_EFF_MASK : CAN_SFF_MASK;
                b->filter_count++;
            }
        }
    }

    // Wenn der Socket bereits geöffnet ist, den Filter sofort im Kernel aktualisieren
    if (b->sockfd >= 0) {
        apply_filters_to_socket(b);
    }
}

int can_backend_connect(t_can_backend *b, const char *device_or_channel) {
    if (!b) return 0;
    if (b->is_connected) return 1;

    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) {
        pd_error(b->pd_obj, "iemcan (Linux): Socket-Erstellung fehlgeschlagen");
        return 0;
    }

    struct ifreq ifr;
    const char *ifname = (device_or_channel && strlen(device_or_channel) > 0) ? device_or_channel : "can0";
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        pd_error(b->pd_obj, "iemcan (Linux): Interface '%s' nicht gefunden", ifname);
        close(s);
        return 0;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    b->sockfd = s;

    // Filter anwenden (vor dem Bind / während der Verbindung)
    apply_filters_to_socket(b);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        pd_error(b->pd_obj, "iemcan (Linux): Bind an '%s' fehlgeschlagen", ifname);
        close(s);
        b->sockfd = -1;
        return 0;
    }

    b->is_connected = 1;
    sys_addpollfn(b->sockfd, (t_fdpollfn)linux_can_poll_callback, b);

    post("iemcan (Linux): Verbindung mit %s hergestellt.", ifname);
    return 1;
}

void can_backend_disconnect(t_can_backend *b) {
    if (!b || !b->is_connected) return;

    if (b->sockfd >= 0) {
        sys_rmpollfn(b->sockfd);
        close(b->sockfd);
        b->sockfd = -1;
    }
    b->is_connected = 0;
    post("iemcan (Linux): Socket getrennt.");
}

void can_backend_free(t_can_backend *b) {
    if (!b) return;
    can_backend_disconnect(b);
    free(b);
}

int can_backend_send(t_can_backend *b, unsigned int can_id, int dlc, const unsigned char *data) {
    if (!b || !b->is_connected || b->sockfd < 0) {
        pd_error(b ? b->pd_obj : NULL, "CANsend (Linux): Nicht verbunden!");
        return 0;
    }

    struct can_frame frame;
    memset(&frame, 0, sizeof(frame));

    frame.can_id = can_id;
    if (can_id > 0x7FF) {
        frame.can_id |= CAN_EFF_FLAG;
    }

    frame.can_dlc = dlc > 8 ? 8 : dlc;
    for (int i = 0; i < frame.can_dlc; i++) {
        frame.data[i] = data[i];
    }

    if (write(b->sockfd, &frame, sizeof(struct can_frame)) != sizeof(struct can_frame)) {
        pd_error(b->pd_obj, "CANsend (Linux): Fehler beim Senden");
        return 0;
    }

    return 1;
}
