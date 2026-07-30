# Insta360 X3 BLE Remote Controller

An ESP32-S3 application that connects to an **Insta360 X3** over Bluetooth Low Energy (BLE) and provides simple physical controls for taking photos and toggling the camera's App Mode.

The project communicates directly with the camera's BLE service (BE80/BE81) without using the official Bluetooth Remote functionality.

---

## Features

* Automatic BLE scanning and connection to the Insta360 X3
* Direct communication using the camera's BLE command interface
* Physical button to capture photos
* Physical button to toggle App Mode
* Serial Monitor commands for testing camera functions
* Automatic reconnection after BLE disconnects
* BLE notification logging for debugging

---

## Hardware Requirements

* ESP32-S3 SuperMini
* Insta360 X3
* Two normally-open push buttons

---

## Wiring

| ESP32-S3 Pin | Function                           |
| ------------ | ----------------------------------- |
| GPIO4        | Capture Photo                      |
| GPIO2        | Toggle App Mode                    |
| GND          | Common connection for both buttons |

Both buttons use the ESP32's internal pull-up resistors (`INPUT_PULLUP`), so pressing a button connects the GPIO pin to **GND**.

---

## Operation

After power-up, the controller:

1. Initializes Bluetooth Low Energy.
2. Scans for an Insta360 X3.
3. Connects to the camera.
4. Locates the required BLE characteristics.
5. Automatically enters App Mode.
6. Waits for button presses or Serial commands.

Once connected:

* **GPIO4** captures a photo.
* **GPIO2** alternates between entering and closing App Mode.

---

## Button Functions

| Button | Description                                                |
| ------ | ----------------------------------------------------------- |
| GPIO4  | Sends the **Take Photo** command to the camera.            |
| GPIO2  | Toggles between **Enter App Mode** and **Close App Mode**. |

---

## Serial Commands

Open the Serial Monitor at **115200 baud**.

| Command | Action                   |
| ------- | ------------------------ |
| `p`     | Take photo               |
| `v`     | Start video recording    |
| `x`     | Stop video recording     |
| `s`     | Toggle video recording   |
| `a`     | Enter App Mode           |
| `c`     | Close App Mode           |
| `r`     | Disconnect and reconnect |
| `h`     | Display help menu        |

---

## Configuration

User-configurable parameters are located near the beginning of the source file.

### Button Pins

```cpp
constexpr uint8_t BUTTON_PIN = 4;
constexpr uint8_t APP_MODE_BUTTON_PIN = 2;
```

### Camera Address

The controller can automatically discover the camera using its advertised BLE service.
If preferred, a specific BLE MAC address can be provided.

```cpp
constexpr char TARGET_CAMERA_MAC[] = "";
```

Leave this empty to enable automatic discovery.

---

## BLE Services

| UUID | Purpose                            |
| ---- | ----------------------------------- |
| BE80 | Camera service                     |
| BE81 | Write commands to the camera       |
| BE82 | Camera responses and notifications |

---

## Project Structure

The application consists of several functional modules within a single source file:

* BLE device scanning
* BLE connection management
* Camera command generation
* Physical button handling
* Serial command interface
* Automatic reconnection
* BLE notification handling

---

## Notes

* The Insta360 X3 must already be powered on before connecting.
* Close the Insta360 mobile application while using this controller.
* The project communicates directly with the camera's BLE interface.
* App Mode is automatically enabled after a successful connection.
* The controller automatically attempts to reconnect if the BLE connection is lost.

---

## Tested Hardware

* ESP32-S3 SuperMini
* Insta360 X3

---

## License

This project is provided for educational, research, and personal use.
