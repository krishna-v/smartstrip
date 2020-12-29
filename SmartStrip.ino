#include <Adafruit_NeoPixel.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <FS.h>

#define LED D0
#define CFGFILE "/config.json"
#define PATTERNFILE "/autorun.json"
#define UPLOAD_BUF "/.upload"

#define MAXPATTERNS 256

// "Magic" colors to indicate special operations on Pixels. 
#define COLOR_HOLD     0x01000000
#define COLOR_FLIP     0x02000000
#define COLOR_RAINBOW  0x03000000
#define COLOR_RANDOM   0x04000000

#define STRIP_DEFAULT_LEN    50
#define STRIP_DEFAULT_PIN    5
#define STRIP_DEFAULT_SPEED  NEO_KHZ800
#define STRIP_DEFAULT_LAYOUT NEO_RGB

#define C_RED(color) (uint8_t) ((color>>16) & 0xFF)
#define C_GREEN(color) (uint8_t) ((color>>8) & 0xFF)
#define C_BLUE(color) (uint8_t) (color & 0xFF)
// #define C_WHITE(color) (uint8_t) ((color>>24) & 0xFF)

bool spiffsActive = false;
bool wifiOK = false;
bool needReset = false;

StaticJsonDocument<4096> patterns;
Adafruit_NeoPixel strip(STRIP_DEFAULT_LEN, STRIP_DEFAULT_PIN, STRIP_DEFAULT_LAYOUT + STRIP_DEFAULT_SPEED);
ESP8266WebServer *webserver = NULL;

void(* resetFunc) (void) = 0; // declare reset function at address 0
int (*getPatternFunc(const char *funcstring))(void);

enum PixelOp { STRIPE, SCALE, SWAP, CYCLE };

enum PixelOp getPixelOp(const char * str) {
  if(!strcmp(str, "STRIPE")) return STRIPE;
  else if(!strcmp(str, "SCALE")) return SCALE;
  else if(!strcmp(str, "SWAP")) return SWAP;
  else if(!strcmp(str, "CYCLE")) return CYCLE;
  else return STRIPE;
}

uint32_t getColor(const char *c) {
  uint32_t color = 0;
  if(!strcmp(c, "hold")) color = COLOR_HOLD;
  else if(!strcmp(c, "flip")) color = COLOR_FLIP;
  else if(!strcmp(c, "rainbow")) color = COLOR_RAINBOW;
  else if(!strcmp(c, "random")) color = COLOR_RANDOM;
  else color = strtoul(c, NULL, 16);
  return color;
}

uint32_t midColor(uint32_t first, uint32_t sec, uint8_t pos) {
  if(pos <= 1) return sec;
  uint8_t red = C_RED(first) + ((C_RED(sec) - C_RED(first)) / pos);
  uint8_t green = C_GREEN(first) + ((C_GREEN(sec) - C_GREEN(first)) / pos);
  uint8_t blue = C_BLUE(first) + ((C_BLUE(sec) - C_BLUE(first)) / pos);
  return Adafruit_NeoPixel::Color(red, green, blue);
}

struct PatternState {
  public:
    int pat_idx;                  // index of the current pattern in the current pattern file.
    int (*pfunc)(void);           // current pattern function pointer.
    int iter1;                    // Iterator for use within pattern execution. Resets every repeat of a pattern.
    int iter2;                    // Iterator for use within pattern execution. Persists across repeats.
    int spansize;                 // Span length for pattern draw, used for some patterns.
    bool forward = true;          // Direction of pattern draw.
    enum PixelOp op = STRIPE;     // How to paint the colors across the strip.
    unsigned long lastActionTime; // timer for execution pacing
    int loops_left;               // Number of repeats left.
    uint32_t colors[16];          // Array of colors to paint on the strip.
    int numcolors;                // Number of colors in the array.
    int rundelay;                 // execution speed.
    bool pauserun;                // pause execution if set.
    
    int initialize(int idx, JsonObjectConst v) {
      _initialize();
      pat_idx = idx;
      if(v.isNull()) return -1;
      for (JsonObjectConst::iterator it=v.begin(); it!=v.end(); ++it) {
        if(!strcmp(it->key().c_str(), "pattern")) pfunc = getPatternFunc(it->value().as<const char*>());
        else if(!strcmp(it->key().c_str(), "speed")) rundelay = 100 - it->value().as<unsigned int>();
        else if(!strcmp(it->key().c_str(), "delay")) rundelay = it->value().as<unsigned int>();
        else if(!strcmp(it->key().c_str(), "repeat")) {
          loops_left = it->value().as<int>();
          if(loops_left < 0) loops_left = -(loops_left * strip.numPixels());
          loops_left--;
        }
        else if(!strcmp(it->key().c_str(), "span")) spansize = it->value().as<unsigned int>();
        else if(!strcmp(it->key().c_str(), "direction"))
          forward = strcmp(it->value().as<const char *>(), "reverse") ? true : false;
        else if(!strcmp(it->key().c_str(), "operation")) op = getPixelOp(it->value().as<const char *>());
        else if(!strcmp(it->key().c_str(), "colors")) {
          JsonArrayConst colorlist = it->value().as<JsonArrayConst>();
          numcolors = colorlist.size();
          if(numcolors > 16) numcolors = 16;
          for(int i=0; i < numcolors; i++) {
            colors[i] = getColor(colorlist[i].as<const char *>());
          }

        } else {
          Serial.print("Don't know what to do with key: ");
          Serial.println(it->key().c_str());
        }
      }
      if(rundelay < 0) rundelay = 0;
      if(loops_left < 0) loops_left = 0;
      if(spansize <= 0 || spansize > strip.numPixels()) spansize = strip.numPixels();
      return 0;
    }

    int nextloop() {
      iter1 = 0;
      // iter2 = 0; retain iter2 across loops for the same pattern.
      loops_left--;
      return loops_left;
    }

    void nextPattern() {
      if(initialize(pat_idx+1, patterns.as<JsonArrayConst>()[pat_idx + 1].as<JsonObjectConst>()) == -1) {
        initialize(0, patterns.as<JsonArrayConst>()[0].as<JsonObjectConst>());
      }
    }
    
    PatternState() {
      _initialize();
      lastActionTime = 0;
    }
    
  protected:
    void _initialize() {
      op = STRIPE;
      forward = true;
      pat_idx = 0;
      spansize = 0;
      iter1 = 0;
      iter2 = 0;
      loops_left = 0;
      pfunc = NULL;
      numcolors = 0;
      rundelay = 0;
      pauserun = false;
    }
} patternState;



// ####################### NeoPixel Functions #########################

neoPixelType getStripType(String rgb, unsigned int speed) {
  int len = rgb.length();
  if(len > 4) {
    Serial.println(rgb + ": Invalid string for RGB in getStripType");
    return NEO_RGB;
  }
  int r = rgb.indexOf("R");
  int g = rgb.indexOf("G");
  int b = rgb.indexOf("B");
  int w = rgb.indexOf("W");

  neoPixelType out = ((byte) r << 4) | ((byte) g << 2) | (byte) b;
  out = (w == -1) ? out | ((out & 48) << 2) : out | (w << 6);
  return out + ((speed == 400) ? NEO_KHZ400 : NEO_KHZ800);
}

// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
uint32_t colorWheel(byte wheelPos) {
  if(wheelPos < 85) {
    return Adafruit_NeoPixel::Color(wheelPos * 3, 255 - wheelPos * 3, 0);
  } else if(wheelPos < 170) {
   wheelPos -= 85;
   return Adafruit_NeoPixel::Color(255 - wheelPos * 3, 0, wheelPos * 3);
  } else {
   wheelPos -= 170;
   return Adafruit_NeoPixel::Color(0, wheelPos * 3, 255 - wheelPos * 3);
  }
}


#define MIN(a,b) ((a) > (b) ? (b) : (a))

uint32_t indexColor(int pix_id) {
  uint32_t color;
  switch(patternState.op) {     
    case SCALE:
    {
      int factor = strip.numPixels() / patternState.numcolors;
      int remainder = strip.numPixels() % patternState.numcolors;
      int baseidx = pix_id / factor;
      int color_idx = (pix_id - MIN(baseidx, remainder)) / factor;
      color = patternState.colors[color_idx];
    }
    break;
    
    case STRIPE:
    default:
    {
      color = patternState.colors[pix_id % patternState.numcolors];
    }
    break;
  }
  return color;
}

uint32_t calculateColor(int pix_id, uint32_t color, int modifier = 0) {
  if(color == COLOR_HOLD) return strip.getPixelColor(pix_id);
  else if(color == COLOR_FLIP) {
    return 0xFFFFFF - strip.getPixelColor(pix_id); // TODO: Broken for RGBW strips.
  } else if(color == COLOR_RAINBOW) {
    return colorWheel(((pix_id * 256 / strip.numPixels()) + modifier) & 255);
  } else if(color == COLOR_RANDOM) {
    return colorWheel(random(256));
  } else return color;
}

void setPixelColor(int idx, uint32_t color, int modifier = 0) {
  strip.setPixelColor(idx, calculateColor(idx, color, modifier));
}

void doPixelOp(int pix_id) {
  setPixelColor(pix_id, indexColor(pix_id));
}

int fillerFunc() {
  for(int i=0; i<strip.numPixels(); i++) doPixelOp(i);
  strip.show();
  return 1;
}

int faderFunc() {
  for(int i=0; i<strip.numPixels(); i++)
    strip.setPixelColor(i, midColor(strip.getPixelColor(i), calculateColor(i, indexColor(i)), patternState.loops_left+1));
  strip.show();
  return 1;  
}

int wiperFunc() {
  for(int i = 0; i + patternState.iter1 < strip.numPixels(); i += patternState.spansize) {
    doPixelOp(patternState.forward ? (i + patternState.iter1) : ((strip.numPixels() - 1) - (i + patternState.iter1)));
  }
  strip.show();
  patternState.iter1++;
  if(patternState.iter1 >= patternState.spansize) {
    patternState.iter1 = 0;
    return 1;
  }
  return 0;
}

int curtainFunc() {
  int pix1, pix2;
  if(patternState.forward) {
    pix1 = ((strip.numPixels() - 1) / 2) - patternState.iter1;
    pix2 = ((strip.numPixels()) / 2) + patternState.iter1;
  } else {
    pix1 = patternState.iter1;
    pix2 = strip.numPixels()  - (patternState.iter1 + 1);   
  }
  if(patternState.op == SWAP) { 
    uint32_t color = strip.getPixelColor(pix1);
    strip.setPixelColor(pix1, strip.getPixelColor(pix2));
    strip.setPixelColor(pix2, color);
  } else {
    doPixelOp(pix1);
    doPixelOp(pix2);
  }
  strip.show();
  patternState.iter1++;
  if(patternState.iter1 >= strip.numPixels() / 2) {
    patternState.iter1 = 0;
    return 1;
  }
  return 0;
}

int chaserFunc() {
  if(patternState.iter1 == 0) {
    patternState.colors[0] = strip.getPixelColor(patternState.forward ? (strip.numPixels() - 1) : 0);
  }
  int pix_id = patternState.forward ? patternState.iter1 : strip.numPixels() - (patternState.iter1 + 1);
  uint32_t c = strip.getPixelColor(pix_id);
  strip.setPixelColor(pix_id, patternState.colors[0]);
  strip.show();
  patternState.colors[0] = c;
  patternState.iter1++;
  if(patternState.iter1 >= strip.numPixels()) {
    patternState.iter1 = 0;
    return 1;
  }
  return 0;
}

int shifterFunc() {
  uint32_t color;

  if(patternState.numcolors > 0 && patternState.iter2 < strip.numPixels()) {
    int idx = patternState.forward ? strip.numPixels() - (patternState.iter2 + 1) : patternState.iter2;
    color = calculateColor(idx, indexColor(idx));
  }
  else color = strip.getPixelColor(patternState.forward ? (strip.numPixels() - 1) : 0);
  for(int i = 0; i < strip.numPixels(); i++) {
    int idx = patternState.forward ? i : (strip.numPixels() - (i + 1));
    uint32_t c = strip.getPixelColor(idx);
    strip.setPixelColor(idx, color);
    color = c;
  }
  strip.show();
  patternState.iter2++;
  return 1;
}

int dummyFunc() { // Just to introduce delays
  return 1;
}

int (*getPatternFunc(const char *funcstring))(void) {
  if(strcmp(funcstring, "fill") == 0) return fillerFunc;
  if(strcmp(funcstring, "fade") == 0) return faderFunc;
  if(strcmp(funcstring, "wipe") == 0) return wiperFunc;
  if(strcmp(funcstring, "curtain") == 0) return curtainFunc;
  if(strcmp(funcstring, "shift") == 0) return shifterFunc;
  if(strcmp(funcstring, "chase") == 0) return chaserFunc;
  if(strcmp(funcstring, "wait") == 0) return dummyFunc;
  return NULL;
}

void patternFill(uint32_t pattern[], uint8_t len) {
  for(int i=0; i<strip.numPixels(); i++) {
    setPixelColor(i, pattern[i % len]);
  }
  strip.show();
}

// ################ Web Server Stuff ##################################

const char rootpage[] PROGMEM = {
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"<title>Smart Light Strip Controller</title>\n"
"</head>\n"
"<body>\n"
  "<div class='header'><h1>Smart Light Strip Controller</h1></div>\n"
  "<div class='commands'>\n"
    "<h2>Commands:</h2><br>\n"
    "<p>Commands are of the form http://hostname/cmd/{cmdname}[?args], where cmdname is one of:</p>\n"
    "<table>\n"
      "<tr><th>Command</th><th>Description</th><th>Arguments</th></tr\n"
      "<tr><td>pause</td><td>Pause execution of patterns</td><td>None</td></tr>\n"
      "<tr><td>resume</td><td>Resume execution of patterns</td><td>None</td></tr>\n"
      "<tr><td>fill</td><td>Fill the strip with specified color(s)</td><td>colors={comma separated hex values}</td><td>None</td></tr>\n"
      "<tr><td>off</td><td>turn off the strip ( same as fill?colors=000000 )</td><td>None</td></tr>\n"
    "</table>\n"
  "</div>\n"
    "<div class='patterns'>\n"
    "<h2>Pattern / File operations:</h2><br>\n"
    "<p>Commands are of the form http://hostname/pattern/{operation}[?args], where cmdname is one of:</p>\n"
    "<table>\n"
      "<tr><th>Command</th><th>Description</th><th>Arguments</th></tr\n"
      "<tr><td>show</td><td>Show pattern set currently in working memory.</td><td>None</td></tr>\n"
      "<tr><td>list</td><td>list saved patterns</td><td>None</td></tr>\n"
      "<tr><td>load</td><td>upload pattern (from file / post data) into working memory.</td><td>Pattern Data</td></tr>\n"
      "<tr><td>save</td><td>Save in-memory pattern set to a file</td><td>None</td><td>filename, overwrite=true if overwriting</td></tr>\n"
      "<tr><td>open</td><td>Open a stored pattern file and load to working memory</td><td>filename</td></tr>\n"
      "<tr><td>print</td><td>Print out contents of a saved file</td><td>filename</td></tr>\n"
    "</table>\n"
  "</div>\n"
"</body>\n"
"</html>\n"
};

void http_root() {
  webserver->send(200, "text/html", FPSTR(rootpage));
}

void http_configure() {
  StaticJsonDocument<512> cfg;
  if(loadJSONFile(CFGFILE, cfg) != 0) {
    Serial.println("Load Config failed");
    webserver->send(500, "text/plain", "Error: Could not load config file\n");
    return;
  }
  
  String gpio = webserver->arg("gpio");
  String pixels = webserver->arg("pixels");
  String speed = webserver->arg("speed");
  String layout = webserver->arg("layout");

  bool changed = false;
  if(!gpio.isEmpty() && cfg["strip"]["gpio"] != gpio.toInt()) {
    cfg["strip"]["gpio"] = gpio.toInt();
    changed = true;
  }
  if(!pixels.isEmpty() && cfg["strip"]["pixels"] != pixels.toInt()) {
    cfg["strip"]["pixels"] = pixels.toInt();
    changed = true;
  }
  if(!speed.isEmpty() && cfg["strip"]["speed"] != speed.toInt()) {
    cfg["strip"]["speed"] = speed.toInt();
    changed = true;
  }
  if(!layout.isEmpty()) {
    layout.toUpperCase();
    if(!layout.equals(cfg["strip"]["layout"].as<const char *>())) {   
      cfg["strip"]["layout"] = layout; // TODO: Need more robust checks.
      changed = true;
    }
  }
  
  String cfgstr;
  serializeJsonPretty(cfg, cfgstr);
  cfgstr += "\n";

  if(changed) {
    File file = SPIFFS.open(CFGFILE, "w");
    if(!file) {
      Serial.println("Write Config failed");
      webserver->send(500, "text/plain", "Error: Could not write config file\n");
      return;
    }
    file.print(cfgstr);
    file.close();
    needReset = true;
  } else Serial.println("Nothing changed\n");

  webserver->send(200, "application/json", cfgstr);
}

void http_notfound(){
  webserver->send(404, "text/plain", "Not found\n");
}

class CmdHandler : public RequestHandler {

  bool canHandle(HTTPMethod method, String uri) {
    return uri != NULL && uri.startsWith("/cmd/");
  }

  bool handle(ESP8266WebServer& server, HTTPMethod requestMethod, String requestUri) {
    Serial.print("Received Request: ");
    Serial.println(requestUri);
    
    if(requestUri.equals("/cmd/pause")) {
      patternState.pauserun = true;
      // server.send(204, "");
    } else if(requestUri.equals("/cmd/resume")) {
      patternState.pauserun = false;
      // server.send(204, "");
    } else if(requestUri.equals("/cmd/off")) {
      patternState.pauserun = true;
      strip.clear();
      strip.show();
    } else if(requestUri.equals("/cmd/fill")) {
      String colors = server.arg("colors");
      if(colors.isEmpty()) {
        Serial.println("No colors Argument");
      } else {
        Serial.print("colors: ");
        uint32_t colorarray[16];
        int idx = 0;
        int start = 0;
        do {
          int i = colors.indexOf(',', start);
          String c = (i == -1) ? colors.substring(start).c_str() : colors.substring(start,i).c_str();
          colorarray[idx] = getColor(c.c_str());
          Serial.print(String(colorarray[idx]));
          Serial.println(" ");
          idx++;
          start = i+1;
        } while(start != 0 && idx < 16);
        patternState.pauserun = true;
        patternFill(colorarray, idx);
      }
    } else {
      http_notfound();
      return true;
    }
    server.send(204, "");
    return true;
  }
} cmdHandler;



class PatternHandler : public RequestHandler {

  protected:

  bool _uploadInProgress = false;
  int _errCode;
  String _errString;
  File _buf;
  
  void printArgs(ESP8266WebServer& server) {
    Serial.println("==== Args ====");
    for(int i = 0; i < server.args(); i++) {  
      Serial.print(server.argName(i) + ": ");
      Serial.println(server.arg(i));
    }
    Serial.println("==============");
  }

    void printHeaders(ESP8266WebServer& server) {
    Serial.println("==== Headers ====");
    for(int i = 0; i < server.headers(); i++) {   
      Serial.print(server.headerName(i) + ": ");
      Serial.println(server.header(i));      
    }
    Serial.println("=================");
  }
  
  bool showPatterns(ESP8266WebServer& server, HTTPMethod requestMethod, String requestUri) {
      String patstr;
      serializeJsonPretty(patterns, patstr);
      patstr += "\n";
      server.send(200, "application/json", patstr);
      return true;
  }

  bool saveCurrent(ESP8266WebServer& server, HTTPMethod requestMethod, String requestUri) {
    int errCode = 0;
    String errStr;
    do {
      String filename = server.arg("filename");
      if(filename.isEmpty()) {
        errCode = 400;
        errStr = "Error: no filename parameter provided.";
        break;
      }
      if (!filename.startsWith("/")) filename = "/" + filename;
      if(filename.equals("/config.json")) {
        errCode = 400;
        errStr = "Error: Invalid filename.";
        break;
      }
      if (SPIFFS.exists(filename)) {
        String overwrite = server.arg("overwrite");
        if(!overwrite.isEmpty() && overwrite.equals("true")) {
          Serial.println("Overwriting existing file.");
          if (SPIFFS.exists(filename + ".old")) {
            Serial.println("Deleting .old file");
            SPIFFS.remove(filename + ".old");
          }
          SPIFFS.rename(filename, filename + ".old");
        } else {
          errCode = 400;
          errStr = "Error: File exists but overwrite is not set true.";
          break;
        }
      }
      File file = SPIFFS.open(filename, "w");
      if(!file) {
        errCode = 500;
        errStr = "Error: Could not open file for writing.";
        break;
      }
      String patstr;
      serializeJson(patterns, patstr);
      file.print(patstr);
      file.close();
    } while(0);
    if(!errCode) server.send(204, "");
    else {
      Serial.println(errStr);
      server.send(errCode, "text/plain", errStr + "\n");
    }
    return true;
  }

  bool listFiles(ESP8266WebServer& server, HTTPMethod requestMethod, String requestUri) {
    String output;
    Dir root = SPIFFS.openDir("/");  
    while(root.next()){
      Serial.print("FILE: "); Serial.println(root.fileName());
      if(root.fileName().equals("/config.json")) continue;
      output += String("Name: ") + root.fileName() + "\t\tSize: " + root.fileSize() + "\n";
    }
    server.send(200, "text/plain", output);
    return true;
  }


  void loadPatterns(String filename, bool append) {
    _errCode = 200;
    _errString = "Success";
    Serial.println("Loading Patterns into memory...");

    do {
      StaticJsonDocument<4096> tmpdoc;
      bool error = false;
      
      if(loadJSONFile(filename.c_str(), tmpdoc) != 0) {
        Serial.println("error loading upload data into JSON");
        _errCode = 400;
        _errString = "File format error";
        error = true;
        break;
      }
      
      JsonArray ja = tmpdoc.as<JsonArray>();
      if(ja.isNull()) {
        Serial.println("Failed to cast document to JSON Array.");
        _errCode = 400;
        _errString = "File format error";
        error = true;
        break;
      }

      PatternState tmpState;
      int elemcount = ja.size();
      Serial.print(elemcount); Serial.println(" patterns found.");
      if(elemcount == 0) {
        _errCode = 400;
        _errString = "Error: no patterns found";
        error = true;
        break;
      }
      
      Serial.print(elemcount); Serial.println(" elements in pattern set.");
      for(int i=0; i < elemcount; i++) {
        tmpState.initialize(i, ja[i].as<JsonObject>());
        if(tmpState.pfunc == NULL) {
          _errCode = 400;
          _errString = "Error: Bad pattern found";
          error = true;
          break;
        }
      }
      if(error == true) break;
      
      Serial.println("New patterns validated. Loading into memory...");
      bool retcode = append ? patterns.as<JsonArray>().add(ja) : patterns.to<JsonArray>().set(ja);
      if(!retcode) {
        _errCode = 500;
        _errString = "Failed to put new elements into patterns";
        error = true;
        break;   
      }
      patternState.initialize(0, patterns.as<JsonArrayConst>()[0].as<JsonObjectConst>());
    } while(0);
    return;
  }

  bool loadFromFile(ESP8266WebServer& server, HTTPMethod requestMethod, String requestUri) {
    String filename = server.arg("filename");
    if(filename.isEmpty()) {
      server.send(400, "text/plain", "Error: Missing filename argument.\n");
      return true;
    }
    if (!filename.startsWith("/")) filename = "/" + filename;
    
    bool append = false;
    String opmode = server.arg("mode"); 
    if(opmode.isEmpty() || opmode == "overwrite") append = false;
    else if(opmode == "append") append = true;
    loadPatterns(filename, append);
    Serial.print(String(_errCode) + ", ");
    Serial.println(_errString);
    server.send(_errCode, "text/plain", _errString + "\n\n");
    return true;
  }

  bool printFile(ESP8266WebServer& server, HTTPMethod requestMethod, String requestUri) {
    String filename = server.arg("filename");
    
    if(filename.isEmpty()) {
      server.send(400, "text/plain", "Error: filename argument required.");
      return true;
    }
    if (!filename.startsWith("/")) filename = "/" + filename;
    if (SPIFFS.exists(filename)) {
      File file = SPIFFS.open(filename, "r");
      size_t sent = server.streamFile(file, "application/json");
      file.close();
    } else http_notfound();
    return true;
  }

  bool processLoad(ESP8266WebServer& server, HTTPMethod requestMethod, String requestUri) {
    Serial.println("processLoad called.");
    
    if(_uploadInProgress) {
      _uploadInProgress = false;
      Serial.println("Processing File Upload data...");
    } else if(server.hasArg("plain")) {
      Serial.println("Processing Argument Upload data...");
      if(SPIFFS.exists(UPLOAD_BUF)) SPIFFS.remove(UPLOAD_BUF);
      File buf = SPIFFS.open(UPLOAD_BUF, "w");
      if(buf) {
        buf.println(server.arg("plain"));
        buf.close();
      } else {
        Serial.println("Error! Failed to write to temp buffer.");
      }
    } else {
      Serial.println("No file upload, and argument data missing as well.");
      server.send(400, "text/plain", "Data missing\n");
      return true;
    }
    
    bool append = false;
    String opmode = server.arg("mode"); 
    if(opmode.isEmpty() || opmode == "overwrite") append = false;
    else if(opmode == "append") append = true;
    loadPatterns(UPLOAD_BUF, append);
    
    Serial.print(String(_errCode) + ", ");
    Serial.println(_errString);
    server.send(_errCode, "text/plain", _errString + "\n\n");
    return true;
  }

  public:
  
  bool canHandle(HTTPMethod method, String uri) {
    Serial.print("canHandle called for URI: "); Serial.println(uri);
    return uri != NULL && uri.startsWith("/pattern/");
  }

  bool canUpload(String uri) {
    Serial.print("canUpload called for URI: "); Serial.println(uri);
    return uri != NULL && uri.equals("/pattern/load");
  }

  void upload(ESP8266WebServer& server, String uri, HTTPUpload& upload) {
    Serial.println("Upload Function called.");
    printArgs(server);
    printHeaders(server);
    if(upload.status == UPLOAD_FILE_START) {
      Serial.println("Got File Start");
    } else if(upload.status == UPLOAD_FILE_WRITE){
      Serial.println("Got File Write");

      if(!_uploadInProgress == true) {
        if(SPIFFS.exists(UPLOAD_BUF)) SPIFFS.remove(UPLOAD_BUF);
        _buf = SPIFFS.open(UPLOAD_BUF, "w");
        _uploadInProgress = true;
      }
      if(_buf) {
        _buf.write(upload.buf, upload.currentSize);

      } else {
        Serial.println("Error! Failed to write to temp file.");
      }
    } else if(upload.status == UPLOAD_FILE_END){
      Serial.println("Got File End");
      Serial.println("\n Total Size: "); Serial.println(upload.totalSize);
      _buf.close();
    }
  }
  
  bool handle(ESP8266WebServer& server, HTTPMethod requestMethod, String requestUri) {
    Serial.print("Received Request: ");
    Serial.println(requestUri);
    
    if(requestUri.equals("/pattern/show")) showPatterns(server, requestMethod, requestUri);
    else if(requestUri.equals("/pattern/save")) saveCurrent(server, requestMethod, requestUri);     
    else if(requestUri.equals("/pattern/list")) listFiles(server, requestMethod, requestUri);
    else if(requestUri.equals("/pattern/load")) processLoad(server, requestMethod, requestUri);
    else if(requestUri.equals("/pattern/open")) loadFromFile(server, requestMethod, requestUri);
    else if(requestUri.equals("/pattern/print")) printFile(server, requestMethod, requestUri);
    else http_notfound();
    Serial.println("Finished Handler.");
    return true;
  }
  
} patternHandler;

// ################################## SPIFFS ############################################

int loadJSONFile(const char *filename, JsonDocument& doc) {
  int retcode = 1;
  do {
    if (!spiffsActive) break;
    retcode++;
    if (!SPIFFS.exists(filename)) break;
    retcode++;
    File f = SPIFFS.open(filename, "r");
    if (!f) break;
    retcode++;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    if(e != DeserializationError::Ok) {
      Serial.println(e.c_str());
      break;
    }
    retcode = 0;
  } while(0);
  return retcode;
}

// #######################################################################################

void setup() {
  randomSeed(analogRead(0));
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting...");
  if (SPIFFS.begin()) {
    spiffsActive = true;
  } else Serial.println("Unable to activate SPIFFS. This means no wifi/webserver, and default strip settings.");
  delay(2000);

  StaticJsonDocument<512> cfg;
  if(loadJSONFile(CFGFILE, cfg) != 0) {
    Serial.println("Load Config failed. This means no wifi/webserver, and default strip settings.");
  } else {
    int pin = cfg["strip"]["gpio"].as<unsigned int>();
    if(pin != STRIP_DEFAULT_PIN) strip.setPin(pin);
    
    neoPixelType striptype = getStripType(cfg["strip"]["layout"].as<String>(), cfg["strip"]["speed"].as<unsigned int>());
    Serial.print("Strip Type: "); Serial.println(striptype, BIN);
    if(striptype != STRIP_DEFAULT_LAYOUT + STRIP_DEFAULT_SPEED) strip.updateType(striptype);
    
    int numpixels = cfg["strip"]["pixels"].as<unsigned int>();
    if(numpixels != STRIP_DEFAULT_LEN) strip.updateLength(numpixels);
      
    const char *ssid = cfg["wifi"]["ssid"];
    const char *pass = cfg["wifi"]["pass"];
    int www_port = cfg["wifi"]["www_port"].as<unsigned int>();
    
    WiFi.begin(ssid, pass);
    while(WiFi.status() != WL_CONNECTED) {
      digitalWrite(LED, LOW); //ON
      delay(250);
      digitalWrite(LED, HIGH); //OFF
      delay(250);
    }
    WiFi.setAutoReconnect(1);
    digitalWrite(LED, LOW); // ON
    Serial.println("\nConnection established!");  
    Serial.print("IP address:\t");
    Serial.println(WiFi.localIP());
  
    ArduinoOTA.begin();
  
    Serial.print("About to start Web server on port ");
    Serial.println(String(www_port));
    webserver = new ESP8266WebServer(www_port);
    webserver->on("/", http_root);
    webserver->on("/configure", http_configure);
    webserver->addHandler(&cmdHandler);
    webserver->addHandler(&patternHandler);
    webserver->onNotFound(http_notfound);
    webserver->begin();
    wifiOK = true;
  }
  strip.begin();
  strip.show();
  
  if(loadJSONFile(PATTERNFILE, patterns) != 0) {
    Serial.println("Load patterns failed");
    patternState.pauserun = true;
  }
  else patternState.initialize(0, patterns.as<JsonArrayConst>()[0].as<JsonObjectConst>());

  Serial.println("Setup complete.");
}

// ####################################################################################

void loop() {
  // put your main code here, to run repeatedly:
  
  if(needReset) {
    Serial.println("Reset required...");
    delay(2000);
    resetFunc();
  }
  
  if(wifiOK) {
    ArduinoOTA.handle();
    webserver->handleClient();
  }
  
  unsigned long now = millis();
  if(patternState.pauserun || now < patternState.lastActionTime + patternState.rundelay) return;
  int done = 1;
  if(patternState.pfunc != NULL) done = (*(patternState.pfunc))();
  if(done == 1 && patternState.nextloop() < 0) {
    patternState.nextPattern();
  }
  patternState.lastActionTime = millis();
}
