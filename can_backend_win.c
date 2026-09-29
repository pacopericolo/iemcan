#include "can_backend.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tchar.h>

#include "USBCAN32.h"

struct _can_backend {
    void *pd_obj;
    t_outlet *msgout;
    t_outlet *errout;
    int is_connected;
};

// Globaler Singleton-Speicher für das gemeinsame Systec-Handle
static tUcanHandle g_hUcan = USBCAN_INVALID_HANDLE;
static int g_refcount = 0;
static HANDLE g_hThread = NULL;
static volatile int g_thread_running = 0;

// Registrierte Objekt-Backends für das Weiterleiten von Empfangsdaten
#define MAX_BACKENDS 16
static t_can_backend* g_backends[MAX_BACKENDS];
static int g_backend_count = 0;
static CRITICAL_SECTION g_cs;
static int g_cs_initialized = 0;

// Windows Thread zum kontinuierlichen Empfang
static DWORD WINAPI win_can_read_thread(LPVOID lpParam) {
    (void)lpParam;
    tCanMsgStruct canMsg;

    while (g_thread_running) {
        BYTE bRet = UcanReadCanMsgEx(g_hUcan, USBCAN_CHANNEL_CH0, &canMsg, NULL);

        if (bRet == USBCAN_SUCCESSFUL) {
            // Empfangene Nachricht an alle registrierten Pd-Empfänger (CANreceive) weiterleiten
            EnterCriticalSection(&g_cs);
            for (int i = 0; i < g_backend_count; i++) {
                t_can_backend *b = g_backends[i];
                if (b && b->msgout && b->is_connected) {
                    unsigned int can_id = canMsg.m_dwID;
                    int dlc = canMsg.m_bDLC > 8 ? 8 : canMsg.m_bDLC;

                    char idbuf[32];
                    snprintf(idbuf, sizeof(idbuf), "0x%X", can_id);
                    t_symbol *s_id = gensym(idbuf);

                    t_atom argv[8];
                    for (int j = 0; j < dlc; j++) {
                        SETFLOAT(&argv[j], canMsg.m_bData[j]);
                    }

                    // Analog zu Linux: ID als Selector-Symbol, Datenbytes als Liste
                    outlet_anything(b->msgout, s_id, dlc, argv);
                }
            }
            LeaveCriticalSection(&g_cs);
        } else {
            Sleep(1); // Entlastung bei leerem Puffer
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

    if (!g_cs_initialized) {
        InitializeCriticalSection(&g_cs);
        g_cs_initialized = 1;
    }

    return b;
}

int can_backend_connect(t_can_backend *b, const char *device_or_channel) {
    (void)device_or_channel;
    if (!b) return 0;

    if (b->is_connected) return 1;

    EnterCriticalSection(&g_cs);

    // Falls die Hardware schon durch eine andere Instanz/DLL geöffnet wurde:
    if (g_hUcan != USBCAN_INVALID_HANDLE) {
        g_refcount++;
        b->is_connected = 1;

        // Registrieren für Empfang
        if (g_backend_count < MAX_BACKENDS) {
            g_backends[g_backend_count++] = b;
        }

        LeaveCriticalSection(&g_cs);
        post("CAN-Backend: Mit bestehender Hardware-Instanz verbunden (Aktive Instanzen: %d)", g_refcount);
        return 1;
    }

    // Erstmalige Initialisierung der Hardware
    BYTE bDeviceNr = USBCAN_ANY_MODULE;
    BYTE bRet = UcanInitHardwareEx(&g_hUcan, bDeviceNr, NULL, NULL);

    if (bRet != USBCAN_SUCCESSFUL) {
        pd_error(b->pd_obj, "CAN-Backend: Fehler bei UcanInitHardwareEx (%d)", bRet);
        LeaveCriticalSection(&g_cs);
        return 0;
    }

    tUcanInitCanParam InitParam;
    memset(&InitParam, 0, sizeof(InitParam));

    InitParam.m_dwSize               = sizeof(InitParam);
    InitParam.m_bMode                = 0; // kUcanModeNormal
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
        pd_error(b->pd_obj, "CAN-Backend: Fehler bei UcanInitCanEx (%d)", bRet);
        UcanDeinitHardware(g_hUcan);
        g_hUcan = USBCAN_INVALID_HANDLE;
        LeaveCriticalSection(&g_cs);
        return 0;
    }

    // Empfangsthread starten
    g_thread_running = 1;
    g_hThread = CreateThread(NULL, 0, win_can_read_thread, NULL, 0, NULL);

    // Backend registrieren & Refcount setzen
    if (g_backend_count < MAX_BACKENDS) {
        g_backends[g_backend_count++] = b;
    }

    g_refcount = 1;
    b->is_connected = 1;

    LeaveCriticalSection(&g_cs);

    post("CAN-Backend: Hardware neu verbunden (Aktive Instanzen: %d)", g_refcount);
    return 1;
}

void can_backend_disconnect(t_can_backend *b) {
    if (!b || !b->is_connected) return;

    EnterCriticalSection(&g_cs);

    // Backend aus Liste entfernen
    for (int i = 0; i < g_backend_count; i++) {
        if (g_backends[i] == b) {
            g_backends[i] = g_backends[g_backend_count - 1];
            g_backend_count--;
            break;
        }
    }

    b->is_connected = 0;
    g_refcount--;

    // Wenn keine Instanz die Hardware mehr nutzt -> Deinitialisieren
    if (g_refcount == 0 && g_hUcan != USBCAN_INVALID_HANDLE) {
        g_thread_running = 0;
        if (g_hThread) {
            WaitForSingleObject(g_hThread, 1000);
            CloseHandle(g_hThread);
            g_hThread = NULL;
        }

        UcanDeinitCanEx(g_hUcan, USBCAN_CHANNEL_CH0);
        UcanDeinitHardware(g_hUcan);
        g_hUcan = USBCAN_INVALID_HANDLE;
        post("CAN-Backend: Hardware-Verbindung getrennt.");
    }

    LeaveCriticalSection(&g_cs);
}

void can_backend_free(t_can_backend *b) {
    if (!b) return;
    can_backend_disconnect(b);
    free(b);
}

void can_backend_set_filter(t_can_backend *b, int argc, t_atom *argv) {
    (void)b; (void)argc; (void)argv;
}

int can_backend_send(t_can_backend *b, unsigned int can_id, int dlc, const unsigned char *data) {
    if (!b || !b->is_connected || g_hUcan == USBCAN_INVALID_HANDLE) {
        pd_error(b ? b->pd_obj : NULL, "CANsend: Nicht mit Hardware verbunden!");
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

    BYTE bRet = UcanWriteCanMsgEx(g_hUcan, USBCAN_CHANNEL_CH0, &canMsg, NULL);
    if (bRet != USBCAN_SUCCESSFUL) {
        pd_error(b->pd_obj, "CANsend: Fehler bei UcanWriteCanMsgEx (%d)", bRet);
        return 0;
    }

    return 1;
}
