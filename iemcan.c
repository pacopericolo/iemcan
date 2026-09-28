#include "m_pd.h"

// Deklaration der Setup-Funktionen der einzelnen Externals
void CANreceive_setup(void);
void CANsend_setup(void);
void CANparse_setup(void);

// Haupt-Setup-Funktion der Bibliothek (wird beim Laden von iemcan.dll aufgerufen)
void iemcan_setup(void) {
    CANreceive_setup();
    CANsend_setup();
    CANparse_setup();
    
    post("iemcan: Bibliothek erfolgreich geladen (CANreceive, CANsend, CANparse).");
}