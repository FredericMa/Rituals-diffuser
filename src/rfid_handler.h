#ifndef RFID_HANDLER_H
#define RFID_HANDLER_H

#include <Arduino.h>
#include "config.h"

#ifdef RC522_ENABLED

// Geur informatie struct
struct ScentInfo {
    const char* name;    // Points to PROGMEM string, do NOT free
    const char* hexCode; // Points to caller-owned buffer
    bool valid;
};

// Initialiseer de RC522 RFID reader
bool rfidInit();

// Check voor nieuwe RFID tags (call regelmatig in loop)
void rfidLoop();

// Haal de laatst gedetecteerde tag UID op
const char* rfidGetLastUID();

// Haal de laatst gedetecteerde geur op
const char* rfidGetLastScent();

// Is er recent een tag gedetecteerd?
bool rfidHasTag();

// Is de cartridge NU aanwezig? (niet timeout)
bool rfidIsCartridgePresent();

// Tijd sinds laatste tag detectie (ms)
unsigned long rfidTimeSinceLastTag();

// Lookup geur naam op basis van UID
ScentInfo rfidLookupScent(const char* hexData);

// Is de RC522 geïnitialiseerd en werkend?
bool rfidIsConnected();

// Debug: Get the version register value read during init (0 if not read)
uint8_t rfidGetVersionReg();

// Suspend/resume RFID scanning (e.g. during firmware upload)
void rfidSuspend();
void rfidResume();

#endif // RC522_ENABLED

#endif // RFID_HANDLER_H
