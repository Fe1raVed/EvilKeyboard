# BLE-RubberDucky (ESP32-S3)

A professional Bluetooth Low Energy (BLE) HID injection tool designed for security auditing and penetration testing. This project transforms an ESP32-S3 into a wireless "Rubber Ducky" style device, allowing remote keystroke injection via a web-based API.

---

## 🚀 Overview

Unlike traditional USB Rubber Duckies, this tool operates over **BLE (Bluetooth Low Energy)**, meaning no physical connection to the target's USB port is required after pairing. It combines a **WiFi SoftAP** for command control and a **BLE HID Stack** for payload execution.



## 🛠️ System Architecture

* **WiFi SoftAP**: Host a local network (Default SSID: `FeiraVed`) to receive payloads.
* **RESTful API**: Accepts commands via HTTP POST requests.
* **FreeRTOS Queue**: Ensures non-blocking and stable execution of keystrokes.
* **BLE HID Stack**: Emulates a standard high-speed keyboard to bypass most basic hardware security filters.

## 📁 Key Components

* `esp_hid_device_main.c`: Core initialization of NVS, WiFi, BLE, and the REST server.
* `char_to_code`: ASCII to HID Scan Code translation engine.
* `ble_hid_demo_task_kbd`: The main execution task that handles keystrokes and shortcut parsing.

## ⚙️ Configuration & Build

This project is optimized for **ESP32-S3** using the **NimBLE** stack.

### Quick Start
1.  Set the target:
    ```bash
    idf.py set-target esp32s3
    ```
2.  Build and Flash:
    ```bash
    idf.py build flash monitor
    ```

### Required sdkconfig Defaults
The build system expects the following configurations:
```ini
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_HID_SERVICE=y
CONFIG_EXAMPLE_KBD_ENABLE=y
```
