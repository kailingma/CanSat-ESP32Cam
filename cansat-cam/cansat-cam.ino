#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <SD_MMC.h>
#include <FS.h>
#include <vector>
#include <algorithm>
#include <functional>
#include <stdarg.h>

// ============================================================================
// CAMERA PIN DEFINITIONS
// ============================================================================
// These pins are specific to the AI-Thinker ESP32-CAM board.
// DO NOT change these unless you have a different ESP32-CAM variant.

#define PWDN_GPIO_NUM     32    // Power down pin
#define RESET_GPIO_NUM    -1    // Reset pin (not used, set to -1)
#define XCLK_GPIO_NUM      0    // External clock pin
#define SIOD_GPIO_NUM     26    // I2C SDA (camera sensor control)
#define SIOC_GPIO_NUM     27    // I2C SCL (camera sensor control)

// Camera data pins (8-bit parallel interface)
#define Y9_GPIO_NUM       35    // Data pin 9
#define Y8_GPIO_NUM       34    // Data pin 8
#define Y7_GPIO_NUM       39    // Data pin 7
#define Y6_GPIO_NUM       36    // Data pin 6
#define Y5_GPIO_NUM       21    // Data pin 5
#define Y4_GPIO_NUM       19    // Data pin 4
#define Y3_GPIO_NUM       18    // Data pin 3
#define Y2_GPIO_NUM        5    // Data pin 2

// Camera control pins
#define VSYNC_GPIO_NUM    25    // Vertical sync signal
#define HREF_GPIO_NUM     23    // Horizontal reference signal
#define PCLK_GPIO_NUM     22    // Pixel clock signal

// ============================================================================
// COMMUNICATION PIN DEFINITIONS
// ============================================================================
// These pins connect to the remote Arduino controller.

// IMPORTANT:
// - SD card is initialized in 1-bit SD_MMC mode to free GPIO12 and GPIO13.
// - UART is moved to GPIO13/GPIO12 (RX/TX) to keep GPIO1/GPIO3 free for USB serial monitor.
// - GPIO4 is used for trigger input (it also drives the flash LED on many boards).
#define TRIGGER_PIN        4    // Input pin: Arduino pulls LOW to request photo capture
#define UART_RX           13    // Serial2 RX: receives filename from Arduino
#define UART_TX           12    // Serial2 TX: sends status messages back to Arduino

// ============================================================================
// SD CARD PIN DEFINITIONS
// ============================================================================
// On the AI-Thinker ESP32-CAM the onboard microSD is wired as SD_MMC:
//   CLK=GPIO14, CMD=GPIO15, DATA0=GPIO2, DATA1=GPIO4, DATA2=GPIO12, DATA3=GPIO13
// We run SD_MMC in 1-bit mode so DATA2 (GPIO12) and DATA3 (GPIO13) are unused
// by SD and can be repurposed for external UART.

// ============================================================================
// STORAGE MODE ENUMERATION
// ============================================================================
// Tracks which storage device we're currently using

enum StorageMode {
  STORAGE_NONE,       // No storage available
  STORAGE_SD_CARD,    // Using SD card (preferred)
  STORAGE_SPIFFS      // Using internal flash (fallback)
};

// ============================================================================
// GLOBAL STATE VARIABLES
// ============================================================================

// Flag set by interrupt when trigger pin goes LOW
volatile bool captureFlag = false;
unsigned long ignoreTriggerUntilMs = 0;

// Tracks which storage device is currently in use
StorageMode currentStorage = STORAGE_NONE;

// Counter for failed captures (no filename, timeout, or invalid format)
uint16_t failCounter = 0;

// Next auto-increment number for SPIFFS camera roll (/NNNN.jpg)
uint16_t spiffsImageCounter = 1;

// ============================================================================
// WEB SERVER GLOBAL STATE
// ============================================================================

// HTTP server instance on port 80
WebServer server(80);

// Tracks whether the web server is currently active
bool webServerRunning = false;

// WiFi AP credentials
// NOTE: Change AP_PASSWORD to a stronger value before deploying in the field.
// The default "12345678" is intentionally simple for first-time setup only.
static const char* AP_SSID     = "ESP32-CAM-Browser";
static const char* AP_PASSWORD = "12345678";

// ============================================================================
// LIGHTWEIGHT LOG BUFFER (FOR /logs WEB VIEW)
// ============================================================================
constexpr size_t LOG_BUFFER_SIZE = 4096;

class LogBuffer {
public:
  void begin() { head = 0; used = 0; }

  void print(const char* text, bool includeInWebLog = false) {
    if (!text) return;
    if (Serial) {
      Serial.print(text);
    }
    if (includeInWebLog) {
      append(text);
    }
  }

  void printf(bool includeInWebLog, const char* fmt, ...) {
    if (!fmt) return;
    char line[192];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n > 0) {
      print(line, includeInWebLog);
    }
  }

  String snapshot() const {
    String out;
    out.reserve(used + 64);
    out += "ESP32-CAM recent log buffer\n";
    out += "---------------------------\n";
    size_t start = (head + LOG_BUFFER_SIZE - used) % LOG_BUFFER_SIZE;
    for (size_t i = 0; i < used; i++) {
      out += buffer[(start + i) % LOG_BUFFER_SIZE];
    }
    if (used == 0) {
      out += "(empty)\n";
    }
    return out;
  }

private:
  void append(const char* text) {
    while (*text) {
      buffer[head] = *text++;
      head = (head + 1) % LOG_BUFFER_SIZE;
      if (used < LOG_BUFFER_SIZE) used++;
    }
  }

  char buffer[LOG_BUFFER_SIZE];
  size_t head = 0;
  size_t used = 0;
};

LogBuffer logs;

// ============================================================================
// PATH VISIBILITY HELPERS
// ============================================================================
// Hidden entries are any files or directories whose name starts with '.'
// (e.g. ".secret", "/logs/.tmp/cap.jpg"). These are excluded from terminal
// listings and from all web server file listing/fetch routes.

bool isHiddenPath(const String& fullPath) {
  int segmentStart = 0;
  while (segmentStart < fullPath.length()) {
    int nextSlash = fullPath.indexOf('/', segmentStart);
    if (nextSlash == -1) {
      nextSlash = fullPath.length();
    }

    if (nextSlash > segmentStart) {
      String segment = fullPath.substring(segmentStart, nextSlash);
      if (segment.length() > 0 && segment[0] == '.') {
        return true;
      }
    }

    segmentStart = nextSlash + 1;
  }
  return false;
}

bool isHiddenName(const char* name) {
  return name != nullptr && name[0] == '.';
}

String getContentType(const String& path) {
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".gif")) return "image/gif";
  if (path.endsWith(".bmp")) return "image/bmp";
  if (path.endsWith(".txt")) return "text/plain";
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".json")) return "application/json";
  return "application/octet-stream";
}

// ============================================================================
// INTERRUPT SERVICE ROUTINE (ISR)
// ============================================================================
// This function is called when the trigger pin transitions from HIGH to LOW.
// It sets a flag that the main loop checks, rather than doing heavy processing
// in the ISR itself (which is a best practice for embedded systems).
//
// IRAM_ATTR places this function in fast RAM for quick execution.

void IRAM_ATTR triggerISR() {
  captureFlag = true;  // Signal main loop to capture a photo
}

// ============================================================================
// SD CARD INITIALIZATION
// ============================================================================
// Attempts to initialize the SD card module via SPI.
// If successful, SD card becomes the primary storage device.
//
// Returns:
//   true: SD card initialized successfully
//   false: SD card initialization failed

bool initSDCard() {
  // ---- Attempt SD Card Initialization ----
  // 1-bit mode frees GPIO12/GPIO13 from SD data lines.
  if (!SD_MMC.begin("/sdcard", true /* mode1bit */)) {
    Serial.println("SD card initialization failed");
    return false;
  }

  // ---- Verify SD Card is Readable ----
  uint8_t cardType = SD_MMC.cardType();

  if (cardType == CARD_NONE) {
    Serial.println("No SD card detected");
    return false;
  }

  // ---- Log SD Card Information ----
  if (cardType == CARD_MMC) {
    Serial.println("SD card type: MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SD card type: SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SD card type: SDHC");
  } else {
    Serial.println("SD card type: UNKNOWN");
  }

  // ---- Print Storage Statistics ----
  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("SD card size: %lluMB\n", cardSize);

  currentStorage = STORAGE_SD_CARD;
  Serial.println("SD card ready (primary storage)");

  return true;
}

// ============================================================================
// SPIFFS (Flash Storage) INITIALIZATION
// ============================================================================
// Sets up the file system on the ESP32's built-in flash memory.
// This is used as a fallback if the SD card is unavailable.

bool initSPIFFS() {
  // ---- Attempt SPIFFS Mount ----
  // The 'true' parameter means: if mount fails, format and try again
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
    return false;
  }

  Serial.println("SPIFFS initialized (fallback storage)");

  // ---- Print Storage Statistics ----
  size_t total = SPIFFS.totalBytes();
  size_t used = SPIFFS.usedBytes();
  Serial.printf("SPIFFS: %d bytes total, %d bytes used\n", total, used);

  currentStorage = STORAGE_SPIFFS;

  return true;
}

// ============================================================================
// FAIL COUNTER INITIALIZATION
// ============================================================================
// Scans the /fail/ directory for existing failed captures and initializes
// the fail counter to the next available number. This ensures we don't
// overwrite previous failed captures if the system reboots.

void initializeFailCounter() {
  // ---- Initialize to 1 (starting point) ----
  uint16_t maxFound = 0;

  // ---- Scan Existing Failed Captures ----
  if (currentStorage == STORAGE_SD_CARD) {
    // ---- SD Card /fail/ Directory ----
    File failDir = SD_MMC.open("/fail");
    if (failDir && failDir.isDirectory()) {
      Serial.println("Scanning SD card /fail/ directory...");
      
      File file = failDir.openNextFile();
      while (file) {
        String filename = file.name();
        Serial.printf("  Found file: %s\n", filename.c_str());
        
        // ---- Parse Filename to Extract Counter ----
        if (filename.length() >= 8 && filename.endsWith(".jpg")) {
          String numberPart = filename.substring(0, 4);
          
          bool isValidNumber = true;
          for (int i = 0; i < 4; i++) {
            if (!isdigit(numberPart[i])) {
              isValidNumber = false;
              break;
            }
          }
          
          if (isValidNumber) {
            uint16_t fileNumber = numberPart.toInt();
            Serial.printf("    Parsed number: %04d\n", fileNumber);
            if (fileNumber > maxFound) {
              maxFound = fileNumber;
            }
          }
        }
        file.close();
        file = failDir.openNextFile();
      }
      failDir.close();
    } else {
      Serial.println("SD card /fail/ directory not found or empty");
    }
  } else if (currentStorage == STORAGE_SPIFFS) {
    // ---- SPIFFS /fail/ Directory ----
    Serial.println("Scanning SPIFFS /fail/ directory...");
    
    File root = SPIFFS.open("/");
    if (root) {
      File file = root.openNextFile();
      while (file) {
        String filename = file.name();
        
        // ---- Check if this is a /fail/ file ----
        if (filename.startsWith("/fail/") && filename.endsWith(".jpg")) {
          Serial.printf("  Found file: %s\n", filename.c_str());
          
          // ---- Extract 4-digit number from /fail/XXXX.jpg ----
          String numberPart = filename.substring(6, 10);
          
          bool isValidNumber = true;
          for (int i = 0; i < 4; i++) {
            if (!isdigit(numberPart[i])) {
              isValidNumber = false;
              break;
            }
          }
          
          if (isValidNumber) {
            uint16_t fileNumber = numberPart.toInt();
            Serial.printf("    Parsed number: %04d\n", fileNumber);
            if (fileNumber > maxFound) {
              maxFound = fileNumber;
            }
          }
        }
        file.close();
        file = root.openNextFile();
      }
      root.close();
    } else {
      Serial.println("SPIFFS root cannot be opened");
    }
  }

  // ---- Set Fail Counter to Next Available ----
  failCounter = maxFound + 1;
  Serial.printf("Fail counter initialized to: %04d\n\n", failCounter);
}

// ============================================================================
// SPIFFS IMAGE COUNTER INITIALIZATION
// ============================================================================
// Scans SPIFFS for files matching /NNNN.jpg (at root) and initializes
// spiffsImageCounter to the next available number.

uint16_t getNextSPIFFSImageNumber() {
  if (currentStorage != STORAGE_SPIFFS) {
    return 1;
  }

  uint16_t maxFound = 0;
  File root = SPIFFS.open("/");
  if (!root) {
    return 1;
  }

  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String filename = file.name();
      // Root files look like "/0001.jpg".
      if (filename.length() == 9 && filename[0] == '/' && filename.endsWith(".jpg")) {
        String numberPart = filename.substring(1, 5);
        bool isValidNumber = true;
        for (int i = 0; i < 4; i++) {
          if (!isdigit(numberPart[i])) {
            isValidNumber = false;
            break;
          }
        }
        if (isValidNumber) {
          uint16_t fileNumber = numberPart.toInt();
          if (fileNumber > maxFound) {
            maxFound = fileNumber;
          }
        }
      }
    }
    file.close();
    file = root.openNextFile();
  }
  root.close();

  uint16_t next = maxFound + 1;
  if (next > 9999) {
    next = 1;
  }
  return next;
}

// ============================================================================
// FILEPATH VALIDATION FUNCTION
// ============================================================================
// Validates that a received filepath matches expected format.
// Expected format: /runXX/XXXXX.jpg where XX and XXXXX are digits.

bool isValidFilepath(const String& filepath) {
  // ---- Check Minimum Length ----
  if (filepath.length() < 7) {
    Serial.printf("Invalid filepath: too short (%d chars)\n", filepath.length());
    return false;
  }

  // ---- Check Path Prefix ----
  if (filepath[0] != '/') {
    Serial.println("Invalid filepath: does not start with /");
    return false;
  }

  // ---- Check File Extension ----
  if (!filepath.endsWith(".jpg")) {
    Serial.println("Invalid filepath: does not end with .jpg");
    return false;
  }

  // ---- Check for Directory Separator ----
  int slashCount = 0;
  for (int i = 0; i < filepath.length(); i++) {
    if (filepath[i] == '/') {
      slashCount++;
    }
  }
  
  if (slashCount < 2) {
    Serial.println("Invalid filepath: missing directory structure");
    return false;
  }

  // ---- Check for Invalid Characters ----
  for (int i = 0; i < filepath.length(); i++) {
    char c = filepath[i];
    if (!isalnum(c) && c != '/' && c != '.' && c != '_') {
      Serial.printf("Invalid filepath: contains illegal character '%c'\n", c);
      return false;
    }
  }

  Serial.printf("Filepath validation passed: %s\n", filepath.c_str());
  return true;
}

// ============================================================================
// DIRECTORY CREATION HELPER
// ============================================================================
// Creates a directory in the storage device if it doesn't already exist.

bool ensureDirectoryExists(const char* path) {
  if (currentStorage == STORAGE_SD_CARD) {
    // ---- SD Card Directory Handling ----
    File dir = SD_MMC.open(path);
    
    if (dir && dir.isDirectory()) {
      dir.close();
      return true;
    }
    
    if (dir) {
      dir.close();
    }

    // ---- Create Directory on SD Card ----
    if (SD_MMC.mkdir(path)) {
      Serial.printf("Created directory: %s\n", path);
      return true;
    } else {
      Serial.printf("Failed to create directory: %s\n", path);
      return false;
    }
  } else {
    // ---- SPIFFS Directory Handling ----
    // SPIFFS is flat - directory structure is simulated with naming
    Serial.printf("SPIFFS flat filesystem - simulated directory: %s\n", path);
    return true;
  }
}

// ============================================================================
// PHOTO CAPTURE AND SAVE FUNCTION
// ============================================================================
// Captures a frame from the camera and saves it to the active storage device.
// The path can include directory structure (e.g., "/run01/12345.jpg").
// If the filepath is invalid or missing, saves to /fail/XXXX.jpg instead.

bool captureAndSave(const char* filepath) {
  // ---- Capture Frame from Camera ----
  camera_fb_t *fb = esp_camera_fb_get();

  if (!fb) {
    Serial.println("Camera capture failed");
    return false;
  }

  // ---- SPIFFS naming behavior ----
  // SPIFFS is treated as a flat "camera roll": any requested filename is
  // ignored and we always auto-increment to the next available NNNN.jpg.
  if (currentStorage == STORAGE_SPIFFS) {
    bool success = captureAndSaveAutoIncrementSPIFFS(fb);
    esp_camera_fb_return(fb);
    return success;
  }

  // ---- Validate Filepath Format (SD card only) ----
  if (!isValidFilepath(filepath)) {
    Serial.printf("Invalid filepath format: %s\n", filepath);
    Serial.println("Saving to failure fallback location");

    // ---- Save to Failure Fallback ----
    bool success = captureAndSaveFailure(fb);
    esp_camera_fb_return(fb);

    return success;
  }

  // ---- Extract Directory Path ----
  char dirPath[64] = "";
  const char* lastSlash = strrchr(filepath, '/');

  if (lastSlash != NULL && lastSlash != filepath) {
    size_t dirLength = lastSlash - filepath;
    strncpy(dirPath, filepath, dirLength);
    dirPath[dirLength] = '\0';

    // ---- Ensure Directory Exists ----
    if (!ensureDirectoryExists(dirPath)) {
      esp_camera_fb_return(fb);
      return false;
    }
  }

  // ---- Attempt Save ----
  bool success = false;
  
  if (currentStorage == STORAGE_SD_CARD) {
    File file = SD_MMC.open(filepath, FILE_WRITE);
    if (file) {
      size_t written = file.write(fb->buf, fb->len);
      file.close();
      
      if (written == fb->len) {
        Serial.printf("Saved to SD card: %s (%d bytes)\n", filepath, written);
        success = true;
      } else {
        Serial.printf("SD card write incomplete. Wrote %d of %d bytes\n", written, fb->len);
      }
    }
  } else if (currentStorage == STORAGE_SPIFFS) {
    File file = SPIFFS.open(filepath, FILE_WRITE);
    if (file) {
      size_t written = file.write(fb->buf, fb->len);
      file.close();
      
      if (written == fb->len) {
        Serial.printf("Saved to SPIFFS: %s (%d bytes)\n", filepath, written);
        success = true;
      } else {
        Serial.printf("SPIFFS write incomplete. Wrote %d of %d bytes\n", written, fb->len);
      }
    }
  }

  esp_camera_fb_return(fb);
  return success;
}

// ============================================================================
// SPIFFS AUTO-INCREMENT SAVE
// ============================================================================
// When SPIFFS is the active backend, ignore the requested filename and instead
// save as /NNNN.jpg where NNNN is one higher than the highest existing number.

bool captureAndSaveAutoIncrementSPIFFS(camera_fb_t *fb) {
  // Use cached counter (initialized at boot, updated after each save).
  // This avoids re-scanning SPIFFS on every capture.
  uint16_t nextNumber = spiffsImageCounter;
  char path[16];
  snprintf(path, sizeof(path), "/%04d.jpg", nextNumber);

  Serial.printf("SPIFFS auto-increment save: %s\n", path);

  File file = SPIFFS.open(path, FILE_WRITE);
  if (!file) {
    Serial.printf("SPIFFS open failed: %s\n", path);
    return false;
  }

  size_t written = file.write(fb->buf, fb->len);
  file.close();

  if (written != fb->len) {
    Serial.printf("SPIFFS write incomplete. Wrote %d of %d bytes\n", written, fb->len);
    return false;
  }

  Serial.printf("Saved to SPIFFS: %s (%d bytes)\n", path, written);

  spiffsImageCounter++;
  if (spiffsImageCounter > 9999) {
    spiffsImageCounter = 1;
  }
  return true;
}

// ============================================================================
// FAILED CAPTURE FALLBACK SAVE
// ============================================================================
// Handles captures that failed due to timeout, no filename, or invalid format.
// Generates automatic filename in /fail/ directory with format: /fail/XXXX.jpg

bool captureAndSaveFailure(camera_fb_t *fb) {
  // ---- Generate Automatic Filepath ----
  char failPath[64];
  snprintf(failPath, sizeof(failPath), "/fail/%04d.jpg", failCounter);

  Serial.printf("Saving to failure fallback: %s\n", failPath);

  // ---- Ensure /fail/ Directory Exists ----
  if (currentStorage == STORAGE_SD_CARD) {
    ensureDirectoryExists("/fail");
  }

  // ---- Attempt Save ----
  bool success = false;
  
  if (currentStorage == STORAGE_SD_CARD) {
    File file = SD_MMC.open(failPath, FILE_WRITE);
    if (file) {
      size_t written = file.write(fb->buf, fb->len);
      file.close();
      
      if (written == fb->len) {
        Serial.printf("Saved failure to SD card: %s (%d bytes)\n", failPath, written);
        success = true;
      }
    }
  } else if (currentStorage == STORAGE_SPIFFS) {
    File file = SPIFFS.open(failPath, FILE_WRITE);
    if (file) {
      size_t written = file.write(fb->buf, fb->len);
      file.close();
      
      if (written == fb->len) {
        Serial.printf("Saved failure to SPIFFS: %s (%d bytes)\n", failPath, written);
        success = true;
      }
    }
  }

  // ---- Increment Counter ----
  if (success) {
    failCounter++;
    if (failCounter > 9999) {
      failCounter = 0;
      Serial.println("Warn: Fail counter wrapped around to 0000");
    }
  }

  return success;
}

// ============================================================================
// CAMERA INITIALIZATION
// ============================================================================
// Configures all camera hardware settings and initializes the camera module.

void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 10;
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
  } else {
    Serial.println("Camera initialized successfully");
  }
}

// ============================================================================
// SERIAL COMMUNICATION FUNCTION
// ============================================================================
// Waits to receive a filepath from the remote Arduino over UART.
// Filepath can include directory structure (e.g., "/run01/12345.jpg")

String readFilepathFromSerial() {
  // ---- Initialize Empty String ----
  String filepath = "";

  // ---- Calculate Timeout Deadline ----
  unsigned long timeout = millis() + 5000;

  // ---- Wait for Filepath Data ----
  while (millis() < timeout) {
    if (Serial2.available()) {
      char c = Serial2.read();

      // ---- Check for End-of-Line Marker ----
      if (c == '\n' || c == '\r') {
        if (filepath.length() > 0) {
          return filepath;
        }
      } else {
        filepath += c;
      }
    }
  }

  // ---- Timeout Occurred ----
  return "";
}

// ============================================================================
// TERMINAL COMMAND READING
// ============================================================================
// Reads commands from Serial monitor (debug console).
// Only reads when data is available (non-blocking).

String readTerminalCommand() {
  String command = "";
  
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (command.length() > 0) {
        return command;
      }
    } else {
      command += c;
    }
  }

  return "";
}

// ============================================================================
// TERMINAL COMMANDS - LIST FILES
// ============================================================================
// Lists all files and directories on the storage device.
// Display format depends on filesystem type (FAT32 for SD, flat for SPIFFS).

void listFiles(const char* path = "/") {
  // ---- Check Storage Available ----
  if (currentStorage == STORAGE_NONE) {
    Serial.println("No storage available\n");
    return;
  }

  // ---- Print Storage Information ----
  if (currentStorage == STORAGE_SD_CARD) {
    Serial.println("Storage: SD Card (FAT32)");
    Serial.println("Filesystem: Hierarchical with directory tree\n");
  } else if (currentStorage == STORAGE_SPIFFS) {
    Serial.println("Storage: Internal Flash (SPIFFS)");
    Serial.println("Filesystem: Flat (simulated directory structure via full paths)\n");
  }

  Serial.printf("Contents of %s:\n", path);

  if (currentStorage == STORAGE_SD_CARD) {
    // ---- List SD Card Files (Hierarchical) ----
    listFilesSDCard(path, 0);

  } else if (currentStorage == STORAGE_SPIFFS) {
    // ---- List SPIFFS Files (Flat with full paths) ----
    listFilesSPIFFS();
  }

  Serial.println();
}

// ============================================================================
// SD CARD LISTING - HIERARCHICAL WITH RECURSION
// ============================================================================
// Lists files and directories on SD card with proper tree structure.
// Recursively lists subdirectories with indentation.

void listFilesSDCard(const char* path, int indent) {
  // ---- Create Indentation String ----
  String indentStr = "";
  for (int i = 0; i < indent; i++) {
    indentStr += "  ";
  }

  // ---- Attempt to Open Directory ----
  File dir = SD_MMC.open(path);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("%sDirectory not found: %s\n", indentStr.c_str(), path);
    return;
  }

  // ---- Open First File in Directory ----
  File file = dir.openNextFile();
  
  // ---- Check if Directory is Empty ----
  if (!file) {
    Serial.printf("%s(empty)\n", indentStr.c_str());
    dir.close();
    return;
  }

  // ---- Iterate Through All Files and Directories ----
  while (file) {
    String entryName = String(file.name());
    if (isHiddenName(file.name())) {
      file.close();
      file = dir.openNextFile();
      continue;
    }

    if (file.isDirectory()) {
      // ---- Display Directory Entry ----
      Serial.printf("%s[DIR] %s/\n", indentStr.c_str(), entryName.c_str());
      
      // ---- Build Full Path for Recursion ----
      char fullPath[128];
      if (strcmp(path, "/") == 0) {
        // If current path is root, just append filename
        snprintf(fullPath, sizeof(fullPath), "/%s", entryName.c_str());
      } else {
        // Otherwise append to current path
        snprintf(fullPath, sizeof(fullPath), "%s/%s", path, entryName.c_str());
      }

      // ---- Recursively List Subdirectory ----
      listFilesSDCard(fullPath, indent + 1);

    } else {
      // ---- Display File Entry ----
      Serial.printf("%s%s (%d bytes)\n", indentStr.c_str(), entryName.c_str(), file.size());
    }

    // ---- Move to Next File ----
    file.close();
    file = dir.openNextFile();
  }

  dir.close();
}

// ============================================================================
// SPIFFS LISTING - FLAT FILESYSTEM WITH FULL PATHS
// ============================================================================
// Lists all files in SPIFFS filesystem with organized directory grouping.
// Shows complete file paths since SPIFFS has no real directory structure.
// Files are grouped by their path prefix for better organization.

void listFilesSPIFFS() {
  // ---- Open Root Directory ----
  File root = SPIFFS.open("/");
  if (!root) {
    Serial.println("Cannot open root\n");
    return;
  }

  // ---- Collect All Files (heap allocation) ----
  struct FileEntry {
    char path[128];
    uint32_t size;
  };
  
  FileEntry* files = (FileEntry*)malloc(100 * sizeof(FileEntry));
  if (!files) {
    Serial.println("Memory allocation failed\n");
    root.close();
    return;
  }
  
  int fileCount = 0;

  File file = root.openNextFile();
  while (file && fileCount < 100) {
    String fullPath = file.name();
    if (isHiddenPath(fullPath)) {
      file.close();
      file = root.openNextFile();
      continue;
    }
    
    // ---- Store File Info ----
    strncpy(files[fileCount].path, fullPath.c_str(), sizeof(files[fileCount].path) - 1);
    files[fileCount].size = file.size();
    fileCount++;
    
    file.close();
    file = root.openNextFile();
  }
  root.close();

  // ---- Check if Filesystem is Empty ----
  if (fileCount == 0) {
    Serial.println("(empty)\n");
    free(files);
    return;
  }

  // ---- Simple Bubble Sort (rest of code stays the same) ----
  for (int i = 0; i < fileCount - 1; i++) {
    for (int j = 0; j < fileCount - i - 1; j++) {
      if (strcmp(files[j].path, files[j + 1].path) > 0) {
        FileEntry temp = files[j];
        files[j] = files[j + 1];
        files[j + 1] = temp;
      }
    }
  }

  // ---- Display Files (rest of code stays the same) ----
  String lastDir = "";
  for (int i = 0; i < fileCount; i++) {
    String filepath = files[i].path;
    int lastSlash = filepath.lastIndexOf('/');
    String dir = filepath.substring(0, lastSlash + 1);

    if (dir != lastDir) {
      if (lastDir.length() > 0) {
        Serial.println();
      }
      Serial.printf("[DIR] %s\n", dir.c_str());
      lastDir = dir;
    }

    String filename = filepath.substring(lastSlash + 1);
    Serial.printf("  %s (%d bytes)\n", filename.c_str(), files[i].size);
  }

  Serial.println();
  free(files);  // ---- Clean up heap allocation ----
}


// ============================================================================
// TERMINAL COMMANDS - DELETE FILE
// ============================================================================
// Deletes a specific file from storage.

void deleteFile(const char* filepath) {
  // ---- Check Storage Available ----
  if (currentStorage == STORAGE_NONE) {
    Serial.println("No storage available\n");
    return;
  }

  bool success = false;
  if (currentStorage == STORAGE_SD_CARD) {
    success = SD_MMC.remove(filepath);
  } else if (currentStorage == STORAGE_SPIFFS) {
    success = SPIFFS.remove(filepath);
  }

  if (success) {
    Serial.printf("Deleted: %s\n\n", filepath);
  } else {
    Serial.printf("Delete failed: %s\n\n", filepath);
  }
}

// ============================================================================
// TERMINAL COMMANDS - FORMAT STORAGE
// ============================================================================
// Formats the storage device with user confirmation.
// WARNING: This erases all data!

void formatStorage() {
  // ---- Check Storage Available ----
  if (currentStorage == STORAGE_NONE) {
    Serial.println("No storage available\n");
    return;
  }

  // ---- Ask for Confirmation ----
  Serial.print("WARNING: Format will erase ALL data! Continue? (y/n): ");
  unsigned long timeout = millis() + 5000;

  while (millis() < timeout) {
    if (Serial.available()) {
      char c = Serial.read();
      Serial.println(c);
      
      if (c == 'y' || c == 'Y') {
        // ---- Perform Format ----
        Serial.println("Formatting...");
        
        if (currentStorage == STORAGE_SD_CARD) {
          std::vector<String> directories;
          std::vector<String> files;

          std::function<void(const char*)> scan = [&](const char* dirPath) {
            File dir = SD_MMC.open(dirPath);
            if (!dir || !dir.isDirectory()) {
              return;
            }

            File entry = dir.openNextFile();
            while (entry) {
              String fullPath = (strcmp(dirPath, "/") == 0)
                                  ? String("/") + entry.name()
                                  : String(dirPath) + "/" + entry.name();

              if (entry.isDirectory()) {
                directories.push_back(fullPath);
                entry.close();
                scan(fullPath.c_str());
              } else {
                files.push_back(fullPath);
                entry.close();
              }
              entry = dir.openNextFile();
            }
            dir.close();
          };

          scan("/");

          for (const String& p : files) {
            SD_MMC.remove(p);
          }
          for (int i = directories.size() - 1; i >= 0; --i) {
            SD_MMC.rmdir(directories[i]);
          }
          Serial.println("SD card contents erased recursively\n");
        } else if (currentStorage == STORAGE_SPIFFS) {
          // ---- Format SPIFFS ----
          SPIFFS.format();
          Serial.println("SPIFFS formatted\n");
        }
        
        // ---- Reinitialize Fail Counter ----
        failCounter = 1;
        Serial.println("Fail counter reset to 0001\n");
        return;
      } else if (c == 'n' || c == 'N') {
        Serial.println("Cancelled\n");
        return;
      }
    }
  }

  Serial.println("Timeout\n");
}

// ============================================================================
// TERMINAL COMMANDS - SIMULATE CAPTURE
// ============================================================================
// Simulates a trigger event without filename provided.
// Captures image and saves to /fail/XXXX.jpg fallback.

void simulateCapture() {
  Serial.println("Simulating trigger (no filename)...");

  // Ignore any GPIO4 transition side-effects caused by camera/flash activity
  // during this local simulation path.
  ignoreTriggerUntilMs = millis() + 1000;
  captureFlag = false;
  
  // ---- Capture Image ----
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed\n");
    return;
  }

  // ---- Save to Failure Fallback ----
  if (captureAndSaveFailure(fb)) {
    Serial.println("Simulated capture successful\n");
  } else {
    Serial.println("Simulated capture failed\n");
  }
  
  esp_camera_fb_return(fb);
}

// ============================================================================
// WEB SERVER - FILE COLLECTION HELPERS
// ============================================================================

// Recursively collects every file path from an SD card directory into a vector.
// entry.name() returns only the basename on ESP32 SD, so the parent path is
// prepended here — the same convention used by listFilesSDCard().
void collectFilesRecursive(const char* dirPath, std::vector<String>& files) {
  Serial.printf("[WebServer] Scanning SD directory: %s\n", dirPath);

  File dir = SD_MMC.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("[WebServer] Cannot open directory: %s\n", dirPath);
    return;
  }

  File entry = dir.openNextFile();
  while (entry) {
    // Build the full absolute path from the parent path and the basename
    String fullPath;
    if (strcmp(dirPath, "/") == 0) {
      fullPath = String("/") + entry.name();
    } else {
      fullPath = String(dirPath) + "/" + entry.name();
    }

    if (isHiddenPath(fullPath)) {
      entry.close();
      entry = dir.openNextFile();
      continue;
    }

    if (entry.isDirectory()) {
      Serial.printf("[WebServer]   Entering directory: %s\n", fullPath.c_str());
      // Close the directory entry BEFORE recursing: the ESP32 SD library has a
      // limited number of open file handles. Releasing this handle first prevents
      // exhaustion in deep directory trees — the recursive call opens the
      // subdirectory independently via SD_MMC.open(fullPath).
      entry.close();
      collectFilesRecursive(fullPath.c_str(), files);
    } else {
      Serial.printf("[WebServer]   Found file: %s\n", fullPath.c_str());
      files.push_back(fullPath);
      entry.close();
    }

    entry = dir.openNextFile();
  }

  dir.close();
}

// Collects every file path from SPIFFS.
// SPIFFS entry.name() already returns the full absolute path (e.g. "/photo.jpg").
void collectFilesSPIFFSFlat(std::vector<String>& files) {
  Serial.println("[WebServer] Scanning SPIFFS filesystem...");

  File root = SPIFFS.open("/");
  if (!root) {
    Serial.println("[WebServer] Cannot open SPIFFS root");
    return;
  }

  File entry = root.openNextFile();
  while (entry) {
    String fullPath = String(entry.name());
    if (!entry.isDirectory() && !isHiddenPath(fullPath)) {
      Serial.printf("[WebServer]   Found file: %s\n", fullPath.c_str());
      files.push_back(fullPath);
    }
    // Close before advancing — required to free the file handle
    entry.close();
    entry = root.openNextFile();
  }

  root.close();
}

// ============================================================================
// WEB SERVER - ROUTE HANDLERS
// ============================================================================

// Serves a plain, unformatted HTML page that lists every file on storage
// and provides a link to the live snapshot page.
void handleHomePage() {
  logs.print("[WebServer] Home page requested — collecting file list...\n", true);

  // Collect all file paths from whichever storage backend is active
  std::vector<String> files;
  if (currentStorage == STORAGE_SD_CARD) {
    collectFilesRecursive("/", files);
  } else if (currentStorage == STORAGE_SPIFFS) {
    collectFilesSPIFFSFlat(files);
  }

  logs.printf(true, "[WebServer] Sending file list: %u file(s)\n", (unsigned)files.size());

  // Build a minimal HTML page — no CSS, no JavaScript, no formatting
  String html = "<!DOCTYPE html><html><body>\n";
  html += "<p>Files on storage (" + String(files.size()) + "):</p>\n";
  for (const String& path : files) {
    html += "<a href=\"" + path + "\">" + path + "</a><br>\n";
  }
  html += "<p><a href=\"/snapshot\">/snapshot</a> - live camera image</p>\n";
  html += "</body></html>\n";

  server.send(200, "text/html", html);
}

// Serves the fixed-size in-memory log buffer as plain text.
void handleLogs() {
  server.send(200, "text/plain", logs.snapshot());
}

// Captures a live frame from the camera and streams it to the browser as JPEG.
// The camera DMA buffer is written directly to the TCP socket and released
// immediately — no file is written and no extra heap copy is made.
void handleSnapshot() {
  logs.print("[WebServer] Snapshot requested — capturing frame...\n", true);

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    logs.print("[WebServer] Snapshot: camera capture failed\n", true);
    server.send(503, "text/plain", "Camera capture failed");
    return;
  }

  logs.printf(true, "[WebServer] Snapshot: captured %u bytes\n", (unsigned)fb->len);

  // Send headers first, then body as a single sendContent chunk.
  // sendContent() appends body bytes to the already-open HTTP response
  // without starting a new response — this is the correct ESP32 WebServer
  // pattern for streaming pre-sized binary data.
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma",        "no-cache");
  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");          // flush headers, empty body
  server.sendContent((const char*)fb->buf, fb->len);  // stream body bytes

  // MUST be called after every esp_camera_fb_get() to return the DMA slot
  esp_camera_fb_return(fb);

  logs.print("[WebServer] Snapshot: response sent and frame buffer released\n", true);
}

void handleFileFetch() {
  String path = server.uri();
  int q = path.indexOf('?');
  if (q >= 0) {
    path = path.substring(0, q);
  }
  path = server.urlDecode(path);
  if (!path.startsWith("/")) {
    path = "/" + path;
  }
  if (path.length() == 0) {
    server.send(400, "text/plain", "Invalid path");
    return;
  }

  if (isHiddenPath(path)) {
    server.send(404, "text/plain", "Not found");
    return;
  }

  if (path == "/" || path == "/snapshot") {
    server.send(404, "text/plain", "Not found");
    return;
  }

  fs::FS* fs = nullptr;
  if (currentStorage == STORAGE_SD_CARD) {
    fs = &SD_MMC;
  } else if (currentStorage == STORAGE_SPIFFS) {
    fs = &SPIFFS;
  } else {
    server.send(503, "text/plain", "No storage available");
    return;
  }

  if (!fs->exists(path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  File file = fs->open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    server.send(404, "text/plain", "File not found");
    if (file) {
      file.close();
    }
    return;
  }

  String contentType = getContentType(path);
  server.streamFile(file, contentType);
  file.close();
}

// ============================================================================
// WEB SERVER - SERVER MANAGEMENT
// ============================================================================

// Starts the WiFi access point and HTTP server, registering all routes.
// Nothing in this function runs until the user types 'start'.
void startWebServer() {
  if (webServerRunning) {
    Serial.println("Web server is already running.\n");
    return;
  }

  // Start the access point
  Serial.println("[WebServer] Starting WiFi Access Point...");
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[WebServer] AP started  SSID: %s  Password: %s  IP: %s\n",
                AP_SSID, AP_PASSWORD, ip.toString().c_str());

  // Register routes — only reachable after start()
  server.on("/",         HTTP_GET, handleHomePage);
  server.on("/snapshot", HTTP_GET, handleSnapshot);
  server.on("/logs",     HTTP_GET, handleLogs);
  server.onNotFound(handleFileFetch);
  Serial.println("[WebServer] Routes registered: /  /snapshot  /logs");

  server.begin();
  webServerRunning = true;

  Serial.println("[WebServer] HTTP server listening on port 80");
  Serial.printf("[WebServer] File list:     http://%s/\n",         ip.toString().c_str());
  Serial.printf("[WebServer] Live snapshot: http://%s/snapshot\n", ip.toString().c_str());
  Serial.printf("[WebServer] Logs:          http://%s/logs\n\n", ip.toString().c_str());
}

// Stops the HTTP server and shuts down the WiFi access point.
void stopWebServer() {
  if (!webServerRunning) {
    Serial.println("Web server is not running.\n");
    return;
  }

  server.stop();
  WiFi.softAPdisconnect(true);
  webServerRunning = false;
  Serial.println("Web server stopped.\n");
}

// ============================================================================
// TERMINAL COMMANDS - PROCESS COMMAND
// ============================================================================
// Parses and executes terminal commands from user input.

void processTerminalCommand(String command) {
  // ---- Normalize Command ----
  command.trim();
  command.toLowerCase();

  // ---- Echo Command ----
  Serial.println(command);
  Serial.println();

  // ---- Parse and Execute ----
  if (command == "ls") {
    // ---- List All Files ----
    listFiles("/");
  } else if (command.startsWith("ls ")) {
    // ---- List Specific Path ----
    String path = command.substring(3);
    listFiles(path.c_str());
  } else if (command == "fmt") {
    // ---- Format Storage ----
    formatStorage();
  } else if (command.startsWith("del ")) {
    // ---- Delete File ----
    String filepath = command.substring(4);
    deleteFile(filepath.c_str());
  } else if (command == "capture") {
    // ---- Simulate Capture ----
    simulateCapture();
  } else if (command == "start") {
    // ---- Start Web Server ----
    startWebServer();
  } else if (command == "stop") {
    // ---- Stop Web Server ----
    stopWebServer();
  } else if (command == "help") {
    // ---- Show Help ----
    Serial.println("Available Commands:");
    Serial.println("  ls [path]     - List all files and directories");
    Serial.println("  del <path>    - Delete a specific file");
    Serial.println("  fmt           - Format storage (with confirmation)");
    Serial.println("  capture       - Simulate trigger capture");
    Serial.println("  start         - Start WiFi AP and web file browser");
    Serial.println("  stop          - Stop web file browser and WiFi AP");
    Serial.println("  help          - Show this help message");
    Serial.println();
  } else if (command.length() > 0) {
    // ---- Unknown Command ----
    Serial.println("Unknown command. Type 'help' for available commands.\n");
  }
}

// ============================================================================
// SETUP FUNCTION
// ============================================================================
// Called once at startup. Initializes all hardware and systems.

void setup() {
  // ---- Initialize Debug Serial Monitor ----
  Serial.begin(115200);
  logs.begin();
  delay(1000);

  Serial.println("\n\nESP32-CAM Remote Capture System with SD Card");
  Serial.println("=============================================\n");

  // ---- Initialize UART Communication ----
  Serial2.begin(9600, SERIAL_8N1, UART_RX, UART_TX);
  Serial.println("UART initialized at 9600 baud");
  Serial.printf("  RX pin: GPIO %d\n", UART_RX);
  Serial.printf("  TX pin: GPIO %d\n", UART_TX);

  // ---- Setup Trigger Input Pin ----
  // Keep trigger stable/high when the remote controller is disconnected.
  pinMode(TRIGGER_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TRIGGER_PIN), triggerISR, FALLING);
  Serial.printf("Trigger pin ready (GPIO %d, active LOW)\n\n", TRIGGER_PIN);

  // ---- Initialize Storage Systems ----
  Serial.println("--- Storage Initialization ---");
  
  if (!initSDCard()) {
    Serial.println("SD card unavailable, initializing SPIFFS fallback...");
    if (!initSPIFFS()) {
      Serial.println("ERROR: No storage available!");
    }
  }

  // ---- Initialize SPIFFS Auto-Increment Counter ----
  if (currentStorage == STORAGE_SPIFFS) {
    spiffsImageCounter = getNextSPIFFSImageNumber();
    Serial.printf("SPIFFS image counter initialized to: %04d\n", spiffsImageCounter);
  }

  // ---- Initialize Fail Counter ----
  Serial.println("\n--- Initializing Failure Fallback System ---");
  initializeFailCounter();

  // ---- Initialize Camera Hardware ----
  Serial.println("\n--- Camera Initialization ---");
  initCamera();

  // ---- Print Ready Message ----
  Serial.println("\n--- System Ready ---");
  Serial.println("Waiting for trigger from Arduino...");
  Serial.println("Arduino should:");
  Serial.printf("  1. Pull GPIO %d to LOW\n", TRIGGER_PIN);
  Serial.println("  2. Send filepath over UART (e.g., \"/run01/12345.jpg\")\n");
  Serial.println("Type 'help' for terminal commands\n");
  Serial.print("> ");
}

// ============================================================================
// MAIN LOOP
// ============================================================================
// Runs repeatedly. Continuously checks for capture requests and terminal input.

void loop() {
  // ---- Check Terminal Commands ----
  // Only reads when data is available (non-blocking)
  String termCommand = readTerminalCommand();
  if (termCommand.length() > 0) {
    processTerminalCommand(termCommand);
    Serial.print("> ");
  }

  // ---- Handle Incoming Web Requests ----
  // Checked every loop iteration but only active after 'start' is typed.
  // When the server is not running this branch costs a single boolean test.
  if (webServerRunning) {
    server.handleClient();
  }

  // ---- Check Trigger from Arduino ----
  if (captureFlag) {
    if (millis() < ignoreTriggerUntilMs) {
      captureFlag = false;
      return;
    }

    // ---- Clear Trigger Flag ----
    captureFlag = false;

    Serial.println("\n[Trigger detected from Arduino]");
    Serial2.println("READY");

    // ---- Receive Filepath ----
    String filepath = readFilepathFromSerial();

    // ---- Validate Filepath ----
    if (filepath.length() == 0) {
      // No filepath received - timeout occurred
      Serial.println("ERROR: No filepath received (timeout)");
      Serial.println("Saving to failure fallback location instead");
      Serial2.println("ERR:NO_FILEPATH");
      
      // ---- Attempt Capture to Failure Fallback ----
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        if (captureAndSaveFailure(fb)) {
          Serial.println("Failure capture successful");
          Serial2.println("OK");
        } else {
          Serial.println("Failure capture failed");
          Serial2.println("ERR:CAPTURE_FAILED");
        }
        esp_camera_fb_return(fb);
      } else {
        Serial.println("Camera capture failed");
        Serial2.println("ERR:CAPTURE_FAILED");
      }
    } else {
      // ---- Log Received Filepath ----
      Serial.printf("Received filepath: %s\n", filepath.c_str());

      // ---- Attempt Photo Capture and Save ----
      if (captureAndSave(filepath.c_str())) {
        Serial.println("Capture successful");
        Serial2.println("OK");
      } else {
        Serial.println("Capture failed");
        Serial2.println("ERR:CAPTURE_FAILED");
      }
    }

    Serial.print("\n> ");
  }

  // ---- Prevent Tight Loop ----
  delay(10);
}