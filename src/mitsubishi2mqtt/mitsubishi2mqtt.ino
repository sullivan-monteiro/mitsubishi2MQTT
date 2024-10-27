/*
  mitsubishi2mqtt - Mitsubishi Heat Pump to MQTT control for Home Assistant.
  Copyright (c) 2022 gysmo38, dzungpv, shampeon, endeavour, jascdk, chrdavis, alekslyse.  All right reserved.
  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.
  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.
  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "FS.h"   // SPIFFS for store config
#include <WiFi.h> // WIFI for ESP32
#include <WiFiUdp.h>
#include <ESPmDNS.h>   // mDNS for ESP32
#include <WebServer.h> // webServer for ESP32
#include <ESPAsyncWebServer.h>
#include <WebSocketsServer.h>
#include "SPIFFS.h"        // ESP32 SPIFFS for store config
AsyncWebServer server(80); // ESP32 web
WebSocketsServer webSocket = WebSocketsServer(81);

#include <ArduinoJson.h>  // json to process MQTT: ArduinoJson 6.11.4
#include <PubSubClient.h> // MQTT: PubSubClient 2.8.0
#include <math.h>         // for rounding to Fahrenheit values
#include <map>
#include <cmath> // For roundf function

#include <ArduinoOTA.h> // for OTA
#include <HeatPump.h>   // SwiCago library: https://github.com/SwiCago/HeatPump
// #include <Ticker.h>     // for LED status (Using a Wemos D1-Mini)
#include "config.h"            // config file
#include "html_common.h"       // common code HTML (like header, footer)
#include "javascript_common.h" // common code javascript (like refresh page)
#include "html_init.h"         // code html for initial config
#include "html_menu.h"         // code html for menu
#include "html_pages.h"        // code html for pages
#include "html_metrics.h"      // prometheus metrics
// Languages
#include "languages/fr-FR.h" // default language French

// wifi, mqtt and heatpump client instances
WiFiClient espClient;
PubSubClient mqtt_client(espClient);

boolean remoteTempActive = false;

// HVAC
HeatPump hp;
unsigned long lastTempSend;
unsigned long lastMqttRetry;
unsigned long lastHpSync;
unsigned int hpConnectionRetries;
unsigned int hpConnectionTotalRetries;
unsigned long lastRemoteTemp;

// Local state
StaticJsonDocument<JSON_OBJECT_SIZE(12)> rootInfo;

// Web OTA
int uploaderror = 0;

void setup()
{
  // Start serial for debug before HVAC connect to serial
  Serial.begin(115200);
  // Serial.println(F("Starting Mitsubishi2MQTT"));
  // Mount SPIFFS filesystem
  if (SPIFFS.begin())
  {
    // Serial.println(F("Mounted file system"));
  }
  else
  {
    // Serial.println(F("Failed to mount FS -> formating"));
    SPIFFS.format();
    // if (SPIFFS.begin())
    // Serial.println(F("Mounted file system after formating"));
  }
  // set led pin as output
  pinMode(blueLedPin, OUTPUT);
  /*
    ticker.attach(0.6, tick);
  */

  // Define hostname
  hostname += hostnamePrefix;
  hostname += getId();
  setDefaults();
  loadOthers();
  loadUnit();
  mqtt_client_id = hostname;
  WiFi.mode(WIFI_STA);
  WiFi.begin("reseau-maison", "9vsKXMx$B9gKO1nY@zq15K");
  WiFi.setHostname(hostname.c_str());
  if (SPIFFS.exists(console_file))
  {
    SPIFFS.remove(console_file);
  }
  // write_log("Starting Mitsubishi2MQTT");
  // Web interface
  server.on("/", handleRoot);
  server.on("/control", handleControl);
  server.on("/setup", handleSetup);
  server.on("/unit", handleUnit);
  server.on("/status", handleStatus);
  server.on("/others", handleOthers);
  server.on("/metrics", handleMetrics);
  server.on("/logs", [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/index.html", "text/html"); });
  server.onNotFound(handleNotFound);

  server.on("/upgrade", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/update.html", "text/html"); });
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request)
            {
              // Cette fonction est appelée après la fin du téléchargement
              // La réponse est déjà envoyée dans handleUpdate
            },
            handleUpdate);

  server.begin();
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
  lastMqttRetry = 0;
  lastHpSync = 0;
  hpConnectionRetries = 0;
  hpConnectionTotalRetries = 0;

  // Configuration MQTT directe
  mqtt_fn = "easydan";
  mqtt_server = "10.0.0.1";
  mqtt_port = "1883";
  mqtt_username = "tasmota";
  mqtt_password = "DVkM$iNX$4pkg^JLdF";
  mqtt_topic = "mitsubishi2mqtt";

  mqtt_client_id = hostname;

  // setup HA topics
  ha_mode_set_topic = mqtt_topic + "/" + mqtt_fn + "/mode/set";
  ha_temp_set_topic = mqtt_topic + "/" + mqtt_fn + "/temp/set";
  ha_remote_temp_set_topic = mqtt_topic + "/" + mqtt_fn + "/remote_temp/set";
  ha_fan_set_topic = mqtt_topic + "/" + mqtt_fn + "/fan/set";
  ha_vane_set_topic = mqtt_topic + "/" + mqtt_fn + "/vane/set";
  ha_wideVane_set_topic = mqtt_topic + "/" + mqtt_fn + "/wideVane/set";
  ha_settings_topic = mqtt_topic + "/" + mqtt_fn + "/settings";
  ha_state_topic = mqtt_topic + "/" + mqtt_fn + "/state";
  ha_debug_pckts_topic = mqtt_topic + "/" + mqtt_fn + "/debug/packets";
  ha_debug_pckts_set_topic = mqtt_topic + "/" + mqtt_fn + "/debug/packets/set";
  ha_debug_logs_topic = mqtt_topic + "/" + mqtt_fn + "/debug/logs";
  ha_debug_logs_set_topic = mqtt_topic + "/" + mqtt_fn + "/debug/logs/set";
  ha_custom_packet = mqtt_topic + "/" + mqtt_fn + "/custom/send";
  ha_availability_topic = mqtt_topic + "/" + mqtt_fn + "/availability";
  ha_system_set_topic = mqtt_topic + "/" + mqtt_fn + "/system/set";

  if (others_haa)
  {
    ha_config_topic = others_haa_topic + "/climate/" + mqtt_fn + "/config";
  }

  // startup mqtt connection
  initMqtt();

  // write_log("Connection to HVAC");
  hp.setSettingsChangedCallback(hpSettingsChanged);
  hp.setStatusChangedCallback(hpStatusChanged);
  hp.setPacketCallback(hpPacketDebug);
  // Allow Remote/Panel
  hp.enableExternalUpdate();
  hp.enableAutoUpdate();
  hp.connect(&Serial);
  heatpumpStatus currentStatus = hp.getStatus();
  heatpumpSettings currentSettings = hp.getSettings();
  rootInfo["roomTemperature"] = convertCelsiusToLocalUnit(currentStatus.roomTemperature, useFahrenheit);
  rootInfo["temperature"] = convertCelsiusToLocalUnit(currentSettings.temperature, useFahrenheit);
  rootInfo["fan"] = hpGetFan(currentSettings);
  rootInfo["vane"] = currentSettings.vane;
  rootInfo["wideVane"] = currentSettings.wideVane;
  rootInfo["mode"] = hpGetMode(currentSettings);
  rootInfo["action"] = hpGetAction(currentStatus, currentSettings);
  rootInfo["compressorFrequency"] = currentStatus.compressorFrequency;
  lastTempSend = millis();

  initOTA();
}

bool loadUnit()
{
  if (!SPIFFS.exists(unit_conf))
  {
    // Serial.println(F("Unit config file not exist!"));
    return false;
  }
  File configFile = SPIFFS.open(unit_conf, "r");
  if (!configFile)
  {
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024)
  {
    return false;
  }
  std::unique_ptr<char[]> buf(new char[size]);

  configFile.readBytes(buf.get(), size);
  const size_t capacity = JSON_OBJECT_SIZE(3) + 200;
  DynamicJsonDocument doc(capacity);
  deserializeJson(doc, buf.get());
  // unit
  String unit_tempUnit = doc["unit_tempUnit"].as<String>();
  if (unit_tempUnit == "fah")
    useFahrenheit = true;
  min_temp = doc["min_temp"].as<uint8_t>();
  max_temp = doc["max_temp"].as<uint8_t>();
  temp_step = doc["temp_step"].as<String>();
  // mode
  String supportMode = doc["support_mode"].as<String>();
  if (supportMode == "nht")
    supportHeatMode = false;
  
  return true;
}

bool loadOthers()
{
  if (!SPIFFS.exists(others_conf))
  {
    // Serial.println(F("Others config file not exist!"));
    return false;
  }
  File configFile = SPIFFS.open(others_conf, "r");
  if (!configFile)
  {
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024)
  {
    return false;
  }
  std::unique_ptr<char[]> buf(new char[size]);

  configFile.readBytes(buf.get(), size);
  const size_t capacity = JSON_OBJECT_SIZE(4) + 200;
  DynamicJsonDocument doc(capacity);
  deserializeJson(doc, buf.get());
  // unit
  String unit_tempUnit = doc["unit_tempUnit"].as<String>();
  if (unit_tempUnit == "fah")
    useFahrenheit = true;
  others_haa_topic = doc["haat"].as<String>();
  String haa = doc["haa"].as<String>();
  String debugPckts = doc["debugPckts"].as<String>();
  String debugLogs = doc["debugLogs"].as<String>();
  if (strcmp(haa.c_str(), "OFF") == 0)
  {
    others_haa = false;
  }
  if (strcmp(debugPckts.c_str(), "ON") == 0)
  {
    _debugModePckts = true;
  }
  if (strcmp(debugLogs.c_str(), "ON") == 0)
  {
    _debugModeLogs = true;
  }
  return true;
}
void saveUnit(String tempUnit, String supportMode, String loginPassword, String minTemp, String maxTemp, String tempStep)
{
  const size_t capacity = JSON_OBJECT_SIZE(6) + 200;
  DynamicJsonDocument doc(capacity);
  // if temp unit is empty, we use default celcius
  if (tempUnit.isEmpty())
    tempUnit = "cel";
  doc["unit_tempUnit"] = tempUnit;
  // if minTemp is empty, we use default 16
  if (minTemp.isEmpty())
    minTemp = 16;
  doc["min_temp"] = minTemp;
  // if maxTemp is empty, we use default 31
  if (maxTemp.isEmpty())
    maxTemp = 31;
  doc["max_temp"] = maxTemp;
  // if tempStep is empty, we use default 1
  if (tempStep.isEmpty())
    tempStep = 1;
  doc["temp_step"] = tempStep;
  // if support mode is empty, we use default all mode
  if (supportMode.isEmpty())
    supportMode = "all";
  doc["support_mode"] = supportMode;

  File configFile = SPIFFS.open(unit_conf, "w");
  if (!configFile)
  {
    // Serial.println(F("Failed to open config file for writing"));
  }
  serializeJson(doc, configFile);
  configFile.close();
}

void saveOthers(String haa, String haat, String debugPckts, String debugLogs)
{
  const size_t capacity = JSON_OBJECT_SIZE(4) + 130;
  DynamicJsonDocument doc(capacity);
  doc["haa"] = haa;
  doc["haat"] = haat;
  doc["debugPckts"] = debugPckts;
  doc["debugLogs"] = debugLogs;
  File configFile = SPIFFS.open(others_conf, "w");
  if (!configFile)
  {
    // Serial.println(F("Failed to open wifi file for writing"));
  }
  serializeJson(doc, configFile);
  delay(10);
  configFile.close();
}

void initMqtt()
{
  mqtt_client.setServer(mqtt_server.c_str(), atoi(mqtt_port.c_str()));
  mqtt_client.setCallback(mqttCallback);
  mqttConnect();
}

// Enable OTA only when connected as a client.
void initOTA()
{
  // write_log("Start OTA Listener");
  ArduinoOTA.setHostname(hostname.c_str());
  if (ota_pwd.length() > 0)
  {
    ArduinoOTA.setPassword(ota_pwd.c_str());
  }
  ArduinoOTA.onStart([]()
                     {
                       // write_log("Start");
                     });
  ArduinoOTA.onEnd([]()
                   {
                     // write_log("\nEnd");
                   });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        {
                          //    write_log("Progress: %u%%\r", (progress / (total / 100)));
                        });
  ArduinoOTA.onError([](ota_error_t error)
                     {
                       //    write_log("Error[%u]: ", error);
                       // if (error == OTA_AUTH_ERROR) Serial.println(F("Auth Failed"));
                       // else if (error == OTA_BEGIN_ERROR) Serial.println(F("Begin Failed"));
                       // else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
                       // else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
                       // else if (error == OTA_END_ERROR) Serial.println(F("End Failed"));
                     });
  ArduinoOTA.begin();
}
void setDefaults()
{
  others_haa = true;
  others_haa_topic = "homeassistant";
}

// Handler webserver response

void sendWrappedHTML(String content, AsyncWebServerRequest *request)
{
  String headerContent = FPSTR(html_common_header);
  String footerContent = FPSTR(html_common_footer);
  String toSend = headerContent + content + footerContent;
  toSend.replace(F("_UNIT_NAME_"), hostname);
  toSend.replace(F("_VERSION_"), m2mqtt_version);
  request->send(200, F("text/html"), toSend);
}

void handleNotFound(AsyncWebServerRequest *request)
{

  AsyncWebServerResponse *response = request->beginResponse(302);
  response->addHeader("Location", "/");
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
  return;
}

void handleReboot(AsyncWebServerRequest *request)
{
  String initRebootPage = FPSTR(html_init_reboot);
  initRebootPage.replace("_TXT_INIT_REBOOT_", FPSTR(txt_init_reboot));
  sendWrappedHTML(initRebootPage, request);
  delay(500);
  ESP.restart();
}

void handleRoot(AsyncWebServerRequest *request)
{
  if (request->hasArg("REBOOT"))
  {
    String rebootPage = FPSTR(html_page_reboot);
    String countDown = FPSTR(count_down_script);
    rebootPage.replace("_TXT_M_REBOOT_", FPSTR(txt_m_reboot));
    sendWrappedHTML(rebootPage + countDown, request);
    delay(500);
    ESP.restart();
  }
  else
  {
    String menuRootPage = FPSTR(html_menu_root);
    menuRootPage.replace("_SHOW_LOGOUT_", (String)(login_password.length() > 0));
    // not show control button if hp not connected
    menuRootPage.replace("_SHOW_CONTROL_", (String)(hp.isConnected()));
    menuRootPage.replace("_TXT_CONTROL_", FPSTR(txt_control));
    menuRootPage.replace("_TXT_SETUP_", FPSTR(txt_setup));
    menuRootPage.replace("_TXT_STATUS_", FPSTR(txt_status));
    menuRootPage.replace("_TXT_FW_UPGRADE_", FPSTR(txt_firmware_upgrade));
    menuRootPage.replace("_TXT_REBOOT_", FPSTR(txt_reboot));
    menuRootPage.replace("_TXT_LOGOUT_", FPSTR(txt_logout));
    request->send(200, "text/html", menuRootPage);
  }
}

void handleSetup(AsyncWebServerRequest *request)
{
  if (request->hasArg("RESET"))
  {
    String pageReset = FPSTR(html_page_reset);
    String ssid = hostnamePrefix;
    ssid += getId();
    pageReset.replace("_TXT_M_RESET_", FPSTR(txt_m_reset));
    pageReset.replace("_SSID_", ssid);
    sendWrappedHTML(pageReset, request);
    SPIFFS.format();
    delay(500);
    ESP.restart();
  }
  else
  {
    String menuSetupPage = FPSTR(html_menu_setup);
    menuSetupPage.replace("_TXT_MQTT_", FPSTR(txt_MQTT));
    menuSetupPage.replace("_TXT_WIFI_", FPSTR(txt_WIFI));
    menuSetupPage.replace("_TXT_UNIT_", FPSTR(txt_unit));
    menuSetupPage.replace("_TXT_OTHERS_", FPSTR(txt_others));
    menuSetupPage.replace("_TXT_RESET_", FPSTR(txt_reset));
    menuSetupPage.replace("_TXT_BACK_", FPSTR(txt_back));
    menuSetupPage.replace("_TXT_RESETCONFIRM_", FPSTR(txt_reset_confirm));
    sendWrappedHTML(menuSetupPage, request);
  }
}

void rebootAndSendPage(AsyncWebServerRequest *request)
{
  String saveRebootPage = FPSTR(html_page_save_reboot);
  String countDown = FPSTR(count_down_script);
  saveRebootPage.replace("_TXT_M_SAVE_", FPSTR(txt_m_save));
  sendWrappedHTML(saveRebootPage + countDown, request);
  delay(500);
  ESP.restart();
}

void handleOthers(AsyncWebServerRequest *request)
{
  if (request->method() == HTTP_POST)
  {
    saveOthers(request->arg("HAA"), request->arg("haat"), request->arg("DebugPckts"), request->arg("DebugLogs"));
    rebootAndSendPage(request);
  }
  else
  {
    String othersPage = FPSTR(html_page_others);
    othersPage.replace("_TXT_SAVE_", FPSTR(txt_save));
    othersPage.replace("_TXT_BACK_", FPSTR(txt_back));
    othersPage.replace("_TXT_F_ON_", FPSTR(txt_f_on));
    othersPage.replace("_TXT_F_OFF_", FPSTR(txt_f_off));
    othersPage.replace("_TXT_OTHERS_TITLE_", FPSTR(txt_others_title));
    othersPage.replace("_TXT_OTHERS_HAAUTO_", FPSTR(txt_others_haauto));
    othersPage.replace("_TXT_OTHERS_HATOPIC_", FPSTR(txt_others_hatopic));
    othersPage.replace("_TXT_OTHERS_DEBUG_PCKTS_", FPSTR(txt_others_debug_packets));
    othersPage.replace("_TXT_OTHERS_DEBUG_LOGS_", FPSTR(txt_others_debug_log));

    othersPage.replace("_HAA_TOPIC_", others_haa_topic);
    if (others_haa)
    {
      othersPage.replace("_HAA_ON_", "selected");
    }
    else
    {
      othersPage.replace("_HAA_OFF_", "selected");
    }
    if (_debugModePckts)
    {
      othersPage.replace("_DEBUG_PCKTS_ON_", "selected");
    }
    else
    {
      othersPage.replace("_DEBUG_PCKTS_OFF_", "selected");
    }
    if (_debugModeLogs)
    {
      othersPage.replace("_DEBUG_LOGS_ON_", "selected");
    }
    else
    {
      othersPage.replace("_DEBUG_LOGS_OFF_", "selected");
    }
    sendWrappedHTML(othersPage, request);
  }
}

void handleUnit(AsyncWebServerRequest *request)
{
  if (request->method() == HTTP_POST)
  {
    saveUnit(request->arg("tu"), request->arg("md"), request->arg("lpw"), (String)convertLocalUnitToCelsius(request->arg("min_temp").toFloat(), useFahrenheit), (String)convertLocalUnitToCelsius(request->arg("max_temp").toFloat(), useFahrenheit), request->arg("temp_step"));
    rebootAndSendPage(request);
  }
  else
  {
    String unitPage = FPSTR(html_page_unit);
    unitPage.replace("_TXT_SAVE_", FPSTR(txt_save));
    unitPage.replace("_TXT_BACK_", FPSTR(txt_back));
    unitPage.replace("_TXT_UNIT_TITLE_", FPSTR(txt_unit_title));
    unitPage.replace("_TXT_UNIT_TEMP_", FPSTR(txt_unit_temp));
    unitPage.replace("_TXT_UNIT_MINTEMP_", FPSTR(txt_unit_mintemp));
    unitPage.replace("_TXT_UNIT_MAXTEMP_", FPSTR(txt_unit_maxtemp));
    unitPage.replace("_TXT_UNIT_STEPTEMP_", FPSTR(txt_unit_steptemp));
    unitPage.replace("_TXT_UNIT_MODES_", FPSTR(txt_unit_modes));
    unitPage.replace("_TXT_UNIT_PASSWORD_", FPSTR(txt_unit_password));
    unitPage.replace("_TXT_F_CELSIUS_", FPSTR(txt_f_celsius));
    unitPage.replace("_TXT_F_FH_", FPSTR(txt_f_fh));
    unitPage.replace("_TXT_F_ALLMODES_", FPSTR(txt_f_allmodes));
    unitPage.replace("_TXT_F_NOHEAT_", FPSTR(txt_f_noheat));
    unitPage.replace(F("_MIN_TEMP_"), String(convertCelsiusToLocalUnit(min_temp, useFahrenheit)));
    unitPage.replace(F("_MAX_TEMP_"), String(convertCelsiusToLocalUnit(max_temp, useFahrenheit)));
    unitPage.replace(F("_TEMP_STEP_"), String(temp_step));
    // temp
    if (useFahrenheit)
      unitPage.replace(F("_TU_FAH_"), F("selected"));
    else
      unitPage.replace(F("_TU_CEL_"), F("selected"));
    // mode
    if (supportHeatMode)
      unitPage.replace(F("_MD_ALL_"), F("selected"));
    else
      unitPage.replace(F("_MD_NONHEAT_"), F("selected"));
    unitPage.replace(F("_LOGIN_PASSWORD_"), login_password);
    sendWrappedHTML(unitPage, request);
  }
}

void handleStatus(AsyncWebServerRequest *request)
{
  String statusPage = FPSTR(html_page_status);
  statusPage.replace("_TXT_BACK_", FPSTR(txt_back));
  statusPage.replace("_TXT_STATUS_TITLE_", FPSTR(txt_status_title));
  statusPage.replace("_TXT_STATUS_HVAC_", FPSTR(txt_status_hvac));
  statusPage.replace("_TXT_STATUS_MQTT_", FPSTR(txt_status_mqtt));
  statusPage.replace("_TXT_STATUS_WIFI_", FPSTR(txt_status_wifi));
  statusPage.replace("_TXT_RETRIES_HVAC_", FPSTR(txt_retries_hvac));

  if (request->hasArg("mrconn"))
    mqttConnect();

  String connected = F("<span style='color:#47c266'><b>");
  connected += FPSTR(txt_status_connect);
  connected += F("</b><span>");

  String disconnected = F("<span style='color:#d43535'><b>");
  disconnected += FPSTR(txt_status_disconnect);
  disconnected += F("</b></span>");

  if ((Serial) and hp.isConnected())
    statusPage.replace(F("_HVAC_STATUS_"), connected);
  else
    statusPage.replace(F("_HVAC_STATUS_"), disconnected);
  if (mqtt_client.connected())
    statusPage.replace(F("_MQTT_STATUS_"), connected);
  else
    statusPage.replace(F("_MQTT_STATUS_"), disconnected);
  statusPage.replace(F("_HVAC_RETRIES_"), String(hpConnectionTotalRetries));
  statusPage.replace(F("_MQTT_REASON_"), String(mqtt_client.state()));
  statusPage.replace(F("_WIFI_STATUS_"), String(WiFi.RSSI()));
  sendWrappedHTML(statusPage, request);
}

void handleControl(AsyncWebServerRequest *request)
{
  // not connected to hp, redirect to status page
  if (!hp.isConnected())
  {
    AsyncWebServerResponse *response = request->beginResponse(302);
    response->addHeader("Location", "/status");
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
    return;
  }
  heatpumpSettings settings = hp.getSettings();
  settings = change_states(settings, request);
  String controlPage = FPSTR(html_page_control);
  String headerContent = FPSTR(html_common_header);
  String footerContent = FPSTR(html_common_footer);
  // write_log("Enter HVAC control");
  headerContent.replace("_UNIT_NAME_", hostname);
  footerContent.replace("_VERSION_", m2mqtt_version);
  controlPage.replace("_TXT_BACK_", FPSTR(txt_back));
  controlPage.replace("_UNIT_NAME_", hostname);
  controlPage.replace("_RATE_", "60");
  controlPage.replace("_ROOMTEMP_", String(convertCelsiusToLocalUnit(hp.getRoomTemperature(), useFahrenheit)));
  controlPage.replace("_USE_FAHRENHEIT_", (String)useFahrenheit);
  controlPage.replace("_TEMP_SCALE_", getTemperatureScale());
  controlPage.replace("_HEAT_MODE_SUPPORT_", (String)supportHeatMode);
  controlPage.replace(F("_MIN_TEMP_"), String(convertCelsiusToLocalUnit(min_temp, useFahrenheit)));
  controlPage.replace(F("_MAX_TEMP_"), String(convertCelsiusToLocalUnit(max_temp, useFahrenheit)));
  controlPage.replace(F("_TEMP_STEP_"), String(temp_step));
  controlPage.replace("_TXT_CTRL_CTEMP_", FPSTR(txt_ctrl_ctemp));
  controlPage.replace("_TXT_CTRL_TEMP_", FPSTR(txt_ctrl_temp));
  controlPage.replace("_TXT_CTRL_TITLE_", FPSTR(txt_ctrl_title));
  controlPage.replace("_TXT_CTRL_POWER_", FPSTR(txt_ctrl_power));
  controlPage.replace("_TXT_CTRL_MODE_", FPSTR(txt_ctrl_mode));
  controlPage.replace("_TXT_CTRL_FAN_", FPSTR(txt_ctrl_fan));
  controlPage.replace("_TXT_CTRL_VANE_", FPSTR(txt_ctrl_vane));
  controlPage.replace("_TXT_CTRL_WVANE_", FPSTR(txt_ctrl_wvane));
  controlPage.replace("_TXT_F_ON_", FPSTR(txt_f_on));
  controlPage.replace("_TXT_F_OFF_", FPSTR(txt_f_off));
  controlPage.replace("_TXT_F_AUTO_", FPSTR(txt_f_auto));
  controlPage.replace("_TXT_F_HEAT_", FPSTR(txt_f_heat));
  controlPage.replace("_TXT_F_DRY_", FPSTR(txt_f_dry));
  controlPage.replace("_TXT_F_COOL_", FPSTR(txt_f_cool));
  controlPage.replace("_TXT_F_FAN_", FPSTR(txt_f_fan));
  controlPage.replace("_TXT_F_QUIET_", FPSTR(txt_f_quiet));
  controlPage.replace("_TXT_F_SPEED_", FPSTR(txt_f_speed));
  controlPage.replace("_TXT_F_SWING_", FPSTR(txt_f_swing));
  controlPage.replace("_TXT_F_POS_", FPSTR(txt_f_pos));

  if (strcmp(settings.power, "ON") == 0)
  {
    controlPage.replace("_POWER_ON_", "selected");
  }
  else if (strcmp(settings.power, "OFF") == 0)
  {
    controlPage.replace("_POWER_OFF_", "selected");
  }

  if (strcmp(settings.mode, "HEAT") == 0)
  {
    controlPage.replace("_MODE_H_", "selected");
  }
  else if (strcmp(settings.mode, "DRY") == 0)
  {
    controlPage.replace("_MODE_D_", "selected");
  }
  else if (strcmp(settings.mode, "COOL") == 0)
  {
    controlPage.replace("_MODE_C_", "selected");
  }
  else if (strcmp(settings.mode, "FAN") == 0)
  {
    controlPage.replace("_MODE_F_", "selected");
  }
  else if (strcmp(settings.mode, "AUTO") == 0)
  {
    controlPage.replace("_MODE_A_", "selected");
  }

  if (strcmp(settings.fan, "AUTO") == 0)
  {
    controlPage.replace("_FAN_A_", "selected");
  }
  else if (strcmp(settings.fan, "QUIET") == 0)
  {
    controlPage.replace("_FAN_Q_", "selected");
  }
  else if (strcmp(settings.fan, "1") == 0)
  {
    controlPage.replace("_FAN_1_", "selected");
  }
  else if (strcmp(settings.fan, "2") == 0)
  {
    controlPage.replace("_FAN_2_", "selected");
  }
  else if (strcmp(settings.fan, "3") == 0)
  {
    controlPage.replace("_FAN_3_", "selected");
  }
  else if (strcmp(settings.fan, "4") == 0)
  {
    controlPage.replace("_FAN_4_", "selected");
  }

  controlPage.replace("_VANE_V_", settings.vane);
  if (strcmp(settings.vane, "AUTO") == 0)
  {
    controlPage.replace("_VANE_A_", "selected");
  }
  else if (strcmp(settings.vane, "1") == 0)
  {
    controlPage.replace("_VANE_1_", "selected");
  }
  else if (strcmp(settings.vane, "2") == 0)
  {
    controlPage.replace("_VANE_2_", "selected");
  }
  else if (strcmp(settings.vane, "3") == 0)
  {
    controlPage.replace("_VANE_3_", "selected");
  }
  else if (strcmp(settings.vane, "4") == 0)
  {
    controlPage.replace("_VANE_4_", "selected");
  }
  else if (strcmp(settings.vane, "5") == 0)
  {
    controlPage.replace("_VANE_5_", "selected");
  }
  else if (strcmp(settings.vane, "SWING") == 0)
  {
    controlPage.replace("_VANE_S_", "selected");
  }

  controlPage.replace("_WIDEVANE_V_", settings.wideVane);
  if (strcmp(settings.wideVane, "<<") == 0)
  {
    controlPage.replace("_WVANE_1_", "selected");
  }
  else if (strcmp(settings.wideVane, "<") == 0)
  {
    controlPage.replace("_WVANE_2_", "selected");
  }
  else if (strcmp(settings.wideVane, "|") == 0)
  {
    controlPage.replace("_WVANE_3_", "selected");
  }
  else if (strcmp(settings.wideVane, ">") == 0)
  {
    controlPage.replace("_WVANE_4_", "selected");
  }
  else if (strcmp(settings.wideVane, ">>") == 0)
  {
    controlPage.replace("_WVANE_5_", "selected");
  }
  else if (strcmp(settings.wideVane, "<>") == 0)
  {
    controlPage.replace("_WVANE_6_", "selected");
  }
  else if (strcmp(settings.wideVane, "SWING") == 0)
  {
    controlPage.replace("_WVANE_S_", "selected");
  }
  controlPage.replace("_TEMP_", String(convertCelsiusToLocalUnit(hp.getTemperature(), useFahrenheit)));

  // We need to send the page content in chunks to overcome
  // a limitation on the maximum size we can send at one
  // time (approx 6k).
  AsyncResponseStream *response = request->beginResponseStream("text/html");

  response->print(headerContent);
  response->print(controlPage);
  response->print(footerContent);

  request->send(response);
  // delay(100);
}

void handleLogs(AsyncWebServerRequest *request)
{
}

void handleMetrics(AsyncWebServerRequest *request)
{
  String metrics = FPSTR(html_metrics);

  heatpumpSettings currentSettings = hp.getSettings();
  heatpumpStatus currentStatus = hp.getStatus();

  String hppower = currentSettings.power == "ON" ? "1" : "0";

  String hpfan = currentSettings.fan;
  if (hpfan == "AUTO")
    hpfan = "-1";
  if (hpfan == "QUIET")
    hpfan = "0";

  String hpvane = currentSettings.vane;
  if (hpvane == "AUTO")
    hpvane = "-1";
  if (hpvane == "SWING")
    hpvane = "0";

  String hpwidevane = "-2";
  if (currentSettings.wideVane == "SWING")
    hpwidevane = "0";
  if (currentSettings.wideVane == "<<")
    hpwidevane = "1";
  if (currentSettings.wideVane == "<")
    hpwidevane = "2";
  if (currentSettings.wideVane == "|")
    hpwidevane = "3";
  if (currentSettings.wideVane == ">")
    hpwidevane = "4";
  if (currentSettings.wideVane == ">>")
    hpwidevane = "5";
  if (currentSettings.wideVane == "<>")
    hpwidevane = "6";

  String hpmode = "-2";
  if (currentSettings.mode == "AUTO")
    hpmode = "-1";
  if (currentSettings.mode == "COOL")
    hpmode = "1";
  if (currentSettings.mode == "DRY")
    hpmode = "2";
  if (currentSettings.mode == "HEAT")
    hpmode = "3";
  if (currentSettings.mode == "FAN")
    hpmode = "4";
  if (hppower == "0")
    hpmode = "0";

  metrics.replace("_UNIT_NAME_", hostname);
  metrics.replace("_VERSION_", m2mqtt_version);
  metrics.replace("_POWER_", hppower);
  metrics.replace("_ROOMTEMP_", (String)currentStatus.roomTemperature);
  metrics.replace("_TEMP_", (String)currentSettings.temperature);
  metrics.replace("_FAN_", hpfan);
  metrics.replace("_VANE_", hpvane);
  metrics.replace("_WIDEVANE_", hpwidevane);
  metrics.replace("_MODE_", hpmode);
  metrics.replace("_OPER_", (String)currentStatus.operating);
  metrics.replace("_COMPFREQ_", (String)currentStatus.compressorFrequency);
  request->send(200, F("text/plain"), metrics);
}

// Gestionnaire de mise à jour
void handleUpdate(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
  size_t contentLength, currentLength;
  bool updateStarted;
  if (!index)
  { // Début de l'upload
    Serial.println("Début de la mise à jour");
    Serial.printf("Nom du fichier: %s\n", filename.c_str());

    // Vérifier si c'est un fichier .bin
    if (!filename.endsWith(".bin"))
    {
      request->send(400, "text/plain", "Fichier non valide");
      return;
    }

    contentLength = request->contentLength();
    Serial.printf("Taille du fichier: %u bytes\n", contentLength);

    // Démarrer la mise à jour
    if (!Update.begin(contentLength))
    {
      Update.printError(Serial);
      request->send(400, "text/plain", "OTA impossible. Erreur #1");
      return;
    }

    updateStarted = true;
  }

  // Écriture des données
  if (updateStarted)
  {
    if (Update.write(data, len) != len)
    {
      Update.printError(Serial);
      request->send(400, "text/plain", "OTA impossible. Erreur #2");
      return;
    }

    currentLength += len;
    // Calcul du pourcentage
    int progress = (currentLength * 100) / contentLength;
    Serial.printf("Progress: %d%%\n", progress);
  }

  // Fin de l'upload
  if (final)
  {
    if (!Update.end(true))
    {
      Update.printError(Serial);
      request->send(400, "text/plain", "OTA impossible. Erreur #3");
      return;
    }

    Serial.println("Mise à jour terminée");
    request->send(200, "text/plain", "OK");

    // Redémarrage différé
    delay(500);
    ESP.restart();
  }
}

void write_log(String log)
{
  File logFile = SPIFFS.open(console_file, "a");
  logFile.println(log);
  logFile.close();
}

heatpumpSettings change_states(heatpumpSettings settings, AsyncWebServerRequest *request)
{
  if (request->hasArg("CONNECT"))
  {
    hp.connect(&Serial);
  }
  else
  {
    bool update = false;
    if (request->hasArg("POWER"))
    {
      settings.power = request->arg("POWER").c_str();
      update = true;
    }
    if (request->hasArg("MODE"))
    {
      settings.mode = request->arg("MODE").c_str();
      update = true;
    }
    if (request->hasArg("TEMP"))
    {
      settings.temperature = convertLocalUnitToCelsius(request->arg("TEMP").toFloat(), useFahrenheit);
      update = true;
    }
    if (request->hasArg("FAN"))
    {
      settings.fan = request->arg("FAN").c_str();
      update = true;
    }
    if (request->hasArg("VANE"))
    {
      settings.vane = request->arg("VANE").c_str();
      update = true;
    }
    if (request->hasArg("WIDEVANE"))
    {
      settings.wideVane = request->arg("WIDEVANE").c_str();
      update = true;
    }
    if (update)
    {
      hp.setSettings(settings);
    }
  }
  return settings;
}

void readHeatPumpSettings()
{
  heatpumpSettings currentSettings = hp.getSettings();

  rootInfo.clear();
  rootInfo["temperature"] = convertCelsiusToLocalUnit(currentSettings.temperature, useFahrenheit);
  rootInfo["fan"] = hpGetFan(currentSettings);
  rootInfo["vane"] = currentSettings.vane;
  rootInfo["wideVane"] = currentSettings.wideVane;
  rootInfo["mode"] = hpGetMode(currentSettings);
}

void hpSettingsChanged()
{
  // send room temp, operating info and all information
  readHeatPumpSettings();

  String mqttOutput;
  serializeJson(rootInfo, mqttOutput);

  if (!mqtt_client.publish(ha_settings_topic.c_str(), mqttOutput.c_str(), true))
  {
    if (_debugModeLogs)
      mqtt_client.publish(ha_debug_logs_topic.c_str(), (char *)("Failed to publish hp settings"));
  }

  hpStatusChanged(hp.getStatus());
}

String hpGetMode(heatpumpSettings hpSettings)
{
  // Map the heat pump state to one of HA's HVAC_MODE_* values.
  // https://github.com/home-assistant/core/blob/master/homeassistant/components/climate/const.py#L17-L37

  String hppower = String(hpSettings.power);
  if (hppower.equalsIgnoreCase("off"))
  {
    return "off";
  }

  String hpmode = String(hpSettings.mode);
  hpmode.toLowerCase();

  if (hpmode == "fan")
    return "fan_only";
  else if (hpmode == "auto")
    return "heat_cool";
  else
    return hpmode; // cool, heat, dry
}

String hpGetFan(heatpumpSettings hpSettings)
{
  // Map the fan speed to one of HA's FAN_* values.
  // https://github.com/home-assistant/core/blob/master/homeassistant/components/climate/const.py#L75-L85

  String hpfan = String(hpSettings.fan);
  hpfan.toLowerCase();

  if (hpfan == "quiet")
    return "diffuse";
  else if (hpfan == "1")
    return "low";
  else if (hpfan == "2")
    return "middle";
  else if (hpfan == "3")
    return "medium";
  else if (hpfan == "4")
    return "high";
  else
    return hpfan; // auto
}

String hpGetAction(heatpumpStatus hpStatus, heatpumpSettings hpSettings)
{
  // Map heat pump state to one of HA's CURRENT_HVAC_* values.
  // https://github.com/home-assistant/core/blob/master/homeassistant/components/climate/const.py#L80-L86

  String hppower = String(hpSettings.power);
  if (hppower.equalsIgnoreCase("off"))
  {
    return "off";
  }

  String hpmode = String(hpSettings.mode);
  hpmode.toLowerCase();

  if (hpmode == "fan")
    return "fan";
  else if (!hpStatus.operating)
    return "idle";
  else if (hpmode == "auto")
    return "idle";
  else if (hpmode == "cool")
    return "cooling";
  else if (hpmode == "heat")
    return "heating";
  else if (hpmode == "dry")
    return "drying";
  else
    return hpmode; // unknown
}

void hpStatusChanged(heatpumpStatus currentStatus)
{
  if (millis() - lastTempSend > SEND_ROOM_TEMP_INTERVAL_MS)
  {                      // only send the temperature every SEND_ROOM_TEMP_INTERVAL_MS (millis rollover tolerant)
    hpCheckRemoteTemp(); // if the remote temperature feed from mqtt is stale, disable it and revert to the internal thermometer.

    // send room temp, operating info and all information
    heatpumpSettings currentSettings = hp.getSettings();

    if (currentStatus.roomTemperature == 0)
      return;

    rootInfo.clear();
    rootInfo["roomTemperature"] = convertCelsiusToLocalUnit(currentStatus.roomTemperature, useFahrenheit);
    rootInfo["temperature"] = convertCelsiusToLocalUnit(currentSettings.temperature, useFahrenheit);
    rootInfo["fan"] = hpGetFan(currentSettings);
    rootInfo["vane"] = currentSettings.vane;
    rootInfo["wideVane"] = currentSettings.wideVane;
    rootInfo["mode"] = hpGetMode(currentSettings);
    rootInfo["action"] = hpGetAction(currentStatus, currentSettings);
    rootInfo["compressorFrequency"] = currentStatus.compressorFrequency;
    String mqttOutput;
    serializeJson(rootInfo, mqttOutput);

    if (!mqtt_client.publish_P(ha_state_topic.c_str(), mqttOutput.c_str(), false))
    {
      if (_debugModeLogs)
        mqtt_client.publish(ha_debug_logs_topic.c_str(), (char *)("Failed to publish hp status change"));
    }

    lastTempSend = millis();
  }
}

void hpCheckRemoteTemp()
{
  if (remoteTempActive && (millis() - lastRemoteTemp > CHECK_REMOTE_TEMP_INTERVAL_MS))
  { // if it's been 5 minutes since last remote_temp message, revert back to HP internal temp sensor
    remoteTempActive = false;
    float temperature = 0;
    hp.setRemoteTemperature(temperature);
    hp.update();
  }
}

void hpPacketDebug(byte *packet, unsigned int length, const char *packetDirection)
{
  if (_debugModePckts)
  {
    String message;
    for (unsigned int idx = 0; idx < length; idx++)
    {
      if (packet[idx] < 16)
      {
        message += "0"; // pad single hex digits with a 0
      }
      message += String(packet[idx], HEX) + " ";
    }

    const size_t bufferSize = JSON_OBJECT_SIZE(10);
    StaticJsonDocument<bufferSize> root;

    root[packetDirection] = message;
    String mqttOutput;
    serializeJson(root, mqttOutput);
    if (!mqtt_client.publish(ha_debug_pckts_topic.c_str(), mqttOutput.c_str()))
    {
      mqtt_client.publish(ha_debug_logs_topic.c_str(), (char *)("Failed to publish to heatpump/debug topic"));
    }
  }
}

// Used to send a dummy packet in state topic to validate action in HA interface
// HA change GUI appareance before having a valid state from the unit
void hpSendLocalState()
{

  String mqttOutput;
  serializeJson(rootInfo, mqttOutput);
  mqtt_client.publish(ha_debug_pckts_topic.c_str(), mqttOutput.c_str(), false);
  if (!mqtt_client.publish_P(ha_state_topic.c_str(), mqttOutput.c_str(), false))
  {
    if (_debugModeLogs)
      mqtt_client.publish(ha_debug_logs_topic.c_str(), (char *)("Failed to publish dummy hp status change"));
  }

  // Restart counter for waiting enought time for the unit to update before sending a state packet
  lastTempSend = millis();
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{

  // Copy payload into message buffer
  char message[length + 1];
  for (unsigned int i = 0; i < length; i++)
  {
    message[i] = (char)payload[i];
  }
  message[length] = '\0';

  // HA topics
  // Receive power topic
  if (strcmp(topic, ha_mode_set_topic.c_str()) == 0)
  {
    String modeUpper = message;
    modeUpper.toUpperCase();
    if (modeUpper == "OFF")
    {
      rootInfo["mode"] = "off";
      rootInfo["action"] = "off";
      hpSendLocalState();
      hp.setPowerSetting("OFF");
    }
    else
    {
      if (modeUpper == "HEAT_COOL")
      {
        rootInfo["mode"] = "heat_cool";
        rootInfo["action"] = "idle";
        modeUpper = "AUTO";
      }
      else if (modeUpper == "HEAT")
      {
        rootInfo["mode"] = "heat";
        rootInfo["action"] = "heating";
      }
      else if (modeUpper == "COOL")
      {
        rootInfo["mode"] = "cool";
        rootInfo["action"] = "cooling";
      }
      else if (modeUpper == "DRY")
      {
        rootInfo["mode"] = "dry";
        rootInfo["action"] = "drying";
      }
      else if (modeUpper == "FAN_ONLY")
      {
        rootInfo["mode"] = "fan_only";
        rootInfo["action"] = "fan";
        modeUpper = "FAN";
      }
      else
      {
        return;
      }
      hpSendLocalState();
      hp.setPowerSetting("ON");
      hp.setModeSetting(modeUpper.c_str());
    }
  }
  else if (strcmp(topic, ha_temp_set_topic.c_str()) == 0)
  {
    float temperature = strtof(message, NULL);
    float temperature_c = convertLocalUnitToCelsius(temperature, useFahrenheit);
    if (temperature_c < min_temp || temperature_c > max_temp)
    {
      temperature_c = 23;
      rootInfo["temperature"] = convertCelsiusToLocalUnit(temperature_c, useFahrenheit);
    }
    else
    {
      rootInfo["temperature"] = temperature;
    }
    hpSendLocalState();
    hp.setTemperature(temperature_c);
  }
  else if (strcmp(topic, ha_fan_set_topic.c_str()) == 0)
  {
    String fanUpper = message;
    fanUpper.toUpperCase();
    String fanSpeed = fanUpper;
    if (fanUpper == "DIFFUSE")
    {
      fanSpeed = "QUIET";
    }
    else if (fanUpper == "LOW")
    {
      fanSpeed = "1";
    }
    else if (fanUpper == "MIDDLE")
    {
      fanSpeed = "2";
    }
    else if (fanUpper == "MEDIUM")
    {
      fanSpeed = "3";
    }
    else if (fanUpper == "HIGH")
    {
      fanSpeed = "4";
    }
    rootInfo["fan"] = (String)message;
    hpSendLocalState();
    hp.setFanSpeed(fanSpeed.c_str());
  }
  else if (strcmp(topic, ha_vane_set_topic.c_str()) == 0)
  {
    rootInfo["vane"] = (String)message;
    hpSendLocalState();
    hp.setVaneSetting(message);
  }
  else if (strcmp(topic, ha_wideVane_set_topic.c_str()) == 0)
  {
    rootInfo["wideVane"] = (String)message;
    hpSendLocalState();
    hp.setWideVaneSetting(message);
  }
  else if (strcmp(topic, ha_remote_temp_set_topic.c_str()) == 0)
  {
    float temperature = strtof(message, NULL);
    if (temperature == 0)
    {                           // Remote temp disabled by mqtt topic set
      remoteTempActive = false; // clear the remote temp flag
      hp.setRemoteTemperature(0.0);
    }
    else
    {
      remoteTempActive = true;   // Remote temp has been pushed.
      lastRemoteTemp = millis(); // Note time
      hp.setRemoteTemperature(convertLocalUnitToCelsius(temperature, useFahrenheit));
    }
  }
  else if (strcmp(topic, ha_system_set_topic.c_str()) == 0)
  { // We receive command for board
    if (strcmp(message, "reboot") == 0)
    { // We receive reboot command
      ESP.restart();
    }
  }
  else if (strcmp(topic, ha_debug_pckts_set_topic.c_str()) == 0)
  { // if the incoming message is on the heatpump_debug_set_topic topic...
    if (strcmp(message, "on") == 0)
    {
      _debugModePckts = true;
      mqtt_client.publish(ha_debug_pckts_topic.c_str(), (char *)("Debug packets mode enabled"));
    }
    else if (strcmp(message, "off") == 0)
    {
      _debugModePckts = false;
      mqtt_client.publish(ha_debug_pckts_topic.c_str(), (char *)("Debug packets mode disabled"));
    }
  }
  else if (strcmp(topic, ha_debug_logs_set_topic.c_str()) == 0)
  { // if the incoming message is on the heatpump_debug_set_topic topic...
    if (strcmp(message, "on") == 0)
    {
      _debugModeLogs = true;
      mqtt_client.publish(ha_debug_logs_topic.c_str(), (char *)("Debug logs mode enabled"));
    }
    else if (strcmp(message, "off") == 0)
    {
      _debugModeLogs = false;
      mqtt_client.publish(ha_debug_logs_topic.c_str(), (char *)("Debug logs mode disabled"));
    }
  }
  else if (strcmp(topic, ha_custom_packet.c_str()) == 0)
  { // send custom packet for advance user
    String custom = message;

    // copy custom packet to char array
    char buffer[(custom.length() + 1)]; // +1 for the NULL at the end
    custom.toCharArray(buffer, (custom.length() + 1));

    byte bytes[20]; // max custom packet bytes is 20
    int byteCount = 0;
    char *nextByte;

    // loop over the byte string, breaking it up by spaces (or at the end of the line - \n)
    nextByte = strtok(buffer, " ");
    while (nextByte != NULL && byteCount < 20)
    {
      bytes[byteCount] = strtol(nextByte, NULL, 16); // convert from hex string
      nextByte = strtok(NULL, "   ");
      byteCount++;
    }

    // dump the packet so we can see what it is. handy because you can run the code without connecting the ESP to the heatpump, and test sending custom packets
    hpPacketDebug(bytes, byteCount, "customPacket");

    hp.sendCustomPacket(bytes, byteCount);
  }
  else
  {
    mqtt_client.publish(ha_debug_logs_topic.c_str(), strcat((char *)"heatpump: wrong mqtt topic: ", topic));
  }
}

void haConfig()
{

  // send HA config packet
  // setup HA payload device
  const size_t capacity = JSON_ARRAY_SIZE(5) + 2 * JSON_ARRAY_SIZE(6) + JSON_ARRAY_SIZE(7) + JSON_OBJECT_SIZE(24) + 2048;
  DynamicJsonDocument haConfig(capacity);

  haConfig["name"] = nullptr;
  haConfig["unique_id"] = getId();

  JsonArray haConfigModes = haConfig.createNestedArray("modes");
  haConfigModes.add("heat_cool"); // native AUTO mode
  haConfigModes.add("cool");
  haConfigModes.add("dry");
  if (supportHeatMode)
  {
    haConfigModes.add("heat");
  }
  haConfigModes.add("fan_only"); // native FAN mode
  haConfigModes.add("off");

  haConfig["mode_cmd_t"] = ha_mode_set_topic;
  haConfig["mode_stat_t"] = ha_state_topic;
  haConfig["mode_stat_tpl"] = F("{{ value_json.mode if (value_json is defined and value_json.mode is defined and value_json.mode|length) else 'off' }}"); // Set default value for fix "Could not parse data for HA"
  haConfig["temp_cmd_t"] = ha_temp_set_topic;
  haConfig["temp_stat_t"] = ha_state_topic;
  haConfig["avty_t"] = ha_availability_topic;          // MQTT last will (status) messages topic
  haConfig["pl_not_avail"] = mqtt_payload_unavailable; // MQTT offline message payload
  haConfig["pl_avail"] = mqtt_payload_available;       // MQTT online message payload
  // Set default value for fix "Could not parse data for HA"
  String temp_stat_tpl_str = F("{% if (value_json is defined and value_json.temperature is defined) %}{% if (value_json.temperature|int >= ");
  temp_stat_tpl_str += (String)convertCelsiusToLocalUnit(min_temp, useFahrenheit) + " and value_json.temperature|int <= ";
  temp_stat_tpl_str += (String)convertCelsiusToLocalUnit(max_temp, useFahrenheit) + ") %}{{ value_json.temperature }}";
  temp_stat_tpl_str += "{% elif (value_json.temperature|int < " + (String)convertCelsiusToLocalUnit(min_temp, useFahrenheit) + ") %}" + (String)convertCelsiusToLocalUnit(min_temp, useFahrenheit) + "{% elif (value_json.temperature|int > " + (String)convertCelsiusToLocalUnit(max_temp, useFahrenheit) + ") %}" + (String)convertCelsiusToLocalUnit(max_temp, useFahrenheit) + "{% endif %}{% else %}" + (String)convertCelsiusToLocalUnit(22, useFahrenheit) + "{% endif %}";
  haConfig["temp_stat_tpl"] = temp_stat_tpl_str;
  haConfig["curr_temp_t"] = ha_state_topic;
  String curr_temp_tpl_str = F("{{ value_json.roomTemperature if (value_json is defined and value_json.roomTemperature is defined and value_json.roomTemperature|int > ");
  curr_temp_tpl_str += (String)convertCelsiusToLocalUnit(1, useFahrenheit) + ") }}"; // Set default value for fix "Could not parse data for HA"
  haConfig["curr_temp_tpl"] = curr_temp_tpl_str;
  haConfig["min_temp"] = convertCelsiusToLocalUnit(min_temp, useFahrenheit);
  haConfig["max_temp"] = convertCelsiusToLocalUnit(max_temp, useFahrenheit);
  haConfig["temp_step"] = temp_step;
  haConfig["temperature_unit"] = useFahrenheit ? "F" : "C";

  JsonArray haConfigFan_modes = haConfig.createNestedArray("fan_modes");
  haConfigFan_modes.add("auto");
  haConfigFan_modes.add("diffuse");
  haConfigFan_modes.add("low");
  haConfigFan_modes.add("middle");
  haConfigFan_modes.add("medium");
  haConfigFan_modes.add("high");

  haConfig["fan_mode_cmd_t"] = ha_fan_set_topic;
  haConfig["fan_mode_stat_t"] = ha_state_topic;
  haConfig["fan_mode_stat_tpl"] = F("{{ value_json.fan if (value_json is defined and value_json.fan is defined and value_json.fan|length) else 'AUTO' }}"); // Set default value for fix "Could not parse data for HA"

  JsonArray haConfigSwing_modes = haConfig.createNestedArray("swing_modes");
  haConfigSwing_modes.add("AUTO");
  haConfigSwing_modes.add("1");
  haConfigSwing_modes.add("2");
  haConfigSwing_modes.add("3");
  haConfigSwing_modes.add("4");
  haConfigSwing_modes.add("5");
  haConfigSwing_modes.add("SWING");

  haConfig["swing_mode_cmd_t"] = ha_vane_set_topic;
  haConfig["swing_mode_stat_t"] = ha_state_topic;
  haConfig["swing_mode_stat_tpl"] = F("{{ value_json.vane if (value_json is defined and value_json.vane is defined and value_json.vane|length) else 'AUTO' }}"); // Set default value for fix "Could not parse data for HA"
  haConfig["action_topic"] = ha_state_topic;
  haConfig["action_template"] = F("{{ value_json.action if (value_json is defined and value_json.action is defined and value_json.action|length) else 'idle' }}"); // Set default value for fix "Could not parse data for HA"

  JsonObject haConfigDevice = haConfig.createNestedObject("device");

  haConfigDevice["ids"] = mqtt_fn;
  haConfigDevice["name"] = mqtt_fn;
  haConfigDevice["sw"] = "Mitsubishi2MQTT " + String(m2mqtt_version);
  haConfigDevice["mdl"] = "HVAC MITSUBISHI";
  haConfigDevice["mf"] = "MITSUBISHI ELECTRIC";
  haConfigDevice["configuration_url"] = "http://" + WiFi.localIP().toString();

  // Additional attributes are in the state
  // For now, only compressorFrequency
  haConfig["json_attr_t"] = ha_state_topic;
  haConfig["json_attr_tpl"] = F("{{ {'compressorFrequency': value_json.compressorFrequency if (value_json is defined and value_json.compressorFrequency is defined) else '-1' } | tojson }}");

  String mqttOutput;
  serializeJson(haConfig, mqttOutput);
  mqtt_client.beginPublish(ha_config_topic.c_str(), mqttOutput.length(), true);
  mqtt_client.print(mqttOutput);
  mqtt_client.endPublish();
}

void mqttConnect()
{
  // Loop until we're reconnected
  int attempts = 0;
  while (!mqtt_client.connected())
  {
    // Attempt to connect
    mqtt_client.connect(mqtt_client_id.c_str(), mqtt_username.c_str(), mqtt_password.c_str(), ha_availability_topic.c_str(), 1, true, mqtt_payload_unavailable);
    // If state < 0 (MQTT_CONNECTED) => network problem we retry 5 times and then waiting for MQTT_RETRY_INTERVAL_MS and retry reapeatly
    if (mqtt_client.state() < MQTT_CONNECTED)
    {
      if (attempts == 5)
      {
        lastMqttRetry = millis();
        return;
      }
      else
      {
        delay(10);
        attempts++;
      }
    }
    // If state > 0 (MQTT_CONNECTED) => config or server problem we stop retry
    else if (mqtt_client.state() > MQTT_CONNECTED)
    {
      return;
    }
    // We are connected
    else
    {
      mqtt_client.subscribe(ha_system_set_topic.c_str());
      mqtt_client.subscribe(ha_debug_pckts_set_topic.c_str());
      mqtt_client.subscribe(ha_debug_logs_set_topic.c_str());
      mqtt_client.subscribe(ha_mode_set_topic.c_str());
      mqtt_client.subscribe(ha_fan_set_topic.c_str());
      mqtt_client.subscribe(ha_temp_set_topic.c_str());
      mqtt_client.subscribe(ha_vane_set_topic.c_str());
      mqtt_client.subscribe(ha_wideVane_set_topic.c_str());
      mqtt_client.subscribe(ha_remote_temp_set_topic.c_str());
      mqtt_client.subscribe(ha_custom_packet.c_str());
      mqtt_client.publish(ha_availability_topic.c_str(), mqtt_payload_available, true); // publish status as available
      if (others_haa)
      {
        haConfig();
      }
    }
  }
}

// temperature helper these are direct mappings based on the remote
float toFahrenheit(float fromCelsius)
{
  // Lookup table for specific mappings
  const std::map<float, int> lookupTable = {
      {16.0, 61}, {16.5, 62}, {17.0, 63}, {17.5, 64}, {18.0, 65}, {18.5, 66}, {19.0, 67}, {20.0, 68}, {21.0, 69}, {21.5, 70}, {22.0, 71}, {22.5, 72}, {23.0, 73}, {23.5, 74}, {24.0, 75}, {24.5, 76}, {25.0, 77}, {25.5, 78}, {26.0, 79}, {26.5, 80}, {27.0, 81}, {27.5, 82}, {28.0, 83}, {28.5, 84}, {29.0, 85}, {29.5, 86}, {30.0, 87}, {30.5, 88}};

  // Check if the input is in the lookup table
  auto it = lookupTable.find(fromCelsius);
  if (it != lookupTable.end())
  {
    return it->second;
  }

  // Default conversion and rounding to nearest integer
  return roundf(fromCelsius * 1.8 + 32.0);
}

// temperature helper these are direct mappings based on the remote
float toCelsius(float fromFahrenheit)
{
  // Lookup table for specific mappings
  const std::map<int, float> lookupTable = {
      {61, 16.0}, {62, 16.5}, {63, 17.0}, {64, 17.5}, {65, 18.0}, {66, 18.5}, {67, 19.0}, {68, 20.0}, {69, 21.0}, {70, 21.5}, {71, 22.0}, {72, 22.5}, {73, 23.0}, {74, 23.5}, {75, 24.0}, {76, 24.5}, {77, 25.0}, {78, 25.5}, {79, 26.0}, {80, 26.5}, {81, 27.0}, {82, 27.5}, {83, 28.0}, {84, 28.5}, {85, 29.0}, {86, 29.5}, {87, 30.0}, {88, 30.5}};

  // Check if the input is in the lookup table
  auto it = lookupTable.find(static_cast<int>(fromFahrenheit));
  if (it != lookupTable.end())
  {
    return it->second;
  }

  // Default conversion and rounding to nearest 0.5
  return roundf((fromFahrenheit - 32.0) / 1.8 * 2) / 2.0;
}

float convertCelsiusToLocalUnit(float temperature, bool isFahrenheit)
{
  if (isFahrenheit)
  {
    return toFahrenheit(temperature);
  }
  else
  {
    return temperature;
  }
}

float convertLocalUnitToCelsius(float temperature, bool isFahrenheit)
{
  if (isFahrenheit)
  {
    return toCelsius(temperature);
  }
  else
  {
    return temperature;
  }
}

String getTemperatureScale()
{
  if (useFahrenheit)
  {
    return "F";
  }
  else
  {
    return "C";
  }
}

String getId()
{
  uint64_t macAddress = ESP.getEfuseMac();
  uint32_t chipID = macAddress >> 24;
  return String(chipID, HEX);
}

// Check if header is present and correct
bool is_authenticated(AsyncWebServerRequest *request)
{
  if (request->hasHeader("Cookie"))
  {
    // Found cookie;
    String cookie = request->header("Cookie");
    if (cookie.indexOf("M2MSESSIONID=1") != -1)
    {
      // Authentication Successful
      return true;
    }
  }
  // Authentication Failed
  return false;
}

void onWebSocketEvent(uint8_t client_num, WStype_t type, uint8_t *payload, size_t length)
{
  bool clientConnected;
  switch (type)
  {
  case WStype_DISCONNECTED:
    Serial.printf("[%u] Déconnecté!\n", client_num);
    clientConnected = false;
    break;
  case WStype_CONNECTED:
    Serial.printf("[%u] Connecté!\n", client_num);
    clientConnected = true;
    break;
  }
}

void loop()
{
  server.begin();
  ArduinoOTA.handle();

  // Sync HVAC UNIT
  if (!hp.isConnected())
  {
    // Use exponential backoff for retries, where each retry is double the length of the previous one.
    unsigned long durationNextSync = (1 << hpConnectionRetries) * HP_RETRY_INTERVAL_MS;
    if (((millis() - lastHpSync > durationNextSync) or lastHpSync == 0))
    {
      lastHpSync = millis();
      // If we've retried more than the max number of tries, keep retrying at that fixed interval, which is several minutes.
      hpConnectionRetries = min(hpConnectionRetries + 1u, HP_MAX_RETRIES);
      hpConnectionTotalRetries++;
      hp.sync();
    }
  }
  else
  {
    hpConnectionRetries = 0;
    hp.sync();
  }

    // MQTT failed retry to connect
    if (mqtt_client.state() < MQTT_CONNECTED)
    {
      if ((millis() - lastMqttRetry > MQTT_RETRY_INTERVAL_MS) or lastMqttRetry == 0)
      {
        mqttConnect();
      }
    }
    // MQTT config problem on MQTT do nothing
    else if (mqtt_client.state() > MQTT_CONNECTED)
      return;
    // MQTT connected send status
    else
    {
      hpStatusChanged(hp.getStatus());
      mqtt_client.loop();
    }
}
