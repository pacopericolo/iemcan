#include "can_backend.h"
#include <m_pd.h>
#include <s_stuff.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/error.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <stdlib.h>
#include <stdio.h>

struct _can_backend {
    void *pd_obj;
    t_outlet *msgout;
    t_outlet *errout;
    int sockfd;
};

// Callback für eingehende Daten aus dem SocketCAN FD
static void linux_can_read(t_can_backend *b, int fd) {
    if (!b || fd < 0) return;

    t_atom atoms[65];
    struct canfd_frame cfd;
    ssize_t nbytes = read(fd, &cfd, CANFD_MTU);

    if (nbytes < (ssize_t)CAN_MTU) {
        pd_error(b->pd_obj, "CAN-Backend (Linux): Ungültiger CAN/CANFD Frame gelesen");
        return;
    }

    // Standard- / Extended- / Error-ID extrahieren
    unsigned int can_id = cfd.can_id & CAN_EFF_MASK;
    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "0x%X", can_id);
    SETSYMBOL(&argv_sym, gensym(idbuf));

    // Behandlung von Remote-Transmission-Request (RTR)
    if (cfd.can_id & CAN_RTR_FLAG) {
        if (b->errout) {
            t_atom rtr_atom;
            SETSYMBOL(&rtr_atom, gensym(idbuf));
            outlet_anything(b->errout, gensym("RTR"), 1, &rtr_atom);
        }
        return;
    }

    // Behandlung von CAN Error-Frames
    if (cfd.can_id & CAN_ERR_FLAG) {
        if (b->errout) {
            t_atom err_atom;
            SETSYMBOL(&err_atom, gensym(idbuf));
            outlet_anything(b->errout, gensym("error"), 1, &err_atom);
        }
        return;
    }

    // Daten-Nutzlast an Pd senden
    int dlc = cfd.len > 8 ? 8 : cfd.len;
    t_atom out_atoms[9];
    SETSYMBOL(&out_atoms[0], gensym(idbuf));

    for (int i = 0; i < dlc; i++) {
        SETFLOAT(&out_atoms[1 + i], cfd.data[i]);
    }

    if (b->msgout) {
        outlet_list(b->msgout, &s_list, dlc + 1, out_atoms);
    }
}

t_can_backend* can_backend_init(void *pd_obj, t_outlet *msgout, t_outlet *errout) {
    t_can_backend *b = (t_can_backend *)calloc(1, sizeof(t_can_backend));
    if (!b) return NULL;

    b->pd_obj = pd_obj;
    b->msgout = msgout;
    b->errout = errout;
    b->sockfd = -1;

    return b;
}

int can_backend_connect(t_can_backend *b, const char *device_or_channel) {
    if (!b) return 0;

    if (b->sockfd >= 0) {
        can_backend_disconnect(b);
    }

    int sfd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (sfd < 0) {
        pd_error(b->pd_obj, "CAN-Backend (Linux): Erstellen des SocketCAN Sockets fehlgeschlagen");
        return 0;
    }

    struct sockaddr_can addr;
    struct ifreq ifr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;

    if (device_or_channel && *device_or_channel) {
        strncpy(ifr.ifr_name, device_or_channel, IFNAMSIZ - 1);
        ifr.ifr_name[IFNAMSIZ - 1] = '\0';

        if (ioctl(sfd, SIOCGIFINDEX, &ifr) < 0) {
            pd_error(b->pd_obj, "CAN-Backend (Linux): Device '%s' nicht gefunden", device_or_channel);
            close(sfd);
            return 0;
        }
        addr.can_ifindex = ifr.ifr_ifindex;
    } else {
        addr.can_ifindex = 0; // Auf allen Schnittstellen lauschen
    }

    // CAN FD Funktionalität aktivieren
    const int canfd_on = 1;
    setsockopt(sfd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &canfd_on, sizeof(canfd_on));

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        pd_error(b->pd_obj, "CAN-Backend (Linux): Binden an Device '%s' fehlgeschlagen", 
                 (device_or_channel && *device_or_channel) ? device_or_channel : "<all>");
        close(sfd);
        return 0;
    }

    // In Pd Event-Loop für Lesezugriffe registrieren
    sys_addpollfn(sfd, (t_fdpollfn)linux_can_read, b);
    b->sockfd = sfd;

    post("CAN-Backend (Linux): Erfolgreich verbunden mit SocketCAN (%s)", 
         (device_or_channel && *device_or_channel) ? device_or_channel : "all");
    return 1;
}

void can_backend_disconnect(t_can_backend *b) {
    if (!b || b->sockfd < 0) return;

    sys_rmpollfn(b->sockfd);
    sys_closesocket(b->sockfd);
    b->sockfd = -1;

    post("CAN-Backend (Linux): SocketCAN verbindung getrennt.");
}

void can_backend_free(t_can_backend *b) {
    if (!b) return;
    can_backend_disconnect(b);
    free(b);
}

void can_backend_set_filter(t_can_backend *b, int argc, t_atom *argv) {
    (void)argc; (void)argv;
    if (!b || b->sockfd < 0) return;
    // Software-basiertes Filtering findet im plattformunabhängigen CANreceive statt
}

int can_backend_send(t_can_backend *b, unsigned int can_id, int dlc, const unsigned char *data) {
    if (!b || b->sockfd < 0) {
        pd_error(b ? b->pd_obj : NULL, "CANsend (Linux): Nicht verbunden!");
        return 0;
    }

    struct canfd_frame frame;
    memset(&frame, 0, sizeof(frame));

    frame.can_id = can_id;
    if (can_id > 0x7FF) {
        frame.can_id |= CAN_EFF_FLAG; // Extended Frame Format (29-Bit ID)
    }

    frame.len = dlc > 8 ? 8 : dlc;
    for (int i = 0; i < frame.len; i++) {
        frame.data[i] = data[i];
    }

    ssize_t bytes_sent = write(b->sockfd, &frame, CAN_MTU);
    if (bytes_sent != CAN_MTU) {
        pd_error(b->pd_obj, "CANsend (Linux): Fehler beim Schreiben auf SocketCAN");
        return 0;
    }

    return 1;
}