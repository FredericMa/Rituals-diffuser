#include "rfid_handler.h"

#if RC522_ENABLED

#include <SPI.h>
#include <MFRC522.h>
#include "mqtt_handler.h"  // For state publish on cartridge change

// RC522 instance
static MFRC522* mfrc522 = nullptr;
static bool rc522Connected = false;
static uint8_t rc522VersionReg = 0;  // Store version for debug

// Laatste gedetecteerde tag
// Use fixed char arrays to avoid heap fragmentation
static char lastUID[24] = "";           // UID max ~14 chars for 7-byte UID + null
static char lastScent[48] = "";         // Scent name
static char lastScentCode[12] = "";     // 8 hex chars + null
static unsigned long lastTagTime = 0;
static unsigned long lastScanTime = 0;
static bool hasValidTag = false;
static bool cartridgePresent = false;

// Scan intervals (ms) - longer when cartridge present to reduce SPI load
#define SCAN_INTERVAL_DETECT_MS  1000  // When waiting for new cartridge
#define SCAN_INTERVAL_PRESENT_MS 2000  // When confirming presence (less critical)
// Declare cartridge removed after N consecutive failed WUPA scans.
// Count-based (not time-based) so intermittent reads through metal enclosure
// don't cause false "cartridge removed" events.
#define CARTRIDGE_MISS_THRESHOLD 5     // 5 misses at 2s = ~10s before removal
// After N consecutive failures (no card), do a full PCD_Init recovery
#define RFID_RECOVERY_THRESHOLD  30

static uint8_t consecutiveFailures = 0;
static uint8_t consecutiveScanMisses = 0;
static bool rfidSuspended = false;

// Reduce the MFRC522 transceive timeout from default 25ms to 15ms.
// ISO 14443-A cards respond within ~91μs; 15ms is generous margin for metal enclosures.
// NOTE: PCD_CommunicateWithPICC() already calls yield() inside its busy-wait loop,
// so the WiFi stack is not fully starved during a scan. The primary RF interference
// source is the CONSTANT 13.56MHz carrier from PCD_AntennaOn() — mitigated by
// calling PCD_AntennaOff() after each scan (antenna-off approach below).
static void rfidSetFastTimeout() {
    if (mfrc522 == nullptr) return;
    // Timer freq = 13.56MHz / (2*(169+1)) ≈ 40kHz (set by PCD_Init)
    // PCD_Init sets reload 1000 (0x03E8) = 25ms; we override to 600 (0x0258) = 15ms
    // 15ms is safer than 10ms through metal enclosure (more margin for weak field)
    mfrc522->PCD_WriteRegister(mfrc522->TReloadRegH, 0x02);
    mfrc522->PCD_WriteRegister(mfrc522->TReloadRegL, 0x58);
}

// Geurtabel - officieel gedeeld
// Use PROGMEM to store table in Flash instead of RAM (no-op on ESP32)
struct ScentEntry {
    const char* uid;      // UID prefix (eerste 4 bytes als hex)
    const char* name;
};

// Geurtabel met hex codes - zowel 3-letter ASCII als officiële codes
// Elke geur heeft lowercase, uppercase (capitalized) en officiële hex varianten
#ifdef PLATFORM_ESP8266
#include <pgmspace.h>
#endif
static const ScentEntry scentTable[] PROGMEM = {
    // ============ KARMA ============
    {"6B6172", "The Ritual of Karma"},           // "kar" ASCII lowercase
    {"4B6172", "The Ritual of Karma"},           // "Kar" ASCII uppercase
    {"06B617", "The Ritual of Karma"},           // Officieel (alternatief)

    // ============ DAO ============
    {"64616F", "The Ritual of Dao"},             // "dao" ASCII lowercase
    {"44616F", "The Ritual of Dao"},             // "Dao" ASCII uppercase
    {"044616", "The Ritual of Dao"},             // Officieel

    // ============ HAPPY BUDDHA ============
    {"686170", "The Ritual of Happy Buddha"},    // "hap" ASCII lowercase
    {"486170", "The Ritual of Happy Buddha"},    // "Hap" ASCII uppercase
    {"04C617", "The Ritual of Happy Buddha"},    // Officieel

    // ============ SAKURA ============
    {"73616B", "The Ritual of Sakura"},          // "sak" ASCII lowercase
    {"53616B", "The Ritual of Sakura"},          // "Sak" ASCII uppercase
    {"053616", "The Ritual of Sakura"},          // Officieel

    // ============ AYURVEDA ============
    {"617975", "The Ritual of Ayurveda"},        // "ayu" ASCII lowercase
    {"417975", "The Ritual of Ayurveda"},        // "Ayu" ASCII uppercase
    {"047975", "The Ritual of Ayurveda"},        // Officieel

    // ============ HAMMAM ============
    {"68616D", "The Ritual of Hammam"},          // "ham" ASCII lowercase
    {"48616D", "The Ritual of Hammam"},          // "Ham" ASCII uppercase
    {"048616", "The Ritual of Hammam"},          // Officieel

    // ============ JING ============
    {"6A696E", "The Ritual of Jing"},            // "jin" ASCII lowercase
    {"4A696E", "The Ritual of Jing"},            // "Jin" ASCII uppercase
    {"04A696", "The Ritual of Jing"},            // Officieel

    // ============ MEHR ============
    {"6D6568", "The Ritual of Mehr"},            // "meh" ASCII lowercase
    {"4D6568", "The Ritual of Mehr"},            // "Meh" ASCII uppercase
    {"06D656", "The Ritual of Mehr"},            // Officieel

    // ============ SPRING GARDEN ============
    {"737072", "The Ritual of Spring Garden"},   // "spr" ASCII lowercase
    {"537072", "The Ritual of Spring Garden"},   // "Spr" ASCII uppercase
    {"057072", "The Ritual of Spring Garden"},   // Officieel

    // ============ PRIVATE COLLECTION ============
    {"676F6A", "Private Collection Goji Berry"},          // "goj" ASCII lowercase
    {"476F6A", "Private Collection Goji Berry"},          // "Goj" ASCII uppercase
    {"0476F6", "Private Collection Goji Berry"},          // Officieel

    {"766574", "Private Collection Oriental Vetiver"},    // "vet" ASCII lowercase
    {"566574", "Private Collection Oriental Vetiver"},    // "Vet" ASCII uppercase
    {"04F726", "Private Collection Oriental Vetiver"},    // Officieel

    {"6F7564", "Private Collection Black Oudh"},          // "oud" ASCII lowercase
    {"4F7564", "Private Collection Black Oudh"},          // "Oud" ASCII uppercase
    {"0426C6", "Private Collection Black Oudh"},          // Officieel  ⚠ DUPLICATE: also used by Cotton Blossom

    {"616D62", "Private Collection Precious Amber"},      // "amb" ASCII lowercase
    {"416D62", "Private Collection Precious Amber"},      // "Amb" ASCII uppercase
    {"057265", "Private Collection Precious Amber"},      // Officieel  ⚠ DUPLICATE: also used by Green Cardamom

    {"6A6173", "Private Collection Sweet Jasmine"},       // "jas" ASCII lowercase
    {"4A6173", "Private Collection Sweet Jasmine"},       // "Jas" ASCII uppercase
    {"057765", "Private Collection Sweet Jasmine"},       // Officieel

    {"726F73", "Private Collection Imperial Rose"},       // "ros" ASCII lowercase
    {"526F73", "Private Collection Imperial Rose"},       // "Ros" ASCII uppercase
    {"0496D7", "Private Collection Imperial Rose"},       // Officieel

    {"736176", "Private Collection Savage Garden"},       // "sav" ASCII lowercase
    {"536176", "Private Collection Savage Garden"},       // "Sav" ASCII uppercase
    {"056176", "Private Collection Savage Garden"},       // Officieel

    {"76616E", "Private Collection Suede Vanilla"},       // "van" ASCII lowercase
    {"56616E", "Private Collection Suede Vanilla"},       // "Van" ASCII uppercase
    {"056616", "Private Collection Suede Vanilla"},       // Officieel

    {"636F74", "Private Collection Cotton Blossom"},      // "cot" ASCII lowercase
    {"436F74", "Private Collection Cotton Blossom"},      // "Cot" ASCII uppercase
    {"0426C6", "Private Collection Cotton Blossom"},      // Officieel  ⚠ DUPLICATE: same as Black Oudh (first match wins)

    {"636172", "Private Collection Green Cardamom"},      // "car" ASCII lowercase
    {"436172", "Private Collection Green Cardamom"},      // "Car" ASCII uppercase
    {"047265", "Private Collection Green Cardamom"},      // Officieel  ⚠ DUPLICATE: same as Precious Amber (first match wins)

    {"746561", "Private Collection Royal Tea"},           // "tea" ASCII lowercase
    {"546561", "Private Collection Royal Tea"},           // "Tea" ASCII uppercase
    {"047275", "Private Collection Royal Tea"},           // Officieel

    // ============ JING NIGHT ============
    {"6E6967", "The Ritual of Jing Night"},      // "nig" ASCII lowercase
    {"4E6967", "The Ritual of Jing Night"},      // "Nig" ASCII uppercase
    {"047375", "The Ritual of Jing Night"},      // Officieel

    // ============ INVALID ============
    {"013A0C", "Cartridge tag invalid"},         // Officieel

    {nullptr, nullptr}  // End marker
};

bool rfidInit() {
    Serial.println("[RFID] Initializing RC522...");
    Serial.printf("[RFID] Pins: SCK=%d, MOSI=%d, MISO=%d, CS=%d, RST=%d\n",
                  RC522_SCK_PIN, RC522_MOSI_PIN, RC522_MISO_PIN, RC522_CS_PIN, RC522_RST_PIN);

    // Setup CS and RST pins BEFORE SPI init
    pinMode(RC522_CS_PIN, OUTPUT);
    pinMode(RC522_RST_PIN, OUTPUT);
    digitalWrite(RC522_CS_PIN, HIGH);   // CS inactive
    digitalWrite(RC522_RST_PIN, HIGH);  // Not in reset
    Serial.println("[RFID] CS and RST pins configured");

    // Initialize SPI - platform specific
#ifdef PLATFORM_ESP8266
    // ESP8266 uses fixed HSPI pins (GPIO14=SCK, GPIO12=MISO, GPIO13=MOSI)
    Serial.println("[RFID] ESP8266: Using hardware HSPI");
    SPI.begin();
#else
    // ESP32 supports custom SPI pins
    Serial.println("[RFID] ESP32: Using custom SPI pins");
    SPI.begin(RC522_SCK_PIN, RC522_MISO_PIN, RC522_MOSI_PIN, RC522_CS_PIN);
#endif

    delay(50);  // Let SPI stabilize

    // Perform hardware reset
    Serial.println("[RFID] Performing hardware reset...");
    digitalWrite(RC522_RST_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(RC522_RST_PIN, HIGH);
    delay(50);  // Wait for oscillator startup

    // Create MFRC522 instance (delete existing if re-initializing to prevent memory leak)
    if (mfrc522 != nullptr) {
        delete mfrc522;
        mfrc522 = nullptr;
    }
    mfrc522 = new MFRC522(RC522_CS_PIN, RC522_RST_PIN);

    // Initialize the MFRC522
    Serial.println("[RFID] Calling PCD_Init()...");
    mfrc522->PCD_Init();
    delay(100);

    // Read version register multiple times to check stability
    Serial.println("[RFID] Reading version register...");
    byte version1 = mfrc522->PCD_ReadRegister(mfrc522->VersionReg);
    delay(10);
    byte version2 = mfrc522->PCD_ReadRegister(mfrc522->VersionReg);
    delay(10);
    byte version3 = mfrc522->PCD_ReadRegister(mfrc522->VersionReg);

    Serial.printf("[RFID] Version reads: 0x%02X, 0x%02X, 0x%02X\n", version1, version2, version3);

    // Use the most common value (simple majority vote)
    byte version = version1;
    if (version1 == version2 || version1 == version3) {
        version = version1;
    } else if (version2 == version3) {
        version = version2;
    }

    // Store for debug access via web API
    rc522VersionReg = version;

    if (version == 0x91 || version == 0x92 || version == 0x88) {
        rc522Connected = true;
        Serial.printf("[RFID] RC522 detected! Firmware version: 0x%02X", version);
        if (version == 0x91) Serial.println(" (v1.0)");
        else if (version == 0x92) Serial.println(" (v2.0)");
        else if (version == 0x88) Serial.println(" (clone)");
        else Serial.println();

        // NOTE: Self-test (PCD_PerformSelfTest) deliberately omitted.
        // It takes ~60ms blocking, disables crypto (requiring another PCD_Init),
        // and is purely diagnostic — a successful version register read already
        // confirms SPI communication and RC522 functionality.

        // Maximize receiver gain for metal enclosure environments
        mfrc522->PCD_SetAntennaGain(mfrc522->RxGain_max);
        Serial.println("[RFID] Antenna gain set to maximum");

        // Reduce transceive timeout from 25ms to 15ms
        rfidSetFastTimeout();
        Serial.println("[RFID] Transceive timeout set to 15ms");

        // Turn antenna off after init — it will be toggled on/off per scan
        // to eliminate constant 13.56MHz RF emission between scans (WiFi stability)
        mfrc522->PCD_AntennaOff();
        Serial.println("[RFID] Antenna off (will be toggled per scan)");

        return true;
    } else {
        rc522Connected = false;
        Serial.printf("[RFID] RC522 NOT detected! Got version: 0x%02X\n", version);
        if (version == 0x00) {
            Serial.println("[RFID] Version 0x00 suggests: no communication (check wiring/CS pin)");
        } else if (version == 0xFF) {
            Serial.println("[RFID] Version 0xFF suggests: no communication (check wiring/power)");
        }
        Serial.println("[RFID] Expected: 0x91 (v1.0), 0x92 (v2.0), or 0x88 (clone)");
        Serial.println("[RFID] Check wiring!");

        // Debug: try reading other registers
        Serial.println("[RFID] Debug - reading other registers:");
        byte commandReg = mfrc522->PCD_ReadRegister(mfrc522->CommandReg);
        byte statusReg = mfrc522->PCD_ReadRegister(mfrc522->Status1Reg);
        Serial.printf("[RFID] CommandReg: 0x%02X, Status1Reg: 0x%02X\n", commandReg, statusReg);

        return false;
    }
}

void rfidLoop() {
    if (!rc522Connected || mfrc522 == nullptr || rfidSuspended) {
        return;
    }

    unsigned long now = millis();

    // Use longer interval when cartridge is confirmed present (less SPI contention)
    unsigned long interval = cartridgePresent ? SCAN_INTERVAL_PRESENT_MS : SCAN_INTERVAL_DETECT_MS;
    if (now - lastScanTime < interval) {
        return;
    }
    lastScanTime = now;

    // ============================================================
    // MODE 1: PRESENCE CHECK (cartridge already detected)
    // ============================================================
    // Antenna was off since last scan. Turn it on briefly, send WUPA
    // to confirm card is still in the RF field, then turn antenna off.
    // WUPA wakes HALT-state and IDLE-state (power-cycled) cards.
    // Antenna-off between scans reduces 13.56MHz RF interference with WiFi.
    if (cartridgePresent) {
        mfrc522->PCD_AntennaOn();
        delay(5);  // Allow card to power up from RF field (~5ms)

        byte bufferATQA[2];
        byte bufferSize = sizeof(bufferATQA);
        MFRC522::StatusCode status = mfrc522->PICC_WakeupA(bufferATQA, &bufferSize);

        mfrc522->PCD_AntennaOff();  // RF off immediately after scan

        if (status == MFRC522::STATUS_OK) {
            // Card still in field - reset counters
            consecutiveFailures = 0;
            consecutiveScanMisses = 0;
            lastTagTime = now;
        } else {
            consecutiveFailures++;
            consecutiveScanMisses++;

            // Count-based removal: only declare removed after N consecutive misses
            if (consecutiveScanMisses >= CARTRIDGE_MISS_THRESHOLD) {
                cartridgePresent = false;
                consecutiveScanMisses = 0;
                Serial.println("[RFID] Cartridge removed");
                mqttHandler.requestStatePublish();
            }

            // Full PCD_Init recovery after many consecutive failures
            if (consecutiveFailures >= RFID_RECOVERY_THRESHOLD) {
                mfrc522->PCD_Init();
                mfrc522->PCD_SetAntennaGain(mfrc522->RxGain_max);
                rfidSetFastTimeout();
                mfrc522->PCD_AntennaOff();  // Keep antenna off after recovery
                consecutiveFailures = 0;
            }
        }
        return;  // Done - one SPI operation per loop iteration
    }

    // ============================================================
    // MODE 2: DETECTION (no cartridge, waiting for new card)
    // ============================================================
    // Turn antenna on, send REQA, then off again (antenna-off per scan).
    // New/idle cards respond to REQA. Card was power-cycled so it's in IDLE state.
    mfrc522->PCD_AntennaOn();
    delay(5);  // Allow card to power up from RF field

    bool cardFound = mfrc522->PICC_IsNewCardPresent();

    if (!cardFound) {
        mfrc522->PCD_AntennaOff();  // RF off - no card found
        return;
    }

    // New card responding! Select it (antenna still on).
    if (!mfrc522->PICC_ReadCardSerial()) {
        mfrc522->PCD_AntennaOff();
        return;
    }

    // Kaart gedetecteerd - update timestamp
    lastTagTime = now;
    bool wasPresent = cartridgePresent;
    cartridgePresent = true;

    // Build UID string using char array to avoid heap allocation
    char uid[24];
    uid[0] = '\0';
    for (byte i = 0; i < mfrc522->uid.size && i < 10; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", mfrc522->uid.uidByte[i]);
        strcat(uid, hex);
    }

    // Check of dit dezelfde kaart is als vorige keer
    bool isNewCard = (strcmp(uid, lastUID) != 0) || !wasPresent;

    // Update state
    hasValidTag = true;

    // Als het dezelfde kaart is, alleen timestamp updaten en stoppen
    if (!isNewCard) {
        // Halt PICC and turn antenna off until next scan
        mfrc522->PICC_HaltA();
        mfrc522->PCD_AntennaOff();
        return;
    }

    // NIEUWE KAART - volledige verwerking
    strncpy(lastUID, uid, sizeof(lastUID) - 1);
    lastUID[sizeof(lastUID) - 1] = '\0';

    // Get tag type
    MFRC522::PICC_Type piccType = mfrc522->PICC_GetType(mfrc522->uid.sak);
    Serial.println();
    Serial.println("========== NEW CARTRIDGE DETECTED ==========");
    Serial.printf("UID: %s (%d bytes)\n", uid, mfrc522->uid.size);
    Serial.printf("Tag type: %s\n", mfrc522->PICC_GetTypeName(piccType));

    // Minimal read - only page 4 (scent code) to avoid blocking the main loop.
    // MIFARE_Read reads 16 bytes starting from page, so page 4 gives us pages 4-7
    byte buffer[18];  // 16 bytes + 2 CRC
    byte size = sizeof(buffer);
    
    MFRC522::StatusCode status = mfrc522->MIFARE_Read(4, buffer, &size);
    if (status == MFRC522::STATUS_OK) {
        // Extract page 4 (first 4 bytes of buffer)
        char page4Hex[9];
        snprintf(page4Hex, sizeof(page4Hex), "%02X%02X%02X%02X", 
                 buffer[0], buffer[1], buffer[2], buffer[3]);
        strncpy(lastScentCode, page4Hex, sizeof(lastScentCode) - 1);
        lastScentCode[sizeof(lastScentCode) - 1] = '\0';
        
        // ASCII for unknown scents
        char page4Ascii[5];
        for (int i = 0; i < 4; i++) {
            page4Ascii[i] = (buffer[i] >= 32 && buffer[i] < 127) ? (char)buffer[i] : '.';
        }
        page4Ascii[4] = '\0';
        
        Serial.printf("[RFID] Page 4: %s (ASCII: %s)\n", page4Hex, page4Ascii);
        
        // Lookup scent
        ScentInfo info = rfidLookupScent(page4Hex);
        if (info.valid) {
            strncpy(lastScent, info.name, sizeof(lastScent) - 1);
            lastScent[sizeof(lastScent) - 1] = '\0';
            Serial.printf("[RFID] Matched scent: %s\n", lastScent);
        } else {
            snprintf(lastScent, sizeof(lastScent), "Unknown: %s", page4Ascii);
            Serial.printf("[RFID] Unknown scent\n");
        }
    } else {
        Serial.printf("[RFID] Read failed: %d\n", status);
        strncpy(lastScent, "Read Error", sizeof(lastScent) - 1);
        lastScent[sizeof(lastScent) - 1] = '\0';
    }

    Serial.println("============================================\n");

    // Notify MQTT of new cartridge
    mqttHandler.requestStatePublish();

    // Halt PICC and turn antenna off until next scan
    mfrc522->PICC_HaltA();
    mfrc522->PCD_AntennaOff();
}

const char* rfidGetLastUID() {
    return lastUID;
}

const char* rfidGetLastScent() {
    return lastScent;
}

bool rfidHasTag() {
    // Retourneert true als er OOIT een tag was
    return hasValidTag;
}

bool rfidIsCartridgePresent() {
    // Retourneert true als cartridge NU aanwezig is (niet timeout)
    return cartridgePresent;
}

unsigned long rfidTimeSinceLastTag() {
    if (!hasValidTag) {
        return UINT32_MAX;
    }
    return millis() - lastTagTime;
}

ScentInfo rfidLookupScent(const char* hexData) {
    ScentInfo info;
    info.valid = false;
    info.name = nullptr;
    info.hexCode = hexData;

    if (hexData == nullptr || hexData[0] == '\0') return info;

    // Normalize to uppercase for matching (page4Hex is already uppercase from snprintf %02X)
    // Search scent table - compare first 6 chars of page4Hex (8 chars) against table entries (6 chars)
    // This is a prefix match: the 8-char page4 hex starts with the 6-char scent code
    ScentEntry entry;
    for (int i = 0; ; i++) {
        memcpy_P(&entry, &scentTable[i], sizeof(ScentEntry));
        if (entry.uid == nullptr) break;
        // Compare the 6-char scent code against the start of the page4 hex
        if (strncmp(hexData, entry.uid, 6) == 0) {
            info.name = entry.name;
            info.valid = true;
            Serial.printf("[RFID] Found hex pattern: %s\n", entry.uid);
            break;
        }
    }

    return info;
}

bool rfidIsConnected() {
    return rc522Connected;
}

uint8_t rfidGetVersionReg() {
    return rc522VersionReg;
}

void rfidSuspend() {
    rfidSuspended = true;
    // Halt any active card communication and turn off RF field to save power
    if (mfrc522 != nullptr && rc522Connected) {
        mfrc522->PICC_HaltA();
        // Turn off antenna
        mfrc522->PCD_AntennaOff();
    }
    Serial.println("[RFID] Suspended");
}

void rfidResume() {
    rfidSuspended = false;
    // Re-enable antenna for scanning
    if (mfrc522 != nullptr && rc522Connected) {
        mfrc522->PCD_AntennaOn();
    }
    consecutiveFailures = 0;
    consecutiveScanMisses = 0;
    Serial.println("[RFID] Resumed");
}

#endif // RC522_ENABLED
