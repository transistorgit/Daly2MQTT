/*
DALY BMS to MQTT Project
https://github.com/softwarecrash/DALY-BMS-to-MQTT
This code is free for use without any waranty.
when copy code or reuse make a note where the codes comes from.
*/

#include <Arduino.h>

// json crack: https://jsoncrack.com/editor
#include <daly-bms-uart.h> // This is where the library gets pulled in
#define BMS_SERIAL Serial  // Set the serial port for communication with the Daly BMS
#define DALY_BMS_DEBUG Serial1 // Uncomment the below #define to enable debugging print statements.

#define ARDUINOJSON_USE_DOUBLE 0

#include <PubSubClient.h>

#include <ArduinoJson.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncWiFiManager.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include "Settings.h"

#include "webpages/htmlCase.h"     // The HTML Konstructor
#include "webpages/main.h"         // landing page with menu
#include "webpages/settings.h"     // settings page
#include "webpages/settingsedit.h" // mqtt settings page

WiFiClient client;
Settings _settings;
PubSubClient mqttclient(client);
int jsonBufferSize = 2048;
char jsonBuffer[2048];

DynamicJsonDocument bmsJson(jsonBufferSize);                      // main Json
JsonObject packJson = bmsJson.createNestedObject("Pack");         // battery package data
JsonObject cellVJson = bmsJson.createNestedObject("CellV");       // nested data for cell voltages
JsonObject cellTempJson = bmsJson.createNestedObject("CellTemp"); // nested data for cell temp

String topicStrg;

unsigned long mqtttimer = 0;
unsigned long bmstimer = 0;
unsigned long RestartTimer = 0;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncWebSocketClient *wsClient;
DNSServer dns;
Daly_BMS_UART bms(BMS_SERIAL);

// flag for saving data and other things
bool shouldSaveConfig = false;
char mqtt_server[40];
bool restartNow = false;
bool updateProgress = false;
bool dataCollect = false;
int crcErrCount = 0;
bool firstPublish = false;
// vars vor wakeup
#define WAKEUPPIN 4              // GPIO pin for the wakeup transistor
#define WAKEUPINTERVAL 10000     // interval for wakeupHandler()
#define WAKEUPDURATION 100       // duration how long the pin is switched
unsigned long wakeuptimer = WAKEUPINTERVAL; //dont run immediately after boot, wait for first intervall
bool wakeupPinActive = false;

// vars for relais
#define RELAISPIN 5
#define RELAISINTERVAL 5000     // interval for relaisHandler()
#define RELAISHYSTERESIS 0.02   // hysteresis to prevent toggeling too quickly
unsigned long relaistimer = RELAISINTERVAL; //dont run immediately after boot, wait for first intervall
float relaisCompareValueTmp = 0;
bool relaisComparsionResult = false;
//----------------------------------------------------------------------
void saveConfigCallback()
{
  Serial.println(F("Should save config"));
  shouldSaveConfig = true;
}

static void handle_update_progress_cb(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{

  uint32_t free_space = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
  if (!index)
  {
#ifdef DALY_BMS_DEBUG
    DALY_BMS_DEBUG.println(F("Starting Firmware Update"));
#endif
    Update.runAsync(true);
    if (!Update.begin(free_space, U_FLASH))
    {
#ifdef DALY_BMS_DEBUG
      Update.printError(DALY_BMS_DEBUG);
#endif
      ESP.restart();
    }
  }

  if (Update.write(data, len) != len)
  {
#ifdef DALY_BMS_DEBUG
    Update.printError(DALY_BMS_DEBUG);
#endif
    ESP.restart();
  }

  if (final)
  {
    if (!Update.end(true))
    {
#ifdef DALY_BMS_DEBUG
      Update.printError(DALY_BMS_DEBUG);
#endif
      ESP.restart();
    }
    else
    {
      AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "Please wait while the device is booting new Firmware");
      response->addHeader("Refresh", "10; url=/");
      response->addHeader("Connection", "close");
      request->send(response);

#ifdef DALY_BMS_DEBUG
      DALY_BMS_DEBUG.println("Update complete");
#endif
      RestartTimer = millis();
      restartNow = true; // Set flag so main loop can issue restart call
    }
  }
}

void notifyClients()
{
  if (wsClient != nullptr && wsClient->canSend())
  {
    serializeJson(bmsJson, jsonBuffer);
    wsClient->text(jsonBuffer);
  }
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len)
{
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
  {
    data[len] = 0;
    updateProgress = true;
    if (strcmp((char *)data, "dischargeFetSwitch_on") == 0)
    {
      bms.setDischargeMOS(true);
    }
    if (strcmp((char *)data, "dischargeFetSwitch_off") == 0)
    {
      bms.setDischargeMOS(false);
    }
    if (strcmp((char *)data, "chargeFetSwitch_on") == 0)
    {
      bms.setChargeMOS(true);
    }
    if (strcmp((char *)data, "chargeFetSwitch_off") == 0)
    {
      bms.setChargeMOS(false);
    }
    delay(200); // give the bms time to react
    updateProgress = false;
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_CONNECT:
    wsClient = client;
    bms.update();
    getJsonData();
    notifyClients();
    break;
  case WS_EVT_DISCONNECT:
    wsClient = nullptr;
    break;
  case WS_EVT_DATA:
    handleWebSocketMessage(arg, data, len);
    break;
  case WS_EVT_PONG:
  case WS_EVT_ERROR:
    break;
  }
}

bool wakeupHandler(){
  if (_settings.data.wakeupEnable && (millis() > wakeuptimer))
  {
#ifdef DALY_BMS_DEBUG
      DALY_BMS_DEBUG.println();
      DALY_BMS_DEBUG.println("wakeupHandler()");
      DALY_BMS_DEBUG.print("this run:\t");
      DALY_BMS_DEBUG.println(millis());
      DALY_BMS_DEBUG.print("next run:\t");
      DALY_BMS_DEBUG.println(wakeuptimer);
#endif
  if(wakeupPinActive)
  {
    wakeupPinActive = false;
    wakeuptimer = millis() + WAKEUPINTERVAL;
    digitalWrite(WAKEUPPIN, LOW);
  } else
  {
    wakeupPinActive = true;
    wakeuptimer = millis() + WAKEUPDURATION;
    digitalWrite(WAKEUPPIN, HIGH);
  }
#ifdef DALY_BMS_DEBUG
    DALY_BMS_DEBUG.print("PIN IS NOW:\t");
    DALY_BMS_DEBUG.println(digitalRead(WAKEUPPIN));
#endif
  }
  return true;
}

bool relaisHandler(){
  if (_settings.data.relaisEnable && (millis() - relaistimer > RELAISINTERVAL))
  {
    relaistimer = millis();
#ifdef DALY_BMS_DEBUG
    DALY_BMS_DEBUG.println();
    DALY_BMS_DEBUG.println("relaisHandler()");
    DALY_BMS_DEBUG.print("this run:\t");
    DALY_BMS_DEBUG.println(millis());
    DALY_BMS_DEBUG.print("next run:\t");
    DALY_BMS_DEBUG.println(relaistimer);
#endif

    /*
    bool _settings.data.relaisInvert = false;  // invert relais output?
    byte _settings.data.relaisFunction = 0;    // function mode - 0 = Lowest Cell Voltage, 1 = Highest Cell Voltage, 2 = Pack Voltage, 3 = Temperature
    byte _settings.data.relaisComparsion = 0;  // comparsion mode - 0 = Higher or equal than, 1 = Lower or equal than
    float _settings.data.relaissetvalue = 0.0; // value to compare to
    bool relaisComparsionResult = false;

    Lowest Cell Voltage ->  bms.get.minCellmV / 1000
    Highest Cell Voltage -> bms.get.maxCellmV / 1000
    Pack Cell Voltage -> bms.get.packVoltage
    Temperature -> bms.get.tempAverage
    */

    // read the value to compare to depending on the mode
    switch (_settings.data.relaisFunction)
    {
      case 0:
        // Mode 0 - Lowest Cell Voltage
        relaisCompareValueTmp = bms.get.minCellmV / 1000;
        break;
      case 1:
        // Mode 1 - Highest Cell Voltage
        relaisCompareValueTmp = bms.get.maxCellmV / 1000;
        break; 
      case 2:    
        // Mode 2 - Pack Voltage
        relaisCompareValueTmp = bms.get.packVoltage;
        break;
      case 3:
        // Mode 3 - Temperature
        relaisCompareValueTmp = bms.get.tempAverage;
        break;
      case 4:
        // Mode 4 - Manual per WEB or MQTT
        break;
    }
    
    if(relaisCompareValueTmp == NULL){
      return false;
    }
    // now compare depending on the mode
    if(_settings.data.relaisFunction != 4)
    {
      // other modes
      switch (_settings.data.relaisComparsion)
      {
        case 0:
          // Higher or equal than
          // check if value is already true so we have to use hysteresis to switch off
          if(relaisComparsionResult){
            relaisComparsionResult = relaisCompareValueTmp >= (_settings.data.relaissetvalue - RELAISHYSTERESIS) ? true : false;
          } else {
            //check if value is greater than
            relaisComparsionResult = relaisCompareValueTmp >= (_settings.data.relaissetvalue) ? true : false;
          }
          break;
        case 1:
          // Lower or equal than
          // check if value is already true so we have to use hysteresis to switch off
          if(relaisComparsionResult){
            //use hystersis to switch off
            relaisComparsionResult = relaisCompareValueTmp <= (_settings.data.relaissetvalue + RELAISHYSTERESIS) ? true : false;
          } else {
            //check if value is greater than
            relaisComparsionResult = relaisCompareValueTmp <= (_settings.data.relaissetvalue) ? true : false;
          }
          break;
      }
    } else {
      // manual mode, currently no need to set anything, relaisComparsionResult is set by WEB or MQTT
      // i keep this just here for better reading of the code. The else {} statement can be removed later
    }

    #ifdef DALY_BMS_DEBUG
      DALY_BMS_DEBUG.print("relaisComparsionResult:\t");
      DALY_BMS_DEBUG.println(relaisComparsionResult);
    #endif

    if(relaisComparsionResult == true)
    {
      if(_settings.data.relaisInvert == true)
      {
        digitalWrite(RELAISPIN, LOW);
      } else
      {
        digitalWrite(RELAISPIN, HIGH);
      }
    } else
    {
      if(_settings.data.relaisInvert == true)
      {
        digitalWrite(RELAISPIN, HIGH);
      } else
      {
        digitalWrite(RELAISPIN, LOW);
      }
    }
    return true;
  }
  return false;
}

void setup()
{
  wifi_set_sleep_type(LIGHT_SLEEP_T);
#ifdef DALY_BMS_DEBUG
  DALY_BMS_DEBUG.begin(9600); // Debugging towards UART1
#endif

  _settings.load();
  if(_settings.data.wakeupEnable)
    pinMode(WAKEUPPIN, OUTPUT);
  if(_settings.data.relaisEnable)
    pinMode(RELAISPIN, OUTPUT);
  bms.Init();                                      // init the bms driver
  WiFi.persistent(true);                           // fix wifi save bug
  packJson["Device_Name"] = _settings.data.deviceName; // set the device name in json string
  topicStrg = (_settings.data.mqttTopic + String("/") + _settings.data.deviceName).c_str();
  AsyncWiFiManager wm(&server, &dns);
  wm.setDebugOutput(false); // disable wifimanager debug output
  bmstimer = millis();
  mqtttimer = millis();
  wm.setSaveConfigCallback(saveConfigCallback);

#ifdef DALY_BMS_DEBUG
  DALY_BMS_DEBUG.println();
  DALY_BMS_DEBUG.print(F("Device Name:\t"));
  DALY_BMS_DEBUG.println(_settings.data.deviceName);
  DALY_BMS_DEBUG.print(F("Mqtt Server:\t"));
  DALY_BMS_DEBUG.println(_settings.data.mqttServer);
  DALY_BMS_DEBUG.print(F("Mqtt Port:\t"));
  DALY_BMS_DEBUG.println(_settings.data.mqttPort);
  DALY_BMS_DEBUG.print(F("Mqtt User:\t"));
  DALY_BMS_DEBUG.println(_settings.data.mqttUser);
  DALY_BMS_DEBUG.print(F("Mqtt Passwort:\t"));
  DALY_BMS_DEBUG.println(_settings.data.mqttPassword);
  DALY_BMS_DEBUG.print(F("Mqtt Interval:\t"));
  DALY_BMS_DEBUG.println(_settings.data.mqttRefresh);
  DALY_BMS_DEBUG.print(F("Mqtt Topic:\t"));
  DALY_BMS_DEBUG.println(_settings.data.mqttTopic);
#endif
  AsyncWiFiManagerParameter custom_mqtt_server("mqtt_server", "MQTT server", NULL, 32);
  AsyncWiFiManagerParameter custom_mqtt_user("mqtt_user", "MQTT User", NULL, 32);
  AsyncWiFiManagerParameter custom_mqtt_pass("mqtt_pass", "MQTT Password", NULL, 32);
  AsyncWiFiManagerParameter custom_mqtt_topic("mqtt_topic", "MQTT Topic", "BMS01", 32);
  AsyncWiFiManagerParameter custom_mqtt_port("mqtt_port", "MQTT Port", "1883", 5);
  AsyncWiFiManagerParameter custom_mqtt_refresh("mqtt_refresh", "MQTT Send Interval", "300", 4);
  AsyncWiFiManagerParameter custom_device_name("device_name", "Device Name", "DALY-BMS-to-MQTT", 32);

  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_user);
  wm.addParameter(&custom_mqtt_pass);
  wm.addParameter(&custom_mqtt_topic);
  wm.addParameter(&custom_mqtt_port);
  wm.addParameter(&custom_mqtt_refresh);
  wm.addParameter(&custom_device_name);

  bool res = wm.autoConnect("DALY-BMS-AP");

  wm.setConnectTimeout(30);       // how long to try to connect for before continuing
  wm.setConfigPortalTimeout(120); // auto close configportal after n seconds

  // save settings if wifi setup is fire up
  if (shouldSaveConfig)
  {
    strcpy(_settings.data.mqttServer, custom_mqtt_server.getValue());
    strcpy(_settings.data.mqttUser, custom_mqtt_user.getValue());
    strcpy(_settings.data.mqttPassword, custom_mqtt_pass.getValue());
    _settings.data.mqttPort = atoi(custom_mqtt_port.getValue());
    strcpy(_settings.data.deviceName, custom_device_name.getValue());
    strcpy(_settings.data.mqttTopic, custom_mqtt_topic.getValue());
    _settings.data.mqttRefresh = atoi(custom_mqtt_refresh.getValue());

    _settings.save();
    delay(500);
    //_settings.load();
    ESP.restart();
  }

if(_settings.data.mqttServer != (char*)"-1")
{
  mqttclient.setServer(_settings.data.mqttServer, _settings.data.mqttPort);
}
  mqttclient.setCallback(callback);
  mqttclient.setBufferSize(jsonBufferSize);
  // check is WiFi connected
  if (!res)
  {
#ifdef DALY_BMS_DEBUG
    DALY_BMS_DEBUG.println(F("Failed to connect to WiFi or hit timeout"));
#endif
  }
  else
  {

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                AsyncResponseStream *response = request->beginResponseStream("text/html");
                response->printf_P(HTML_HEAD);
                response->printf_P(HTML_MAIN);
                response->printf_P(HTML_FOOT);
                request->send(response); 
                });

    server.on("/livejson", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                AsyncResponseStream *response = request->beginResponseStream("application/json");
                serializeJson(bmsJson, *response);
                request->send(response); });

    server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "Please wait while the device reboots...");
                response->addHeader("Refresh", "3; url=/");
                response->addHeader("Connection", "close");
                request->send(response);
                RestartTimer = millis();
                restartNow = true; });

    server.on("/confirmreset", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                AsyncResponseStream *response = request->beginResponseStream("text/html");
                response->printf_P(HTML_HEAD);
                response->printf_P(HTML_CONFIRM_RESET);
                response->printf_P(HTML_FOOT);
                request->send(response); });

    server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "Device is Erasing...");
                response->addHeader("Refresh", "15; url=/");
                response->addHeader("Connection", "close");
                request->send(response);
                delay(1000);
                _settings.reset();
                ESP.eraseConfig();
                ESP.restart(); });

    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                AsyncResponseStream *response = request->beginResponseStream("text/html");
                response->printf_P(HTML_HEAD);
                response->printf_P(HTML_SETTINGS);
                response->printf_P(HTML_FOOT);
                request->send(response); });

    server.on("/settingsedit", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                AsyncResponseStream *response = request->beginResponseStream("text/html");
                response->printf_P(HTML_HEAD);
                response->printf_P(HTML_SETTINGS_EDIT);
                response->printf_P(HTML_FOOT);
                request->send(response); });

    server.on("/settingsjson", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                AsyncResponseStream *response = request->beginResponseStream("application/json");
                DynamicJsonDocument SettingsJson(512);
                SettingsJson["device_name"] = _settings.data.deviceName;
                SettingsJson["mqtt_server"] = _settings.data.mqttServer;
                SettingsJson["mqtt_port"] = _settings.data.mqttPort;
                SettingsJson["mqtt_topic"] = _settings.data.mqttTopic;
                SettingsJson["mqtt_user"] = _settings.data.mqttUser;
                SettingsJson["mqtt_password"] = _settings.data.mqttPassword;
                SettingsJson["mqtt_refresh"] = _settings.data.mqttRefresh;
                SettingsJson["mqtt_json"] = _settings.data.mqttJson;
                SettingsJson["wakeup_enable"] = _settings.data.wakeupEnable;
                SettingsJson["wakeup_invert"] = _settings.data.wakeupInvert;
                SettingsJson["relais_enable"] = _settings.data.relaisEnable;
                SettingsJson["relais_invert"] = _settings.data.relaisInvert;
                SettingsJson["relais_function"] = _settings.data.relaisFunction;
                SettingsJson["relais_comparsion"] = _settings.data.relaisComparsion;
                SettingsJson["relais_setvalue"] = _settings.data.relaissetvalue;

                serializeJson(SettingsJson, *response);
                request->send(response); });

    server.on("/settingssave", HTTP_POST, [](AsyncWebServerRequest *request)
              {
                //request->arg("post_mqttServer").toCharArray(_settings.data.mqttServer,request->arg("post_mqttServer").length() + 1);
                strcpy(_settings.data.mqttServer, request->arg("post_mqttServer").c_str());
                _settings.data.mqttPort = request->arg("post_mqttPort").toInt();
                strcpy(_settings.data.mqttUser, request->arg("post_mqttUser").c_str());
                strcpy(_settings.data.mqttPassword, request->arg("post_mqttPassword").c_str());
                strcpy(_settings.data.mqttTopic, request->arg("post_mqttTopic").c_str());
                _settings.data.mqttRefresh = request->arg("post_mqttRefresh").toInt() < 1 ? 1 : request->arg("post_mqttRefresh").toInt(); // prevent lower numbers
                strcpy(_settings.data.deviceName, request->arg("post_deviceName").c_str());
/*
                if (request->arg("post_mqttjson") == "true"){
                  _settings.data.mqttJson = true;
                } else {
                  _settings.data.mqttJson = false;
                }                  

                if (request->arg("post_wakeupenable") == "true")
                  _settings.data.wakeupEnable = true;
                if (request->arg("post_wakeupenable") != "true")
                  _settings.data.wakeupEnable = false;

                if (request->arg("post_wakeupinvert") == "true")
                  _settings.data.wakeupInvert = true;
                if (request->arg("post_wakeupinvert") != "true")
                  _settings.data.wakeupInvert = false;

                if (request->arg("post_relaisenable") == "true")
                  _settings.data.relaisEnable = true;
                if (request->arg("post_relaisenable") != "true")
                  _settings.data.relaisEnable = false;

                if (request->arg("post_relaisinvert") == "true")
                  _settings.data.relaisInvert = true;
                if (request->arg("post_relaisinvert") != "true")
                  _settings.data.relaisInvert = false;
*/

                _settings.data.mqttJson = request->arg("post_mqttjson") == "true" ? true : false;
                _settings.data.wakeupEnable = request->arg("post_wakeupenable") == "true" ? true : false;
                _settings.data.wakeupInvert = request->arg("post_wakeupinvert") == "true" ? true : false;
                _settings.data.relaisEnable = request->arg("post_relaisenable") == "true" ? true : false;
                _settings.data.relaisInvert = request->arg("post_relaisinvert") == "true" ? true : false;
                  
                _settings.data.relaisFunction = request->arg("post_relaisfunction").toInt();
                _settings.data.relaisComparsion = request->arg("post_relaiscomparsion").toInt();
                _settings.data.relaissetvalue = request->arg("post_relaissetvalue").toFloat();
                
                _settings.save();
                request->redirect("/reboot"); });

    server.on("/set", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                AsyncWebParameter *p = request->getParam(0);
                if (p->name() == "chargefet")
                {
#ifdef DALY_BMS_DEBUG
                    DALY_BMS_DEBUG.println("Webcall: charge fet to: "+(String)p->value());
#endif
                    if(p->value().toInt() == 1){
                      bms.setChargeMOS(true);
                      bms.get.chargeFetState = true;
                    }
                    if(p->value().toInt() == 0){
                      bms.setChargeMOS(false);
                      bms.get.chargeFetState = false;
                    }
                }
                if (p->name() == "dischargefet")
                {
#ifdef DALY_BMS_DEBUG
                    DALY_BMS_DEBUG.println("Webcall: discharge fet to: "+(String)p->value());
#endif
                    if(p->value().toInt() == 1){
                      bms.setDischargeMOS(true);
                      bms.get.disChargeFetState = true;
                    }
                    if(p->value().toInt() == 0){
                      bms.setDischargeMOS(false);
                      bms.get.disChargeFetState = false;
                    }
                }
                if (p->name() == "soc")
                {
#ifdef DALY_BMS_DEBUG
                    DALY_BMS_DEBUG.println("Webcall: setsoc SOC set to: "+(String)p->value());
#endif
                    if(p->value().toInt() >= 0 && p->value().toInt() <= 100 ){
                      bms.setSOC(p->value().toInt());
                    }
                }
                request->send(200, "text/plain", "message received"); });

    server.on(
        "/update", HTTP_POST, [](AsyncWebServerRequest *request)
        {
          Serial.end();
          updateProgress = true;
          ws.enable(false);
          ws.closeAll();
          request->send(200);
          // request->redirect("/");
        },
        handle_update_progress_cb);

    // set the device name
    MDNS.begin(_settings.data.deviceName);
    WiFi.hostname(_settings.data.deviceName);
    ws.onEvent(onEvent);
    server.addHandler(&ws);
    server.begin();
    MDNS.addService("http", "tcp", 80);
#ifdef DALY_BMS_DEBUG
    DALY_BMS_DEBUG.println(F("Webserver Running..."));
#endif
  connectMQTT();
  }
  //set the timers
 mqtttimer = millis();
 bmstimer = millis();
}
// end void setup

void loop()
{
  // Make sure wifi is in the right mode
  if (WiFi.status() == WL_CONNECTED)
  {                      // No use going to next step unless WIFI is up and running.
    ws.cleanupClients(); // clean unused client connections
    MDNS.update();
    mqttclient.loop(); // Check if we have something to read from MQTT

    if (!updateProgress)
    {
      bool updatedData = false;
      if (millis() > (bmstimer + (5 * 1000)) && wsClient != nullptr && wsClient->canSend())
      {
        bmstimer = millis();
        if (bms.update()) // ask the bms for new data
        {
          getJsonData();
          crcErrCount = 0;
          updatedData = true;
        }
        else
        {
          crcErrCount++;
          if (crcErrCount >= 3)
          {
            clearJsonData(); // by no connection, clear all data
            updatedData = false;
          }
        }
        notifyClients();
      }
      else if (millis() > (mqtttimer + (_settings.data.mqttRefresh * 1000)))
      {
        mqtttimer = millis();
        if (millis() < (bmstimer + (5 * 1000)) && updatedData == true) // if the last request shorter then 3 use the data from last web request
        {
          sendtoMQTT(); // Update data to MQTT server if we should
        }
        else // get new data
        {
          if (bms.update()) // ask the bms for new data
          {
            getJsonData(); // prepare data for json string sending
            sendtoMQTT();  // Update data to MQTT server if we should
            crcErrCount = 0;
            updatedData = true;
          }
          else
          {
            crcErrCount++;
            if (crcErrCount >= 3)
            {
              clearJsonData(); // by no connection, clear all data
              updatedData = false;
            }
          }
        }
      }
    }
    if (wsClient == nullptr)
    {
      delay(2); // for power saving test
    }
  }
  if (restartNow && millis() >= (RestartTimer + 500))
  {
#ifdef DALY_BMS_DEBUG
    DALY_BMS_DEBUG.println(F("Restart"));
#endif
    ESP.restart();
  }
  if (_settings.data.wakeupEnable && (millis() > wakeuptimer))
    wakeupHandler();
  
  if (_settings.data.relaisEnable && (millis() > relaistimer))
    relaisHandler();

  yield();
}
// End void loop
void getJsonData()
{
  // prevent buffer leak
  if (int(bmsJson.memoryUsage()) >= (jsonBufferSize - 8))
  {
    bmsJson.garbageCollect();
  }
  packJson["Device_IP"] = WiFi.localIP().toString();
  packJson["Voltage"] = bms.get.packVoltage;
  packJson["Current"] = bms.get.packCurrent;
  packJson["Power"] = (bms.get.packCurrent * bms.get.packVoltage);
  packJson["SOC"] = bms.get.packSOC;
  packJson["Remaining_mAh"] = bms.get.resCapacitymAh;
  packJson["Cycles"] = bms.get.bmsCycles;
  packJson["BMS_Temp"] = bms.get.tempAverage;
  packJson["Cell_Temp"] = bms.get.cellTemperature[0];
  packJson["High_CellNr"] = bms.get.maxCellVNum;
  packJson["High_CellV"] = bms.get.maxCellmV / 1000;
  packJson["Low_CellNr"] = bms.get.minCellVNum;
  packJson["Low_CellV"] = bms.get.minCellmV / 1000;
  packJson["Cell_Diff"] = bms.get.cellDiff;
  packJson["DischargeFET"] = bms.get.disChargeFetState ? true : false;
  packJson["ChargeFET"] = bms.get.chargeFetState ? true : false;
  packJson["Status"] = bms.get.chargeDischargeStatus;
  packJson["Cells"] = bms.get.numberOfCells;
  packJson["Heartbeat"] = bms.get.bmsHeartBeat;
  packJson["Balance_Active"] = bms.get.cellBalanceActive ? true : false;
  packJson["Relais_Active"] = relaisComparsionResult ? true : false;
  packJson["Relais_Manual"] = _settings.data.relaisFunction == 4 ? true : false;


  for (size_t i = 0; i < size_t(bms.get.numberOfCells); i++)
  {
    cellVJson["CellV " + String(i + 1)] = bms.get.cellVmV[i] / 1000;
    cellVJson["Balance " + String(i + 1)] = bms.get.cellBalanceState[i];
  }

  for (size_t i = 0; i < size_t(bms.get.numOfTempSensors); i++)
  {
    cellTempJson["Cell_Temp" + String(i + 1)] = bms.get.cellTemperature[i];
  }
}

void clearJsonData()
{
  packJson["Voltage"] = nullptr;
  packJson["Current"] = nullptr;
  packJson["Power"] = nullptr;
  packJson["SOC"] = nullptr;
  packJson["Remaining_mAh"] = nullptr;
  packJson["Cycles"] = nullptr;
  packJson["BMS_Temp"] = nullptr;
  packJson["Cell_Temp"] = nullptr;
  packJson["High_CellNr"] = nullptr;
  packJson["High_CellV"] = nullptr;
  packJson["Low_CellNr"] = nullptr;
  packJson["Low_CellV"] = nullptr;
  packJson["Cell_Diff"] = nullptr;
  packJson["DischargeFET"] = nullptr;
  packJson["ChargeFET"] = nullptr;
  packJson["Status"] = nullptr;
  packJson["Cells"] = nullptr;
  packJson["Heartbeat"] = nullptr;
  packJson["Balance_Active"] = nullptr;
  packJson["Relais_Active"] = nullptr;
  packJson["Relais_Manual"] = nullptr;
  cellVJson.clear();
  cellTempJson.clear();
}

bool sendtoMQTT()
{
  if (!connectMQTT())
  {
#ifdef DALY_BMS_DEBUG
    DALY_BMS_DEBUG.println(F("Error: No connection to MQTT Server, cant send Data!"));
#endif
    firstPublish = false;
    return false;
  }

  if (!_settings.data.mqttJson)
  {
    mqttclient.publish((topicStrg + ("/Device_IP")).c_str(), (WiFi.localIP().toString()).c_str());
    char msgBuffer[20];
    mqttclient.publish((topicStrg + "/Pack_Voltage").c_str(), dtostrf(bms.get.packVoltage, 4, 1, msgBuffer));
    mqttclient.publish((topicStrg + "/Pack_Current").c_str(), dtostrf(bms.get.packCurrent, 4, 1, msgBuffer));
    mqttclient.publish((topicStrg + "/Pack_Power").c_str(), dtostrf((bms.get.packVoltage * bms.get.packCurrent), 4, 1, msgBuffer));
    mqttclient.publish((topicStrg + "/Pack_SOC").c_str(), dtostrf(bms.get.packSOC, 6, 2, msgBuffer));
    mqttclient.publish((topicStrg + "/Pack_Remaining_mAh").c_str(), String(bms.get.resCapacitymAh).c_str());
    mqttclient.publish((topicStrg + "/Pack_Cycles").c_str(), String(bms.get.bmsCycles).c_str());
    mqttclient.publish((topicStrg + "/Pack_BMS_Temperature").c_str(), String(bms.get.tempAverage).c_str());
    mqttclient.publish((topicStrg + "/Pack_High_Cell").c_str(), (dtostrf(bms.get.maxCellVNum, 1, 0, msgBuffer) + String(".- ") + dtostrf(bms.get.maxCellmV / 1000, 5, 3, msgBuffer)).c_str());
    mqttclient.publish((topicStrg + "/Pack_Low_Cell").c_str(), (dtostrf(bms.get.minCellVNum, 1, 0, msgBuffer) + String(".- ") + dtostrf(bms.get.minCellmV / 1000, 5, 3, msgBuffer)).c_str());
    mqttclient.publish((topicStrg + "/Pack_Cell_Difference").c_str(), String(bms.get.cellDiff).c_str());
    mqttclient.publish((topicStrg + "/Pack_ChargeFET").c_str(), bms.get.chargeFetState ? "true" : "false");
    mqttclient.publish((topicStrg + "/Pack_DischargeFET").c_str(), bms.get.disChargeFetState ? "true" : "false");
    mqttclient.publish((topicStrg + "/Pack_Status").c_str(), bms.get.chargeDischargeStatus.c_str());
    mqttclient.publish((topicStrg + "/Pack_Cells").c_str(), String(bms.get.numberOfCells).c_str());
    mqttclient.publish((topicStrg + "/Pack_Heartbeat").c_str(), String(bms.get.bmsHeartBeat).c_str());
    mqttclient.publish((topicStrg + "/Pack_Balance_Active").c_str(), String(bms.get.cellBalanceActive ? "true" : "false").c_str());

    for (size_t i = 0; i < size_t(bms.get.numberOfCells); i++)
    {
      mqttclient.publish((topicStrg + "/Pack_Cells_Voltage/Cell_" + (i + 1)).c_str(), dtostrf(bms.get.cellVmV[i] / 1000, 5, 3, msgBuffer));
      mqttclient.publish((topicStrg + "/Pack_Cells_Balance/Cell_" + (i + 1)).c_str(), String(bms.get.cellBalanceState[i] ? "true" : "false").c_str());
    }

    for (size_t i = 0; i < size_t(bms.get.numOfTempSensors); i++)
    {
      mqttclient.publish((topicStrg + "/Pack_Cell_Temperature_" + (i + 1)).c_str(), String(bms.get.cellTemperature[i]).c_str());
    }
  }
  else
  {
    size_t n = serializeJson(bmsJson, jsonBuffer);
    mqttclient.publish((String(topicStrg)).c_str(), jsonBuffer, n);
  }
  firstPublish = true;
  return true;
}

void callback(char *topic, byte *payload, unsigned int length)
{
  if (firstPublish == false)
    return;
  updateProgress = true;
  if (!_settings.data.mqttJson)
  {
    String messageTemp;
    char *top = topic;
    for (unsigned int i = 0; i < length; i++)
    {
      messageTemp += (char)payload[i];
    }
#ifdef DALY_BMS_DEBUG
    DALY_BMS_DEBUG.println("message recived: " + messageTemp);
#endif
    // set Relais
    if (strcmp(top, (topicStrg + "/SET/Relais").c_str()) == 0)
    {
#ifdef DALY_BMS_DEBUG
      DALY_BMS_DEBUG.println("message recived: " + messageTemp);
      DALY_BMS_DEBUG.println("set Relais");
#endif
      if(_settings.data.relaisFunction == 4 && messageTemp == "true")
      {
#ifdef DALY_BMS_DEBUG
        DALY_BMS_DEBUG.println("switching Relais on");
#endif
        relaisComparsionResult = true;
        relaisHandler();
      }
      if(_settings.data.relaisFunction == 4 && messageTemp == "false")
      {
#ifdef DALY_BMS_DEBUG
        DALY_BMS_DEBUG.println("switching Relais off");
#endif
        relaisComparsionResult = false;
        relaisHandler();
      }
    }

    // set SOC
    if (strcmp(top, (topicStrg + "/SET/Pack_SOC").c_str()) == 0)
    {
#ifdef DALY_BMS_DEBUG
      DALY_BMS_DEBUG.println("message recived: " + messageTemp);
      DALY_BMS_DEBUG.println("set SOC");
#endif

      if (bms.get.packSOC != messageTemp.toInt())
      {
        bms.setSOC(messageTemp.toInt());
      }
    }

    // Switch the Discharging port
    if (strcmp(top, (topicStrg + "/SET/Pack_DischargeFET").c_str()) == 0)
    {
#ifdef DALY_BMS_DEBUG
      DALY_BMS_DEBUG.println("message recived: " + messageTemp);
#endif

      if (messageTemp == "true" && !bms.get.disChargeFetState)
      {
#ifdef DALY_BMS_DEBUG
        DALY_BMS_DEBUG.println("switching Discharging mos on");
#endif
        bms.setDischargeMOS(true);
      }
      if (messageTemp == "false" && bms.get.disChargeFetState)
      {
#ifdef DALY_BMS_DEBUG
        DALY_BMS_DEBUG.println("switching Discharging mos off");
#endif
        bms.setDischargeMOS(false);
      }
    }

    // Switch the Charging Port
    if (strcmp(top, (topicStrg + "/SET/Pack_ChargeFET").c_str()) == 0)
    {
#ifdef DALY_BMS_DEBUG
      DALY_BMS_DEBUG.println("message recived: " + messageTemp);
#endif

      if (messageTemp == "true" && !bms.get.chargeFetState)
      {
#ifdef DALY_BMS_DEBUG
        DALY_BMS_DEBUG.println("switching Charging mos on");
#endif
        bms.setChargeMOS(true);
      }
      if (messageTemp == "false" && bms.get.chargeFetState)
      {
#ifdef DALY_BMS_DEBUG
        DALY_BMS_DEBUG.println("switching Charging mos off");
#endif
        bms.setChargeMOS(false);
      }
    }
  }
  else
  {
    StaticJsonDocument<1024> mqttJsonAnswer;
    deserializeJson(mqttJsonAnswer, (const byte *)payload, length);
    bms.setChargeMOS(mqttJsonAnswer["Pack"]["SOC"]);

    if (mqttJsonAnswer["Pack"]["ChargeFET"] == true)
    {
      bms.setChargeMOS(true);
    }
    else if (mqttJsonAnswer["Pack"]["ChargeFET"] == false)
    {
      bms.setChargeMOS(false);
    }
    else
    {
#ifdef DALY_BMS_DEBUG
      DALY_BMS_DEBUG.println("No Valid Command from JSON for setChargeMOS");
#endif
    }
    if (mqttJsonAnswer["Pack"]["DischargeFET"] == true)
    {
      bms.setDischargeMOS(true);
    }
    else if (mqttJsonAnswer["Pack"]["DischargeFET"] == false)
    {
      bms.setDischargeMOS(false);
    }
    else
    {
#ifdef DALY_BMS_DEBUG
      DALY_BMS_DEBUG.println("No Valid Command from JSON for setDischargeMOS");
#endif
    }
  }
  updateProgress = false;
}

bool connectMQTT()
{
  if (!mqttclient.connected())
  {
#ifdef DALY_BMS_DEBUG
    DALY_BMS_DEBUG.print("Info: MQTT Client State is: ");
    DALY_BMS_DEBUG.println(mqttclient.state());
#endif
    if (mqttclient.connect(_settings.data.deviceName, _settings.data.mqttUser, _settings.data.mqttPassword))
    {
#ifdef DALY_BMS_DEBUG
      DALY_BMS_DEBUG.println(F("Info: Connected to MQTT Server"));
#endif
      if (mqttclient.connect(_settings.data.deviceName))
      {
        if (!_settings.data.mqttJson)
        {
          mqttclient.subscribe((topicStrg + "/SET/Pack_DischargeFET").c_str());
          mqttclient.subscribe((topicStrg + "/SET/Pack_ChargeFET").c_str());
          mqttclient.subscribe((topicStrg + "/SET/Pack_SOC").c_str());
          if(_settings.data.relaisFunction == 4)
            mqttclient.subscribe((topicStrg + "/SET/Relais").c_str());
        }
        else
        {
          mqttclient.subscribe((topicStrg).c_str());
        }
      }
    }
    else
    {
#ifdef DALY_BMS_DEBUG
      DALY_BMS_DEBUG.println(F("Error: No connection to MQTT Server"));
#endif
      return false; // Exit if we couldnt connect to MQTT brooker
    }
  }
#ifdef DALY_BMS_DEBUG
  DALY_BMS_DEBUG.println(F("Info: Data sent to MQTT Server"));
#endif
  return true;
}
