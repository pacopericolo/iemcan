#include "can_backend.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tchar.h>

#include "USBCAN32.h"

#define MAX_FILTERS 64

struct _can_backend {
    void *pd_obj;
    t_outlet *msgout;
    t_outlet *errout;
    int is_connected;
    
    // Filter-Speicher
    unsigned int filter_ids[MAX_FILTERS];
    int filter_count;
};

static tUcanHandle g_hUcan = USBCAN_INVALID_HANDLE;
static int g_refcount = 0;
static HANDLE g_hThread = NULL;
static volatile int g_thread_running = 0;

#define MAX_BACKENDS 16
static t_can_backend* g_backends[MAX_BACKENDS];
static int g_backend_count = 0;
static CRITICAL_SECTION g_cs;
static int g_cs_initialized = 0;

// Hilfsfunktion: Prüft, ob eine CAN-ID durchgelassen werden darf
static int is_id_allowed(t_can_backend *b, unsigned int can_id) {
    // Wenn kein Filter gesetzt ist (count == 0), lassen wir wie gehabt ALLES durch
    if (b->filter_count == 0) {
        return 1;
    }

    // Wenn Filter definiert sind, prüfen wir auf Übereinstimmung
    for (int i = 0; i < b->filter_count; i++) {
        if (b->filter_ids[i] == can_id) {
            return 1;
        }
    }
    return 0; // ID nicht in der Liste -> verwerfen
}

static DWORD WINAPI win_can_read_thread(LPVOID lpParam) {
    (void)lpParam;
    tCanMsgStruct canMsg;

    while (g_thread_running) {
        BYTE bRet = UcanReadCanMsg(g_hUcan, &canMsg);

        if (bRet == USBCAN_SUCCESSFUL) {
            EnterCriticalSection(&g_cs);
            for (int i = 0; i < g_backend_count; i++) {
                t_can_backend *b = g_backends[i];
                if (b && b->msgout && b->is_connected) {
                    unsigned int can_id = canMsg.m_dwID;

                    // FILTER-CHECK: Nachrichten verworfen, wenn ID nicht gefordert ist!
                    if (!is_id_allowed(b, can_id)) {
                        continue;
                    }

                    int dlc = canMsg.m_bDLC > 8 ? 8 : canMsg.m_bDLC;

                    char idbuf[32];
                    snprintf(idbuf, sizeof(idbuf), "0x%X", can_id);
                    t_symbol *s_id = gensym(idbuf);

                    t_atom argv[8];
                    for (int j = 0; j < dlc; j++) {
                        SETFLOAT(&argv[j], canMsg.m_bData[j]);
                    }

                    outlet_anything(b->msgout, s_id, dlc, argv);
                }
            }
            LeaveCriticalSection(&g_cs);
        } else if (bRet != USBCAN_WARN_NODATA) {
            Sleep(5);
        } else {
            Sleep(1);
        }
    }
    return 0;
}

t_can_backend* can_backend_init(void *pd_obj, t_outlet *msgout, t_outlet *errout) {
    t_can_backend *b = (t_can_backend *)calloc(1, sizeof(t_can_backend));
    if (!b) return NULL;

    b->pd_obj = pd_obj;
    b->msgout = msgout;
    b->errout = errout;
    b->is_connected = 0;
    b->filter_count = 0;

    if (!g_cs_initialized) {
        InitializeCriticalSection(&g_cs);
        g_cs_initialized = 1;
    }

    return b;
}

// Implementierung der Filterfunktion für Pd-Messages wie "filter 0x101 0x200 0x17C"
void can_backend_set_filter(t_can_backend *b, int argc, t_atom *argv) {
    if (!b) return;

    EnterCriticalSection(&g_cs);
    b->filter_count = 0; // Alte Filter verwerfen

    for (int i = 0; i < argc && i < MAX_FILTERS; i++) {
        if (argv[i].a_type == A_FLOAT) {
            b->filter_ids[b->filter_count++] = (unsigned int)atom_getfloat(&argv[i]);
        } else if (argv[i].a_type == A_SYMBOL) {
            const char *sym = atom_getsymbol(&argv[i])->s_name;
            unsigned int id = 0;
            // Unterstützt Hex ("0x17C") und Dezimal
            if (sscanf(sym, "0x%x", &id) == 1 || sscanf(sym, "%u", &id) == 1) {
                b->filter_ids[b->filter_count++] = id;
            }
        }
    }
    LeaveCriticalSection(&g_cs);

    post("iemcan: %d Filter-ID(s) gesetzt.", b->filter_count);
}

int can_backend_connect(t_can_backend *b, const char *device_or_channel) {
    (void)device_or_channel;
    if (!b) return 0;

    if (b->is_connected) return 1;

    EnterCriticalSection(&g_cs);

    if (g_hUcan != USBCAN_INVALID_HANDLE) {
        g_refcount++;
        b->is_connected = 1;

        if (g_backend_count < MAX_BACKENDS) {
            g_backends[g_backend_count++] = b;
        }

        LeaveCriticalSection(&g_cs);
        post("iemcan (Win): Mit bestehender Hardware verbunden (Refcount: %d)", g_refcount);
        return 1;
    }

    BYTE bDeviceNr = USBCAN_ANY_MODULE;
    BYTE bRet = UcanInitHardwareEx(&g_hUcan, bDeviceNr, NULL, NULL);

    if (bRet != USBCAN_SUCCESSFUL) {
        pd_error(b->pd_obj, "iemcan (Win): FEHLER bei UcanInitHardwareEx (Code: %d)", bRet);
        LeaveCriticalSection(&g_cs);
        return 0;
    }

    tUcanInitCanParam InitParam;
    memset(&InitParam, 0, sizeof(InitParam));

    InitParam.m_dwSize               = sizeof(InitParam);
    InitParam.m_bMode                = 0;
    InitParam.m_bBTR0                = HIBYTE(USBCAN_BAUD_500kBit);
    InitParam.m_bBTR1                = LOBYTE(USBCAN_BAUD_500kBit);
    InitParam.m_bOCR                 = 0x1A;
    InitParam.m_dwAMR                = USBCAN_AMR_ALL;
    InitParam.m_dwACR                = USBCAN_ACR_ALL;
    InitParam.m_dwBaudrate           = USBCAN_BAUDEX_USE_BTR01;
    InitParam.m_wNrOfRxBufferEntries = USBCAN_DEFAULT_BUFFER_ENTRIES;
    InitParam.m_wNrOfTxBufferEntries = USBCAN_DEFAULT_BUFFER_ENTRIES;

    bRet = UcanInitCanEx(g_hUcan, &InitParam);
    if (bRet != USBCAN_SUCCESSFUL) {
        pd_error(b->pd_obj, "iemcan (Win): FEHLER bei UcanInitCanEx (Code: %d)", bRet);
        UcanDeinitHardware(g_hUcan);
        g_hUcan = USBCAN_INVALID_HANDLE;
        LeaveCriticalSection(&g_cs);
        return 0;
    }

    g_thread_running = 1;
    g_hThread = CreateThread(NULL, 0, win_can_read_thread, NULL, 0, NULL);

    if (g_backend_count < MAX_BACKENDS) {
        g_backends[g_backend_count++] = b;
    }

    g_refcount = 1;
    b->is_connected = 1;

    LeaveCriticalSection(&g_cs);

    post("iemcan (Win): Hardware vollständig verbunden (500 kBit/s).");
    return 1;
}

void can_backend_disconnect(t_can_backend *b) {
    if (!b || !b->is_connected) return;

    EnterCriticalSection(&g_cs);

    for (int i = 0; i < g_backend_count; i++) {
        if (g_backends[i] == b) {
            g_backends[i] = g_backends[g_backend_count - 1];
            g_backend_count--;
            break;
        }
    }

    b->is_connected = 0;
    g_refcount--;

    if (g_refcount == 0 && g_hUcan != USBCAN_INVALID_HANDLE) {
        g_thread_running = 0;
        if (g_hThread) {
            WaitForSingleObject(g_hThread, 1000);
            CloseHandle(g_hThread);
            g_hThread = NULL;
        }

        UcanDeinitCan(g_hUcan);
        UcanDeinitHardware(g_hUcan);
        g_hUcan = USBCAN_INVALID_HANDLE;
        post("iemcan (Win): Hardware getrennt.");
    }

    LeaveCriticalSection(&g_cs);
}

void can_backend_free(t_can_backend *b) {
    if (!b) return;
    can_backend_disconnect(b);
    free(b);
}

int can_backend_send(t_can_backend *b, unsigned int can_id, int dlc, const unsigned char *data) {
    if (!b || !b->is_connected || g_hUcan == USBCAN_INVALID_HANDLE) {
        pd_error(b ? b->pd_obj : NULL, "CANsend: Nicht verbunden!");
        return 0;
    }

    tCanMsgStruct canMsg;
    memset(&canMsg, 0, sizeof(canMsg));

    canMsg.m_dwID = (DWORD)can_id;
    canMsg.m_bDLC = (BYTE)(dlc > 8 ? 8 : dlc);
    canMsg.m_bFF  = (can_id > 0x7FF) ? USBCAN_MSG_FF_EXT : USBCAN_MSG_FF_STD;

    for (int i = 0; i < canMsg.m_bDLC; i++) {
        canMsg.m_bData[i] = data[i];
    }

    BYTE bRet = UcanWriteCanMsg(g_hUcan, &canMsg);
    if (bRet != USBCAN_SUCCESSFUL) {
        pd_error(b->pd_obj, "CANsend: Fehler bei UcanWriteCanMsg (%d)", bRet);
        return 0;
    }

    return 1;
}
