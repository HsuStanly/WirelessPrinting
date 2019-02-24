// Required: https://github.com/greiman/SdFat

#include <ArduinoOTA.h>
#if defined(ESP8266)
  #include <ESP8266mDNS.h>        // https://github.com/esp8266/Arduino/tree/master/libraries/ESP8266mDNS
#elif defined(ESP32)
  #include <ESPmDNS.h>
#endif
#include <ArduinoJson.h>          // https://github.com/bblanchon/ArduinoJson (for implementing a subset of the OctoPrint API)
#include <DNSServer.h>
#include "StorageFS.h"
#include <ESPAsyncWebServer.h>    // https://github.com/me-no-dev/ESPAsyncWebServer
#include <ESPAsyncWiFiManager.h>  // https://github.com/alanswx/ESPAsyncWiFiManager/
#include <SPIFFSEditor.h>

#include "CommandQueue.h"

WiFiServer telnetServer(23);
WiFiClient serverClient;

AsyncWebServer server(80);
DNSServer dns;

// Configurable parameters
#define SKETCH_VERSION "2.0"
#define USE_FAST_SD                     // Use Default fast SD clock, comment if your SD is an old or slow one.
#define OTA_UPDATES                     // Enable OTA firmware updates, comment if you don't want it (OTA may lead to security issues because someone may load any code on device)
//#define OTA_PASSWORD ""               // Remove '//' to protect OTA updates
#define MAX_SUPPORTED_EXTRUDERS 6       // Number of supported extruder

#define PRINTER_RX_BUFFER_SIZE 0        // This is printer firmware 'RX_BUFFER_SIZE'. If such parameter is unknown please use 0
#define TEMPERATURE_REPORT_INTERVAL 2   // Ask the printer for its temperatures status every 2 seconds
#define KEEPALIVE_INTERVAL 2500         // Marlin defaults to 2 seconds, get a little of margin
const uint32_t serialBauds[] = { 1000000, 500000, 250000, 115200, 57600 };   // Marlin valid bauds (removed very low bauds)

#define API_VERSION     "0.1"
#define VERSION         "1.3.10"

// Information from M115
String fwMachineType = "Unknown";
uint8_t fwExtruders = 1;
bool fwAutoreportTempCap, fwProgressCap, fwBuildPercentCap;

// Printer status
bool printerConnected,
     startPrint,
     isPrinting,
     printPause,
     restartPrint,
     cancelPrint,
     autoreportTempEnabled;

uint32_t printStartTime;
float printCompletion;

// Serial communication
#define GotValidResponse() { \
  lastReceivedResponse = serialResponse; \
  lineStartPos = 0; \
  serialResponse = ""; \
}

String lastCommandSent, lastReceivedResponse;
uint32_t lastPrintedLine;

uint8_t serialBaudIndex;
uint16_t printerUsedBuffer;
uint32_t serialReceiveTimeoutTimer;

// Uploaded file information
String uploadedFullname;
size_t uploadedFileSize, filePos;
uint32_t uploadedFileDate = 1378847754;

// Temperature for printer status reporting
#define AUTOTEMP_COMMAND "M155 S"

struct Temperature {
  String actual, target;
};

uint32_t temperatureTimer;

Temperature toolTemperature[MAX_SUPPORTED_EXTRUDERS];
Temperature bedTemperature;


// https://forum.arduino.cc/index.php?topic=228884.msg2670971#msg2670971
inline String IpAddress2String(const IPAddress& ipAddress) {
  return String(ipAddress[0]) + "." +
         String(ipAddress[1]) + "." +
         String(ipAddress[2]) + "." +
         String(ipAddress[3]);
}

inline void setLed(const bool status) {
  digitalWrite(LED_BUILTIN, status ? LOW : HIGH);   // Note: LOW turn the LED on
}

inline void telnetSend(const String line) {
  if (serverClient && serverClient.connected())     // send data to telnet client if connected
    serverClient.println(line);
}

// Parse temperatures from printer responses like
// ok T:32.8 /0.0 B:31.8 /0.0 T0:32.8 /0.0 @:0 B@:0
bool parseTemp(const String response, const String whichTemp, Temperature *temperature) {
  int tpos = response.indexOf(whichTemp + ":");
  if (tpos != -1) { // This response contains a temperature
    int slashpos = response.indexOf(" /", tpos);
    int spacepos = response.indexOf(" ", slashpos + 1);
    // if match mask T:xxx.xx /xxx.xx
    if (slashpos != -1 && spacepos - tpos < 17) {
      temperature->actual = response.substring(tpos + whichTemp.length() + 1, slashpos);
      temperature->target = response.substring(slashpos + 2, spacepos);

      return true;
    }
  }

  return false;
}

bool parseTemperatures(const String response) {
  bool tempResponse;

  if (fwExtruders == 1)
    tempResponse = parseTemp(response, " T", &toolTemperature[0]);
  else {
    tempResponse = false;
    for (int t = 0; t < fwExtruders; t++)
      tempResponse |= parseTemp(response, " T" + String(t), &toolTemperature[t]);
  }
  tempResponse |= parseTemp(response, " B", &bedTemperature);

  return tempResponse;
}

inline void lcd(const String text) {
  commandQueue.push("M117 " + text);
}

inline void playSound() {
  commandQueue.push("M300 S500 P50");
}

inline String getUploadedFilename() {
  return uploadedFullname == "" ? "Unknown" : uploadedFullname.substring(1);
}

void handlePrint() {
  static FileWrapper gcodeFile;
  static float prevM73Completion, prevM532Completion;

  if (isPrinting) {
    const bool abortPrint = (restartPrint || cancelPrint);
    if (abortPrint || !gcodeFile.available()) {
      gcodeFile.close();
      if (fwProgressCap)
        commandQueue.push("M530 S0");
      if (!abortPrint)
        lcd("Complete");
      printPause = false;
      isPrinting = false;
    }
    else if (!printPause && commandQueue.getFreeSlots() > 4) {    // Keep some space for "service" commands
      ++lastPrintedLine;
      String line = gcodeFile.readStringUntil('\n'); // The G-Code line being worked on
      filePos += line.length();
      int pos = line.indexOf(';');
      if (line.length() > 0 && pos != 0 && line[0] != '(' && line[0] != '\r') {
        if (pos != -1)
          line = line.substring(0, pos);
        commandQueue.push(line);
      }

      // Send to printer completion (if supported)
      printCompletion = (float)filePos / uploadedFileSize * 100;
      if (fwBuildPercentCap && printCompletion - prevM73Completion >= 1) {
        commandQueue.push("M73 P" + String((int)printCompletion));
        prevM73Completion = printCompletion;
      }
      if (fwProgressCap && printCompletion - prevM532Completion >= 0.1) {
        commandQueue.push("M532 X" + String((int)(printCompletion * 10) / 10.0));
        prevM532Completion = printCompletion;
      }
    }
  }

  if (!isPrinting && (startPrint || restartPrint)) {
    startPrint = restartPrint = false;

    filePos = 0;
    lastPrintedLine = 0;
    prevM73Completion = prevM532Completion = 0.0;

    gcodeFile = storageFS.open(uploadedFullname);
    if (!gcodeFile)
      lcd("Can't open file");
    else {
      lcd("Printing...");
      playSound();
      printStartTime = millis();
      isPrinting = true;
      if (fwProgressCap) {
        commandQueue.push("M530 S1 L0");
        commandQueue.push("M531 " + getUploadedFilename());
      }
    }
  }
}

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  static FileWrapper file;

  if (!index) {
    lcd("Receiving...");

    if (uploadedFullname != "")
      storageFS.remove(uploadedFullname);     // Remove previous file
    int pos = filename.lastIndexOf("/");
    uploadedFullname = pos == -1 ? "/" + filename : filename.substring(pos);
    if (uploadedFullname.length() > storageFS.getMaxPathLength())
      uploadedFullname = "/cached.gco";   // TODO maybe a different solution
    file = storageFS.open(uploadedFullname, "w"); // create or truncate file
  }

  file.write(data, len);

  if (final) { // upload finished
    file.close();
    uploadedFileSize = index + len;
  }
  else
    uploadedFileSize = 0;
}

int apiPrinterCommandHandler(const uint8_t* data) {
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(data);
  if (root.success()) {
    if (root.containsKey("command")) {
      telnetSend(root["command"]);
      String command = root["command"].asString();
      commandQueue.push(command);
    }
  }
  else if (root.containsKey("commands")) {
    JsonArray& node = root["commands"];
    for (JsonArray::iterator item = node.begin(); item != node.end(); ++item)
      commandQueue.push(item->as<char*>());
  }

  return 204;
}

// Job commands http://docs.octoprint.org/en/master/api/job.html#issue-a-job-command
int apiJobHandler(const uint8_t* data) {
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(data);
  if (root.success() && root.containsKey("command")) {
    telnetSend(root["command"]);
    String command = root["command"].asString();
    if (command == "cancel") {
      if (!isPrinting)
        return 409;
      cancelPrint = true;
    }
    else if (command == "start") {
      if (isPrinting || !printerConnected || uploadedFullname == "")
        return 409;
      startPrint = true;
    }
    else if (command == "restart") {
      if (!printPause)
        return 409;
      restartPrint = true;
    }
    else if (command == "pause") {
      if (!isPrinting)
        return 409;
      if (!root.containsKey("action"))
        printPause = !printPause;
      else {
        telnetSend(root["action"]);
        String action = root["action"].asString();
        if (action == "pause")
          printPause = true;
        else if (action == "resume")
          printPause = false;
        else if (action == "toggle")
          printPause = !printPause;
      }
    }
  }

  return 204;
}

String M115ExtractString(const String response, const String field) {
  int spos = response.indexOf(field);

  if (spos != -1) {
    spos += field.length();
    if (response[spos] == ':')    // pre Marlin 1.1.8 compatibility (don't have ":" after field)
      ++spos;


    int epos = response.indexOf(':', spos);
    if (epos == -1)
      epos = response.indexOf('\n', spos);
    if (epos == -1)
      return response.substring(spos);
    else {
      while (epos >= spos && response[epos] != ' ' && response[epos] != '\n')
        --epos;
      return response.substring(spos, epos);
    }
  }

  return "";
}

bool M115ExtractBool(const String response, const String field, const bool onErrorValue = false) {
  String result = M115ExtractString(response, field);

  return result == "" ? onErrorValue : (result == "1" ? true : false);
}

inline String getDeviceName() {
  return fwMachineType + " (" + String(ESP.getChipId(), HEX) + ")";
}

void mDNSInit() {
  #ifdef OTA_UPDATES
    MDNS.setInstanceName(getDeviceName().c_str());    // Can't call MDNS.init because it has been already done by 'ArduinoOTA.begin', here I just change instance name
  #else
    if (!MDNS.begin(getDeviceName().c_str()))
      return;
  #endif

  // For Cura WirelessPrint - deprecated in favor of the OctoPrint API
  MDNS.addService("wirelessprint", "tcp", 80);
  MDNS.addServiceTxt("wirelessprint", "tcp", "version", SKETCH_VERSION);

  // OctoPrint API
  // Unfortunately, Slic3r doesn't seem to recognize it
  MDNS.addService("octoprint", "tcp", 80);
  MDNS.addServiceTxt("octoprint", "tcp", "path", "/");
  MDNS.addServiceTxt("octoprint", "tcp", "api", API_VERSION);
  MDNS.addServiceTxt("octoprint", "tcp", "version", VERSION);

  // For compatibility with Slic3r
  // Unfortunately, Slic3r doesn't seem to recognize it either. Library bug?
  MDNS.addService("http", "tcp", 80);
  MDNS.addServiceTxt("http", "tcp", "path", "/");
  MDNS.addServiceTxt("http", "tcp", "api", API_VERSION);
  MDNS.addServiceTxt("http", "tcp", "version", VERSION);
}

bool detectPrinter() {
  static int printerDetectionState;

  switch (printerDetectionState) {
    case 0:
      // Start printer detection
      serialBaudIndex = 0;
      printerDetectionState = 10;
      break;

    case 10:
      // Initialize baud and send a request to printezr
      Serial.begin(serialBauds[serialBaudIndex]);
      telnetSend("Connecting at " + String(serialBauds[serialBaudIndex]));
      commandQueue.push("M115"); // M115 - Firmware Info
      commandQueue.push("M115"); // M115 - Send it al least twice
      printerDetectionState = 20;
      break;

    case 20:
      // Check if there is a printer response
      if (commandQueue.isEmpty()) {
        String value = M115ExtractString(lastReceivedResponse, "MACHINE_TYPE");
        if (value == "") {
          ++serialBaudIndex;
          if (serialBaudIndex < sizeof(serialBauds) / sizeof(serialBauds[0]))
            printerDetectionState = 10;
          else
            printerDetectionState = 0;
        }
        else {
          telnetSend("Connected");

          fwMachineType = value;
          value = M115ExtractString(lastReceivedResponse, "EXTRUDER_COUNT");
          fwExtruders = value == "" ? 1 : min(value.toInt(), (long)MAX_SUPPORTED_EXTRUDERS);
          fwAutoreportTempCap = M115ExtractBool(lastReceivedResponse, "Cap:AUTOREPORT_TEMP");
          fwProgressCap = M115ExtractBool(lastReceivedResponse, "Cap:PROGRESS");
          fwBuildPercentCap = M115ExtractBool(lastReceivedResponse, "Cap:BUILD_PERCENT");

          mDNSInit();

          String text = IpAddress2String(WiFi.localIP()) + " " + storageFS.getActiveFS();
          lcd(text);
          playSound();

          if (fwAutoreportTempCap)
            commandQueue.push(AUTOTEMP_COMMAND + String(TEMPERATURE_REPORT_INTERVAL));   // Start auto report temperatures
          else
            temperatureTimer = millis();

          return true;
        }
      }
      break;
  }

  return false;
}

void initUploadedFilename() {
  FileWrapper dir = storageFS.open("/");
  if (dir) {
    FileWrapper file = dir.openNextFile();
    while (file && file.isDirectory()) {
      file.close();
      file = dir.openNextFile();
    }
    if (file) {
      uploadedFullname = "/" + file.name();
      uploadedFileSize = file.size();
      file.close();
    }
    dir.close();
  }
}

inline String getState() {
  return !printerConnected ? "Discovering printer" : (isPrinting ? "Printing" : "Operational");
}

inline String stringify(bool value) {
  return value ? "true" : "false";
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);     // Initialize the LED_BUILTIN pin as an output

  #ifdef USE_FAST_SD
    storageFS.begin(true);
  #else
    storageFS.begin(false);
  #endif

  for (int t = 0; t < MAX_SUPPORTED_EXTRUDERS; t++)
    toolTemperature[t] = { "0.0", "0.0" };
  bedTemperature = { "0.0", "0.0" };

  // Wait for connection
  setLed(true);
  AsyncWiFiManager wifiManager(&server, &dns);
  // wifiManager.resetSettings();   // Uncomment this to reset the settings on the device, then you will need to reflash with USB and this commented out!
  wifiManager.setDebugOutput(false);  // So that it does not send stuff to the printer that the printer does not understand
  wifiManager.autoConnect("AutoConnectAP");
  setLed(false);

  telnetServer.begin();
  telnetServer.setNoDelay(true);

  if (storageFS.activeSPIFFS())
    server.addHandler(new SPIFFSEditor());

  initUploadedFilename();

  server.onNotFound([](AsyncWebServerRequest * request) {
    telnetSend("404 | Page '" + request->url() + "' not found\r\n");
    request->send(404, "text/html", "<h1>Page not found!</h1>");
  });

  server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest * request) {
    // http://docs.octoprint.org/en/master/api/version.html
    request->send(200, "application/json", "{\r\n"
                                           "  \"api\": \"" API_VERSION "\",\r\n"
                                           "  \"server\": \"" VERSION "\"\r\n"
                                           "}");  });

  server.on("/api/connection", HTTP_GET, [](AsyncWebServerRequest * request) {
    // http://docs.octoprint.org/en/master/api/connection.html#get-connection-settings
    request->send(200, "application/json", "{\r\n"
                                           "  \"current\": {\r\n"
                                           "    \"state\": \"" + getState() + "\",\r\n"
                                           "    \"port\": \"Serial\",\r\n"
                                           "    \"baudrate\": " + serialBauds[serialBaudIndex] + ",\r\n"
                                           "    \"printerProfile\": \"Default\"\r\n"
                                           "  },\r\n"
                                           "  \"options\": {\r\n"
                                           "    \"ports\": \"Serial\",\r\n"
                                           "    \"baudrate\": " + serialBauds[serialBaudIndex] + ",\r\n"
                                           "    \"printerProfiles\": \"Default\",\r\n"
                                           "    \"portPreference\": \"Serial\",\r\n"
                                           "    \"baudratePreference\": " + serialBauds[serialBaudIndex] + ",\r\n"
                                           "    \"printerProfilePreference\": \"Default\",\r\n"
                                           "    \"autoconnect\": true\r\n"
                                           "  }\r\n"
                                           "}");
  });

  // Todo: http://docs.octoprint.org/en/master/api/connection.html#post--api-connection

  // Main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    String message = "<h1>" + getDeviceName() + "</h1>"
                     "<form enctype=\"multipart/form-data\" action=\"/api/files/local\" method=\"POST\">\n"
                     "<p>You can also print from the command line using curl:</p>\n"
                     "<pre>curl -F \"file=@\\\"/path/to/some.gcode\\\";print=true\" " + IpAddress2String(WiFi.localIP()) + "/api/files/local</pre>\n"
                     "Choose a file to upload: <input name=\"file\" type=\"file\"/><br/>\n"
                     "<input type=\"submit\" value=\"Upload\" />\n"
                     "</form>"
                     "<p><a href=\"/download\">Download</a></p>"
                     "<p><a href=\"/info\">Info</a></p>";
    request->send(200, "text/html", message);
  });

  // Info page
  server.on("/info", HTTP_GET, [](AsyncWebServerRequest * request) {
    String message = "<pre>"
                     "Free heap: " + String(ESP.getFreeHeap()) + "\n\n"
                     "File system: " + storageFS.getActiveFS() + "\n";
    if (storageFS.isActive()) {
      message += "Filename length limit: " + String(storageFS.getMaxPathLength()) + "\n";
      if (uploadedFullname != "") {
        message += "Uploaded file: " + getUploadedFilename() + "\n"
                   "Uploaded file size: " + String(uploadedFileSize) + "\n";
      }
    }
    message += "\n"
               "Last command sent: " + lastCommandSent + "\n"
               "Last received response: " + lastReceivedResponse + "\n";
    if (printerConnected) {
      message += "\n"
                 "EXTRUDER_COUNT: " + String(fwExtruders) + "\n"
                 "AUTOREPORT_TEMP: " + stringify(fwAutoreportTempCap) + "\n"
                 "PROGRESS: " + stringify(fwProgressCap) + "\n"
                 "BUILD_PERCENT: " + stringify(fwBuildPercentCap) + "\n";
    }
    message += "</pre>";
    request->send(200, "text/html", message);
  });

  // File Operations
  //server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest * request) {
  //  Pending: http://docs.octoprint.org/en/master/api/files.html#retrieve-all-files
  //  request->send(200, "application/json", "{\r\n"
  //                                         "  \"files\": {\r\n"
  //                                         "  }\r\n"
  //                                         "}");
  //});

  // For Slic3r OctoPrint compatibility
  server.on("/api/files/local", HTTP_POST, [](AsyncWebServerRequest * request) {
    // https://docs.octoprint.org/en/master/api/files.html?highlight=api%2Ffiles%2Flocal#upload-file-or-create-folder
    lcd("Received");
    playSound();

    if (request->hasParam("print", true))
      startPrint = printerConnected && !isPrinting && uploadedFullname != "";

    request->send(200, "application/json", "{\r\n"
                                           "  \"files\": {\r\n"
                                           "    \"local\": {\r\n"
                                           "      \"name\": \"" + getUploadedFilename() + "\",\r\n"
                                           "      \"origin\": \"local\"\r\n"
                                           "    }\r\n"
                                           "  },\r\n"
                                           "  \"done\": true\r\n"
                                           "}");
  }, handleUpload);

  server.on("/api/job", HTTP_GET, [](AsyncWebServerRequest * request) {
    // http://docs.octoprint.org/en/master/api/job.html#retrieve-information-about-the-current-job
    int printTime = 0, printTimeLeft = 0;
    if (isPrinting) {
      printTime = (millis() - printStartTime) / 1000;
      printTimeLeft = printTimeLeft / printCompletion * (100 - printCompletion);
    }
    request->send(200, "application/json", "{\r\n"
                                           "  \"job\": {\r\n"
                                           "    \"file\": {\r\n"
                                           "      \"name\": \"" + getUploadedFilename() + "\",\r\n"
                                           "      \"origin\": \"local\",\r\n"
                                           "      \"size\": " + String(uploadedFileSize) + ",\r\n"
                                           "      \"date\": " + String(uploadedFileDate) + "\r\n"
                                           "    },\r\n"
                                           //"    \"estimatedPrintTime\": \"" + estimatedPrintTime + "\",\r\n"
                                           "    \"filament\": {\r\n"
                                           //"      \"length\": \"" + filementLength + "\",\r\n"
                                           //"      \"volume\": \"" + filementVolume + "\"\r\n"
                                           "    }\r\n"
                                           "  },\r\n"
                                           "  \"progress\": {\r\n"
                                           "    \"completion\": " + String(printCompletion) + ",\r\n"
                                           "    \"filepos\": " + String(filePos) + ",\r\n"
                                           "    \"printTime\": " + String(printTime) + ",\r\n"
                                           "    \"printTimeLeft\": " + String(printTimeLeft) + "\r\n"
                                           "  }\r\n"
                                           "}");
  });

  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest * request) {
    // https://github.com/probonopd/WirelessPrinting/issues/30
    // https://github.com/probonopd/WirelessPrinting/issues/18#issuecomment-321927016
    request->send(200, "application/json", "{}");
  });

  server.on("/api/printer", HTTP_GET, [](AsyncWebServerRequest * request) {
    // https://docs.octoprint.org/en/master/api/printer.html#retrieve-the-current-printer-state
    String sdReadyState = stringify(storageFS.activeSD());   //  This should request SD status to the printer
    String readyState = stringify(printerConnected);
    String message = "{\r\n"
                     "  \"state\": {\r\n"
                     "    \"text\": \"" + getState() + "\",\r\n"
                     "    \"flags\": {\r\n"
                     "      \"operational\": " + readyState + ",\r\n"
                     "      \"paused\": " + stringify(printPause) + ",\r\n"
                     "      \"printing\": " + stringify(isPrinting) + ",\r\n"
                     "      \"pausing\": false,\r\n"
                     "      \"cancelling\": " + stringify(cancelPrint) + ",\r\n"
                     "      \"sdReady\": " + sdReadyState + ",\r\n"
                     "      \"error\": false,\r\n"
                     "      \"ready\": " + readyState + ",\r\n"
                     "      \"closedOrError\": false\r\n"
                     "    }\r\n"
                     "  },\r\n"
                     "  \"temperature\": {\r\n";
    for (int t = 0; t < fwExtruders; t++) {
      message += "    \"tool" + String(t) + "\": {\r\n"
                 "      \"actual\": " + toolTemperature[t].actual + ",\r\n"
                 "      \"target\": " + toolTemperature[t].target + ",\r\n"
                 "      \"offset\": 0\r\n"
                 "    },\r\n";
    }
    message += "    \"bed\": {\r\n"
               "      \"actual\": " + bedTemperature.actual + ",\r\n"
               "      \"target\": " + bedTemperature.target + ",\r\n"
               "      \"offset\": 0\r\n"
               "    }\r\n"
               "  },\r\n"
               "  \"sd\": {\r\n"
               "    \"ready\": " + sdReadyState + "\r\n"
               "  }\r\n"
               "}";
    request->send(200, "application/json", message);
  });

  // Parse POST JSON data, https://github.com/me-no-dev/ESPAsyncWebServer/issues/195
  server.onRequestBody([](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {

    int returnCode;
    if (request->url() == "/api/printer/command") {
      // http://docs.octoprint.org/en/master/api/printer.html#send-an-arbitrary-command-to-the-printer
      returnCode = apiPrinterCommandHandler(data);
    }
    else if (request->url() == "/api/job") {
      // http://docs.octoprint.org/en/master/api/job.html
      returnCode = apiJobHandler(data);
    }
    else
      returnCode = 204;

    request->send(returnCode);
  });

  // For legacy PrusaControlWireless - deprecated in favor of the OctoPrint API
  server.on("/print", HTTP_POST, [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", "Received");
  }, handleUpload);

  // For legacy Cura WirelessPrint - deprecated in favor of the OctoPrint API
  server.on("/api/print", HTTP_POST, [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", "Received");
  }, handleUpload);

  server.begin();

  #ifdef OTA_UPDATES
    // OTA setup
    ArduinoOTA.setHostname(getDeviceName().c_str());
    #ifdef OTA_PASSWORD
      ArduinoOTA.setPassword(OTA_PASSWORD);
    #endif
    ArduinoOTA.begin();
  #endif
}

void loop() {
  #ifdef OTA_UPDATES
    //****************
    //* OTA handling *
    //****************
    ArduinoOTA.handle();
  #endif


  //********************
  //* Printer handling *
  //********************
  if (!printerConnected)
    printerConnected = detectPrinter();
  else {
    #ifndef OTA_UPDATES
      MDNS.update();    // When OTA is active it's called by 'handle' method
    #endif

    handlePrint();

    if (cancelPrint && !isPrinting) { // Only when cancelPrint has been processed by 'handlePrint'
      cancelPrint = false;
      commandQueue.clear();
      printerUsedBuffer = 0;
      // Apparently we need to decide how to handle this
      // For now using M112 - Emergency Stop
      // http://marlinfw.org/docs/gcode/M112.html
      telnetSend("Should cancel print! This is not working yet");
      commandQueue.push("M112"); // Send to 3D Printer immediately w/o waiting for anything
      //playSound();
      //lcd("Print cancelled");
    }

    if (!autoreportTempEnabled) {
      unsigned long curMillis = millis();
      if (curMillis - temperatureTimer >= TEMPERATURE_REPORT_INTERVAL * 1000) {
        commandQueue.push("M105");
        temperatureTimer = curMillis;
      }
    }
  }

  SendCommands();
  ReceiveResponses();


  //*******************
  //* Telnet handling *
  //*******************
  // look for Client connect trial
  if (telnetServer.hasClient() && (!serverClient || !serverClient.connected())) {
    if (serverClient)
      serverClient.stop();

    serverClient = telnetServer.available();
    serverClient.flush();  // clear input buffer, else you get strange characters
  }

  while (serverClient && serverClient.available())  // get data from Client
    Serial.write(serverClient.read());
}

inline uint32_t restartSerialTimeout(uint16_t timeout) {
  serialReceiveTimeoutTimer = millis() + timeout;
}

void SendCommands() {
  String command = commandQueue.peekSend();  //gets the next command to be sent
  if (command != "") {
    bool noResponsePending = commandQueue.isAckEmpty();
    if (noResponsePending || printerUsedBuffer < PRINTER_RX_BUFFER_SIZE * 3 / 4) {  // Let's use no more than 75% of printer RX buffer
      if (noResponsePending)
        restartSerialTimeout(KEEPALIVE_INTERVAL);   // Receive timeout has to be reset only when sending a command and no pending response is expected
      Serial.println(command);          // Send to 3D Printer
      printerUsedBuffer += command.length();
      lastCommandSent = command;
      commandQueue.popSend();

      telnetSend("> " + command);
    }
  }
}

void ReceiveResponses() {
  static int lineStartPos;
  static String serialResponse;

  while (Serial.available()) {
    char ch = (char)Serial.read();
    serialResponse += ch;
    restartSerialTimeout(500);    // Once a char is received timeout may be shorter
    if (ch == '\n') {
      if (serialResponse.startsWith("ok", lineStartPos)) {
        GotValidResponse();
        commandAcknowledged();
        telnetSend("< " + lastReceivedResponse + "\r\n  " + millis() + "\r\n  free heap RAM: " + ESP.getFreeHeap() + "\r\n");

        autoreportTempEnabled |= (fwAutoreportTempCap && lastCommandSent.startsWith(AUTOTEMP_COMMAND) && lastCommandSent[6] != '0');
      }
      else if (autoreportTempEnabled && parseTemperatures(serialResponse)) {
        GotValidResponse();
        telnetSend("< AutoReportTemps parsed");
      }
      else if (serialResponse.startsWith("echo:busy")) {
        GotValidResponse();
        restartSerialTimeout(KEEPALIVE_INTERVAL);
        telnetSend("< Printer is busy, giving it more time");
      }
      else if (serialResponse.startsWith("echo: cold extrusion prevented")) {
        GotValidResponse();
        // To do: Pause sending gcode, or do something similar
        telnetSend("< Printer is cold, can't move");
      }
      else if (serialResponse.startsWith("error")) {
        GotValidResponse();
        telnetSend("< Error Received");
      }
      else {
        lineStartPos = serialResponse.length();
        telnetSend("< New line but nothing to do with it");
      }
    }
  }

  if (!commandQueue.isAckEmpty() && serialReceiveTimeoutTimer - millis() <= 0) {  // Command has been lost by printer, buffer has been freed
    lineStartPos = 0;
    serialResponse = "";
    commandAcknowledged();

    telnetSend("< #TIMEOUT#");
  }
}

inline void commandAcknowledged() {
  unsigned int cmdLen = commandQueue.popAcknowledge().length();
  printerUsedBuffer = max(printerUsedBuffer - cmdLen, 0u);
  restartSerialTimeout(KEEPALIVE_INTERVAL);
}
