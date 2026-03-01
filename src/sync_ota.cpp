#include "config.h"  // Must be first for PLATFORM_ESP8266 detection

#ifdef PLATFORM_ESP8266

#include "sync_ota.h"
#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <Updater.h>
#include <LittleFS.h>
#include "wifi_manager.h"
#include "mqtt_handler.h"
#include "led_controller.h"
// Note: Don't include logger.h - we avoid flash writes during OTA

#if RC522_ENABLED
#include "rfid_handler.h"
#endif

// External variables from main.cpp
extern bool otaInProgress;
extern void updateLedStatus();

// Flag to signal main loop to switch to sync OTA mode
volatile bool requestSyncOTAMode = false;

// Linker symbols for filesystem size
extern "C" uint32_t _FS_start;
extern "C" uint32_t _FS_end;

// Forward declaration of webServer stop function
class WebServer;
extern WebServer webServer;

// =====================================================
// Synchronous OTA Server for ESP8266
// Used because AsyncWebServer + Update causes __yield panic
// =====================================================

// GitHub releases URL for manual checking
#define GITHUB_RELEASES_URL "https://github.com/" UPDATE_GITHUB_REPO "/releases"

// Generate OTA page with XHR-based uploads and progress bars
String generateOTAPage() {
    String p = F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Firmware Update</title><style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "background:linear-gradient(135deg,#1a1a2e,#16213e);min-height:100vh;color:#fff;padding:20px}"
        ".c{max-width:500px;margin:0 auto}"
        "h1{text-align:center;margin-bottom:8px;font-size:1.5em}"
        ".sub{text-align:center;color:#888;margin-bottom:24px;font-size:.9em}"
        ".cd{background:rgba(255,255,255,.05);border-radius:16px;padding:20px;margin-bottom:16px;"
        "border:1px solid rgba(255,255,255,.1)}"
        ".cd h2{font-size:1em;margin-bottom:12px}"
        ".ver{color:#888;font-size:.85em;margin-bottom:12px}"
        "input[type=file]{width:100%;padding:12px;margin:8px 0;background:rgba(255,255,255,.1);"
        "border:1px solid rgba(255,255,255,.2);border-radius:8px;color:#fff}"
        ".b{width:100%;padding:14px;border:none;border-radius:10px;font-size:1em;font-weight:600;"
        "cursor:pointer;color:#fff;margin-top:8px}"
        ".bu{background:linear-gradient(135deg,#6366f1,#8b5cf6)}"
        ".bu:hover:not(:disabled){opacity:.9}"
        ".bu:disabled{opacity:.5;cursor:not-allowed}"
        ".be{display:block;text-align:center;text-decoration:none;background:#374151;color:#fff}"
        ".pg{display:none;margin-top:12px}"
        ".pg.v{display:block}"
        ".pt{height:8px;background:rgba(255,255,255,.1);border-radius:4px;overflow:hidden;margin-bottom:6px}"
        ".pf{height:100%;background:linear-gradient(90deg,#6366f1,#22c55e);width:0%;transition:width .3s}"
        ".px{font-size:.85em;color:#888;text-align:center}"
        ".w{background:rgba(245,158,11,.2);color:#f59e0b;padding:12px;border-radius:8px;font-size:.85em;margin-bottom:12px}"
        ".ok{color:#22c55e;margin-top:8px;font-weight:600}"
        ".er{color:#ef4444;margin-top:8px;font-weight:600}"
        ".lk{display:inline-block;padding:14px 20px;background:linear-gradient(135deg,#6366f1,#8b5cf6);"
        "color:#fff;text-decoration:none;border-radius:10px;font-weight:600;text-align:center;width:100%;margin-top:8px}"
        "</style></head><body><div class='c'>"
        "<h1>Firmware Update</h1>"
        "<p class='sub'>ESP8266 Safe Update Mode</p>"
        "<div class='cd'><h2>Version Info</h2>"
        "<p class='ver'>Current: <b>");
    p += FIRMWARE_VERSION;
    p += F("</b></p>"
        "<a class='lk' href='" GITHUB_RELEASES_URL "' target='_blank'>View Releases on GitHub</a></div>"
        "<div class='cd'><h2>Upload Firmware</h2>"
        "<div class='w'>Do not interrupt the update!</div>"
        "<input type='file' id='fw' accept='.bin'>"
        "<button class='b bu' id='fwb' disabled>Upload Firmware</button>"
        "<div class='pg' id='fwp'><div class='pt'><div class='pf' id='fwf'></div></div>"
        "<div class='px' id='fwt'>0%</div></div>"
        "<div id='fws'></div></div>"
        "<div class='cd'><h2>Upload Web Interface</h2>"
        "<input type='file' id='fs' accept='.bin'>"
        "<button class='b bu' id='fsb' disabled>Upload Web Interface</button>"
        "<div class='pg' id='fsp'><div class='pt'><div class='pf' id='fsf'></div></div>"
        "<div class='px' id='fst'>0%</div></div>"
        "<div id='fss'></div></div>"
        "<a class='b be' href='/restart'>Exit Safe Mode</a></div>"
        "<script>"
        "function $(i){return document.getElementById(i)}"
        "$('fw').onchange=function(){$('fwb').disabled=!this.files.length};"
        "$('fs').onchange=function(){$('fsb').disabled=!this.files.length};"
        "$('fwb').onclick=function(){U('fw','/update')};"
        "$('fsb').onclick=function(){U('fs','/update-fs')};"
        "function U(id,url){"
        "var f=$(id).files[0];if(!f)return;"
        "$(id+'b').disabled=true;"
        "$(id+'p').classList.add('v');"
        "var s=$(id+'s');s.className='';s.textContent='';"
        "var d=new FormData();d.append('file',f);"
        "var x=new XMLHttpRequest();"
        "x.open('POST',url,true);"
        "x.upload.onprogress=function(e){"
        "if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);"
        "$(id+'f').style.width=p+'%';"
        "$(id+'t').textContent=p+'%';}};"
        "x.onload=function(){"
        "if(x.status===200){"
        "s.className='ok';s.textContent='Update successful! Restarting...';"
        "$(id+'t').textContent='Complete!';"
        "setTimeout(function(){location.href='/'},10000);}"
        "else{s.className='er';s.textContent='Failed: '+x.responseText;"
        "$(id+'b').disabled=false;}};"
        "x.onerror=function(){"
        "s.className='er';s.textContent='Connection lost during upload';"
        "$(id+'b').disabled=false;};"
        "x.send(d);}"
        "</script></body></html>");
    return p;
}

// Run the synchronous OTA server (blocking - takes over from main loop)
void runSyncOTAServer() {
    Serial.println("[OTA-SYNC] Starting synchronous OTA server...");
    // Note: Don't use logger during OTA - it writes to flash which can conflict

    // Stop MQTT to free memory and prevent interference
    mqttHandler.disconnect();
    Serial.println("[OTA-SYNC] MQTT disconnected");

    // Stop RFID to prevent SPI interference during flash writes
    #if RC522_ENABLED
    rfidSuspend();
    Serial.println("[OTA-SYNC] RFID suspended");
    #endif

    // Stop the async web server by calling its stop method
    // We use extern to access it without including the header
    extern void stopAsyncWebServer();
    stopAsyncWebServer();
    Serial.println("[OTA-SYNC] Async web server stopped");

    // Show OTA LED status
    otaInProgress = true;
    updateLedStatus();

    // Give some time for connections to close and memory to be freed
    delay(500);

    // Log free heap after cleanup
    Serial.printf("[OTA-SYNC] Free heap after cleanup: %u bytes\n", ESP.getFreeHeap());

    // Create synchronous web server
    ESP8266WebServer syncServer(80);

    // Serve the OTA page with dynamic version info
    syncServer.on("/", HTTP_GET, [&syncServer]() {
        String page = generateOTAPage();
        syncServer.send(200, "text/html", page);
    });

    // Exit safe mode and restart
    syncServer.on("/restart", HTTP_GET, [&syncServer]() {
        syncServer.send(200, "text/html",
            "<html><body style='background:#1a1a2e;color:#fff;font-family:sans-serif;text-align:center;padding:50px'>"
            "<h2>Restarting...</h2><p>Returning to normal mode.</p>"
            "<script>setTimeout(()=>location.href='/',5000)</script></body></html>");
        delay(500);
        ESP.restart();
    });

    // Handle firmware upload
    syncServer.on("/update", HTTP_POST, [&syncServer]() {
        if (Update.hasError()) {
            Serial.printf("[OTA-SYNC] Firmware update error: %s\n", Update.getErrorString().c_str());
            syncServer.send(500, "text/plain", Update.getErrorString());
        } else {
            syncServer.send(200, "text/plain", F("OK"));
            delay(1000);
            ESP.restart();
        }
    }, [&syncServer]() {
        HTTPUpload& upload = syncServer.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("[OTA-SYNC] Firmware upload start: %s\n", upload.filename.c_str());
            uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
            if (!Update.begin(maxSketchSpace, U_FLASH)) {
                Serial.printf("[OTA-SYNC] Update.begin failed: %s\n", Update.getErrorString().c_str());
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Serial.printf("[OTA-SYNC] Update.write failed: %s\n", Update.getErrorString().c_str());
            }
            // Feed watchdog
            ESP.wdtFeed();
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.printf("[OTA-SYNC] Firmware update success: %u bytes\n", upload.totalSize);
            } else {
                Serial.printf("[OTA-SYNC] Update.end failed: %s\n", Update.getErrorString().c_str());
            }
        }
    });

    // Handle filesystem upload
    syncServer.on("/update-fs", HTTP_POST, [&syncServer]() {
        if (Update.hasError()) {
            Serial.printf("[OTA-SYNC] Filesystem update error: %s\n", Update.getErrorString().c_str());
            syncServer.send(500, "text/plain", Update.getErrorString());
        } else {
            syncServer.send(200, "text/plain", F("OK"));
            delay(1000);
            ESP.restart();
        }
    }, [&syncServer]() {
        HTTPUpload& upload = syncServer.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("[OTA-SYNC] Filesystem upload start: %s\n", upload.filename.c_str());
            size_t fsSize = ((size_t)&_FS_end - (size_t)&_FS_start);
            LittleFS.end();  // Unmount filesystem before update
            if (!Update.begin(fsSize, U_FS)) {
                Serial.printf("[OTA-SYNC] Update.begin failed: %s\n", Update.getErrorString().c_str());
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Serial.printf("[OTA-SYNC] Update.write failed: %s\n", Update.getErrorString().c_str());
            }
            ESP.wdtFeed();
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.printf("[OTA-SYNC] Filesystem update success: %u bytes\n", upload.totalSize);
            } else {
                Serial.printf("[OTA-SYNC] Update.end failed: %s\n", Update.getErrorString().c_str());
            }
        }
    });

    syncServer.begin();
    Serial.println("[OTA-SYNC] Server started on port 80");
    Serial.println("[OTA-SYNC] Navigate to http://" + wifiManager.getIP() + "/ to upload firmware");

    // Run the server indefinitely (until reboot after update)
    // This is blocking - takes over from main loop
    while (true) {
        syncServer.handleClient();
        ESP.wdtFeed();
        delay(10);
    }
}

#endif // PLATFORM_ESP8266
