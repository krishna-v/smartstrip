# smartstrip
ESP8266 controller software for WS8211/8212 based RGB LED strips.
smartstrip provides the following capabilities:
- Web based control and management of RGB LED strips.
- Ability to upload and manage a library of pattern sequences on the device. 
- Ability to OTA flash new software versions.

## Hardware Components
- ESP8266 based board with Flash chip able to house a SPIFFS filesystem. I've used [NodeMCU](https://en.wikipedia.org/wiki/NodeMCU) boards.
- WS8211 / WS8212 based RGB LED strip. (Any strip supported by the [Adafruit NeoPixel Library](https://github.com/adafruit/Adafruit_NeoPixel) should work.)
- Good quality power supply to drive the LEDs. Many issues are caused by inadequate power. I have used repurposed laptop power supplies with cheap XL4015 based buck converter boards.

## Software Dependencies
- [Adafruit NeoPixel Library](https://github.com/adafruit/Adafruit_NeoPixel)
- [ESP8266WebServer Library](https://github.com/esp8266/Arduino/tree/master/libraries/ESP8266WebServer)
- [ArduinoJSON](https://arduinojson.org/)
- [ArduinoOTA](https://www.arduino.cc/reference/en/libraries/arduinoota/)
- [SPIFFS Filesystem](https://github.com/pellepl/spiffs) - I realize LittleFS is the currently preferred way to go, but for this use case, SPIFFS is perfectly adequate and has less overhead.
- [Arduino ESP8266 filesystem uploader](https://github.com/esp8266/arduino-esp8266fs-plugin) - to initialize the filesystem with some required files.
- [Arduino IDE](https://www.arduino.cc/en/software) of course.
