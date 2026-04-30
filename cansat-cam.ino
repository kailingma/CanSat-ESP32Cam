#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <SD.h>
#include <FS.h>
#include <vector>
#include <algorithm>

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

#define TRIGGER_PIN       13    // Input pin: Arduino pulls LOW to request photo capture
#define UART_RX           14    // Serial2 RX: receives filename from Arduino
#define UART_TX           15    // Serial2 TX: sends status messages back to Arduino

// ============================================================================
// SD CARD PIN DEFINITIONS
// ============================================================================
// These pins connect to the SD card module via SPI.

#define SD_CS             5     // Chip select pin for SD card SPI

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

// Tracks which storage device is currently in use
StorageMode currentStorage = STORAGE_NONE;

// Counter for failed captures (no filename, timeout, or invalid format)
uint16_t failCounter = 0;

// ============================================================================
// WEB SERVER GLOBAL STATE
// ============================================================================

// HTTP server instance on port 80
WebServer server(80);

// Tracks whether the web server is currently active
bool webServerRunning = false;

// WiFi AP credentials
static const char* AP_SSID     = "ESP32-CAM-Browser";
static const char* AP_PASSWORD = "12345678";

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
  // SD.begin() initializes SPI communication with the SD card
  if (!SD.begin(SD_CS)) {
    Serial.println("SD card initialization failed");
    return false;
  }

  // ---- Verify SD Card is Readable ----
  uint8_t cardType = SD.cardType();

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
  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
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
    File failDir = SD.open("/fail");
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
    File dir = SD.open(path);
    
    if (dir && dir.isDirectory()) {
      dir.close();
      return true;
    }
    
    if (dir) {
      dir.close();
    }

    // ---- Create Directory on SD Card ----
    if (SD.mkdir(path)) {
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

  // ---- Validate Filepath Format ----
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
    File file = SD.open(filepath, FILE_WRITE);
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
    File file = SD.open(failPath, FILE_WRITE);
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
  File dir = SD.open(path);
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
    if (file.isDirectory()) {
      // ---- Display Directory Entry ----
      Serial.printf("%s[DIR] %s/\n", indentStr.c_str(), file.name());
      
      // ---- Build Full Path for Recursion ----
      char fullPath[128];
      if (strcmp(path, "/") == 0) {
        // If current path is root, just append filename
        snprintf(fullPath, sizeof(fullPath), "/%s", file.name());
      } else {
        // Otherwise append to current path
        snprintf(fullPath, sizeof(fullPath), "%s/%s", path, file.name());
      }

      // ---- Recursively List Subdirectory ----
      listFilesSDCard(fullPath, indent + 1);

    } else {
      // ---- Display File Entry ----
      Serial.printf("%s%s (%d bytes)\n", indentStr.c_str(), file.name(), file.size());
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
    success = SD.remove(filepath);
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
          // ---- Format SD Card ----
          // Note: SD library doesn't have a direct format function
          // We'll erase all files instead
          File root = SD.open("/");
          if (root) {
            File file = root.openNextFile();
            while (file) {
              if (!file.isDirectory()) {
                SD.remove(file.name());
              }
              file.close();
              file = root.openNextFile();
            }
            root.close();
          }
          Serial.println("SD card contents cleared\n");
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
// WEB SERVER - FILE METADATA
// ============================================================================

// Holds the full path and byte-size of a single file on storage.
// Populated during directory traversal so each file is opened only once.
struct FileInfo {
  String path;   // Absolute path from root (e.g. "/run01/photo.jpg")
  size_t size;   // File size in bytes
};

// ============================================================================
// WEB SERVER - FILE COLLECTION HELPERS
// ============================================================================

// Recursively walks an SD card directory tree and appends every regular file
// (with its size) to 'files'.  Each file is opened exactly once so the size
// is read during the same traversal — no second open is needed later.
void collectFilesRecursive(const char* dirPath, std::vector<FileInfo>& files) {
  Serial.printf("[WebServer] Scanning SD directory: %s\n", dirPath);

  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("[WebServer] Cannot open directory: %s\n", dirPath);
    return;
  }

  File entry = dir.openNextFile();
  while (entry) {
    // ---- Build Absolute Path ----
    // entry.name() returns only the basename on ESP32 SD, so we must
    // prepend the parent path ourselves (same logic as listFilesSDCard).
    String fullPath;
    if (strcmp(dirPath, "/") == 0) {
      fullPath = String("/") + entry.name();
    } else {
      fullPath = String(dirPath) + "/" + entry.name();
    }

    if (entry.isDirectory()) {
      // ---- Recurse into Subdirectory ----
      Serial.printf("[WebServer]   Entering directory: %s\n", fullPath.c_str());
      entry.close();
      collectFilesRecursive(fullPath.c_str(), files);
    } else {
      // ---- Record File (size read now — no second open later) ----
      FileInfo info;
      info.path = fullPath;
      info.size = entry.size();
      Serial.printf("[WebServer]   Found file: %s  (%u bytes)\n",
                    info.path.c_str(), (unsigned)info.size);
      files.push_back(info);
      entry.close();
    }

    entry = dir.openNextFile();
  }

  dir.close();
}

// Collects every file in SPIFFS (flat filesystem) into 'files'.
// SPIFFS's entry.name() already returns the full path (e.g. "/photo.jpg"),
// and size is read here so no second open is needed in the JSON builder.
void collectFilesSPIFFSFlat(std::vector<FileInfo>& files) {
  Serial.println("[WebServer] Scanning SPIFFS filesystem...");

  File root = SPIFFS.open("/");
  if (!root) {
    Serial.println("[WebServer] Cannot open SPIFFS root");
    return;
  }

  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      FileInfo info;
      info.path = String(entry.name());   // Already absolute (e.g. "/data.txt")
      info.size = entry.size();
      Serial.printf("[WebServer]   Found file: %s  (%u bytes)\n",
                    info.path.c_str(), (unsigned)info.size);
      files.push_back(info);
    }
    entry = root.openNextFile();
  }

  root.close();
}

// Builds a JSON array describing all files on the active storage backend.
// Uses FileInfo structs so every file is opened at most once during collection.
// Format: [{"name":"photo.jpg","path":"/run01/photo.jpg","size":45678}, ...]
String generateFileListJSON() {
  std::vector<FileInfo> files;

  // ---- Collect File Metadata (one pass, one open per file) ----
  if (currentStorage == STORAGE_SD_CARD) {
    collectFilesRecursive("/", files);
  } else if (currentStorage == STORAGE_SPIFFS) {
    collectFilesSPIFFSFlat(files);
  }

  Serial.printf("[WebServer] Total files found: %u\n", (unsigned)files.size());

  // ---- Serialize to JSON ----
  String json = "[";
  for (size_t i = 0; i < files.size(); i++) {
    if (i > 0) json += ",";

    const String& path = files[i].path;

    // Derive display name from the last path component
    String name = path;
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash >= 0) name = path.substring(lastSlash + 1);

    json += "{\"name\":\"" + name
          + "\",\"path\":\"" + path
          + "\",\"size\":"  + String(files[i].size)
          + "}";
  }
  json += "]";

  return json;
}

// ============================================================================
// WEB SERVER - ROUTE HANDLERS
// ============================================================================

// Returns the MIME type string for a given file path based on its extension.
String getMimeType(const String& path) {
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".gif"))  return "image/gif";
  if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".txt"))  return "text/plain";
  if (path.endsWith(".csv"))  return "text/plain";
  return "application/octet-stream";
}

// Serves the single-page file browser UI.
// The entire page is a single self-contained HTML document with embedded
// CSS and JavaScript so no external resources are required.
void handleHomePage() {
  Serial.println("[WebServer] Serving home page to client");

  String html = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP32-CAM File Browser</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: 'Segoe UI', Arial, sans-serif; background: #f0f2f5; }
  header {
    background: linear-gradient(135deg, #1a237e, #283593);
    color: #fff;
    padding: 20px 24px;
    display: flex;
    align-items: center;
    gap: 12px;
  }
  header h1 { font-size: 1.4rem; font-weight: 600; }
  header span { font-size: 2rem; }
  #status { font-size: 0.8rem; opacity: 0.8; margin-top: 4px; }
  main { max-width: 900px; margin: 24px auto; padding: 0 16px; }
  #file-list { background: #fff; border-radius: 10px; box-shadow: 0 2px 8px rgba(0,0,0,.12); overflow: hidden; }
  .file-item {
    display: flex;
    align-items: center;
    padding: 14px 20px;
    border-bottom: 1px solid #f0f0f0;
    gap: 12px;
    transition: background .15s;
  }
  .file-item:last-child { border-bottom: none; }
  .file-item:hover { background: #f5f7ff; }
  .file-icon { font-size: 1.6rem; min-width: 32px; text-align: center; }
  .file-info { flex: 1; min-width: 0; }
  .file-name { font-weight: 500; font-size: 0.95rem; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .file-meta { font-size: 0.78rem; color: #888; margin-top: 2px; }
  .actions { display: flex; gap: 8px; flex-shrink: 0; }
  .btn {
    padding: 6px 14px;
    border: none;
    border-radius: 6px;
    cursor: pointer;
    font-size: 0.82rem;
    font-weight: 500;
    transition: opacity .15s;
  }
  .btn:hover { opacity: 0.85; }
  .btn-preview { background: #e3f2fd; color: #1565c0; }
  .btn-download { background: #e8f5e9; color: #2e7d32; }
  #preview-overlay {
    display: none;
    position: fixed;
    inset: 0;
    background: rgba(0,0,0,.7);
    z-index: 100;
    align-items: center;
    justify-content: center;
  }
  #preview-overlay.active { display: flex; }
  #preview-box {
    background: #fff;
    border-radius: 12px;
    max-width: 90vw;
    max-height: 90vh;
    overflow: auto;
    padding: 20px;
    position: relative;
  }
  #preview-box img { max-width: 100%; display: block; }
  #preview-box pre { white-space: pre-wrap; word-break: break-all; font-size: 0.85rem; }
  #close-preview {
    position: absolute;
    top: 10px;
    right: 14px;
    background: none;
    border: none;
    font-size: 1.5rem;
    cursor: pointer;
    color: #555;
  }
  #empty { text-align: center; padding: 40px; color: #aaa; font-size: 1rem; }
</style>
</head>
<body>
<header>
  <span>&#128247;</span>
  <div>
    <h1>ESP32-CAM File Browser</h1>
    <div id="status">Loading files&hellip;</div>
  </div>
</header>
<main>
  <div id="file-list"><div id="empty">Loading&hellip;</div></div>
</main>
<div id="preview-overlay">
  <div id="preview-box">
    <button id="close-preview" title="Close">&times;</button>
    <div id="preview-content"></div>
  </div>
</div>
<script>
  const imageExts = ['jpg','jpeg','png','gif'];
  const textExts  = ['txt','csv','json','log','md','html','htm','css','js'];

  function ext(name) { return name.split('.').pop().toLowerCase(); }

  function fileIcon(name) {
    const e = ext(name);
    if (imageExts.includes(e)) return '&#128444;';
    if (textExts.includes(e))  return '&#128196;';
    return '&#128190;';
  }

  function fmtSize(b) {
    if (b < 1024) return b + ' B';
    if (b < 1048576) return (b/1024).toFixed(1) + ' KB';
    return (b/1048576).toFixed(2) + ' MB';
  }

  function canPreview(name) {
    const e = ext(name);
    return imageExts.includes(e) || textExts.includes(e);
  }

  function previewFile(path, name) {
    const e = ext(name);
    const overlay = document.getElementById('preview-overlay');
    const content = document.getElementById('preview-content');
    content.innerHTML = '';
    if (imageExts.includes(e)) {
      const img = document.createElement('img');
      img.src = '/download?path=' + encodeURIComponent(path);
      content.appendChild(img);
    } else {
      fetch('/download?path=' + encodeURIComponent(path))
        .then(r => r.text())
        .then(t => {
          const pre = document.createElement('pre');
          pre.textContent = t;
          content.appendChild(pre);
        });
    }
    overlay.classList.add('active');
  }

  document.getElementById('close-preview').addEventListener('click', () => {
    document.getElementById('preview-overlay').classList.remove('active');
  });
  document.getElementById('preview-overlay').addEventListener('click', function(e) {
    if (e.target === this) this.classList.remove('active');
  });

  function renderFiles(files) {
    const list = document.getElementById('file-list');
    const status = document.getElementById('status');
    status.textContent = files.length + ' file(s) on storage';
    if (files.length === 0) {
      list.innerHTML = '<div id="empty">No files found on storage.</div>';
      return;
    }
    list.innerHTML = '';
    files.forEach(f => {
      const div = document.createElement('div');
      div.className = 'file-item';
      const actions = canPreview(f.name)
        ? `<button class="btn btn-preview" onclick="previewFile('${f.path.replace(/'/g,"\\'")}','${f.name.replace(/'/g,"\\'")}')">Preview</button>`
        : '';
      div.innerHTML = `
        <div class="file-icon">${fileIcon(f.name)}</div>
        <div class="file-info">
          <div class="file-name" title="${f.path}">${f.name}</div>
          <div class="file-meta">${f.path} &bull; ${fmtSize(f.size)}</div>
        </div>
        <div class="actions">
          ${actions}
          <a href="/download?path=${encodeURIComponent(f.path)}" download="${f.name}">
            <button class="btn btn-download">Download</button>
          </a>
        </div>`;
      list.appendChild(div);
    });
  }

  function loadFiles() {
    fetch('/api/files')
      .then(r => r.json())
      .then(renderFiles)
      .catch(() => {
        document.getElementById('status').textContent = 'Failed to load files';
      });
  }

  loadFiles();
  setInterval(loadFiles, 5000);
</script>
</body>
</html>)rawhtml";

  server.send(200, "text/html", html);
}

// Serves the file list as a JSON array.
void handleFileListAPI() {
  String json = generateFileListJSON();
  server.send(200, "application/json", json);
}

// Streams a requested file to the client in 4 KB chunks.
// Validates the path to prevent directory traversal attacks.
void handleFileDownload() {
  if (!server.hasArg("path")) {
    server.send(400, "text/plain", "Missing path parameter");
    return;
  }

  String path = server.arg("path");

  // ---- Prevent Directory Traversal ----
  if (path.indexOf("..") >= 0) {
    server.send(403, "text/plain", "Forbidden");
    return;
  }

  // ---- Ensure Leading Slash ----
  if (!path.startsWith("/")) path = "/" + path;

  // ---- Open File ----
  File file;
  if (currentStorage == STORAGE_SD_CARD) {
    file = SD.open(path.c_str());
  } else if (currentStorage == STORAGE_SPIFFS) {
    file = SPIFFS.open(path.c_str(), "r");
  }

  if (!file || file.isDirectory()) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  // ---- Send Headers ----
  String mime = getMimeType(path);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + path.substring(path.lastIndexOf('/') + 1) + "\"");
  server.setContentLength(file.size());
  server.send(200, mime, "");

  // ---- Stream File in 4 KB Chunks ----
  static uint8_t buf[4096];
  WiFiClient client = server.client();
  while (file.available() && client.connected()) {
    size_t n = file.read(buf, sizeof(buf));
    client.write(buf, n);
  }
  file.close();
}

// ============================================================================
// WEB SERVER - SERVER MANAGEMENT
// ============================================================================

// Starts the WiFi access point and HTTP server, registering all routes.
void startWebServer() {
  if (webServerRunning) {
    Serial.println("Web server is already running.\n");
    return;
  }

  // ---- Start Access Point ----
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("WiFi AP started  SSID: %s  Password: %s\n", AP_SSID, AP_PASSWORD);
  Serial.printf("Browse files at: http://%s/\n\n", ip.toString().c_str());

  // ---- Register Routes ----
  server.on("/",          HTTP_GET, handleHomePage);
  server.on("/api/files", HTTP_GET, handleFileListAPI);
  server.on("/download",  HTTP_GET, handleFileDownload);

  // ---- Start HTTP Server ----
  server.begin();
  webServerRunning = true;
  Serial.println("HTTP server started on port 80.\n");
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
  delay(1000);

  Serial.println("\n\nESP32-CAM Remote Capture System with SD Card");
  Serial.println("=============================================\n");

  // ---- Initialize UART Communication ----
  Serial2.begin(9600, SERIAL_8N1, UART_RX, UART_TX);
  Serial.println("UART initialized at 9600 baud");
  Serial.printf("  RX pin: GPIO %d\n", UART_RX);
  Serial.printf("  TX pin: GPIO %d\n", UART_TX);

  // ---- Setup Trigger Input Pin ----
  pinMode(TRIGGER_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(TRIGGER_PIN), triggerISR, FALLING);
  Serial.println("Trigger pin ready (GPIO 13, active LOW)\n");

  // ---- Initialize Storage Systems ----
  Serial.println("--- Storage Initialization ---");
  
  if (!initSDCard()) {
    Serial.println("SD card unavailable, initializing SPIFFS fallback...");
    if (!initSPIFFS()) {
      Serial.println("ERROR: No storage available!");
    }
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
  Serial.println("  1. Pull GPIO 13 to LOW");
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

  // ---- Check Trigger from Arduino ----
  if (captureFlag) {
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
