/*
 ESP8266 MQTT client for reporting DHT11 temperature

 Requires ESP8266 board, Adafruit Universal and DHT Sensor libraries

 (c) blaz@kristan-sp.si / 2019-09-10
*/

#include <FS.h>                   //this needs to be first, or it all crashes and burns...

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>          // multicast DNS
#include <WiFiUdp.h>              // UDP handling
#include <ArduinoOTA.h>           // OTA updates
#include <EEPROM.h>
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     //Local WebServer used to serve the configuration portal
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic

#define MQTT_MAX_PACKET_SIZE 1024
#include <PubSubClient.h>

#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

#include "DHT.h"
#include <OneWire.h>
#include <DallasTemperature.h>

#define DEBUG   0

// NOTE: You cannot use both PIR, DHT & Dallas temperature sensors at the same time without changing #defines

// Dallas DS18B20 temperature sensors
#define TEMPERATURE_PRECISION 9
#define ONEWIREPIN 2
OneWire *oneWire = NULL;          // pin GPIO2 (a 4.7K pull-up resistor is necessary)
DallasTemperature *sensors = NULL;
DeviceAddress *thermometers[10];  // arrays to hold device addresses (up to 10)
int numThermometers = 0;

// DHT type temperature/humidity sensors
#define DHTPIN  2           // what pin we're connected to (default GPIO2=2, GPIO0=0, RX=3, TX=1)
float tempAdjust = 0.0;     // temperature adjustment for wacky DHT sensors (retrieved from EEPROM)
DHT *dht = NULL;            // DHT11, DHT22, DHT21, none(0)

// PIR sensor or button/switch pin & state
int PIRPIN = 2;         // pin used for PIR
int PIRState = 0;       // initialize PIR state

// relay pins
const int relays[4] = {0, 2, 3, 1};     // relay pins (GPIO0, GPIO2, RX, TX)
// NOTE: GPIO0 has to be HIGH for a normal boot (LOW for flashing).
// GPIO2 is HIGH at boot.
// Also, some debug output is sent to TX during boot.
// Add 330/470 Ohm resistor between RX pin and USB driver, to prevent shorting.
// Check: https://www.instructables.com/id/How-to-use-the-ESP8266-01-pins/
int relayState[4] = {0, 0, 0, 0};       // relay states
int numRelays = 0;                      // number of relays used
// uncomment next line for saving relay states to EEPROM
//#define EEPROMSAVE 1

// Update these with values suitable for your network.
char mqtt_server[40] = "192.168.70.11";
char mqtt_port[7]    = "1883";
char username[33]    = "";
char password[33]    = "";
char MQTTBASE[16]    = "sensors"; // use shellies for Shelly MQTT Domoticz plugin integration

char c_relays[2]     = "0";
char c_dhttype[8]    = "none";
char c_pirsensor[2]  = "0";
char c_idx[4]        = "0";         // domoticz switch/sensor IDX

// flag for saving data from WiFiManager
bool shouldSaveConfig = false;

unsigned long lastMsg = 0;
char msg[256];
char mac_address[16];
char outTopic[64];  // add MAC address in WiFi setup code
char clientId[20];  // MQTT client ID

WiFiClient espClient;
PubSubClient client(espClient);

// private functions
void mqtt_callback(char*, byte*, unsigned int);
void mqtt_reconnect();
char *ftoa(float,char*,int d=2);
void saveConfigCallback ();


//-----------------------------------------------------------
// main setup
void setup() {
  char str[32];
  delay(3000);

  #if DEBUG
  Serial.begin(115200);
  #else
  // Initialize the BUILTIN_LED pin as an output & set initial state LED on
  pinMode(BUILTIN_LED, OUTPUT);
  digitalWrite(BUILTIN_LED, LOW);
  #endif

  String WiFiMAC = WiFi.macAddress();
  WiFiMAC.replace(":","");
  WiFiMAC.toCharArray(mac_address, 16);
  // Create client ID from MAC address
  sprintf(clientId, "esp-%s", &mac_address[6]);

  // request 21 configuration bytes from EEPROM
  EEPROM.begin(21);

  #if DEBUG
  Serial.println("");
  Serial.print("EEPROM data: ");
  #endif
  for ( int i=0; i<21; i++ ) {
    str[i] = EEPROM.read(i);
    #if DEBUG
    Serial.print(str[i], HEX);
    Serial.print(":");
    #endif
  }
  #if DEBUG
  Serial.print(" (");
  Serial.print(str);
  Serial.println(")");
  #endif

  if ( strncmp(str,"esp",3) == 0 ) {
    #if DEBUG
    Serial.println("Converting EEPROM data.");
    #endif
    //sscanf(str,"esp%3s%-7s%1s%1s%3.1f", c_idx, c_dhttype, c_relays, c_pirsensor, tempAdjust);
    strncpy(c_idx, &str[3], 3);
    strncpy(c_dhttype, &str[6], 7);
    for ( int i=0; i<7; i++ ) {
      if ( c_dhttype[i]==' ' ) {
        c_dhttype[i] = '\0';
        break;
      }
    }
    strncpy(c_relays, &str[13], 1);
    strncpy(c_pirsensor, &str[14], 1);
    tempAdjust = atof(&str[15]);
    #if DEBUG
    Serial.print("idx: ");
    Serial.println(c_idx);
    Serial.print("dhttype: ");
    Serial.println(c_dhttype);
    Serial.print("relays: ");
    Serial.println(c_relays);
    Serial.print("pirsensor: ");
    Serial.println(c_pirsensor);
    Serial.print("tempAdjust: ");
    Serial.println(ftoa(tempAdjust, str, 1));
    #endif
  }

//----------------------------------------------------------
  //read configuration from FS json
  if ( SPIFFS.begin() ) {
    if ( SPIFFS.exists("/config.json") ) {
      #if DEBUG
      Serial.println("Reading SPIFFS data.");
      #endif
      //file exists, reading and loading
      File configFile = SPIFFS.open("/config.json", "r");
      if ( configFile ) {
        size_t size = configFile.size();
        
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        
        DynamicJsonDocument doc(size);
        DeserializationError error = deserializeJson(doc, buf.get());
        if ( !error ) {
          strcpy(mqtt_server, doc["mqtt_server"]);
          strcpy(mqtt_port, doc["mqtt_port"]);
          strcpy(username, doc["username"]);
          strcpy(password, doc["password"]);
          strcpy(MQTTBASE, doc["base"]);

          #if DEBUG
          Serial.print("mqtt_server: ");
          Serial.println(mqtt_server);
          Serial.print("mqtt_port: ");
          Serial.println(mqtt_port);
          Serial.print("username: ");
          Serial.println(username);
          Serial.print("password: ");
          Serial.println(password);
          Serial.print("base: ");
          Serial.println(MQTTBASE);
          #endif
        } else {
        }
      }
    }
  } else {
    //clean FS, for testing
    SPIFFS.format();
  }
  //end read

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 5);
  WiFiManagerParameter custom_username("username", "username", username, 32);
  WiFiManagerParameter custom_password("password", "password", password, 32);
  WiFiManagerParameter custom_mqtt_base("base", "MQTT topic base", MQTTBASE, 15);

  WiFiManagerParameter custom_relays("relays", "Number of relays (0-4)", c_relays, 1);
  WiFiManagerParameter custom_dhttype("dhttype", "Temp. sensor type (DHT11,DHT22,DS18B20,none)", c_dhttype, 7);
  WiFiManagerParameter custom_pirsensor("pirsensor", "PIR sensor enabled (0/1)", c_pirsensor, 1);
  WiFiManagerParameter custom_idx("idx", "Domoticz switch IDX", c_idx, 3);

  // WiFiManager
  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  // reset settings (for debugging)
  //wifiManager.resetSettings();
  
  // set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //set static ip
  //wifiManager.setSTAStaticIPConfig(IPAddress(10,0,1,99), IPAddress(10,0,1,1), IPAddress(255,255,255,0));
  
  //add all your parameters here
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_username);
  wifiManager.addParameter(&custom_password);
  wifiManager.addParameter(&custom_mqtt_base);

  wifiManager.addParameter(&custom_relays);
  wifiManager.addParameter(&custom_dhttype);
  wifiManager.addParameter(&custom_pirsensor);
  wifiManager.addParameter(&custom_idx);

  // set minimum quality of signal so it ignores AP's under that quality
  // defaults to 8%
  //wifiManager.setMinimumSignalQuality(10);
  
  // sets timeout until configuration portal gets turned off
  // useful to make it all retry or go to sleep
  // in seconds
  //wifiManager.setTimeout(120);

  // fetches ssid and pass and tries to connect
  // if it does not connect it starts an access point with the specified name
  // and goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect(clientId)) {
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }
  //if you get here you have connected to the WiFi

  //read updated parameters
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(username, custom_username.getValue());
  strcpy(password, custom_password.getValue());
  strcpy(MQTTBASE, custom_mqtt_base.getValue());

  strcpy(c_relays, custom_relays.getValue());
  strcpy(c_dhttype, custom_dhttype.getValue());
  strcpy(c_pirsensor, custom_pirsensor.getValue());
  strcpy(c_idx, custom_idx.getValue());

  #if DEBUG
  Serial.println("WiFi Manager parameter check.");
  Serial.print("idx: ");
  Serial.println(c_idx);
  Serial.print("dhttype: ");
  Serial.println(c_dhttype);
  Serial.print("relays: ");
  Serial.println(c_relays);
  Serial.print("pirsensor: ");
  Serial.println(c_pirsensor);
  #endif

  numRelays = max(min(atoi(c_relays),4),0);
  c_relays[0] = '0' + numRelays;

  if ( strcmp(c_dhttype,"DHT11")==0 ) {
    dht = new DHT(DHTPIN, DHT11);
  } else if ( strcmp(c_dhttype,"DHT21")==0 ) {
    dht = new DHT(DHTPIN, DHT21);
  } else if ( strcmp(c_dhttype,"DHT22")==0 ) {
    dht = new DHT(DHTPIN, DHT22);
  } else if ( strcmp(c_dhttype,"DS18B20")==0 ) {
    oneWire = new OneWire(ONEWIREPIN);          // no need to free this one
    sensors = new DallasTemperature(oneWire);   // no need to free this one, too
  } else {
    strcpy(c_dhttype,"none");
  }

  //save the custom parameters to FS
  if ( shouldSaveConfig ) {
    #if DEBUG
    Serial.println("Saving FS data.");
    #endif
    DynamicJsonDocument doc(1024);
    doc["mqtt_server"] = mqtt_server;
    doc["mqtt_port"] = mqtt_port;
    doc["username"] = username;
    doc["password"] = password;
    doc["base"] = MQTTBASE;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      // failed to open config file for writing
    } else {
      serializeJson(doc, configFile);
      configFile.close();
      #if DEBUG
      serializeJson(doc, Serial);
      #endif
    }

    #if DEBUG
    Serial.println("Saving EEPROM data.");
    #endif
    // clear 21 bytes from EEPROM
    sprintf(str,"esp%3s%-7s%1s%1s%3.1f", c_idx, c_dhttype, c_relays, c_pirsensor, tempAdjust);
    for ( int i=0; i<21; i++ ) {
      EEPROM.write(i, str[i]);
    }
    EEPROM.commit();
    delay(120);
  }
//----------------------------------------------------------

  #if !DEBUG
  // if connected set state LED off
  digitalWrite(BUILTIN_LED, HIGH);
  #endif

  if ( numRelays > 0 ) {
    // initialize relay pins
    #ifdef EEPROMSAVE
    // 21st byte contains 8 relays (bits) worth of initial states
    int initRelays = EEPROM.read(20);
    #endif
    // relay states are stored in bits
    for ( int i=0; i<numRelays; i++ ) {
      pinMode(relays[i], OUTPUT);
      #ifdef EEPROMSAVE
      relayState[i] = (initRelays >> 1) & 1;
      #endif
      digitalWrite(relays[i], relayState[i]? HIGH: LOW);
    }
  }

  // done reading & writing EEPROM
  EEPROM.end();
  
  #if DEBUG
  Serial.println("Initializing sensors.");
  #endif

  // set up temperature sensor (either DHT or Dallas)
  if ( strcpy(c_dhttype,"none")!=0 ) {
    
    if ( dht ) {
      // initialize DHT sensor
      dht->begin();
    } // DHT
  
    if ( oneWire ) {
      // OneWire temperature sensors
      byte *tmpAddr, addr[8];
    
      // Start up the library
      sensors->begin();
    
      // locate devices on the bus
      oneWire->reset_search();
      for ( int i=0; !oneWire->search(addr) && i<10; i++ ) {
        if ( OneWire::crc8(addr, 7) != addr[7] ) {
        } else {
          // store up to 10 sensor addresses
          tmpAddr = (byte*)malloc(8); // there is no need to free() this memory
          memcpy(tmpAddr,addr,8);
          sensors->setResolution(tmpAddr, TEMPERATURE_PRECISION);
          thermometers[numThermometers++] = (DeviceAddress *)tmpAddr;
        }
      }
    } // end oneWire
  } // end temperature sensors

  // OTA update setup
  //ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname(clientId);
  //ArduinoOTA.setPassword("ota_password");
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = F("sketch");
    } else { // U_FS
      type = F("filesystem");
    }
    #if DEBUG
    Serial.print(F("Start updating "));
    Serial.println(type);
    #endif
  });
  ArduinoOTA.onEnd([]() {
    #if DEBUG
    Serial.println(F("\nEnd"));
    #endif
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    #if DEBUG
    Serial.printf_P(PSTR("Progress: %u%%\r"), (progress / (total / 100)));
    #endif
  });
  ArduinoOTA.onError([](ota_error_t error) {
    #if DEBUG
    Serial.printf_P(PSTR("Error[%u]: "), error);
    if      (error == OTA_AUTH_ERROR)    Serial.println(F("Auth Failed"));
    else if (error == OTA_BEGIN_ERROR)   Serial.println(F("Begin Failed"));
    else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
    else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
    else if (error == OTA_END_ERROR)     Serial.println(F("End Failed"));
    #endif
  });
  ArduinoOTA.begin();

  #if DEBUG
  Serial.println("MQTT connection.");
  #endif
  // initialize MQTT connection & provide callback function
  client.setServer(mqtt_server, atoi(mqtt_port));
  client.setCallback(mqtt_callback);
}

//-----------------------------------------------------------
// main loop
void loop() {
  char tmp[64];
  long now = millis();
  static long lastPIRChg = 0;
  
  // handle OTA updates
  ArduinoOTA.handle();

  if (!client.connected()) {
    mqtt_reconnect();
  }
  client.loop();

  if ( atoi(c_pirsensor) ) {
    // detect motion and publish immediately
    int lPIRState = digitalRead(PIRPIN);
    if ( lPIRState != PIRState && (now - lastPIRChg > 60000 || lPIRState) ) {
      lastPIRChg = now;
      PIRState = lPIRState;
      
      #if DEBUG
      Serial.print("PIR: ");
      Serial.println(PIRState == HIGH? "1": "0");
      #endif
      
      sprintf(outTopic, "%s/%s/sensor/motion", MQTTBASE, clientId);
      client.publish(outTopic, PIRState == HIGH? "1": "0");

      if ( atoi(c_idx) ) {
        // publish Domoticz API
        DynamicJsonDocument doc(256);
        doc["idx"] = atoi(c_idx);
        doc["command"] = "switchlight";
        doc["switchcmd"] = PIRState ? "On" : "Off";
        serializeJson(doc, msg);
        client.publish("domoticz/in", msg);
      }
    }
  }

  // publish status every 60s
  if ( now - lastMsg > 60000 ) {
    lastMsg = now;
    
    if ( dht ) {
      // Reading temperature or humidity takes about 250 milliseconds!
      // Sensor readings may also be up to 2 seconds 'old' (its a very slow sensor)
      float h = dht->readHumidity();
      // Read temperature as Celsius (the default)
      float t = dht->readTemperature();
      // Read temperature as Fahrenheit (isFahrenheit = true)
      float f = dht->readTemperature(true);
      
      // Check if any reads failed and exit early (to try again).
      if (isnan(h) || isnan(t) || isnan(f)) {
        sprintf(outTopic, "%s/%s/sensor/temperature", MQTTBASE, clientId);
        sprintf(msg, "err");
      } else {
        // may use Shelly MQTT API (shellies/shellyht-MAC/sensor/temperature)
        sprintf(outTopic, "%s/%s/sensor/temperature", MQTTBASE, clientId);
        sprintf(msg, "%.1f", t + tempAdjust);
        client.publish(outTopic, msg);
        sprintf(outTopic, "%s/%s/sensor/temperature_f", MQTTBASE, clientId);
        sprintf(msg, "%.1f", f + tempAdjust*(float)(9/5));
        client.publish(outTopic, msg);
        sprintf(outTopic, "%s/%s/sensor/humidity", MQTTBASE, clientId);
        sprintf(msg, "%.1f", h);
        client.publish(outTopic, msg);
        if ( round(tempAdjust*10) != 0.0 ) {
          sprintf(outTopic, "%s/%s/temp_adjust", MQTTBASE, clientId);
          sprintf(msg, "%.1f", tempAdjust);
          client.publish(outTopic, msg);
        }

        if ( atoi(c_idx) ) {
          // publish Domoticz API
          DynamicJsonDocument doc(256);
          doc["idx"] = atoi(c_idx);
          doc["command"] = "udevice";
          doc["nvalue"] = 0;
          sprintf(tmp, "%.1f;%.1f;0", t + tempAdjust, f + tempAdjust*(float)(9/5));
          doc["svalue"] = tmp;
          serializeJson(doc, msg);
          client.publish("domoticz/in", msg);
        }
      }
    }

    if ( oneWire ) {
      // may use non-standard Shelly MQTT API (shellies/shellyhtx-MAC/temperature/i)
      for ( int i=0; i<numThermometers; i++ ) {
        float tempC = sensors->getTempC(*thermometers[i]);
        if ( numThermometers == 1 )
          sprintf(outTopic, "%s/%s/temperature", MQTTBASE, clientId);
        else
          sprintf(outTopic, "%s/%s/temperature/%i", MQTTBASE, clientId, i);
        sprintf(msg, "%.1f", tempC);
        client.publish(outTopic, msg);

        if ( atoi(c_idx) ) {
          // publish Domoticz API
          DynamicJsonDocument doc(256);
          doc["idx"] = atoi(c_idx);
          doc["command"] = "udevice";
          doc["nvalue"] = 0;
          sprintf(tmp, "%.1f", tempC);
          doc["svalue"] = tmp;
          serializeJson(doc, msg);
          client.publish("domoticz/in", msg);
        }
      }
    }

    // may use Shelly MQTT API (shellies/shelly4pro-MAC/relay/i)
    for ( int i=0; i<numRelays; i++ ) {
      sprintf(outTopic, "%s/%s/relay/%i", MQTTBASE, clientId, i);
      sprintf(msg, relayState[i] ? "on" : "off");
      client.publish(outTopic, msg);

      if ( atoi(c_idx) ) {
        // publish Domoticz API
        DynamicJsonDocument doc(256);
        doc["idx"] = atoi(c_idx) + i;
        doc["command"] = "switchlight";
        doc["switchcmd"] = relayState[i] ? "On" : "Off";
        serializeJson(doc, msg);
        client.publish("domoticz/in", msg);
      }
    }
/*
 * No need for status updates on PIR sensor
 * 
    if ( atoi(c_pirsensor) ) {
      // may use Shelly MQTT API as (shellies/shellysense-MAC/sensor/motion)
      sprintf(outTopic, "%s/%s/sensor/motion", MQTTBASE, clientId);
      client.publish(outTopic, PIRState == HIGH? "1": "0");

      if ( atoi(c_idx) ) {
        // publish Domoticz API
        DynamicJsonDocument doc(256);
        doc["idx"] = atoi(c_idx);
        doc["command"] = "switchlight";
        doc["switchcmd"] = PIRState ? "On" : "Off";
        serializeJson(doc, msg);
        client.publish("domoticz/in", msg);
      }
    }
*/
  // end 60s reporting
  }
  
  // wait 250ms until next update (also DHT limitation)
  delay(250);
}

//---------------------------------------------------
// MQTT callback function
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  char tmp[80];
  String newPayload;
  
  payload[length] = '\0'; // "just in case" fix

  // convert to String & int for easier handling
  newPayload = String((char *)payload);

  if ( strcmp(topic,"domoticz/out") == 0 ) {

    DynamicJsonDocument doc(2048);
    deserializeJson(doc, payload);
  
    if ( doc["idx"] == atoi(c_idx) || (doc["idx"] > atoi(c_idx) && doc["idx"] < atoi(c_idx)+numRelays) ) {
      // proper IDX found (for relays virtual switches have to be consecutive)
      String saction = doc["svalue1"];  // get status/value from Domoticz
      int sactionId = saction.toInt();
      int nvalue = doc["nvalue"];       // get value from Domoticz

      #if DEBUG
      Serial.print("Selected action: ");
      Serial.println(saction);
      #endif

      // Perform action
      for ( int i=0; i<numRelays; i++ ) {
        if ( doc["idx"] == atoi(c_idx)+i ) {
          if ( nvalue ) {
            digitalWrite(relays[i], HIGH);  // Turn the relay on
            relayState[i] = 1;
          } else {
            digitalWrite(relays[i], LOW);  // Turn the relay off
            relayState[i] = 0;
          }
    
          // publish relay state
          sprintf(outTopic, "%s/%s/relay/%i", MQTTBASE, clientId, i);
          sprintf(msg, relayState[i]?"on":"off");
          client.publish(outTopic, msg);
        }
      }
    }

  } else if ( strstr(topic, MQTTBASE) ) {

    if ( strstr(topic,"/temperature/command") ) {
      
      // insert temperature adjustment and store it into non-volatile memory
      float temp = atof((char*)payload);
      if ( temp < 100.0 && temp > -100.0 ) {
        tempAdjust = temp;
        sprintf(tmp, "esp%3s%-7s%1s%1s%3.1f", c_idx, c_dhttype, c_relays, c_pirsensor, tempAdjust);
        EEPROM.begin(21);
        for ( int i=0; i<20; i++ ) {
          EEPROM.write(i, tmp[i]);
        }
        EEPROM.commit();
        EEPROM.end();
      }
      
    } else if ( strstr(topic,"/relay/") && strstr(topic,"/command") ) {
      // topic contains relay command
  
      int relayId = (int)(*(strstr(topic,"/command")-1) - '0');  // get the relay id (0-3)
      if ( relayId < numRelays ) {
        if ( strncmp((char*)payload,"on",length)==0 ) {
          // message is on
          digitalWrite(relays[relayId], HIGH);  // Turn the relay on
          relayState[relayId] = 1;
        } else if ( strncmp((char*)payload,"off",length)==0 ) {
          // message is off
          digitalWrite(relays[relayId], LOW);  // Turn the relay off
          relayState[relayId] = 0;
        }
  
        // publish relay state
        sprintf(outTopic, "%s/%s/relay/%i", MQTTBASE, clientId, relayId);
        sprintf(msg, relayState[relayId]?"on":"off");
        client.publish(outTopic, msg);
  
        #ifdef EEPROMSAVE
        // permanently store relay states to non-volatile memory
        byte b=0;
        for ( int i=numRelays; i>0; i-- ) {
          b = (b<<1) | (relayState[i-1]&1);
        }
        EEPROM.begin(21);
        EEPROM.write(20,b);
        EEPROM.commit();
        EEPROM.end();
        #endif
      }

    } else if ( strstr(topic,"/command/idx") ) {
      
      sprintf(c_idx,"%d",max(min((int)newPayload.toInt(),999),1));
      #if DEBUG
      Serial.print("New idx: ");
      Serial.println(c_idx);
      #endif

      // request 21 bytes from EEPROM
      sprintf(tmp, "esp%3s%-7s%1s%1s%3.1f", c_idx, c_dhttype, c_relays, c_pirsensor, tempAdjust);
      EEPROM.begin(21);
      for ( int i=0; i<20; i++ ) {
        EEPROM.write(i, tmp[i]);
      }
      EEPROM.commit();
      EEPROM.end();
      delay(120);
  
    } else if ( strstr(topic,"/command/restart") ) {

      // restart ESP
      ESP.reset();
      delay(1000);
      
    } else if ( strstr(topic,"/command/reset") ) {

      // erase 21 bytes from EEPROM
      EEPROM.begin(21);
      for ( int i=0; i<20; i++ ) {
        EEPROM.write(i, '\0');
      }
      EEPROM.commit();
      EEPROM.end();
      delay(120);  // wait for write to complete

      // clean FS
      SPIFFS.format();

      // clear WiFi data & disconnect
      WiFi.disconnect();

      // restart ESP
      ESP.reset();
      delay(1000);
      
    }
  }
}

//----------------------------------------------------
// MQTT reconnect handling
void mqtt_reconnect() {
  char tmp[64];

  // Loop until we're reconnected
  while ( !client.connected() ) {
    // Attempt to connect
    if ( strlen(username)==0? client.connect(clientId): client.connect(clientId, username, password) ) {
      // Once connected, publish an announcement...
      DynamicJsonDocument doc(256);
      doc["mac"] = WiFi.macAddress(); //.toString().c_str();
      doc["ip"] = WiFi.localIP().toString();  //.c_str();
      doc["idx"] = c_idx;
      doc["relays"] = c_relays;
      doc["dhttype"] = c_dhttype;
      doc["pirsensor"] = c_pirsensor;
      doc["tempadjust"] = ftoa(tempAdjust, tmp, 2);

      size_t n = serializeJson(doc, msg);
      sprintf(outTopic, "%s/%s/announce", MQTTBASE, clientId);
      client.publish(outTopic, msg);

      // ... and resubscribe
      sprintf(tmp, "%s/%s/#", MQTTBASE, clientId);
      client.subscribe(tmp);
      client.subscribe("domoticz/out");
    } else {
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

// reverses a string 'str' of length 'len' 
void reverse(char *str, int len) 
{ 
    int i=0, j=len-1, temp; 
    while (i<j) 
    { 
        temp = str[i]; 
        str[i] = str[j]; 
        str[j] = temp; 
        i++; j--; 
    } 
} 

// Converts a given integer x to string str.  d is the number 
// of digits required in output. If d is more than the number 
// of digits in x, then 0s are added at the beginning. 
int intToStr(int x, char *str, int d) 
{ 
    int i = 0, s = x<0;
    while (x) 
    { 
      str[i++] = (abs(x)%10) + '0'; 
      x = x/10; 
    } 
  
    // If number of digits required is more, then 
    // add 0s at the beginning 
    while (i < d)
      str[i++] = '0';

    if ( s )
      str[i++] = '-';
  
    reverse(str, i); 
    str[i] = '\0'; 
    return i; 
} 
  
// Converts a floating point number to string. 
char *ftoa(float n, char *res, int afterpoint) 
{ 
  // Extract integer part 
  int ipart = (int)n; 
  
  // Extract floating part 
  float fpart = n - (float)ipart; 
  
  // convert integer part to string 
  int i = intToStr(ipart, res, 0); 
  
  // check for display option after point 
  if (afterpoint != 0) 
  { 
    res[i] = '.';  // add dot 

    // Get the value of fraction part upto given no. 
    // of points after dot. The third parameter is needed 
    // to handle cases like 233.007 
    fpart = fpart * pow(10, afterpoint); 

    intToStr(abs((int)fpart), res + i + 1, afterpoint); 
  }
  return res;
} 

//callback notifying us of the need to save config
void saveConfigCallback ()
{
  shouldSaveConfig = true;
}
