#include <BluetoothSerial.h>
#include <OneButton.h>
#include <TinyGPS++.h>
#include <Arduino.h>
#include <logger.h>
#include <WiFi.h>
#include <LoRa.h>
#include <vector>
#include "notification_utils.h"
#include "bluetooth_utils.h"
#include "keyboard_utils.h"
#include "configuration.h"
#include "station_utils.h"
#include "button_utils.h"
#include "pins_config.h"
#include "power_utils.h"
#include "menu_utils.h"
#include "lora_utils.h"
#include "msg_utils.h"
#include "gps_utils.h"
#include "bme_utils.h"
#include "display.h"
#include "utils.h"

#include "APRSPacketLib.h"


Configuration                 Config;
PowerManagement               powerManagement;
HardwareSerial                neo6m_gps(1);
TinyGPSPlus                   gps;
BluetoothSerial               SerialBT;
OneButton userButton          = OneButton(BUTTON_PIN, true, true);

String    versionDate         = "2023.11.11";

int       myBeaconsIndex      = 0;
int       myBeaconsSize       = Config.beacons.size();
Beacon    *currentBeacon      = &Config.beacons[myBeaconsIndex];

int       menuDisplay         = 100;

int       messagesIterator    = 0;
std::vector<String>           loadedAPRSMessages;

bool      displayEcoMode      = Config.displayEcoMode;
bool      displayState        = true;
uint32_t  displayTime         = millis();
uint32_t  refreshDisplayTime  = millis();

bool      sendUpdate          = true;
int       updateCounter       = Config.sendCommentAfterXBeacons;
bool	    sendStandingUpdate  = false;
bool      statusState         = true;
uint32_t  statusTime          = millis();
bool      bluetoothConnected  = false;
bool      bluetoothActive     = Config.bluetooth;

bool      messageLed          = false;
uint32_t  messageLedTime      = millis();
int       lowBatteryPercent   = 21;

uint32_t  lastTelemetryTx     = millis();
uint32_t  telemetryTx         = millis();

uint32_t  lastTx              = 0.0;
uint32_t  txInterval          = 60000L;
uint32_t  lastTxTime          = millis();
double    lastTxLat           = 0.0;
double    lastTxLng           = 0.0;
double    lastTxDistance      = 0.0;
double    currentHeading      = 0;
double    previousHeading     = 0;

uint32_t  menuTime            = millis();
bool      symbolAvailable     = true;

int       screenBrightness    = 1;
bool      keyboardConnected   = false;
bool      keyDetected         = false;
uint32_t  keyboardTime        = millis();
String    messageCallsign     = "";
String    messageText         = "";

bool      digirepeaterActive  = false;
bool      sosActive           = false;

logging::Logger               logger;

void setup() {
  Serial.begin(115200);

  #ifndef DEBUG
  logger.setDebugLevel(logging::LoggerLevel::LOGGER_LEVEL_INFO);
  #endif

  powerManagement.setup();

  setup_display();
  if (Config.notification.buzzerActive) {
    pinMode(Config.notification.buzzerPinTone, OUTPUT);
    pinMode(Config.notification.buzzerPinVcc, OUTPUT);
    NOTIFICATION_Utils::start();
  } 
  if (Config.notification.ledTx){
    pinMode(Config.notification.ledTxPin, OUTPUT);
  }
  if (Config.notification.ledMessage){
    pinMode(Config.notification.ledMessagePin, OUTPUT);
  }
  show_display(" LoRa APRS", "", "     Richonguzman", "     -- CD2RXU --", "", "      " + versionDate, 4000);
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "Main", "RichonGuzman (CD2RXU) --> LoRa APRS Tracker/Station");
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "Main", "Version: %s", versionDate.c_str());

  if (Config.ptt.active) {
    pinMode(Config.ptt.io_pin, OUTPUT);
    digitalWrite(Config.ptt.io_pin, Config.ptt.reverse ? HIGH : LOW);
  }

  MSG_Utils::loadNumMessages();
  GPS_Utils::setup();
  LoRa_Utils::setup();
  BME_Utils::setup();
  STATION_Utils::loadCallsignIndex();

  WiFi.mode(WIFI_OFF);
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "Main", "WiFi controller stopped");
  BLUETOOTH_Utils::setup();

  if (!Config.simplifiedTrackerMode) {
    userButton.attachClick(BUTTON_Utils::singlePress);
    userButton.attachLongPressStart(BUTTON_Utils::longPress);
    userButton.attachDoubleClick(BUTTON_Utils::doublePress);
    KEYBOARD_Utils::setup();
  }

  powerManagement.lowerCpuFrequency();
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "Main", "Smart Beacon is: %s", utils::getSmartBeaconState().c_str());
  logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "Main", "Setup Done!");
  menuDisplay = BUTTON_PIN == -1 ? 20 : 0;
}

void loop() {
  if (Serial.available() > 0) {
    int action = Serial.read();
    if (action == 's') { // set config
      String serialReceived = Serial.readString();
      Serial.println(Config.writeConfigFile(serialReceived) ? F("s1") : F("s0"));
    } else if (action == 'g') { // get config
      Serial.print('g');
      Serial.println(Config.readRawConfigFile());
    } else if (action == 't') { // tx lora
      LoRa_Utils::sendNewPacket(Serial.readString());
      Serial.println(F("t1"));
    }
  }

  currentBeacon = &Config.beacons[myBeaconsIndex];
  if (statusState) {
    Config.validateConfigFile(currentBeacon->callsign);
  }

  powerManagement.batteryManager();
  if (!Config.simplifiedTrackerMode) {
    userButton.tick();
  }
  utils::checkDisplayEcoMode();

  if (keyboardConnected) {
    KEYBOARD_Utils::read();
  }

  GPS_Utils::getData();
  bool gps_time_update = gps.time.isUpdated();
  bool gps_loc_update  = gps.location.isUpdated();
  GPS_Utils::setDateFromData();

  MSG_Utils::checkReceivedMessage(LoRa_Utils::receivePacket());
  MSG_Utils::ledNotification();
  STATION_Utils::checkListenedTrackersByTimeAndDelete();
  BLUETOOTH_Utils::sendToLoRa();

  int currentSpeed = (int) gps.speed.kmph();

  if (gps_loc_update) {
    utils::checkStatus();
    STATION_Utils::checkTelemetryTx();
  }
  lastTx = millis() - lastTxTime;
  if (!sendUpdate && gps_loc_update && currentBeacon->smartBeaconState) {
    GPS_Utils::calculateDistanceTraveled();
    if (!sendUpdate) {
      GPS_Utils::calculateHeadingDelta(currentSpeed);
    }
    STATION_Utils::checkStandingUpdateTime();
  }
  STATION_Utils::checkSmartBeaconState();
  if (sendUpdate && gps_loc_update) {
    STATION_Utils::sendBeacon("GPS");
  }
  if (gps_time_update) {
    STATION_Utils::checkSmartBeaconInterval(currentSpeed);
  }
  
  if (millis() - refreshDisplayTime >= 1000 || gps_time_update) {
    GPS_Utils::checkStartUpFrames();
    MENU_Utils::showOnScreen();
    refreshDisplayTime = millis();
  }
}