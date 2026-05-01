# ESP32-CAM Remote Photo Capture System
## Implementation Specification

---

## 1. SYSTEM OVERVIEW

### 1.1 Purpose
A networked photo capture system where an Arduino acts as a controller and decision-maker, requesting photos from an ESP32-CAM module. The ESP32-CAM handles hardware interfacing, image capture, and file storage. The Arduino handles experiment logic, run management, file naming, and data processing.

### 1.2 Architecture

```
Arduino Controller
    |
    ├─ GPIO 13 (Trigger Signal)
    |       └─> ESP32-CAM GPIO 13
    |
    └─ Serial UART (Filepath + Status)
            ├─ TX → ESP32-CAM RX (GPIO 14)
            └─ RX ← ESP32-CAM TX (GPIO 15)

ESP32-CAM Module
    |
    ├─ Camera Hardware (OV2640 sensor)
    |
    ├─ Storage
    |   ├─ SD Card (Primary)
    |   └─ Internal SPIFFS (Fallback)
    |
    └─ Communication (UART Serial2)
```

---

## 2. ESP32-CAM COMMUNICATION BEHAVIOR

This section describes what the Arduino observes over the wire — the trigger mechanism, response messages, and error conditions. Internal ESP32-CAM implementation details (storage, camera hardware, boot sequence) are documented in the firmware README.

### 2.1 Trigger Mechanism

The Arduino initiates a capture by pulling **GPIO 13** LOW for ~100 ms and then releasing it. The ESP32-CAM detects the falling edge and immediately begins the capture sequence.

```
Arduino: pull GPIO 13 LOW  (hold ~100ms)
Arduino: release GPIO 13
ESP32-CAM: detects edge → begins capture sequence
```

### 2.2 Response Messages

After detecting the trigger, the ESP32-CAM sends one of the following messages over Serial2 (terminated with `\n`):

| Message | When sent |
|---------|-----------|
| `READY` | Immediately after trigger detected — ESP32-CAM is ready to receive the filepath |
| `OK:<filepath>` | Capture and save succeeded; `<filepath>` is the actual path where the image was stored |
| `ERR:NO_FILEPATH` | Arduino did not send a filepath within 5 seconds of `READY`; image was saved to `/fail/XXXX.jpg` |
| `ERR:CAPTURE_FAILED` | Camera capture or file write failed |

The Arduino should use `response.startsWith("OK")` to detect success and extract the saved path after the `:` for logging.

### 2.3 Error Conditions

| Condition | Arduino receives | Arduino can |
|-----------|-----------------|-------------|
| No filepath sent within 5 s | `ERR:NO_FILEPATH` | Retry on next trigger |
| Camera or storage failure | `ERR:CAPTURE_FAILED` | Retry or skip and continue |
| No `READY` within 2 s of trigger | (silence / timeout) | Log error and retry trigger |

---

## 3. ARDUINO IMPLEMENTATION SPECIFICATION

### 3.1 Pin Configuration

```cpp
const int TRIGGER_PIN = 13;        // Output: Pull LOW to trigger capture
const int UART_RX = 0;             // Hardware Serial RX (receives status)
const int UART_TX = 1;             // Hardware Serial TX (sends filepath)
const int BAUD_RATE = 9600;        // Match ESP32-CAM baud rate
```

### 3.2 Basic Communication Flow

```cpp
// Step 1: Pull trigger LOW
digitalWrite(TRIGGER_PIN, LOW);
delay(100);                        // Hold low for 100ms
digitalWrite(TRIGGER_PIN, HIGH);   // Release

// Step 2: Wait for READY message
waitForMessage("READY", 2000);     // 2 second timeout

// Step 3: Send filepath
Serial.println("/run01/12345.jpg");

// Step 4: Wait for status response
String response = readMessage(5000);  // 5 second timeout
if (response.startsWith("OK")) {
  // Photo saved successfully; extract the actual saved path after "OK:"
  String savedPath = response.substring(3);
} else if (response.startsWith("ERR")) {
  // Capture failed, handle error
}
```

### 3.3 Run Management

The Arduino is responsible for:

#### Run Naming Convention
- Maintains current run number (e.g., "run01", "run02")
- Creates new run directory for each experimental run
- ESP32-CAM simply creates the directory when first file is received

#### Photo Counter Per Run
- Maintains counter for photos within current run
- Names files: `/run01/00001.jpg`, `/run01/00002.jpg`, etc.
- Decides when to increment counter or move to new run

#### Filepath Construction
```cpp
String createFilepath(int runNumber, int photoNumber) {
  char filepath[64];
  snprintf(filepath, sizeof(filepath), "/run%02d/%05d.jpg", 
           runNumber, photoNumber);
  return String(filepath);
}
```

### 3.4 Capture Trigger Logic

The Arduino decides WHEN to capture based on:
- Sensor readings
- Timing conditions
- User input
- Experimental state machine
- Environmental conditions
- Any other application-specific logic

**Example: Time-based capture every 5 seconds**
```cpp
unsigned long lastCapture = 0;
const unsigned long CAPTURE_INTERVAL = 5000;  // 5 seconds

void loop() {
  if (millis() - lastCapture >= CAPTURE_INTERVAL) {
    capturePhoto();
    lastCapture = millis();
    photoCounter++;
  }
}
```

**Example: Sensor-triggered capture**
```cpp
void loop() {
  int sensorValue = analogRead(SENSOR_PIN);
  
  if (sensorValue > THRESHOLD) {
    capturePhoto();
    photoCounter++;
  }
}
```

### 3.5 Error Handling on Arduino Side

```cpp
bool capturePhoto() {
  // Pull trigger
  digitalWrite(TRIGGER_PIN, LOW);
  delay(100);
  digitalWrite(TRIGGER_PIN, HIGH);
  
  // Wait for READY
  if (!waitForMessage("READY", 2000)) {
    Serial.println("ERROR: No READY response");
    return false;
  }
  
  // Send filepath
  String filepath = createFilepath(currentRun, photoCounter);
  Serial.println(filepath);
  
  // Wait for status
  String response = readMessage(5000);
  
  if (response == "OK") {
    Serial.printf("Photo saved: %s\n", filepath.c_str());
    return true;
  } else if (response.startsWith("ERR")) {
    Serial.printf("Capture error: %s\n", response.c_str());
    // Decide: retry? skip? abort run?
    return false;
  } else {
    Serial.println("ERROR: Invalid response from ESP32-CAM");
    return false;
  }
}
```

### 3.6 Run Transitions

Arduino manages run lifecycle:

```cpp
void beginNewRun() {
  currentRun++;
  photoCounter = 0;
  Serial.printf("Starting run %02d\n", currentRun);
}

void endCurrentRun() {
  Serial.printf("Completed run %02d with %d photos\n", 
                currentRun, photoCounter);
  // Optional: save metadata, upload files, etc.
}
```

---

## 4. COMMUNICATION PROTOCOL DETAILS

### 4.1 UART Configuration
- **Baud Rate:** 9600
- **Data Bits:** 8
- **Parity:** None
- **Stop Bits:** 1
- **Flow Control:** None

### 4.2 Message Format

#### Arduino → ESP32-CAM
```
<filepath>\n
Example: /run01/12345.jpg\n
```

#### ESP32-CAM → Arduino
```
Status messages terminated with newline:
READY\n           - Ready for filepath
OK\n              - Photo saved successfully
ERR:NO_FILEPATH\n - Timeout waiting for filepath
ERR:CAPTURE_FAILED\n - Capture or save failed
```

### 4.3 Timing Constraints

```
Trigger → READY      : Should occur within 500ms
Send filepath        : Send within 2 seconds of READY
READY → OK/ERR       : Should occur within 5 seconds
Capture retry delay  : Suggested 1 second minimum between captures
```

---

## 5. COMPLETE TRANSACTION TIMELINE

```
T+0ms:     Arduino pulls GPIO 13 LOW
T+100ms:   Arduino releases GPIO 13 (goes HIGH)
T+101ms:   ESP32-CAM interrupt fires, sets captureFlag
T+110ms:   ESP32-CAM main loop detects captureFlag
T+115ms:   ESP32-CAM sends "READY\n" to Arduino
T+200ms:   Arduino receives "READY"
T+205ms:   Arduino sends "/run01/12345.jpg\n"
T+220ms:   ESP32-CAM receives complete filepath
T+230ms:   ESP32-CAM captures image from camera
T+350ms:   Image capture complete (varies with quality)
T+360ms:   ESP32-CAM writes to SD card
T+500ms:   File write complete
T+510ms:   ESP32-CAM sends "OK\n" to Arduino
T+530ms:   Arduino receives "OK"

Total transaction time: ~530ms (varies with storage speed)
```

---

## 6. STATE DIAGRAMS

### ESP32-CAM State Machine

```
[IDLE] 
  ↓ (captureFlag set by interrupt)
[CAPTURE_READY]
  ├─ Send "READY"
  └─ Wait for filepath (5 sec timeout)
      ├─ Timeout → Send "ERR:NO_FILEPATH" → [IDLE]
      └─ Received filepath → [CAPTURE_IMAGE]
         ├─ Camera capture fails → Send "ERR:CAPTURE_FAILED" → [IDLE]
         └─ Capture success → [SAVE_IMAGE]
            ├─ SD save success → Send "OK" → [IDLE]
            ├─ SD save fails → [FALLBACK_SAVE]
            │  ├─ SPIFFS save success → Send "OK" → [IDLE]
            │  └─ SPIFFS save fails → Send "ERR:CAPTURE_FAILED" → [IDLE]
            └─ Free frame buffer → [IDLE]
```

### Arduino State Machine (Example Application)

```
[IDLE]
  ↓ (Sensor threshold exceeded / Timer fires / User input)
[SEND_TRIGGER]
  ├─ Pull GPIO 13 LOW
  ├─ Wait 100ms
  ├─ Release GPIO 13
  └─ → [WAIT_READY]
     ├─ Timeout (2 sec) → Log error → [IDLE]
     └─ Receive "READY" → [SEND_FILEPATH]
        ├─ Send "/runXX/XXXXX.jpg"
        └─ → [WAIT_STATUS]
           ├─ Timeout (5 sec) → Log error → [IDLE]
           ├─ Receive "OK" → Increment counter → [IDLE]
           └─ Receive "ERR:*" → Log error → [IDLE]
```

---

## 7. IMPLEMENTATION CHECKLIST FOR ARDUINO

- [ ] Configure GPIO 13 as output (trigger pin)
- [ ] Initialize Serial @ 9600 baud for ESP32-CAM communication
- [ ] Implement message reading function with timeout
- [ ] Implement trigger function (pull LOW, hold 100ms, release)
- [ ] Implement filepath construction logic
- [ ] Implement run numbering system
- [ ] Implement photo counter per run
- [ ] Implement capture decision logic (when to capture)
- [ ] Implement error handling for failed captures
- [ ] Implement run transition logic
- [ ] Test communication with ESP32-CAM
- [ ] Test photo capture and file naming
- [ ] Validate storage on SD card
- [ ] Test fallback to SPIFFS (simulate SD card failure)
- [ ] Document all sensor/logic triggers
- [ ] Add logging/debugging output
- [ ] Test extended capture sequences (100+ photos)
- [ ] Test run transitions and counter reset
