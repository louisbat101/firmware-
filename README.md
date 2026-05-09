# Arduino Web Keypad

This project implements an on-screen keypad using an Arduino board, allowing users to interact with the keypad through a web interface.

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
   - Arduino IDE installed on your computer.
   - Required libraries for Wi-Fi and web server functionality (e.g., ESP8266WiFi, ESPAsyncWebServer).

3. **Installation Steps:**
   - Clone or download the repository to your local machine.
   - Open `src/main.ino` and set your Wi-Fi credentials (`ssid` / `password`).
   - **Important:** this project serves `/index.html`, `/app.js`, and `/styles.css` from **LittleFS**.
     You must upload the `data/` folder to the board at least once.

### PlatformIO (recommended)

This repo includes `platformio.ini` for ESP32-C3.

1. Build + upload firmware
2. Upload filesystem image (uploads `data/` to LittleFS)
3. Open serial monitor to get the IP address

### Arduino IDE

If you use Arduino IDE, you’ll need an ESP32 LittleFS uploader plugin/tooling and then upload the `data/` folder.

4. **Accessing the Keypad:**
   - Once the Arduino is connected to Wi-Fi, open a web browser.
   - Enter the IP address assigned to your Arduino (check the Serial Monitor for the IP).
   - You should see the on-screen keypad displayed in your browser.

## Usage

- Click on the buttons displayed on the keypad to send key presses to the Arduino.
- The Arduino can be programmed to respond to these key presses as needed.

## License

This project is open-source and available for modification and distribution under the MIT License.