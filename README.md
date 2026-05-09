# ESP32-C3 AP Hotspot Web Keypad

This project runs an **ESP32‑C3** as a **Wi‑Fi hotspot (Access Point)** and serves an on‑screen keypad web app from **LittleFS**.

## Project Structure

```
arduino-web-keypad
├── src
│   └── main.ino          # Main Arduino sketch
├── data
│   ├── index.html        # HTML structure for the keypad
│   ├── app.js            # JavaScript for handling button clicks
│   └── styles.css        # CSS styles for the keypad layout
└── README.md             # Project documentation
```

## Setup Instructions

1. **Hardware Requirements:**
   - An Arduino board with Wi-Fi capabilities (e.g., ESP8266, ESP32).
   - USB cable for connecting the Arduino to your computer.

2. **Software Requirements:**
   - PlatformIO (VS Code extension) **or** Arduino IDE

3. **Installation Steps:**
   - Clone or download the repository to your local machine.
   - **Important:** this project serves `/index.html`, `/app.js`, and `/styles.css` from **LittleFS**.
     You must upload the `data/` folder to the board at least once.

### PlatformIO (recommended)

This repo includes `platformio.ini` for ESP32‑C3.

```bash
pio run
pio run -t upload
pio run -t uploadfs
pio device monitor -b 115200
```

### Arduino IDE

If you use Arduino IDE, you’ll need an ESP32 LittleFS uploader plugin/tooling and then upload the `data/` folder.

4. **Accessing the Keypad (AP / Hotspot mode):**
    - Connect your phone/laptop to the ESP’s Wi‑Fi network:
       - SSID: `ESP32C3-Keypad`
       - Password: `12345678`
    - Open the keypad UI in your browser:
       - `http://192.168.4.1/`

## Usage

- Click on the buttons displayed on the keypad to send key presses to the Arduino.
- The Arduino can be programmed to respond to these key presses as needed.

## License

This project is open-source and available for modification and distribution under the MIT License.