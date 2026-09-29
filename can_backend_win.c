#include "can_backend.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

// Systec USB-CAN SDK Header
#include "usbcan.h"

#define MAX_BACKENDS 16

struct _can_backend {
    void *pd_obj;
    t_outlet *msgout;
    t_outlet *errout;
    int is_connected;
};

// Singleton-Struktur für das gemeinsame Hardware-Interface
typedef struct {
    tUcanHandle hUcan;
    HANDLE hThread;
    BOOL thread_running;
    int ref_count;
} t_win_hardware;

static t_win_hardware g_hw = { USBCAN_INVALID_HANDLE, NULL, FALSE, 0 };
static t_can_backend *g_backends[MAX_BACKENDS];
static int g_backend_count = 0;
static CRITICAL_SECTION g_cs;
static BOOL g_cs_initialized = FALSE;

// Empfangs-Thread für Windows
static DWORD WINAPI win_can_read_thread(LPVOID lpParam) {
    (void)lpParam;
    tUcanHandle handle = g_hw.hUcan;

    while (g_hw.thread_running) {
        tUcanMsg rx_msg;
        BYTE bRet = UcanReadCanMsg(handle, &rx_msg);

        if (bRet == USBCAN_SUCCESSFUL) {
            if (rx_msg.m_bFF & USBCAN_MSG_FF_RTR) {
                continue;
            }

            char idbuf[32];
            snprintf(idbuf, sizeof(idbuf), "0x%X", rx_msg.m_dwID);
            t_symbol *s_id = gensym(idbuf);

            int dlc = rx_msg.m_bLen > 8 ? 8 : rx_msg.m_bLen;
            t_atom out_atoms[8];

            for (int i = 0; i < dlc; i++) {
                SETFLOAT(&out_atoms[i], rx_msg.m_bData[i]);
            }

            EnterCriticalSection(&g_cs);
            for (int i = 0; i < g_backend_count; i++) {
                if (g_backends[i] && g_backends[i]->msgout && g_backends[i]->is_connected) {
                    outlet_anything(g_backends[i]->msgout, s_id, dlc, out_atoms);
                }
            }
            LeaveCriticalSection(&g_cs);
        } else if (bRet == USBCAN_WARN_NODATA) {
            Sleep(1);
        } else {
            Sleep(5);
        }
    }
    return 0;
}

t_can_backend* can_backend_init(void *pd_obj, t_outlet *msgout, t_outlet *errout) {
    if (!g_cs_initialized) {
        InitializeCriticalSection(&g_cs);
        g_cs_initialized = TRUE;
    }

    t_can_backend *b = (t_can_backend *)calloc(1, sizeof(t_can_backend));
    if (!b) return NULL;

    b->pd_obj = pd_obj;
    b->msgout = msgout;
    b->errout = errout;
    b->is_connected = 0;

    EnterCriticalSection(&g_cs);
    if (g_backend_count < MAX_BACKENDS) {
        g_backends[g_backend_count++] = b;
    }
    LeaveCriticalSection(&g_cs);

    return b;
}

int can_backend_connect(t_can_backend *b, const char *device_or_channel) {
    (void)device_or_channel;
    if (!b) return 0;

    EnterCriticalSection(&g_cs);
    if (g_hw.ref_count == 0) {
        tUcanInitParam initParam;
        memset(&initParam, 0, sizeof(initParam));
        initParam.m_dwSize = sizeof(initParam);
        initParam.m_bMode = USBCAN_MODE_NORMAL;
        initParam.m_bBtr0 = USBCAN_BAUD_250k_BTR0; // Standard 250k
        initParam.m_bBtr1 = USBCAN_BAUD_250k_BTR1;

        BYTE bRet = UcanInitHardwareEx(&g_hw.hUcan, USBCAN_ANY_MODULE, &initParam);
        if (bRet != USBCAN_SUCCESSFUL) {
            pd_error(b->pd_obj, "iemcan (Win): UcanInitHardwareEx fehlgeschlagen (Error %d)", bRet);
            LeaveCriticalSection(&g_cs);
            return 0;
        }

        g_hw.thread_running = TRUE;
        g_hw.hThread = CreateThread(NULL, 0, win_can_read_thread, NULL, 0, NULL);
    }

    g_hw.ref_count++;
    b->is_connected = 1;
    LeaveCriticalSection(&g_cs);

    post("iemcan (Win): Verbunden mit Systec USB-CAN Hardware.");
    return 1;
}

void can_backend_disconnect(t_can_backend *b) {
    if (!b || !b->is_connected) return;

    EnterCriticalSection(&g_cs);
    b->is_connected = 0;
    g_hw.ref_count--;

    if (g_hw.ref_count <= 0) {
        g_hw.ref_count = 0;
        g_hw.thread_running = FALSE;
        if (g_hw.hThread) {
            WaitForSingleObject(g_hw.hThread, 1000);
            CloseHandle(g_hw.hThread);
            g_hw.hThread = NULL;
        }
        if (g_hw.hUcan != USBCAN_INVALID_HANDLE) {
            UcanDeinitHardware(g_hw.hUcan);
            g_hw.hUcan = USBCAN_INVALID_HANDLE;
        }
    }
    LeaveCriticalSection(&g_cs);
}

void can_backend_free(t_can_backend *b) {
    if (!b) return;
    can_backend_disconnect(b);

    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_backend_count; i++) {
        if (g_backends[i] == b) {
            g_backends[i] = g_backends[g_backend_count - 1];
            g_backend_count--;
            break;
        }
    }
    LeaveCriticalSection(&g_cs);

    free(b);
}

void can_backend_set_filter(t_can_backend *b, int argc, t_atom *argv) {
    (void)b; (void)argc; (void)argv;
}

int can_backend_send(t_can_backend *b, unsigned int can_id, int dlc, const unsigned char *data) {
    if (!b || !b->is_connected || g_hw.hUcan == USBCAN_INVALID_HANDLE) {
        pd_error(b ? b->pd_obj : NULL, "CANsend (Win): Nicht verbunden!");
        return 0;
    }

    tUcanMsg tx_msg;
    memset(&tx_msg, 0, sizeof(tx_msg));

    tx_msg.m_dwID = can_id;
    tx_msg.m_bLen = dlc > 8 ? 8 : dlc;
    tx_msg.m_bFF = (can_id > 0x7FF) ? USBCAN_MSG_FF_EXT : USBCAN_MSG_FF_STD;

    for (int i = 0; i < tx_msg.m_bLen; i++) {
        tx_msg.m_bData[i] = data[i];
    }

    BYTE bRet = UcanWriteCanMsg(g_hw.hUcan, &tx_msg);
    if (bRet != USBCAN_SUCCESSFUL) {
        pd_error(b->pd_obj, "CANsend (Win): Fehler beim Senden (Error %d)", bRet);
        return 0;
    }

    return 1;
}
