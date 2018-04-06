/*
 * Collect Temp and Humidity data to post to a Rasp Pi data collector
 * 
 * Use ESP_forgetall to reset the configuration and start from scratch
 */
 
#include <FS.h>                   //this needs to be first, or it all crashes and burns...

#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino
#include <ESP8266mDNS.h>          // Needed for ArduinoOTA library

//needed for WifiManager library
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager

#include <WiFiUdp.h>
#include <ArduinoOTA.h>           // Allow over the air updates

#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

#include <WEMOS_DHT12.h>          //https://github.com/wemos/WEMOS_DHT12_Arduino_Library

DHT12 dht12;

const char *CONFIG_FILE = "/config.json";       // File to contain config values collected during wifi setup
const unsigned long MS_BETWEEN_READS = 900000;  // 15 min between reads
//const unsigned long MS_BETWEEN_READS = 1000;  // Every second for debugging

const unsigned long MS_BETWEEN_LOOPS = 100;     // Delay Time between loops

const uint16_t DC_SERVER_PORT = 80;             // Standard HTTP Port
const char *DC_SERVER_URI = "/dc/tandh.cgi";    // Page to post the data to

const char *PASSWORD = "beaconpass";            // Password to use for Access Point and OTA

/*
 * Parameters collected during Wifi Setup
 * Parameters are stored on the internal File System (FS.h) at config.json
 */
char dc_server_host[60];
char beacon_name[60];

char ap_name[60];
char ota_name[60];

unsigned long lastReportTime = 0;

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setup() {
  // init serial for debugging
  Serial.begin(115200);
  Serial.println("Temperature and Humidity Beacon starting...");
  wifi_set_sleep_type(NONE_SLEEP_T);
  //Local intialization. Once its business is done, there is no need to keep it around

  /*********************************************************
   * Get the Config Parameters from the File System (FS.h)
   *********************************************************/

  //read configuration from FS json
  if (SPIFFS.begin()) {
    Serial.println("mounted FS");
    readConfig();
    setBeaconName(beacon_name);
  }
  else {
    Serial.println("failed to mount FS");
  }


  /************************
   * Set the WiFi Manager
   ************************/
  WiFiManager wifiManager;
  //set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // Setup the extra parameters to be asked for during configuration
  WiFiManagerParameter custom_dc_server_host("dcserver", "Data Collector Server URL", dc_server_host, sizeof(dc_server_host)-1);
  wifiManager.addParameter(&custom_dc_server_host);

  WiFiManagerParameter custom_beacon_name("dcname", "Data Collector Name", beacon_name, sizeof(beacon_name)-1);
  wifiManager.addParameter(&custom_beacon_name);
  
  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //here  "AutoConnectAP"
  //and goes into a blocking loop awaiting configuration
  wifiManager.setConfigPortalTimeout(120);
//  if (!wifiManager.autoConnect("AutoConnectAP", "password")) {
  if (!wifiManager.startConfigPortal(ap_name, PASSWORD)) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }

  //if you get here you have connected to the WiFi
  Serial.print("connected to: ");
  Serial.println(WiFi.SSID());
  Serial.print("    local ip: ");
  Serial.println(WiFi.localIP());
  
  /********************************************************
   * Save the Config Parameters to the File System (FS.h)
   ********************************************************/

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    strncpy(dc_server_host, custom_dc_server_host.getValue(), sizeof(dc_server_host));
    strncpy(beacon_name, custom_beacon_name.getValue(), sizeof(beacon_name));
    saveConfig();
    setBeaconName(beacon_name);
  }
  
  /********************************************************
   * Set for accepting Over The Air (OTA) updates
   ********************************************************/

  ArduinoOTA.setHostname(ota_name);
  ArduinoOTA.setPassword(PASSWORD);
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  
  /********************************************************
   * Ready
   ********************************************************/

  Serial.println();
  Serial.println("Temperature and Humidity Beacon ready...");
  Serial.println();
  
  lastReportTime = 0;
}

void loop() {

  // See if there is an OTA request
  ArduinoOTA.handle();

  unsigned long currTime = millis();
  if (currTime - lastReportTime > MS_BETWEEN_READS) {
    // Need to report
    if(dht12.get()==0){
      // Dump to terminal for debugging
      Serial.print("Temperature in Celsius : ");
      Serial.println(dht12.cTemp);
      Serial.print("Temperature in Fahrenheit : ");
      Serial.println(dht12.fTemp);
      Serial.print("Relative Humidity : ");
      Serial.println(dht12.humidity);
      Serial.println();
  
      // Prep the data
      DynamicJsonBuffer jsonBuffer;
      JsonObject& json = jsonBuffer.createObject();
      json["beacon"] = beacon_name;
      json["celsius"] = dht12.cTemp;
      json["fahrenheit"] = dht12.fTemp;
      json["humidity"] = dht12.humidity;
      String payload;
      json.printTo(payload);
      json.printTo(Serial);
      Serial.println();

      // Post to data collection server
      HTTPClient http;
      http.begin(dc_server_host, DC_SERVER_PORT, DC_SERVER_URI);
      int httpCode = http.POST(payload);
      Serial.print("HTTP Code = ");
      Serial.println(httpCode);
      if (httpCode == HTTP_CODE_OK) {
        String response_payload = http.getString();
        Serial.println(response_payload);

        // Only update the last reported time if the report was sucessfull
        lastReportTime = currTime;
      }
      else {
        Serial.println("Error");
      }
      http.end();
    }
  }

  // No need to agressively loop
  delay(MS_BETWEEN_LOOPS);

}



/************************************
 * File System routines
 ************************************/

// Read the configuration values.
void readConfig() {
  if (SPIFFS.exists(CONFIG_FILE)) {
    //file exists, reading and loading
    Serial.print("reading config file ");
    Serial.println(CONFIG_FILE);
    File configFile = SPIFFS.open(CONFIG_FILE, "r");
    if (configFile) {
      Serial.println("opened config file");
      size_t configSize = configFile.size();
      // Allocate a buffer to store contents of the file.
      std::unique_ptr<char[]> buf(new char[configSize]);
  
      configFile.readBytes(buf.get(), configSize);
              DynamicJsonBuffer jsonBuffer;
              JsonObject& json = jsonBuffer.parseObject(buf.get());
              json.printTo(Serial);
              if (json.success()) {
                Serial.println("parsed json");
          
                strncpy(dc_server_host, json["dc_server_host"], sizeof(dc_server_host));
                strncpy(beacon_name, json["beacon_name"], sizeof(beacon_name));
          
              }
              else {
                Serial.print("failed to load json config ");
                Serial.println(CONFIG_FILE);
              }
    }
    else {
      Serial.print("read failed to open config file ");
      Serial.println(CONFIG_FILE);
    }
  }
  else {
    Serial.print("no config file ");
    Serial.println(CONFIG_FILE);
  }
}


// Save the configuration values.
void saveConfig() {
  Serial.print("saving config to ");
  Serial.println(CONFIG_FILE);
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["dc_server_host"] = dc_server_host;
  json["beacon_name"] = beacon_name;

  File configFile = SPIFFS.open(CONFIG_FILE, "w");
  if (configFile) {
    json.printTo(configFile);
    configFile.close();
  
    json.printTo(Serial);
  }
  else {
    Serial.print("write failed to open config file ");
    Serial.println(CONFIG_FILE);
  }
}

/***************
 * Utilities
 ***************
 */

void setBeaconName(char *beacon) {
  char set_name[sizeof(beacon_name)];

  // See if we need a default
  if (strlen(beacon) == 0) {
    String defaultName = WiFi.hostname();
    defaultName.toCharArray(set_name, sizeof(set_name));
  }
  else {
    strncpy(set_name, beacon, sizeof(set_name));
  }

  // Set the name of the beacon
  String beaconName;
  beaconName = String(set_name);
  beaconName += "_beacon";

  // Create the name for the Access Point
  String apName;
  apName = String(set_name);
  apName += "_AP";
  apName.toCharArray(ap_name,sizeof(ap_name));

  // Create the name for receiving OTA updates
  String otaName;
  otaName = String(set_name);
  otaName += "_OTA";
  otaName.toCharArray(ota_name,sizeof(ota_name));

  // Set the Hostname
  Serial.print("Setting hostname to ");
  Serial.print(beaconName);
  Serial.print(" ... ");
  if(WiFi.hostname(beaconName)) {
    Serial.print("success");
  }
  else {
    Serial.print("failed");
  }
  Serial.println();
}

