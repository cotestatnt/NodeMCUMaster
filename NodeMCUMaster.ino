#include <Ticker.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <FS.h>
#include <Hash.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFSEditor.h>
#include <SPI.h>
#include <RF24.h>
#include <RF24Network.h>
#include <NTPClient.h>       
#include <ArduinoJson.h>
#include <CBC.h>
#include <AES.h>
#include <SHA256.h>
#include <EEPROM.h>

#define MAX_NODES 7
#define TIMECHECK 30        // In seconds
const byte ALARM_ON = D0;   // Led D5
const byte HORN_ON = D1;    // Relay K2
const byte REED_IN = 10;    // Reed sensor input (Pull-UP)
const byte UsedPin[] = { REED_IN, HORN_ON, ALARM_ON };

#define TINY_GSM_MODEM_A6
#include <SoftwareSerial.h>
SoftwareSerial SerialAT(D3, D2, false, 1024); // RX, TX

#include <TinyGsmClient.h>
TinyGsm modem(SerialAT);

//Some useful bit manipulation macros
#define BIT_MASK(bit)             (1 << (bit))
#define SET_BIT(value,bit)        ((value) |= BIT_MASK(bit))
#define CLEAR_BIT(value,bit)      ((value) &= ~BIT_MASK(bit))
#define TEST_BIT(value,bit)       (((value) & BIT_MASK(bit)) ? 1 : 0)

const char* ssid = "***********";
const char* password = "************";
const char* hostName = "esp-async";
String adminPswd = "admin";
String adminPIN = "12345";
String phoneNumber = "0123456789";

enum payloadPointer { _NodeH = 0, _NodeL = 1, _Enabled = 2, _MsgType = 3, _Battery = 4, _DeviceNumber = 6};
enum messageType {  SET_DISABLE = 100,  SET_ENABLE = 110,  SEND_ALIVE = 120,  SEND_ALARM = 199};
enum States { DISABLE = 0, ENABLE = 10, RUN = 11, PAUSE = 20, TIME = 30, ALARM = 99 } currentState;

// Webservices
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncEventSource events("/events");

boolean syncEventTriggered = false;   // True if a time even has been triggered
NTPSyncEvent_t ntpEvent;              // Last triggered event

// Instantiate a AES block ciphering 
#define BLOCK_SIZE 16
CBC<AES128> myChiper;
byte encryptKey[BLOCK_SIZE] = {0x5C, 0x39, 0x38, 0x7B, 0x36, 0x60, 0x68, 0x23, 0x34, 0x34, 0x76, 0x5C, 0x34, 0x22, 0x62, 0x55};
byte iv[BLOCK_SIZE] = {0x5C, 0x39, 0x38, 0x7B, 0x36, 0x60, 0x68, 0x23, 0x34, 0x34, 0x76, 0x5C, 0x34, 0x22, 0x62, 0x55};
byte cipherText[2*BLOCK_SIZE];
byte payload[2*BLOCK_SIZE];

// Instance of the radio driver
RF24 radio(D4, D8);				        // nRF24L01(+) radio attached
RF24Network network(radio);       // Network uses that radio
const uint16_t this_node = 00;    // Address of our node in Octal format

struct Node { uint32_t timestamp; uint16_t address; uint16_t deviceId; uint16_t battery; uint8_t state; uint8_t zone; };
Node nodes[MAX_NODES]; // Master always 0 + n nodes

// Global variables
uint16_t NewGPIO, OldGPIO = 0;  
String logMessage, phoneNumber1, phoneNumber2, pushDevId = "";
uint8_t nodeNumber, oldState, pauseSeconds, hornSeconds = 0;
uint32_t startZ1, stopZ1, startZ2, stopZ2, actualTime, secondsOfday = 0;
uint32_t hornTime, pauseTime = 0;
bool Push = false;
bool firstTest = true;

Ticker clientUpdater, SMSchecker, GPIOupdater;

// ***************************************************************************************************** //
// *****************************************    SETUP   ************************************************ //
// ***************************************************************************************************** //
void setup() {    
  static WiFiEventHandler gotIpEventHandler, disconnectedEventHandler;
  EEPROM.begin(128);
  
  // Set encryption key and generate a random initialization vector and set it
  myChiper.setKey(encryptKey, BLOCK_SIZE); 
  for (int i = 0 ; i < BLOCK_SIZE ; i++ ) 
    iv[i]= random(0xFF);
  myChiper.setIV(iv, BLOCK_SIZE);
  
  pinMode(ALARM_ON, OUTPUT);
  pinMode(HORN_ON, OUTPUT);
  digitalWrite(HORN_ON, HIGH);
  digitalWrite(ALARM_ON, LOW);
  pinMode(REED_IN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(REED_IN), handleInterrupt, RISING);

  // Start Serial for debug
  Serial.begin(115200);  
  Serial.println("Booting...");
  Serial.println();    

  // Set GSM module baud rate  
  SerialAT.begin(115200);
  SerialAT.print("AT\r");

  // Start SPIFFS filesystem
  SPIFFS.begin();

  NTP.onSyncEvent([](NTPSyncEvent_t ntpEvent) {         
    switch (ntpEvent) {
    case NTP_EVENT_INIT:    
      Serial.print("Init time sync with NTP server.");
      break;
    case NTP_EVENT_STOP:
      break;
    case NTP_EVENT_NO_RESPONSE:
      Serial.printf("NTP server not reachable.\n");
      break;
    case NTP_EVENT_SYNCHRONIZED:            
      if (timeStatus() == timeSet) {                    
        time_t tm = NTP.getLastSync();
        Serial.printf("\nSync time from NTP server: %s\n", NTP.getTimeDate(tm));                                
        // First sync done, change polling time (seconds)
        NTP.setPollingInterval(600);    
        delay(1000);
        char buf[32];        
        snprintf(buf, sizeof(buf), "AT+CCLK=\"%02d/%02d/%02d, %02d:%02d:%02d+1\"\r", 
                year(tm)%100, month(tm), day(tm), hour(tm), minute(tm), second(tm));
        Serial.print("Setting time of A6 module...");   
        delay(50);
        SerialAT.print(buf);
        delay(50);        
      }
      break;
    }   
  });
 
  gotIpEventHandler = WiFi.onStationModeGotIP(onSTAGotIP);
  disconnectedEventHandler = WiFi.onStationModeDisconnected(onSTADisconnected);
  
  // Load configuration and start wifi
  loadWifiConf();
  
  // Configure and start Webserver
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  events.onConnect([](AsyncEventSourceClient *client) {
    client->send("Hello Client!", NULL, millis(), 1000);
  });
  server.addHandler(&events);
  server.addHandler(new SPIFFSEditor("admin", adminPswd.c_str()));
  
  // serve webserver from folder www (useful also to avoid serving config.json file)
  server.serveStatic("/", SPIFFS, "/www/").setDefaultFile("index.htm");           
  server.serveStatic("/auth/", SPIFFS, "/www/auth/")
		    .setDefaultFile("settings.htm")
		    .setAuthentication("admin", adminPswd.c_str()); 
  server.onNotFound([](AsyncWebServerRequest *request) { request->send(404); });
  // Start TCP (HTTP) server
  server.begin();
  Serial.println("\nWebserver started");  
  
  // Add service to MDNS-SD
  MDNS.begin("smartalarm");
  MDNS.addService("http", "tcp", 80);
  
  // Init nRF24 Network
  SPI.begin();
  radio.begin();
  network.begin(/*channel*/ 90, /*node address*/ this_node);
  
  // Set the last state before restart 
  currentState =  static_cast<States>(EEPROM.read(0));

  // Start tickers (seconds, callback function)
  clientUpdater.attach(1, updateNodes);
  SMSchecker.attach(5, checkSMS);
  GPIOupdater.attach(0.5, updateGPIO);
}


// ***************************************************************************************************** //
// *****************************************    LOOP   ************************************************* //
// ***************************************************************************************************** //    
void loop() { 
  //  if(currentState != oldState){  Serial.print(" ."); Serial.print(currentState);  oldState = currentState;  delay(500); }
  
  if(Push){
    sendPushNotification();    
    Serial.print("Call assigned number: ");
    Serial.println(modem.callNumber(phoneNumber1));
    sendPushNotification();
    delay(5000);
    Serial.print("Hang up:");
    Serial.println(modem.callHangup());
    Push = false;
  }
  
  while (SerialAT.available() > 0)
    Serial.write(SerialAT.read());  
  while (Serial.available() > 0) 
    SerialAT.write(Serial.read());
      
  // Check if nRF24 data message is present
  getRadioData();  

  switch (currentState) {  
  case DISABLE:
    digitalWrite(HORN_ON, HIGH);
    digitalWrite(ALARM_ON, HIGH);
    // Set all node to known state
    for (byte i = 0; i < 7; i++)
      nodes[i].timestamp = actualTime;    
    break;

  // Run alarm system
  case ENABLE:
    // Update timestamp before re-enable the system
    digitalWrite(HORN_ON, HIGH);
    digitalWrite(ALARM_ON, HIGH);   
    currentState = RUN;
    break;

  // Wait for node alive messages and check nodes
  case RUN:  
    getRadioData();  
    if (secondsOfday == stopZ1) {
      digitalWrite(HORN_ON, HIGH);
      digitalWrite(ALARM_ON, HIGH);
      currentState = TIME;
      break;
    }    
    break;
 

  // Only to store the actual status and use it after if restart
  case TIME:        
    if (secondsOfday == startZ1) {
      Serial.println("System ENABLED");
      currentState = ENABLE;
      logMessage = "\nTime: " + String(secondsOfday) + " seconds\nStart: " + String(startZ1) + " seconds\nStop: " + String(stopZ1) + " seconds\n";
      Serial.println(logMessage);
    }
    break;

  // wait a defined time and after re-enable the system
  case PAUSE:
    if (millis() - pauseTime > pauseSeconds * 1000) {
      pauseTime = millis();
      currentState = ENABLE;
    }
    break;

  // Ops, we have an alarm: send message to clients and turn on the horn (after 15seconds) 
  // At this point, status will be changed only after authorized user action
  case ALARM:
    digitalWrite(ALARM_ON, LOW);
    // Activate the horn after user defined time elapsed
    if (millis() - hornTime > 15000) {
      hornTime = millis();
      digitalWrite(HORN_ON, LOW);
      Serial.println("\nAcustic segnalator ON");
    }
    break;
  }
 
}


void updateGPIO(){
  // Read status of all used pins and store in NewGPIO var  
  for (byte i = 0; i < 16; i++)
    for (byte j = 0; j < sizeof(UsedPin); j++)
      if((UsedPin[j] == i)&&(digitalRead(i) == HIGH))
          SET_BIT(NewGPIO, i);

  // Check if some of pins has changed and sent message to clients
  if (NewGPIO != OldGPIO) {
    delay(50);
    OldGPIO = NewGPIO;
    char str[2];
    sprintf(str, "%d", currentState);
    sendDataWs((char *)"status", str);
  }
  
}

void updateNodes() {    
  uint8_t nodesOK = 0;
  // Update connected clients 
  if (timeStatus() == timeSet) 
    actualTime = now() + UTC0100;         
  else
    actualTime++;
  uint8_t h = (actualTime - 2208988800UL + UTC0100 % 86400L) / 3600;
  uint8_t m = (actualTime - 2208988800UL + UTC0100 % 3600) / 60;
  uint8_t s = (actualTime - 2208988800UL + UTC0100 % 60);    
  secondsOfday = h + m + s;  
  char str[2];
  sprintf(str, "%d", currentState);
  sendDataWs((char *)"status", str);    
  if(currentState != DISABLE){               
    for (byte i = 0; i < 7; i++) {
      uint32_t elapsedtime = actualTime - nodes[i].timestamp;         
      if (elapsedtime <= TIMECHECK) 
        nodesOK++;
      else {
        // if sensor is disabled dont't set alarm
        if (nodes[i].state != 0) {
          logMessage = "\nSensor " + String(nodes[i].address) + " not respond since " + String(elapsedtime) + " seconds";
          Serial.println(logMessage);                
          delay(1000);
          hornTime = millis();
          currentState = ALARM;
        }     
      }
    }
    
    // Something wrong -> set Alarm state
    if ((nodesOK < nodeNumber)&!(firstTest)) {    
      digitalWrite(ALARM_ON, LOW);
      logMessage = "\nALARM! Sensors active " + String(nodesOK) + "/" + String(nodeNumber);
      Serial.println(logMessage);
      delay(200);
      hornTime = millis();
      currentState = ALARM;
    }  
  }
  
}

// ***************************************************************************************************** //
// *****************************************    RF24    ************************************************ //
// ***************************************************************************************************** //

// Wait for a message addressed to Master from one of nodes
void getRadioData(void) {  
  // Check the nRF24 network regularly
  network.update();
  while (network.available()) {      
    RF24NetworkHeader header;
    network.read(header, &cipherText, 2*BLOCK_SIZE);
    for(byte i = 0; i <BLOCK_SIZE; i++)
      iv[i] = cipherText[i+BLOCK_SIZE];    
    //*************** DECRYPT **********************//    
    myChiper.setIV(iv, BLOCK_SIZE);
    myChiper.decrypt(payload, cipherText,  BLOCK_SIZE);    
    //*************** DECRYPT **********************//

    Serial.print("\nNode ");
    Serial.print(header.from_node);   
    Serial.print(F(":"));    
    printHex(payload, 10);   
    
    if (payload[_MsgType] == SEND_ALIVE) {          
      uint16_t fromNode = (payload[_NodeH] << 8) | payload[_NodeL];
      // Something wrong -> abort operations
      if(fromNode != header.from_node){
        // Set all node to known state
        for (byte i = 0; i < 7; i++) 
          nodes[i].timestamp = actualTime;              
        break;
      }
      uint16_t deviceId = (payload[_DeviceNumber] << 8) | payload[_DeviceNumber];
      uint16_t battery = (payload[_Battery] << 8) | payload[_Battery];
      nodes[fromNode].address = fromNode;
      nodes[fromNode].state = payload[_Enabled];
      nodes[fromNode].battery = battery;
      nodes[fromNode].deviceId = deviceId;          
      nodes[fromNode].timestamp = actualTime;   
      firstTest = false; 
    }    
    
    if (payload[_MsgType] == SEND_ALARM){
      currentState = ALARM;
    }

  }
  
}

// Send message to one of nodes
bool sendRadioData(uint16_t toNode) {  
  bool TxOK = false;
  Serial.print(F("\nMaster: "));
  printHex(payload, BLOCK_SIZE);
    
  //****************** ENCRYPT CBC AES 128  **********************//
  myChiper.encrypt(cipherText, payload, BLOCK_SIZE);
  //********************* END ENCRYPT **************************//
  
  radio.stopListening();
  radio.flush_tx();
  delayMicroseconds(250);
  RF24NetworkHeader header(toNode);
  if (network.write(header, &cipherText, sizeof(cipherText)))
    TxOK = true;
  radio.startListening();   
  return TxOK;
}

// ***************************************************************************************************** //
// ***************************************    WebSocket    ********************************************* //
// ***************************************************************************************************** //
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type){
    case WS_EVT_CONNECT:
         Serial.printf("\nClient %u connected. Send ping", client->id());
         client->printf("{\"ts\":%lu,\"cmd\":\"status\",\"msg\":\"Client id %u connected\"}", actualTime, client->id());
         client->ping();
         break;
    case WS_EVT_DISCONNECT:
          Serial.printf("\nClient %u disconnected", client->id());
          break;
    case WS_EVT_ERROR:
          Serial.printf("\nClient error(%u): %s", client->id(), (char*)data);
          break;
    case WS_EVT_PONG:
          Serial.printf("\nClient %u pong", client->id());
          client->printf("{\"ts\":%lu,\"cmd\":\"jsVars\",\"timePause\":\"%u\",\"timeHorn\":\"%u\"}", actualTime, pauseSeconds, 15);
          break;
    case WS_EVT_DATA:
          AwsFrameInfo *info = (AwsFrameInfo *)arg;
          String msg = "";
          if (info->final && info->index == 0 && info->len == len) {
            Serial.printf("\nClient %u message:", client->id());
            for (size_t i = 0; i < info->len; i++) 
              msg += (char)data[i];     
            Serial.println(msg);           
          }
          // A message from browser, take action according to our purpose
          processWsMsg(msg);
          break;  
  } // end of switch
}

// Encodes JSON Object and Sends it to All WebSocket Clients
void sendDataWs(char *msgType, char *msg) {
  DynamicJsonBuffer jsonBuffer;
  JsonObject &root = jsonBuffer.createObject();   
  root["ts"] = actualTime;
  root["cmd"] = msgType;
  root["msg"] = msg;
  root["gpio"] = NewGPIO;
  JsonArray &nodeid = root.createNestedArray("nodeid");
  JsonArray &state = root.createNestedArray("state");
  JsonArray &battery = root.createNestedArray("battery");  
  for (byte i = 0; i < MAX_NODES; i++) {    
    nodeid.add(nodes[i].address);
    state.add(nodes[i].state);
    battery.add(nodes[i].battery);    
  }
  size_t len = root.measureLength();
  AsyncWebSocketMessageBuffer *buffer =  ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  if (buffer) {
    root.printTo((char *)buffer->get(), len + 1);
    ws.textAll(buffer);
  }
}

// Web Browser sends some commands, check which command is given
void processWsMsg(String msg) {
  // We got a JSON object from browser, parse it
  DynamicJsonBuffer jsonBuffer;
  JsonObject &root = jsonBuffer.parseObject(msg);
  if (!root.success()) {
    Serial.println("Error parsing JSON message");
    return;
  }

  const char *command = root["command"];
  // Activate ore deactivete an output pin
  if (strcmp(command, "setOutput") == 0) {
    String pinName = root["pinName"];
    int pin = root["pin"];
    int state = root["state"];
    digitalWrite(pin, state);
    Serial.printf("\nSet pin %s %u", pinName.c_str(), state);
    Push = true;
  }
  // Check if the PIN provided is correct
  else if (strcmp(command, "checkThisHash") == 0) {
    String testHash = root["testHash"];
    if (hashThis(adminPIN, testHash)) {
      sendDataWs((char *)"checkThisHash", (char *)"true");
      // PIN is correct -> disable acustic segnalation
      digitalWrite(HORN_ON, HIGH);
      Serial.println(F("Hash result: OK"));
    } else {
      sendDataWs((char *)"checkThisHash", (char *)"false");
      Serial.println(F("Hash result: not OK"));
    }

  }
  // Change system status with user actions
  else if (strcmp(command, "pause") == 0) {
    pauseTime = millis();
    currentState = PAUSE;
  } 
  else if (strcmp(command, "timer") == 0)
    currentState = TIME;
  else if (strcmp(command, "alarmOn") == 0)
    currentState = ENABLE;
  else if (strcmp(command, "alarmOff") == 0)
    currentState = DISABLE;

  // Enable/disable single sensor
  else if (strcmp(command, "sensorToggle") == 0) {
    uint16_t _node = root["sensor"];
    Serial.print("Sensor n° ");
    Serial.print(_node);
    Serial.print(" is");
    Serial.print(nodes[_node].state ? F(" ENABLED\n") : F(" DISABLED\n"));
    bool enabled = nodes[_node].state;
    nodes[_node].state = !enabled;
  }

  // Manage admin and configuration data
  else if (strcmp(command, "saveconfig") == 0) {
    Serial.println(F("Saving config.json..."));
    File configFile = SPIFFS.open("/config.json", "w+");
    if (!configFile)
      Serial.println(F("\nFailed to open for write config.json."));
    configFile.print(msg);
    configFile.close();
    Serial.println("Restarting in 3 seconds...");
    delay(3000);
    ESP.restart();
  }
  
  // Provide config.json to admin settings page
  else if (strcmp(command, "getconf") == 0) {
    File configFile = SPIFFS.open("/config.json", "r");
    if (configFile) {
      size_t len = configFile.size();
      AsyncWebSocketMessageBuffer *buffer =  ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
      if (buffer) {
        configFile.readBytes((char *)buffer->get(), len + 1);
        ws.textAll(buffer);
      }
      configFile.close();
    }
  }

  // Save new status in case of reboot
  Serial.print(F("\nAlarm system: "));
  Serial.println(currentState);
  EEPROM.write(0, currentState);
  EEPROM.commit();
}


// ***************************************************************************************************** //
//************************************  OTHER FUNCTIONS  *********************************************** //
// ***************************************************************************************************** //

// Check if provided hash is correct (used for PIN)
bool hashThis( String data, String testHash) {
  #define HASH_SIZE 32
  #define HASH_BLOCK_SIZE 64
  SHA256 sha256;

  byte hashBuf[HASH_BLOCK_SIZE];
  char myHash[HASH_BLOCK_SIZE];
  
  Serial.println("Test HASH");  
  sha256.reset();
  sha256.update(data.c_str(), data.length());
  sha256.finalize(hashBuf, sizeof(hashBuf));
  
  for (int i = 0; i < HASH_SIZE; i++)
    sprintf(myHash + 2 * i, "%02X", hashBuf[i]);
  Serial.println(String(myHash));
  if (String(myHash) == testHash)
    return true;
  else
    return false;
}

// Interrupt service routine for REED_IN input
void handleInterrupt() {
  if (currentState != DISABLE) {
    Serial.println("Interrupt");
    currentState = ALARM;  
    hornTime = millis() + 15000;
  }
}

// Helper routine to dump a byte array as hex values to Serial.
void printHex(byte *buffer, byte bufferSize) {
  for (byte i = 0; i < bufferSize; i++) {
    Serial.print(buffer[i] < 0x10 ? " 0" : " ");
    Serial.print(buffer[i], HEX);
  }
}

// Helper routine to dump a byte array as dec values to Serial.
void printDec(byte *buffer, byte bufferSize) {
  for (byte i = 0; i < bufferSize; i++) {
    Serial.print(buffer[i] < 0x10 ? " 0" : " ");
    Serial.print(buffer[i], DEC);
  }
}


// ***************************************************************************************************** //
//************************************  WIFI CONNECTION  *********************************************** //
// ***************************************************************************************************** //

// Event Handler when an IP address has been assigned
void onSTAGotIP(WiFiEventStationModeGotIP event) {
  Serial.printf("IP address: %s\n", event.ip.toString().c_str());
  NTP.init((char *)"pool.ntp.org", UTC0100);
  NTP.setPollingInterval(5); // Poll first time with 1 second
}

// Event Handler when WiFi is disconnected
void onSTADisconnected(WiFiEventStationModeDisconnected event) {
  Serial.printf("WiFi connection (%s) dropped.\n", event.ssid.c_str());
  Serial.printf("Reason: %d\n", event.reason);
}

void switchtoAP(void) {
  Serial.print(F("Warning: Failed to load Wifi configuration.\nConnect to "));
  Serial.print(hostName);
  Serial.println(F(" , load 192.168.4.1/auth and update with your SSID and password."));
  WiFi.disconnect(false);
  WiFi.mode(WIFI_AP);
  delay(1000);
  WiFi.softAP(hostName);
}

// Try to load Wifi configuration from config.json
bool loadWifiConf(void) {
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
	  switchtoAP();
	  return false;
  }    
  size_t size = configFile.size();
  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);
  DynamicJsonBuffer jsonBuffer;
  JsonObject &json = jsonBuffer.parseObject(buf.get());
  if (!json.success()) {
	  switchtoAP();
	  return false;
  }
  Serial.println("Config file loaded.");
  // Parse json config file 
  ssid = json["ssid"];
  password = json["pswd"];
  nodeNumber = json["nodeNumber"];  
  adminPswd = json["adminPswd"].as<String>();
  adminPIN = json["pin"].as<String>();
  
  json["phoneNumber1"].printTo(phoneNumber1);
  json["phoneNumber2"].printTo(phoneNumber2);
  json["pushDevId"].printTo(pushDevId);
  pushDevId.replace("\"", "");
  
  String startTimeZ1, startTimeZ2, stopTimeZ1, stopTimeZ2 = "";
  json["startTimeZ1"].printTo(startTimeZ1);
  json["stopTimeZ1"].printTo(stopTimeZ1);
  json["startTimeZ2"].printTo(startTimeZ2);
  json["stopTimeZ2"].printTo(stopTimeZ2);
  startZ1 = startTimeZ1.substring(1, 3).toInt() * 3600 + startTimeZ1.substring(4, 6).toInt() * 60;
  stopZ1 = stopTimeZ1.substring(1, 3).toInt() * 3600 + stopTimeZ1.substring(4, 6).toInt() * 60;
  startZ2 = startTimeZ2.substring(1, 3).toInt() * 3600 + startTimeZ2.substring(4, 6).toInt() * 60;
  stopZ2 = stopTimeZ2.substring(1, 3).toInt() * 3600 + stopTimeZ2.substring(4, 6).toInt() * 60;

  pauseSeconds = json["pauseTime"];
  hornSeconds = json["horseTime"];
 
  bool dhcp = json["dhcp"];    
  if (!dhcp) {
    IPAddress ip, gateway, subnetmask;
    const char *strIpAddress = json["ipAddress"]; // IP Address from config file
    const char *strGateway = json["gateway"]; // Set gateway to match your network (router)
    const char *strSubnetmask = json["subnetmask"]; // Set subnet mask to match your network
    ip.fromString(strIpAddress);
    gateway.fromString(strGateway);
    subnetmask.fromString(strSubnetmask);
    if (!WiFi.config(ip, gateway, subnetmask))
      Serial.println("Warning: Failed to manual setup wifi connection. I'm going to use dhcp");
  }
    
  // Try to connect to WiFi
  WiFi.hostname("smartalarm");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
 
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
	  switchtoAP();
	  return false;
  }   
  return true;
}


// ***************************************************************************************************** //
// *********************************  PUSHBULLET MESSAGES  ********************************************* //
// ***************************************************************************************************** //

bool sendPushNotification(){  
  const char* host = "api.pushingbox.com";      
  // Use WiFiClient class to create TCP connections
  WiFiClient client;
  const int httpPort = 80;
  if (!client.connect(host, httpPort)) {
    Serial.println("Connection failed");
    return false;
  }

  time_t tm = NTP.getLastSync();
  char ore[9];        
  char giorno[11];  
  snprintf(ore, sizeof(ore), "%02d:%02d:%02d", hour(tm), minute(tm), second(tm));
  snprintf(giorno, sizeof(giorno), "%02d/%02d/%04d", day(tm), month(tm), year(tm));    
  // We now create a URI for the request
  String url = "/pushingbox?devid=";  url += pushDevId;
  url += "&time=";  url += ore;
  url += "&date=";  url += giorno;
    
  Serial.print("Requesting URL: ");  Serial.println(url);    
  client.print(String("GET ") + url + " HTTP/1.1\r\n" + "Host: " + host + "\r\n" + "Connection: close\r\n\r\n");
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 5000) {
      Serial.println(">>> Client Timeout !");
      client.stop();
      return false;
    }
  }  
  // Read all the lines of the reply from server and print them to Serial
  while(client.available()){
    String line = client.readStringUntil('\r');
    Serial.print(line);    
  }  
  Serial.println("\nClosing connection");
  return true;
}
  

 
// ***************************************************************************************************** //
// *************************************  SMS MESSAGES  ************************************************ //
// ***************************************************************************************************** //
void checkSMS() {
	int unreadSMSLocs[30] = { 0 };
	int unreadSMSNum = 0;
//	SMSmessage sms; 

  /*
	// Get the memory locations of unread SMS messages.
	unreadSMSNum = A6_GSM.getSMSLocs(unreadSMSLocs, 5);
	for (int i = 0; i < unreadSMSNum; i++) {
		Serial.print("New message at index: ");
		Serial.println(unreadSMSLocs[i], DEC);

		sms = A6_GSM.readSMS(unreadSMSLocs[i]);
		Serial.println(sms.number);
		Serial.println(sms.date);
		Serial.println(sms.message);

		if(deleteAfterRead)
			A6_GSM.deleteSMS(i);
	}
	*/
}

