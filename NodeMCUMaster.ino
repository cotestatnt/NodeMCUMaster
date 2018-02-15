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
#include <ArduinoJson.h>
#include <CBC.h>
#include <AES.h>
#include <SHA256.h>
#include <EEPROM.h>
#include <WiFiUdp.h>
#include <TimeLib.h>

WiFiUDP Udp;
static const char ntpServerName[] = "pool.ntp.org";
const int timeZone = 1;         // Central European Time
unsigned int localPort = 8888;  // local port to listen for UDP packets

#define MAX_NODES 7
#define TIMECHECK 60        // In seconds
const byte ALARM_ON = D0;   // Led D5
const byte HORN_ON = D1;    // Relay K2
const byte REED_IN = 10;    // Reed sensor input (Pull-UP)
const byte UsedPin[] = { REED_IN, HORN_ON, ALARM_ON };

#define TINY_GSM_MODEM_A6
#include <SoftwareSerial.h>
SoftwareSerial SerialAT(D2, D3, false, 1024); // RX, TX

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

// Instantiate a AES block ciphering 
#define BLOCK_SIZE 16
CBC<AES128> myChiper;
byte encryptKey[BLOCK_SIZE] = {0x5C, 0x39, 0x38, 0x7B, 0x36, 0x60, 0x68, 0x23, 0x34, 0x34, 0x76, 0x5C, 0x34, 0x22, 0x62, 0x55};
byte iv[BLOCK_SIZE] = {0x5C, 0x39, 0x38, 0x7B, 0x36, 0x60, 0x68, 0x23, 0x34, 0x34, 0x76, 0x5C, 0x34, 0x22, 0x62, 0x55};
byte cipherText[2*BLOCK_SIZE];
byte payload[2*BLOCK_SIZE];

// Instance of the radio driver
RF24 radio(D4, D8);                // nRF24L01(+) radio attached
RF24Network network(radio);       // Network uses that radio
const uint16_t this_node = 00;    // Address of our node in Octal format

struct Node { uint32_t timestamp; uint16_t address; uint16_t deviceId; uint16_t battery; uint8_t state; uint8_t zone; bool alarm;  };
Node nodes[MAX_NODES]; // Master always 0 + n nodes
byte sensorsZ1[MAX_NODES];
byte sensorsZ2[MAX_NODES];

// Global variables
uint16_t NewGPIO, OldGPIO = 0;  
String phoneNumber1, phoneNumber2, pushDevId = "";
uint8_t nodeNumber, oldState, pauseSeconds, hornSeconds = 0;
uint32_t startZ1, stopZ1, startZ2, stopZ2, actualTime, secondsOfday = 0;
uint32_t hornTime, pauseTime, blinkTime;
bool Notify, Notified, phoneCall, phoneCalled, activateHorn = false;
bool A6RTCsynced = false;
bool notification1, notification2, phonecall1, phonecall2, alarm1, alarm2 = false;
String pushString = "SMS message.";

Ticker statusUpdater, timeUpdater;

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
  digitalWrite(ALARM_ON, LOW);
  pinMode(HORN_ON, OUTPUT);
  digitalWrite(HORN_ON, LOW);
  
  pinMode(REED_IN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(REED_IN), handleInterrupt, RISING);

  // Start Serial for debug
  Serial.begin(115200);    
  Serial.println();    
  Serial.println(F("Start Smart Alarm system."));
  
  // Init GSM module  
  SerialAT.begin(115200);  
  modem.setBaud(57600);
  SerialAT.begin(57600);
  SerialAT.flush();  
  Serial.print(F("Initializing GSM module..."));
  if (!modem.init()) {
    Serial.print(F(" error. Try again..."));    
    SerialAT.println("AT");
    SerialAT.println("AT");
    if (!modem.init()) 
      Serial.println(F(" ERROR. NO GSM functionality!"));
  } 
  else
    Serial.println(F(" done!"));

  // Start SPIFFS filesystem
  SPIFFS.begin();
 
  // Load configuration and start wifi
  if(!loadWifiConf()){
    Serial.println("No WiFi connection; start Access Point mode (192.168.4.41)");
    WiFi.mode(WIFI_AP_STA);  
    WiFi.softAP(hostName);    
  }
  gotIpEventHandler = WiFi.onStationModeGotIP(onSTAGotIP);
  disconnectedEventHandler = WiFi.onStationModeDisconnected(onSTADisconnected);

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
  Serial.print("IP address: "); 
  Serial.println( WiFi.localIP());
  
  // Add service to MDNS-SD
  MDNS.begin("smartalarm");
  MDNS.addService("http", "tcp", 80); 
  
  // Start UDP client for NTP time update
  Udp.begin(localPort);
  time_t syncTime = getNtpTime(); 
  setTime(syncTime);
  if(syncTime > 0)
    A6RTCsynced = syncA6_RTC(syncTime);
  
  // Init nRF24 Network
  SPI.begin();
  radio.begin();
  radio.setDataRate(RF24_250KBPS);
  network.begin(/*channel*/ 90, /*node address*/ this_node);
  
  // Start tickers (seconds, callback function)
  statusUpdater.attach(1, updateStatus);  
    
  // Set the last state before restart  
  currentState = PAUSE;
  hornTime = millis();
  pauseTime = millis();
}


// ***************************************************************************************************** //
// *****************************************    LOOP   ************************************************* //
// ***************************************************************************************************** //    
void loop() { 
  if(currentState != oldState){  Serial.printf("\nState: %d", currentState); oldState = currentState;  }
 
  checkSMS();
  
  while (SerialAT.available() > 0)
    Serial.write(SerialAT.read());
  while (Serial.available() > 0)
    SerialAT.write(Serial.read());
      
  // Check if nRF24 data message is present
  getRadioData();  

  switch (currentState) {  
  case DISABLE:
    digitalWrite(HORN_ON, LOW);
    digitalWrite(ALARM_ON, LOW);    
    break;

  // Run alarm system
  case ENABLE:
    // Update timestamp before re-enable the system
    digitalWrite(HORN_ON, LOW);   
    Notified = false;
    phoneCalled = false;
    for (byte i = 0; i < MAX_NODES; i++){
      nodes[i].timestamp =  now() + timeZone * SECS_PER_HOUR; 
      nodes[i].alarm = false;
    }
    currentState = RUN;
    break;

  // Wait for node alive messages and check nodes
  case RUN:  
    if (millis() - blinkTime > 500) {
      digitalWrite(ALARM_ON, !digitalRead(ALARM_ON));
      blinkTime = millis();
    }
    getRadioData();
    break;

  // Like ENABLE state, but notification and alarms will be setted only in timed interval
  case TIME:
    if (millis() - blinkTime > 1000) {
      digitalWrite(ALARM_ON, !digitalRead(ALARM_ON));
      blinkTime = millis();
    }
    getRadioData();
    break;

  // Wait a defined time and after re-enable the system
  case PAUSE:
    digitalWrite(HORN_ON, LOW);
    digitalWrite(ALARM_ON, LOW);
    if (millis() - pauseTime > pauseSeconds * 1000) {
      pauseTime = millis();
      currentState =  static_cast<States>(EEPROM.read(0));
      Serial.printf("\nGoing to state:%d", currentState);
    }
    for (byte i = 0; i < MAX_NODES; i++){
      nodes[i].timestamp =  now() + timeZone * SECS_PER_HOUR; 
      nodes[i].alarm = false;
      delay(100);
    }    
    break;  

  // Ops, we have an alarm: send message to clients and turn on the horn (after 15seconds) 
  // At this point, status will be changed only after authorized user action
  case ALARM:    
    if (millis() - blinkTime > 100) {
      digitalWrite(ALARM_ON, !digitalRead(ALARM_ON));
      blinkTime = millis();
    }
    EEPROM.write(0, currentState);
    EEPROM.commit();  
    // pushbullet notification
    if (Notify & !Notified) {
      Notify = false;
      Notified = sendPushNotification();
      if (Notified) {
        currentState = PAUSE;
        pauseTime = millis();
      }
      else
        delay(2000);
    }
    // Activate the horn after user defined time elapsed
    if (millis() - hornTime > 15000) {
      hornTime = millis();
      Serial.printf("\n horn: %d; phone: %d; notify: %d; hornSeconds: %u", activateHorn, phoneCall, Notify, hornSeconds);

      if (activateHorn) {
        activateHorn = false;
        digitalWrite(HORN_ON, HIGH);
        Serial.println("\nAcustic segnalator ON");
      }

      if (phoneCall && (!phoneCalled) ){
        phoneCall = false;              
        Serial.println("phoneCalling....");
        phoneCalled = callNumbers();   
      }
    }

   
    break;
  }

   
}

// ***************************************************************************************************** //    
// ***************************************************************************************************** //    
// Check nodes state and update connected clients
void updateStatus() {            
  uint8_t nodesOK = 0;
  // Update connected clients   
  actualTime = now() + timeZone * SECS_PER_HOUR;    
  uint8_t h = hour(actualTime)*60*60;
  uint8_t m = minute(actualTime)*60;
  uint8_t s = second(actualTime);    
  secondsOfday = h + m + s;  
 
  for (byte i = 0; i < 16; i++)
    for (byte j = 0; j < sizeof(UsedPin); j++)
      if((UsedPin[j] == i)&&(digitalRead(i) == HIGH))
          SET_BIT(NewGPIO, i);
   
  char str[2];
  sprintf(str, "%d", currentState);
  sendDataWs((char *)"status", str);    
  if(currentState >= ENABLE){
    for (byte i = 1; i < MAX_NODES; i++) {
      unsigned long int elapsedtime = actualTime - nodes[i].timestamp;         
      if (elapsedtime <= TIMECHECK) {
        nodesOK++;        
        nodes[i].alarm = false;
      }
      else {
        // if sensor is disabled dont't set alarm
        if (nodes[i].state != 0) {
          os_printf("\nSensor %u not respond since %lu seconds", nodes[i].address, elapsedtime);                             
          nodes[i].alarm = true;
        }     
      }
    }
    
    // Something wrong -> less node than required, set Alarm state
    if (nodesOK < nodeNumber) {    
      digitalWrite(ALARM_ON, LOW);      
      os_printf("\nALARM! Sensors active %d/%d", nodesOK, nodeNumber);
      delay(100);
      hornTime = millis();
      currentState = ALARM;      
      return ;
    }  


    for (byte i = 0; i <= MAX_NODES; i++) {  
      bool nodeInAlarm = nodes[i].alarm;
      byte nodeZone = nodes[i].zone;
      if (nodeInAlarm == true) {
        // Serial.printf("Node %d in alarm state. Node zone: %d", i, nodeZone); 
        // Check if we have to set alarm or notification when TIME mode is anabled
        if (currentState == TIME) {
          if ((secondsOfday >= startZ1) && (secondsOfday <= stopZ1) && (nodeZone == 1) && notification1){
            pushString = "Warning attivato in zona 1!"; 
            Notify = true;          
          }         
          if ((secondsOfday >= startZ2) && (secondsOfday <= stopZ2) && (nodeZone == 2) && notification2){
            pushString = "Warning attivato in zona 2!"; 
            Notify = true;        
          }
          if ((secondsOfday >= startZ1) && (secondsOfday <= stopZ1) && (nodeZone == 1) && alarm1){
            phoneCall = true;             
          }
          if ((secondsOfday >= startZ2) && (secondsOfday <= stopZ2) && (nodeZone == 2) && alarm2){
            phoneCall = true;        
          }
        }
        
        // Check if we have to set alarm or notification in all other state
        if ((currentState >= ENABLE) && (currentState < TIME)) {   
          if ( ((nodeZone == 1) && notification1) || ((nodeZone == 2) && notification2) ){            
            pushString = "Warning attivato in zona " + String(nodeZone) + "!";; 
            Notify = true;                      
          }                         
          if ( ((nodeZone == 1) && phonecall1) || ((nodeZone == 2) && phonecall2) ) {
            phoneCall = true;   
            phoneCalled = false;     
            hornTime = millis();
            currentState = ALARM;      
          }

          if ( ((nodeZone == 1) && alarm1) || ((nodeZone == 1) && alarm2) ) {
            activateHorn = true; 
            hornTime = millis();
            currentState = ALARM;                        
          }
          
        }     
      }                
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
    
    os_printf("\nNode %u:", header.from_node);        
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
      os_printf("\nElapsed time: %u seconds", actualTime - nodes[fromNode].timestamp);      

      uint16_t deviceId = (payload[_DeviceNumber] << 8) | payload[_DeviceNumber];
      uint16_t battery = (payload[_Battery] << 8) | payload[_Battery];
      nodes[fromNode].address = fromNode;
      nodes[fromNode].state = payload[_Enabled];
      nodes[fromNode].battery = battery;
      nodes[fromNode].deviceId = deviceId;                
      nodes[fromNode].timestamp = actualTime;   
      nodes[fromNode].alarm = false;      
    }    
    
    if (payload[_MsgType] == SEND_ALARM){
      os_printf("\nALARM message from node %u", header.from_node);          
      nodes[header.from_node].alarm = true;
    }

    // update zone if new sensors added
    for (byte i = 0; i <= MAX_NODES; i++) {        
      nodes[i].zone = 0;       
      for (byte j = 0; j < MAX_NODES; j++){               
        if ((nodes[i].address != 99) && (nodes[i].address == sensorsZ1[j]))
          nodes[i].zone = 1;          
        if ((nodes[i].address != 99) && (nodes[i].address == sensorsZ2[j]))
          nodes[i].zone = 2; 
      }
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
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, uint16_t len) {
  switch (type){
    case WS_EVT_CONNECT:
      os_printf("\nClient %u connected. Send ping", client->id());
      client->printf("{\"ts\":%u,\"cmd\":\"status\",\"msg\":\"Client id %u connected\"}", actualTime, client->id());
      client->ping();
      break;
    case WS_EVT_DISCONNECT:
      os_printf("\nClient %u disconnected", client->id());
      break;
    case WS_EVT_ERROR:
      os_printf("\nClient error(%u): %s", client->id(), (char*)data);
      break;
    case WS_EVT_PONG:
      os_printf("\nClient %u pong", client->id());
      client->printf("{\"ts\":%u,\"cmd\":\"jsVars\",\"timePause\":\"%u\",\"timeHorn\":\"%u\"}", actualTime, pauseSeconds, 15);
      break;
    case WS_EVT_DATA:
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      String msg = "";
      if (info->final && info->index == 0 && info->len == len) {        
        for (uint16_t i = 0; i < info->len; i++)
          msg += (char)data[i];     
        os_printf("\nClient %u message: %s", client->id(), msg.c_str() );        
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
  uint16_t len = root.measureLength();
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
    os_printf("\nSet pin %s %u", pinName.c_str(), state);
    pushString = "Allarme attivato!";
    Notify = true;
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
    EEPROM.write(0, ENABLE);
    EEPROM.commit();
    currentState = PAUSE;
    return;
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
    os_printf("Sensor n° %d is", _node);    
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
    EEPROM.write(0, currentState);
    EEPROM.commit();
    Serial.printf("\nRestart... Current state:%d", currentState);
    delay(1000);
    pauseTime = millis();
    ESP.restart();
  }

  // Update RTC time
  else if (strcmp(command, "setTime") == 0) {
    time_t syncTime = root["syncTime"];
    Serial.printf("\nSet new time: %lu", syncTime);
    A6RTCsynced = syncA6_RTC(syncTime);
  }
  
  // Provide config.json to admin settings page
  else if (strcmp(command, "getconf") == 0) {
    File configFile = SPIFFS.open("/config.json", "r");
    if (configFile) {
      uint16_t len = configFile.size();
      AsyncWebSocketMessageBuffer *buffer =  ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
      if (buffer) {
        configFile.readBytes((char *)buffer->get(), len + 1);
        ws.textAll(buffer);
      }
      configFile.close();
    }
  }

  // Save new status in case of reboot  
  if(currentState != PAUSE){
    EEPROM.write(0, currentState);
    EEPROM.commit();
  }
  
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
    Serial.println("\nInterrupt");
    nodes[0].alarm = true;
    hornTime = millis();
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
  WiFi.mode(WIFI_STA);  
  os_printf("STA IP address: %s\n", event.ip.toString().c_str());
  time_t syncTime = getNtpTime(); 
  if(syncTime > 0){
    setTime(syncTime);
    A6RTCsynced = syncA6_RTC(syncTime);
  }
}


// Event Handler when WiFi is disconnected
void onSTADisconnected(WiFiEventStationModeDisconnected event) {
  os_printf("WiFi connection (%s) dropped. Reason: %d\n", event.ssid.c_str(),event.reason);  
  WiFi.mode(WIFI_AP_STA);  
  WiFi.softAP(hostName);
}


// Try to load Wifi configuration from config.json
bool loadWifiConf(void) {
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("Warning: Failed to load Wifi configuration.\n "); 
    return false;
  }    
  uint16_t size = configFile.size();
  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);
  DynamicJsonBuffer jsonBuffer;
  JsonObject &json = jsonBuffer.parseObject(buf.get());
  if (!json.success()) {
    Serial.println("Warning: Failed to parse config.json\n ");
    return false;
  }
  Serial.println("Config file loaded.");
  // Parse json config file 
  ssid = json["ssid"];
  password = json["pswd"];
  nodeNumber = json["nodeNumber"];  
  adminPswd = json["adminPswd"].as<String>();
  adminPIN = json["pin"].as<String>();
  
  json["phone1"].printTo(phoneNumber1);
  json["phone2"].printTo(phoneNumber2);
  json["pushDevId"].printTo(pushDevId);
  pushDevId.replace("\"", "");

  String zone1; json["zone1"].printTo(zone1);  zone1.replace("\"", "");  zone1.trim();
  String zone2; json["zone2"].printTo(zone2);  zone2.replace("\"", "");  zone2.trim();  
   
  int z = 0;
  char *tz1 = strtok((char*)zone1.c_str(), ",");
  while (tz1 != NULL) {
      sensorsZ1[z++] = atoi(tz1);  
      tz1 = strtok(NULL, ",");    
  }  
  z = 0;
  char *tz2 = strtok((char*)zone2.c_str(), ",");
  while (tz2 != NULL) {
      sensorsZ2[z++] = atoi(tz2);  
      tz2 = strtok(NULL, ",");    
  }
  
  Serial.print("\nZone 1, sensors: ");  
  for (byte i=0; i<MAX_NODES; i++)
    Serial.printf("%02d; ", sensorsZ1[i]);    
    
  Serial.print("\nZone 2, sensors: ");  
  for (byte i=0; i<MAX_NODES; i++)
    Serial.printf("%02d; ", sensorsZ2[i]);
      
  notification1 = json["n1"];
  notification2 = json["n2"];
  phonecall1 = json["p1"];
  phonecall2 = json["p2"];
  alarm1 = json["a1"];
  alarm2 = json["a2"];  
  String startTimeZ1, startTimeZ2, stopTimeZ1, stopTimeZ2 = "";
  json["startZ1"].printTo(startTimeZ1);
  json["stopZ1"].printTo(stopTimeZ1);
  json["startZ2"].printTo(startTimeZ2);
  json["stopZ2"].printTo(stopTimeZ2);
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
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);  
  WiFi.begin(ssid, password);
 
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {  
    os_printf("Warning: Connection to %s failed.\n", ssid); 
    return false;   
  }   
  return true;
}


// ***************************************************************************************************** //
// *************************************      NTP code     ********************************************* //
// ***************************************************************************************************** //

const int NTP_PACKET_SIZE = 48;       // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE];   //buffer to hold incoming & outgoing packets

time_t getNtpTime(){
  IPAddress ntpServerIP;              // NTP server's ip address
  while (Udp.parsePacket() > 0);      // discard any previously received packets  
  // get a random server from the pool
  WiFi.hostByName(ntpServerName, ntpServerIP);
  Serial.printf("Transmit NTP Request to %s (%s)\n", ntpServerName, String(ntpServerIP).c_str() );
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.println("Received NTP Response");
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 = (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
    }
  }
  Serial.println("No NTP Response :-(");
  return 0; // return 0 if unable to get the time
}

void sendNTPpacket(IPAddress &address){
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;            // Stratum, or type of clock
  packetBuffer[2] = 6;            // Polling Interval
  packetBuffer[3] = 0xEC;         // Peer Clock Precision
                                  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}


// ***************************************************************************************************** //
// *********************************  PUSHBULLET MESSAGES  ********************************************* //
// ***************************************************************************************************** //
String URLEncode(const char* msg){
  const char *hex = "0123456789abcdef";
  String encodedMsg = "";
  while (*msg != '\0') {
    if (('a' <= *msg && *msg <= 'z')
      || ('A' <= *msg && *msg <= 'Z')
      || ('0' <= *msg && *msg <= '9')) {
      encodedMsg += *msg;
    }
    else {
      encodedMsg += '%';
      encodedMsg += hex[*msg >> 4];
      encodedMsg += hex[*msg & 15];
    }
    msg++;
  }
  return encodedMsg;
}


// Use WiFiClient class to create TCP connections
WiFiClient client;
char serverName[] = "api.pushingbox.com";

bool sendPushNotification(){
  client.stop();
  Serial.print("Connecting to Pushingbox...");
  if (client.connect(serverName, 80)) {    
    Serial.println(" connected."); 
    time_t tm = now();
    char ore[9];
    char giorno[11];
    snprintf(ore, sizeof(ore), "%02d:%02d:%02d", hour(tm), minute(tm), second(tm));
    snprintf(giorno, sizeof(giorno), "%02d/%02d/%04d", day(tm), month(tm), year(tm)); 
    String  request = pushDevId;
    request += "&time=";    request += ore;
    request += "&date=";    request += giorno;
    request += "&message="; request += URLEncode(pushString.c_str());  
    //Serial.printf( "http://api.pushingbox.com/pushingbox?devid=%s\n", request.c_str());

    client.print("GET /pushingbox?devid=");
    client.print(request.c_str());
    client.println(" HTTP/1.1");
    client.print("Host: ");
    client.println(serverName);
    client.println("User-Agent: Arduino");
    client.println();

    unsigned long timeout = millis();
    while (client.available() == 0) {
      if (millis() - timeout > 5000) {
        Serial.println(">>> Client Timeout !");
        client.stop();
        return false;
      }
    }
    // Read all the lines of the reply from server and print them to Serial
    while (client.available()) {
      String line = client.readStringUntil('\r');
      Serial.print(line);
    }
    client.stop();
    Serial.println("Connection closed");
    return true;    
  }
  else {
    Serial.println("connection failed"); 
    return false;    
  }
}


// ***************************************************************************************************** //
// ************************************* Sync Time  **************************************************** //
// ***************************************************************************************************** //
bool setTimeFromA6RTC() {
  String txt = modem.getRealTimeClock();
  Serial.print("A6 time: ");
  Serial.println(txt);
  if (txt != "") {
    uint16_t year = 2000 + txt.substring(1, 3).toInt();
    uint8_t month = txt.substring(4, 6).toInt();
    uint8_t day = txt.substring(7, 9).toInt();
    uint8_t hour = txt.substring(10, 12).toInt();
    uint8_t minute = txt.substring(13, 15).toInt();
    uint8_t second = txt.substring(16, 18).toInt();
    os_printf("\nActual synced time: %lu\n", now());
    os_printf("%u/%u/%u %u:%u:%u", day, month, year, hour, minute, second);
    setTime(hour, minute, second, day, month, year);
    os_printf("\nNew synced time: %lu\n", now());
    return true;
  }
  else
    return false;
}

bool syncA6_RTC(time_t tm) {
    os_printf("\nActual synced time: %lu\n", now());
    char buf[22];
    Serial.print("Setting time of A6 module...");
    snprintf(buf, sizeof(buf), "%02d/%02d/%02d, %02d:%02d:%02d+1",
      year(tm) % 100, month(tm), day(tm), hour(tm), minute(tm), second(tm));
    Serial.println(buf);    
    bool res = modem.setRealTimeClock(buf);
    Serial.println(res ? " done." : " error.");
    return res;  
}

 
// ***************************************************************************************************** //
// *************************************  GSM Module (MESSAGES, RTC)************************************ //
// ***************************************************************************************************** //
bool checkSMS() { 
  static uint32_t SMSTime = millis();
  if (millis() - SMSTime > 30000) {
    SMSTime = millis();
    if (!modem.testAT()) 
      return false;

    if (modem.countSMS() > 0) {
      String  message, number = "";
      if (modem.readSMS(1, message, number)) {
        Serial.printf("New SMS from %s: %s\n", number.c_str(), message.c_str());
        pushString = message;
        Notify = true;
        modem.deleteSMS(1);

        /////////
        #define DEBUG_SMS true
        /////////

        if ((number == phoneNumber1) || (number == phoneNumber2) || DEBUG_SMS) {
          Serial.println("Verifica pin.");
          if (message.indexOf("START") > 0) {
            String smsPin = message.substring(message.indexOf("START") + 5);
            if (smsPin == adminPIN)
              currentState = ENABLE;
          }
          if (message.indexOf("STOP") > 0) {
            String smsPin = message.substring(message.indexOf("STOP") + 4);
            if (smsPin == adminPIN)
              currentState = DISABLE;
          }
          if (message.indexOf("TIME") > 0) {
            String smsPin = message.substring(message.indexOf("TIME") + 4);
            if (smsPin == adminPIN)
              currentState = TIME;
          }
          if (message.indexOf("code is") > 0) {
            String smsPin = message.substring(message.indexOf("code is ") + 8);
            Serial.print("Pin: ");
            Serial.println(smsPin);
          }
        }
      }
    }
  }
  return true;
}

bool callNumbers() {
  int response = 0;  
  if (phoneNumber1.length() > 5) {
    for (byte i = 0; i < 2; i++) {
      Serial.printf("\nCalling num. %d to the phone number %s; Response: ", i+1, phoneNumber1.c_str());
      response = modem.callNumber(phoneNumber1);
      Serial.println(response);
      // User accept incoming call or voluntarily hook incaming call
      if ((response == 1)||(response == 2)){
        Serial.println("Done.");
        return true;
      }
      // No answer
      if (response == 3){
        Serial.println("No answer.");
        modem.callHangup();
        delay(5000);
      }
    }
  }
  
  // No answer from phone number 1, call second one
  if (phoneNumber2.length() > 5 ) {
    for (byte i = 0; i < 2; i++) {
      Serial.printf("\nCalling num. %d to the phone number %s; Response: ", i+1, phoneNumber2.c_str());
      response = modem.callNumber(phoneNumber2);
      Serial.println(response);
      // User accept incoming call or voluntarily hook incaming call
      if ((response == 1)||(response == 2)){
        Serial.println("Done.");
        return true;
      }
      // No answer
      if (response == 3){
        Serial.println("No answer.");
        modem.callHangup();
        delay(5000);
      }
    }
  }
  return false;
}


