/*
 ESP8266 MQTT client for reporting DHT11 temperature

 Requires ESP8266 board, Adafruit Universal and DHT Sensor libraries

 (c) blaz@kristan-sp.si / 2019-09-10
*/

#include <FS.h>                   //this needs to be first, or it all crashes and burns...

#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     //Local WebServer used to serve the configuration portal
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <PubSubClient.h>
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include "DHT.h"
#include <OneWire.h>
#include <DallasTemperature.h>

#define DEBUG   0

#define TEMPERATURE_PRECISION 9
#define ONEWIRE 2
OneWire oneWire(ONEWIRE);         // pin GPIO2 (a 4.7K pull-up resistor is necessary)
DallasTemperature sensors(&oneWire);
DeviceAddress *thermometers[10];  // arrays to hold device addresses (up to 10)
int numThermometers = 0;

// DHT type temperature/humidity sensors
#define DHTPIN  2           // what pin we're connected to (default GPIO2=2, GPIO0=3, RX=4, TX=5)
#define DHTTYPE DHT11       // DHT11, DHT22
DHT dht(DHTPIN, DHTTYPE);   // Initialize DHT sensor
float tempAdjust = 0.0;     // temperature adjustment for wacky DHT sensors (retrieved from EEPROM)

// relay pins
const int relays[4] = {2, 3, 4, 5};     // relay pins
int relayState[4] = {0, 0, 0, 0};       // relay states
int numRelays = 0;                      // number of relays used

// Update these with values suitable for your network.
char mqtt_server[40] = "192.168.70.11";
char mqtt_port[7]    = "1883";
char username[33]    = "";
char password[33]    = "";
char MQTTBASE[16]    = "shellies"; // use shellies for Shelly MQTT Domoticz plugin integration

char c_relays[2]     = "0";
char c_dhttype[6]    = "none";
char c_onewire[2]    = "0";

// flag for saving data from WiFiManager
bool shouldSaveConfig = false;

long lastMsg = 0;
char msg[50];
char mac_address[16];
char inTopic[64];   // add MAC address in WiFi setup code
char outTopic[64];  // add MAC address in WiFi setup code
char clientId[20];  // MQTT client ID

WiFiClient espClient;
PubSubClient client(espClient);

// private functions
void reconnect();
char *ftoa(float,char*,int d=2);
void saveConfigCallback ();


//---------------------------------------------------
// MQTT callback function
void callback(char* topic, byte* payload, unsigned int length) {

  if ( strstr(topic,"/temperature/command") ) {
    // insert temperature adjustment and store it into non volatile memory
    
    char tmp[8];
    strncpy(tmp,(char*)payload,length>7? 7: length);
    tmp[length>7? 7: length] = '\0'; // terminate string
    float temp = atof(tmp);
    if ( temp < 100.0 && temp > -100.0 ) {
      tempAdjust = temp;
      ftoa(tempAdjust, tmp, 2);
      EEPROM.begin(10);
      EEPROM.put(0, tmp);
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

      // permanently store relay states to nonvolatile memory
      int b=0;
      for ( int i=numRelays; i>0; i-- ) {
        b = (b<<1) | (relayState[i-1]&1);
      }
      EEPROM.begin(10);
      EEPROM.write(9,b);
      EEPROM.commit();
      EEPROM.end();
    }
  }
}


//-----------------------------------------------------------
// main setup
void setup() {
  char str[11];
  delay(5000);

  // Initialize the BUILTIN_LED pin as an output & set initial state LED on
  pinMode(BUILTIN_LED, OUTPUT);
  digitalWrite(BUILTIN_LED, LOW);
  // change RX/TX pins into GPIO output pins (for relays)
  pinMode(3, OUTPUT);     // RX -> GPIO1
  pinMode(4, OUTPUT);     // TX -> GPIO3

  String WiFiMAC = WiFi.macAddress();
  WiFiMAC.replace(":","");
  WiFiMAC.toCharArray(mac_address, 16);
  // Create client ID from MAC address
  sprintf(clientId, "esp01s-%s", &mac_address[6]);

//----------------------------------------------------------
  //read configuration from FS json
  if ( SPIFFS.begin() ) {
    if ( SPIFFS.exists("/config.json") ) {
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
          strcpy(password, doc["base"]);

          strcpy(c_relays, doc["relays"]);
          strcpy(c_dhttype, doc["dhttype"]);
          strcpy(c_onewire, doc["onewire"]);
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
  WiFiManagerParameter custom_dhttype("dhttype", "DHT sensor GPIO2 type (DHT11,DHT22,none)", c_dhttype, 5);
  WiFiManagerParameter custom_onewire("onewire", "OneWire GPIO2 enabled (0/1)", c_onewire, 1);

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
  wifiManager.addParameter(&custom_onewire);

  // set minimu quality of signal so it ignores AP's under that quality
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
  strcpy(c_onewire, custom_onewire.getValue());

  numRelays = max(min(atoi(c_relays),4),0);
  c_relays[0] = '0' + numRelays;

  if ( strcmp(c_dhttype,"DHT11")==0 ) {
    dht = new DHT(DHTPIN, DHT11);
  } else if ( strcmp(c_dhttype,"DHT21")==0 ) {
    dht = new DHT(DHTPIN, DHT21);
  } else if ( strcmp(c_dhttype,"DHT22")==0 ) {
    dht = new DHT(DHTPIN, DHT22);
  } else {
    dht = NULL;
    strcpy(c_dhttype,"none");
  }

  //save the custom parameters to FS
  if ( shouldSaveConfig ) {
    DynamicJsonDocument doc(1024);
    doc["mqtt_server"] = mqtt_server;
    doc["mqtt_port"] = mqtt_port;
    doc["username"] = username;
    doc["password"] = password;
    doc["base"] = MQTTBASE;

    doc["relays"] = c_relays;
    doc["dhttype"] = c_dhttype;
    doc["onewire"] = c_onewire;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      // failed to open config file for writing
    } else {
      serializeJson(doc, configFile);
      configFile.close();
    }

    // clear 10 bytes from EEPROM
    EEPROM.begin(10);
    EEPROM.put(10, "0.0\0\0\0\0\0\0\0");
    EEPROM.commit();
    EEPROM.end();
    delay(120);
  }
//----------------------------------------------------------

  // if connected set state LED off
  digitalWrite(BUILTIN_LED, HIGH);

  // request 10 bytes from EEPROM
  EEPROM.begin(10);

  if ( numRelays > 0 ) {
    // initialize relay pins
    // 10th byte contain 8 relays (bits) worth of initial states
    int initRelays = EEPROM.read(9);
    // relay states are stored in bits
    for ( int i=0; i<numRelays; i++ ) {
      pinMode(relays[i], OUTPUT);
      relayState[i] = (initRelays >> 1) & 1;
      digitalWrite(relays[i], relayState[i]? HIGH: LOW);
    }
  }
  
  if ( dht ) {
    // initialize DHT sensor
    dht->begin();
    char temp[8];
    EEPROM.get(0,temp);
    tempAdjust = atof(temp);  // temperature adjustment (set via MQTT message)
  }

  if ( atoi(c_onewire) ) {
    // OneWire temperature sensors
    byte *tmpAddr, addr[8];
  
    // Start up the library
    sensors.begin();
  
    // locate devices on the bus
    numThermometers = sensors.getDeviceCount();
    oneWire.reset_search();
    for ( int i=0; !oneWire.search(addr) && i<10; i++ ) {
      if ( OneWire::crc8(addr, 7) != addr[7] ) {
        Serial.println("CRC error.");
        thermometers[i] = NULL;
      } else {
        // store up to 16 sensor addresses
        tmpAddr = (byte*)malloc(8); // there is no need to free() this memory
        memcpy(tmpAddr,addr,8);
        sensors.setResolution(tmpAddr, TEMPERATURE_PRECISION);
        thermometers[i] = (DeviceAddress *)tmpAddr;
      }
    }
  }

  // done reading EEPROM
  EEPROM.end();

  // initialize MQTT connection & provide callback function
  client.setServer(mqtt_server, atoi(mqtt_port));
  client.setCallback(callback);
}

//-----------------------------------------------------------
// main loop
void loop() {

  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // publish status every 60s
  long now = millis();
  if (now - lastMsg > 60000) {
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
        Serial.println("Failed to read from DHT sensor!");
        sprintf(outTopic, "%s/%s/sensor/temperature", MQTTBASE, clientId);
        sprintf(msg, "err");
      } else {
        // may use Shelly MQTT API (shellies/shellyht-MAC/sensor/temperature)
        sprintf(outTopic, "%s/%s/sensor/temperature", MQTTBASE, clientId);
        sprintf(msg, "%.2f", t + tempAdjust);
        client.publish(outTopic, msg);
        sprintf(outTopic, "%s/%s/sensor/temperature_f", MQTTBASE, clientId);
        sprintf(msg, "%.2f", f + tempAdjust*(float)(9/5));
        client.publish(outTopic, msg);
        sprintf(outTopic, "%s/%s/sensor/humidity", MQTTBASE, clientId);
        sprintf(msg, "%.2f", h);
        client.publish(outTopic, msg);
        if ( round(tempAdjust*10) == 0.0 ) {
          sprintf(outTopic, "%s/%s/temp_adjust", MQTTBASE, clientId);
          sprintf(msg, "%.2f", tempAdjust);
          client.publish(outTopic, msg);
        }
      }
    }

    if ( atoi(c_onewire) ) {
      // may use non-standard Shelly MQTT API (shellies/shellyhtx-MAC/temperature/i)
      for ( int i=0; i<numThermometers; i++ ) {
        if ( *thermometers[i] ) {
          // not a faulty thermometer
          float tempC = sensors.getTempC(*thermometers[i]);
          if ( numThermometers == 1 )
            sprintf(outTopic, "%s/%s/temperature", MQTTBASE, clientId);
          else
            sprintf(outTopic, "%s/%s/temperature/%i", MQTTBASE, clientId, i);
          sprintf(msg, "%.2f", tempC);
          client.publish(outTopic, msg);
        }
      }
    }

    // may use Shelly MQTT API (shellies/shelly4pro-MAC/relay/i)
    for ( int i=0; i<numRelays; i++ ) {
      sprintf(outTopic, "%s/%s/relay/%i", MQTTBASE, clientId, i);
      sprintf(msg, relayState[i]?"on":"off");
      client.publish(outTopic, msg);
    }

  // end 60s reporting
  }

  // wait 2s until next update (also DHT limitation)
  delay(2000);
}

//----------------------------------------------------
// MQTT reconnect handling
void reconnect() {
  // Loop until we're reconnected
  while ( !client.connected() ) {
    // Attempt to connect
    if ( strlen(username)==0? client.connect(clientId): client.connect(clientId, username, password) ) {
      // Once connected, publish an announcement...
      sprintf(outTopic, "%s/%s/announce", MQTTBASE, clientId);
      sprintf(msg, "Hello there. My IP is %s", WiFi.localIP().toString().c_str());
      client.publish(outTopic, msg);
      // ... and resubscribe
      sprintf(inTopic, "%s/%s/#", MQTTBASE, clientId);
      client.subscribe(inTopic);
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
