# ESP32-C3 Batch Flow + RS485 Valve

This project runs an **ESP32‑C3** as a **Wi‑Fi hotspot (Access Point)** and serves a small web app from **LittleFS**.

Features:

- Flow pulse counting on GPIO2/GPIO3
- Product setup (pulses/gal + valve close time) saved to `/config.json` on LittleFS
- Batch run (dispense by gallons)
- RS485 **Modbus-RTU** valve control + diagnostics

## Project Structure

```
arduino-web-keypad
├── src
│   └── main.ino          # Main Arduino sketch
├── data
│   ├── index.html        # landing page
│   ├── setup.html        # setup (calibration + products)
│   ├── run.html          # run (batch dispense + manual valve controls)
│   ├── diagnostics.html  # RS485 scan + valve checks
│   ├── app.js            # UI logic
│   └── styles.css        # UI styling
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
      - Open the UI in your browser:
       - `http://192.168.4.1/`

## Valve (RS485 / Modbus) protocol notes

This firmware controls a proportional valve over **RS485 Modbus-RTU**.

- UART: **9600 baud**, 8N1 (see `RS485_BAUD` in `src/main.ino`)
- Modbus address: **1** (see `MODBUS_ADDR`)
- Wiring on ESP32-C3:
   - TX = **GPIO4**
   - RX = **GPIO5**
   - DE/RE (TX enable) = optional (only if your RS485 board exposes it)

If your RS485 module/board has **no DE/RE pin** (often marked **NC**), set:

- `RS485_DE_PIN = -1` in `src/main.ino`

Implemented Modbus functions:

- **0x03** Read Holding Registers
- **0x10** Write Multiple Registers

Register usage (as implemented in `src/main.ino`):

- `0x0001` **Position set** (int16, degrees × 100)
   - `0` = open
   - `9000` = closed
- `0x0003` **Position feedback** (int16, degrees × 100)
- `0x0002` **Error state** (uint16)

Diagnostics:

- `http://192.168.4.1/diagnostics.html` includes an RS485 scan that probes a range of Modbus addresses.

## Usage

- Click on the buttons displayed on the keypad to send key presses to the Arduino.
- The Arduino can be programmed to respond to these key presses as needed.

## License

This project is open-source and available for modification and distribution under the MIT License.