/*
    Insta360 X3 BLE Photo Clicker
    Target board: ESP32-S3 SuperMini

    BLE roles:
      ESP32-S3    = BLE client / central
      Insta360 X3 = BLE server / peripheral

    Physical buttons:
      GPIO4 ---- momentary pushbutton ---- GND   (Take Photo)
      GPIO2 ---- momentary pushbutton ---- GND   (Toggle app mode: close/enter)

    Serial Monitor commands:
      p = take photo
      v = start normal video
      x = stop video
      s = toggle video start/stop
      a = enter app-control mode
      c = close app-control mode
      r = disconnect and rescan
      h = show help

    Notes:
      - The camera must already be powered on.
      - Close the Insta360 mobile app while testing.
      - This does not use the camera's Bluetooth Remote menu.
      - Automatic App Mode is disabled below. Uncomment one marked line
        in connectToCamera() if your X3 requires App Mode for commands.
*/

#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEUtils.h>

// -----------------------------------------------------------------------------
// User configuration
// -----------------------------------------------------------------------------

constexpr uint8_t BUTTON_PIN = 4;
constexpr uint8_t APP_MODE_BUTTON_PIN = 2;

// Leave empty to identify the camera by its advertised BE80 service.
// If needed, enter the X3 BLE address found with nRF Connect, for example:
// constexpr char TARGET_CAMERA_MAC[] = "AA:BB:CC:DD:EE:FF";
constexpr char TARGET_CAMERA_MAC[] = "";

constexpr uint32_t SCAN_TIME_SECONDS      = 5;
constexpr uint32_t SCAN_RETRY_DELAY_MS    = 2000;
constexpr uint32_t RECONNECT_DELAY_MS     = 500;
constexpr uint32_t BUTTON_DEBOUNCE_MS     = 40;
constexpr uint32_t COMMAND_SETTLE_MS      = 200;
constexpr uint32_t CONNECTION_SETTLE_MS   = 500;

// Set to true when you need to see every nearby BLE advertisement.
// Keeping this false reduces Serial output and makes debugging easier.
constexpr bool PRINT_ALL_SCAN_RESULTS = false;

// -----------------------------------------------------------------------------
// Insta360 X3 BLE UUIDs
// -----------------------------------------------------------------------------

static BLEUUID CAMERA_SERVICE_UUID(
    "0000be80-0000-1000-8000-00805f9b34fb"
);

static BLEUUID CAMERA_WRITE_UUID(
    "0000be81-0000-1000-8000-00805f9b34fb"
);

static BLEUUID CAMERA_RESPONSE_UUID(
    "0000be82-0000-1000-8000-00805f9b34fb"
);

// -----------------------------------------------------------------------------
// Camera command identifiers
// -----------------------------------------------------------------------------

enum class CameraCommand : uint8_t {
    EnterAppMode = 0x01,
    CloseAppMode = 0x02,
    TakePhoto    = 0x03,
    StartVideo   = 0x04,
    StopVideo    = 0x05
};

// -----------------------------------------------------------------------------
// BLE state
// -----------------------------------------------------------------------------

BLEScan *bleScan = nullptr;
BLEClient *bleClient = nullptr;
BLEAdvertisedDevice *cameraDevice = nullptr;

BLERemoteCharacteristic *cameraWriteCharacteristic = nullptr;
BLERemoteCharacteristic *cameraResponseCharacteristic = nullptr;

volatile bool cameraConnected = false;
volatile bool connectionRequested = false;
volatile uint32_t nextScanTime = 0;

// Two-byte packet sequence used by the reverse-engineered command format.
uint16_t messageSequence = 0x0200;

// Local estimate only. It can become incorrect if recording is changed
// directly on the camera.
bool assumedRecordingState = false;

// Local estimate of app-mode state, used by the GPIO2 toggle button.
// The camera auto-enters app mode on connect (see connectToCamera()),
// so we start "true" and the first GPIO2 press sends Close (c).
bool assumedAppModeState = true;

// -----------------------------------------------------------------------------
// Button state (GPIO4 - take photo)
// -----------------------------------------------------------------------------

bool lastRawButtonState = HIGH;
bool stableButtonState = HIGH;
uint32_t lastButtonChangeTime = 0;

// -----------------------------------------------------------------------------
// Button state (GPIO2 - app mode toggle)
// -----------------------------------------------------------------------------

bool lastRawAppModeButtonState = HIGH;
bool stableAppModeButtonState = HIGH;
uint32_t lastAppModeButtonChangeTime = 0;

// -----------------------------------------------------------------------------
// Utility functions
// -----------------------------------------------------------------------------

void printHex(const uint8_t *data, size_t length) {
    if (data == nullptr || length == 0) {
        return;
    }

    for (size_t i = 0; i < length; ++i) {
        if (data[i] < 0x10) {
            Serial.print('0');
        }

        Serial.print(data[i], HEX);

        if (i + 1 < length) {
            Serial.print(' ');
        }
    }
}

void printHelp() {
    Serial.println();
    Serial.println(F("================================"));
    Serial.println(F("Insta360 X3 Serial Controls"));
    Serial.println(F("================================"));
    Serial.println(F("p = take photo"));
    Serial.println(F("v = start normal video"));
    Serial.println(F("x = stop video"));
    Serial.println(F("s = toggle start/stop video"));
    Serial.println(F("a = enter app-control mode"));
    Serial.println(F("c = close app-control mode"));
    Serial.println(F("r = disconnect and rescan"));
    Serial.println(F("h = show this help"));
    Serial.println();
}

bool isCameraReady() {
    return cameraConnected &&
           bleClient != nullptr &&
           bleClient->isConnected() &&
           cameraWriteCharacteristic != nullptr;
}

void clearRemoteCharacteristics() {
    cameraWriteCharacteristic = nullptr;
    cameraResponseCharacteristic = nullptr;
}

void releaseStoredCameraDevice() {
    if (cameraDevice != nullptr) {
        delete cameraDevice;
        cameraDevice = nullptr;
    }
}

void scheduleNextScan(uint32_t delayMs) {
    nextScanTime = millis() + delayMs;
}

void resetCameraState() {
    cameraConnected = false;
    assumedRecordingState = false;
    assumedAppModeState = true;
    clearRemoteCharacteristics();
}

// -----------------------------------------------------------------------------
// Camera response callback
// -----------------------------------------------------------------------------

static void cameraResponseCallback(
    BLERemoteCharacteristic *characteristic,
    uint8_t *data,
    size_t length,
    bool isNotify
) {
    (void)characteristic;

    Serial.print(isNotify ? F("RX BE82 notify [") : F("RX BE82 indicate ["));
    Serial.print(length);
    Serial.print(F(" bytes]: "));
    printHex(data, length);
    Serial.println();
}

// -----------------------------------------------------------------------------
// BLE client callbacks
// -----------------------------------------------------------------------------

class CameraClientCallbacks final : public BLEClientCallbacks {
public:
    void onConnect(BLEClient *client) override {
        (void)client;
        Serial.println(F("BLE connection established."));
    }

    void onDisconnect(BLEClient *client) override {
        (void)client;

        resetCameraState();
        scheduleNextScan(RECONNECT_DELAY_MS);

        Serial.println();
        Serial.println(F("Camera disconnected."));
        Serial.println(F("Scanning will restart."));
    }
};

// -----------------------------------------------------------------------------
// BLE scanning callback
// -----------------------------------------------------------------------------

class CameraScanCallbacks final : public BLEAdvertisedDeviceCallbacks {
public:
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        const String detectedAddress =
            advertisedDevice.getAddress().toString().c_str();

        const bool matchesService =
            advertisedDevice.haveServiceUUID() &&
            advertisedDevice.isAdvertisingService(CAMERA_SERVICE_UUID);

        const bool matchesConfiguredMac =
            TARGET_CAMERA_MAC[0] != '\0' &&
            detectedAddress.equalsIgnoreCase(TARGET_CAMERA_MAC);

        if (PRINT_ALL_SCAN_RESULTS || matchesService || matchesConfiguredMac) {
            Serial.print(F("BLE device: "));
            Serial.println(advertisedDevice.toString().c_str());
        }

        if (!matchesService && !matchesConfiguredMac) {
            return;
        }

        Serial.println();
        Serial.println(F("Possible Insta360 X3 found."));
        Serial.print(F("Address: "));
        Serial.println(detectedAddress);

        BLEDevice::getScan()->stop();

        releaseStoredCameraDevice();
        cameraDevice = new BLEAdvertisedDevice(advertisedDevice);
        connectionRequested = true;
    }
};

// -----------------------------------------------------------------------------
// Camera commands
// -----------------------------------------------------------------------------

bool sendCameraCommand(CameraCommand command, const __FlashStringHelper *description) {
    if (!isCameraReady()) {
        Serial.println(F("Command rejected: camera is not ready."));
        return false;
    }

    const bool canWriteWithResponse = cameraWriteCharacteristic->canWrite();
    const bool canWriteWithoutResponse =
        cameraWriteCharacteristic->canWriteNoResponse();

    if (!canWriteWithResponse && !canWriteWithoutResponse) {
        Serial.println(F("Command rejected: BE81 is not writable."));
        return false;
    }

    constexpr size_t PACKET_SIZE = 16;
    uint8_t packet[PACKET_SIZE] = {
        0x10,
        0x00, 0x00, 0x00,
        0x04,
        0x00, 0x00,
        static_cast<uint8_t>(command),
        0x00,
        static_cast<uint8_t>((messageSequence >> 8) & 0xFF),
        static_cast<uint8_t>(messageSequence & 0xFF),
        0x00, 0x00,
        0x80, 0x00, 0x00
    };

    ++messageSequence;

    // Prefer write-with-response when available so the BLE stack can report
    // whether the characteristic write was accepted.
    const bool requestWriteResponse = canWriteWithResponse;

    Serial.print(F("TX BE81 - "));
    Serial.println(description);
    Serial.print(F("Packet: "));
    printHex(packet, sizeof(packet));
    Serial.println();

    const bool writeSuccessful = cameraWriteCharacteristic->writeValue(
        packet,
        sizeof(packet),
        requestWriteResponse
    );

    Serial.println(
        writeSuccessful ? F("BLE write completed.") : F("BLE write failed.")
    );

    if (writeSuccessful) {
        delay(COMMAND_SETTLE_MS);
    }

    return writeSuccessful;
}

bool enterAppMode() {
    const bool successful = sendCameraCommand(
        CameraCommand::EnterAppMode,
        F("Enter app-control mode")
    );

    if (successful) {
        assumedAppModeState = true;
    }

    return successful;
}

bool closeAppMode() {
    const bool successful = sendCameraCommand(
        CameraCommand::CloseAppMode,
        F("Close app-control mode")
    );

    if (successful) {
        assumedAppModeState = false;
    }

    return successful;
}

bool takePhoto() {
    return sendCameraCommand(
        CameraCommand::TakePhoto,
        F("Take photo")
    );
}

bool startVideo() {
    const bool successful = sendCameraCommand(
        CameraCommand::StartVideo,
        F("Start normal video")
    );

    if (successful) {
        assumedRecordingState = true;
    }

    return successful;
}

bool stopVideo() {
    const bool successful = sendCameraCommand(
        CameraCommand::StopVideo,
        F("Stop video")
    );

    if (successful) {
        assumedRecordingState = false;
    }

    return successful;
}

void toggleVideo() {
    if (assumedRecordingState) {
        stopVideo();
    } else {
        startVideo();
    }
}

// Camera auto-enters app mode on connect, so the state starts "in app mode".
// First press closes app mode (c), next press re-enters it (a), and so on.
void toggleAppMode() {
    if (assumedAppModeState) {
        closeAppMode();
    } else {
        enterAppMode();
    }
}

// -----------------------------------------------------------------------------
// Camera connection
// -----------------------------------------------------------------------------

bool subscribeToCameraResponses(BLERemoteService *cameraService) {
    cameraResponseCharacteristic =
        cameraService->getCharacteristic(CAMERA_RESPONSE_UUID);

    if (cameraResponseCharacteristic == nullptr) {
        Serial.println(F("Warning: BE82 response characteristic was not found."));
        return false;
    }

    Serial.println(F("BE82 response characteristic found."));

    if (cameraResponseCharacteristic->canNotify()) {
        Serial.println(F("Subscribing to BE82 notifications."));
        cameraResponseCharacteristic->registerForNotify(
            cameraResponseCallback,
            true
        );
        return true;
    }

    if (cameraResponseCharacteristic->canIndicate()) {
        Serial.println(F("Subscribing to BE82 indications."));
        cameraResponseCharacteristic->registerForNotify(
            cameraResponseCallback,
            false
        );
        return true;
    }

    Serial.println(F("Warning: BE82 does not support notify or indicate."));
    return false;
}

bool connectToCamera() {
    if (cameraDevice == nullptr) {
        Serial.println(F("Connection failed: no camera device stored."));
        return false;
    }

    Serial.println();
    Serial.println(F("=============================="));
    Serial.println(F("Connecting to Insta360 X3"));
    Serial.println(F("=============================="));
    Serial.print(F("Camera address: "));
    Serial.println(cameraDevice->getAddress().toString().c_str());

    if (bleClient == nullptr) {
        bleClient = BLEDevice::createClient();
        bleClient->setClientCallbacks(new CameraClientCallbacks());
    }

    if (bleClient->isConnected()) {
        bleClient->disconnect();
        delay(200);
    }

    Serial.println(F("Opening BLE connection..."));

    if (!bleClient->connect(cameraDevice)) {
        Serial.println(F("Failed to connect to the camera."));
        releaseStoredCameraDevice();
        return false;
    }

    // Commands are small, but a larger MTU also supports longer response packets.
    bleClient->setMTU(517);

    Serial.println(F("Looking for BE80 service..."));
    BLERemoteService *cameraService =
        bleClient->getService(CAMERA_SERVICE_UUID);

    if (cameraService == nullptr) {
        Serial.println(F("BE80 service was not found."));
        bleClient->disconnect();
        releaseStoredCameraDevice();
        return false;
    }

    Serial.println(F("BE80 service found."));

    cameraWriteCharacteristic =
        cameraService->getCharacteristic(CAMERA_WRITE_UUID);

    if (cameraWriteCharacteristic == nullptr) {
        Serial.println(F("BE81 write characteristic was not found."));
        bleClient->disconnect();
        releaseStoredCameraDevice();
        return false;
    }

    Serial.print(F("BE81 properties:"));
    if (cameraWriteCharacteristic->canWrite()) {
        Serial.print(F(" WRITE"));
    }
    if (cameraWriteCharacteristic->canWriteNoResponse()) {
        Serial.print(F(" WRITE_NO_RESPONSE"));
    }
    Serial.println();

    subscribeToCameraResponses(cameraService);

    cameraConnected = true;
    assumedRecordingState = false;
    assumedAppModeState = true;
    releaseStoredCameraDevice();

    Serial.println();
    Serial.println(F("Camera connection ready."));

    delay(CONNECTION_SETTLE_MS);

    // The X3 requires App Mode before accepting direct BE81 camera commands.
    // Keep this enabled for reliable photo capture.
    enterAppMode();

    Serial.println();
    Serial.println(F("=============================="));
    Serial.println(F("READY"));
    Serial.println(F("=============================="));
    Serial.println(F("GPIO4 button: take photo"));
    Serial.println(F("GPIO2 button: toggle app mode (close first, then enter)"));
    Serial.println(F("Serial: p=photo, v=start, x=stop, a=app mode, c=close app mode"));

    return true;
}

// -----------------------------------------------------------------------------
// BLE scanning
// -----------------------------------------------------------------------------

void scanForCamera() {
    if (cameraConnected || connectionRequested) {
        return;
    }

    Serial.println();
    Serial.println(F("Scanning for the Insta360 X3..."));
    Serial.println(F("Keep the camera powered on and close the phone app."));

    bleScan->clearResults();
    bleScan->start(SCAN_TIME_SECONDS, false);

    if (connectionRequested) {
        return;
    }

    Serial.println();
    Serial.println(F("Camera was not identified during this scan."));

    if (TARGET_CAMERA_MAC[0] == '\0') {
        Serial.println(F("Enable PRINT_ALL_SCAN_RESULTS or set TARGET_CAMERA_MAC if needed."));
    }

    scheduleNextScan(SCAN_RETRY_DELAY_MS);
}

// -----------------------------------------------------------------------------
// Serial input
// -----------------------------------------------------------------------------

void requestReconnect() {
    Serial.println(F("Manual reconnect requested."));

    if (bleClient != nullptr && bleClient->isConnected()) {
        bleClient->disconnect();
    }

    resetCameraState();
    connectionRequested = false;
    releaseStoredCameraDevice();
    nextScanTime = 0;
}

void handleSerialInput() {
    while (Serial.available() > 0) {
        const char input = Serial.read();

        if (input == '\r' || input == '\n' || input == ' ') {
            continue;
        }

        switch (input) {
            case 'p':
            case 'P':
                takePhoto();
                break;

            case 'v':
            case 'V':
                startVideo();
                break;

            case 'x':
            case 'X':
                stopVideo();
                break;

            case 's':
            case 'S':
                toggleVideo();
                break;

            case 'a':
            case 'A':
                enterAppMode();
                break;

            case 'c':
            case 'C':
                closeAppMode();
                break;

            case 'r':
            case 'R':
                requestReconnect();
                break;

            case 'h':
            case 'H':
                printHelp();
                break;

            default:
                Serial.print(F("Unknown command: "));
                Serial.println(input);
                printHelp();
                break;
        }
    }
}

// -----------------------------------------------------------------------------
// Physical buttons
// -----------------------------------------------------------------------------

void handleButton() {
    const bool rawButtonState = digitalRead(BUTTON_PIN);

    if (rawButtonState != lastRawButtonState) {
        lastRawButtonState = rawButtonState;
        lastButtonChangeTime = millis();
    }

    if (millis() - lastButtonChangeTime < BUTTON_DEBOUNCE_MS) {
        return;
    }

    if (rawButtonState == stableButtonState) {
        return;
    }

    stableButtonState = rawButtonState;

    // INPUT_PULLUP means LOW is the pressed state.
    if (stableButtonState == LOW) {
        Serial.println();
        Serial.println(F("Physical button pressed: taking photo."));
        takePhoto();
    }
}

void handleAppModeButton() {
    const bool rawButtonState = digitalRead(APP_MODE_BUTTON_PIN);

    if (rawButtonState != lastRawAppModeButtonState) {
        lastRawAppModeButtonState = rawButtonState;
        lastAppModeButtonChangeTime = millis();
    }

    if (millis() - lastAppModeButtonChangeTime < BUTTON_DEBOUNCE_MS) {
        return;
    }

    if (rawButtonState == stableAppModeButtonState) {
        return;
    }

    stableAppModeButtonState = rawButtonState;

    // INPUT_PULLUP means LOW is the pressed state.
    if (stableAppModeButtonState == LOW) {
        Serial.println();
        Serial.println(
            assumedAppModeState
                ? F("App-mode button pressed: closing app mode.")
                : F("App-mode button pressed: entering app mode.")
        );
        toggleAppMode();
    }
}

// -----------------------------------------------------------------------------
// Arduino setup and loop
// -----------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(1200);

    Serial.println();
    Serial.println(F("=============================="));
    Serial.println(F("Insta360 X3 BLE Photo Clicker"));
    Serial.println(F("Direct BE80/BE81 Method"));
    Serial.println(F("=============================="));

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(APP_MODE_BUTTON_PIN, INPUT_PULLUP);

    BLEDevice::init("ESP32 Insta360 Controller");

    bleScan = BLEDevice::getScan();
    bleScan->setAdvertisedDeviceCallbacks(new CameraScanCallbacks());
    bleScan->setActiveScan(true);

    // Scan interval and window use BLE units. Window must not exceed interval.
    bleScan->setInterval(100);
    bleScan->setWindow(99);

    printHelp();
    nextScanTime = 0;
}

void loop() {
    handleSerialInput();

    if (cameraConnected) {
        handleButton();
        handleAppModeButton();
    }

    if (connectionRequested) {
        connectionRequested = false;

        if (!connectToCamera()) {
            resetCameraState();
            scheduleNextScan(SCAN_RETRY_DELAY_MS);
        }
    }

    if (!cameraConnected &&
        !connectionRequested &&
        millis() >= nextScanTime) {
        scanForCamera();
    }

    delay(5);
}
