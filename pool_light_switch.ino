/*
Author: Kevin Williams
Description: Pentair 5G LED Pool light Controller
Features:
1) Remote on/off and color via MQTT subscription.
2) Local switch on/off
The pentair 5g light is programmed via on/off cycle within 8 seconds. Each cycle from 1-12 configures a specific scene or color.
relayPin controls dry relay for on/off and programming of lights
switchPin reads value of local switch and turns on/off lights
*/

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "credentials.h"


const long timeBetweenMessages = 1000 * 60;  //periodic status every minute
const long timeBetweenSwitchCheck = 200;  //check switch state every 200ms
const long timeBetweenProgCheck = 200;  //check prog state every

const int relayPin = 14; //d5
const int switchPin = 4; //d2

//enum per light spec.  numbers are number of pulses to program each mode.  1-7 are light shows
//8-12 are solid colors.  13 locks current color in show.  14 returns to previously saved mode
//save occurs at next power off of programming if its off for 10+ sconds
enum colors_enum {Peruvian_Paradise = 1, Super_Nova = 2, Northern_Lights = 3, Tidal_Wave = 4, 
  Patriot_Blue = 5, Desert_Skies = 6, Nova = 7, Blue = 8, Green = 9, Red = 10, White = 11, 
  Pink = 12, Color_Lock = 13, Return = 14};

static const char *colors_string[] = {"Invalid_Color", "Peruvian_Paradise", "Super_Nova",
  "Northern_Lights", "Tidal_Wave", "Patriot_Blue", "Desert_Skies", "Nova", "Blue", "Green",
  "Red", "White", "Pink", "Color_Lock", "Return"};

DynamicJsonDocument doc(200); //subscribe doc
DynamicJsonDocument doc2(200); //publish doc

WiFiClient espClient;
PubSubClient client(espClient);
long lastMsg = 0;  //flag for periodic status every minute
long lastSwitchCheck = 0;  //last time we check the status of the switch
long lastProgCheck = 0; //time last time we did a program switch cycle
int relayOn = false;
int switchState = 0;
bool programming = false;
int progVal = 0;
int currColor = -1;  //we have no way of knowing currently programmed color at startup so this is unknown

int status = WL_IDLE_STATUS;     // the starting Wifi radio's status

void setup_wifi() {
  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, pswd);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

int getQuality() {
  if (WiFi.status() != WL_CONNECTED)
    return -1;
  int dBm = WiFi.RSSI();
  if (dBm <= -100)
    return 0;
  if (dBm >= -50)
    return 100;
  return 2 * (dBm + 100);
}

bool setProgramming(bool state) { 
    if(programming != state) {
      if(state) {
        Serial.println("programming chg from off to on");
        relayOn = true;  //start with light on if not already
        setRelay();
        delay(500);
        Serial.println("done delayng, start programming now");
      } else {
        Serial.println("programming chg from on to off");
      }
          
      programming = state;
      publishStatus();
      return true;  //indicate we made a change
    } else {
      Serial.println("prog cmd ignored since we are already in that state");
    }

  return false;
}

int findColor(const char * str) {
  int n = Return+1; //1 larger than the largest supported color in the enumeration
  int i = 0;
  int maxColorNameSize = 15;
  
  for (i = 0; i < 15; ++i) {
    if (!strncmp(str, colors_string[i],maxColorNameSize)) {
      Serial.print("found ");Serial.print(str);Serial.print(" at "); Serial.println(i);
      return i;
    }
  }

  if (i >= n) {
    Serial.print("couldnt find ");Serial.println(str);
  }

  return -1;
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  DeserializationError error = deserializeJson(doc, payload);
  if (error)
    Serial.println("error deserializing json");
  else {
    Serial.println("success deserializing json");  

    //Serial.println("starting power doc vars");
    const char* power_j  = doc["power"];
    
    //Serial.println("starting power doc analysis");
    if (power_j) {
      //Serial.println("power doc valid");
      if (strncmp(power_j,"on",2) == 0) {
        Serial.println("got pwr on command");
        relayOn = true;
        setRelay();
      } else if(strncmp(power_j,"off",3) == 0) {
        Serial.println("got off command");
        relayOn = false;
        setRelay();
      }
    } else
      Serial.println("power doc invalid");

    
    Serial.println("starting prog doc vars");
    const char* prog_j  = doc["programming"][0];
    const char* progColor_j  = doc["programming"][1];
    
    Serial.println("starting prog doc analysis");
    if(prog_j && progColor_j) {
      Serial.println("prog_J and progColor_j valid");
      if (strncmp(prog_j,"on",2) == 0) {
        Serial.println("got prog on command");
      
        progVal = findColor(progColor_j);
        if(progVal > 0) {                 //-1 is not found and 0 is an invalid color
          Serial.print("programming color: "); Serial.println(colors_string[progVal]);
          setProgramming(true);
        } else
          Serial.println("invalid programming color");
      
      } else if(strncmp(prog_j,"off",3) == 0) {
        Serial.println("got prog off command");
        setProgramming(false);
      }
    } else
      Serial.println("invalid prog_j");
  }
}

String macToStr(const uint8_t* mac)
{
  String result;
  for (int i = 0; i < 6; ++i) {
    result += String(mac[i], 16);
    if (i < 5)
      result += ':';
  }
  return result;
}

String composeClientID() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  String clientId;
  clientId += "esp-";
  clientId += macToStr(mac);
  return clientId;
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");

    String clientId = composeClientID() ;
    clientId += "-";
    clientId += String(micros() & 0xff, 16); // to randomise. sort of

    // Attempt to connect
    if (client.connect(clientId.c_str(),mqttuser,mqttpw)) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      client.publish(topic, ("connected " + composeClientID()).c_str() , true );
      // ... and resubscribe
      // topic + clientID + in
      String subscription;
      subscription += topic;
      subscription += "/";
      subscription += composeClientID() ;
      subscription += "/in";
      client.subscribe(subscription.c_str() );
      Serial.print("subscribed to : ");
      Serial.println(subscription);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.print(" wifi=");
      Serial.print(WiFi.status());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void setup() {
  pinMode(switchPin, INPUT_PULLUP); //connected to float switch (float connected to ground)
  switchState = digitalRead(switchPin);
  pinMode(relayPin, OUTPUT); //connected to NO valve basin fill relay
  digitalWrite(relayPin, HIGH);  //pull output high (relay is LOW on)
  Serial.begin(115200);

  setup_wifi();
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
}

void publishStatus() {

  String payload;
  doc2["micros"] = micros();
  doc2["client"] = composeClientID();
  if(relayOn)
    doc2["power"] = "on";
  else
    doc2["power"] = "off";
  
  doc2["RSSI"] = getQuality();
  
  if(programming)
    doc2["programming"] = "on";
  else
    doc2["programming"] = "off";

  if(currColor < 1)
    doc2["color"] = "Unknown";
  else
    doc2["color"] = colors_string[currColor];
  
  serializeJson(doc2, payload);

  String pubTopic;
   pubTopic += topic ;
   pubTopic += "/";
   pubTopic += composeClientID();
   pubTopic += "/out";

  Serial.print("Publish topic: ");
  Serial.println(pubTopic);
  Serial.print("Publish message: ");
  Serial.println(payload);
  
  client.publish( (char*) pubTopic.c_str() , payload.c_str(), true );
}

void togglePower(bool currSwitchState) {
  Serial.println("swtich state changed");
  switchState = currSwitchState;  //save new switch state
  if(!programming) {
    relayOn = !relayOn; //flip relay state
    setRelay();
  }
}

void setRelay() {
  if(!programming) {
    //Serial.println("relay changing");
    if(relayOn) {
      digitalWrite(relayPin, LOW);
      publishStatus();
    } else {
      digitalWrite(relayPin, HIGH);
      publishStatus();
    } 
  } else {
    Serial.println("ignorning relay chg due to pogramming ongoing");
  }
}

//1 programming cycle
void toggleRelay(int delVal) {
  
  digitalWrite(relayPin, HIGH); //off
  delay(delVal);
  digitalWrite(relayPin, LOW); //on
  
}

void loop() {
  // confirm still connected to mqtt server
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  long now = millis();

    //send mqtt status on start and then perodically
  if (!programming && (lastMsg == 0 || (now - lastMsg > timeBetweenMessages)) ) {
    lastMsg = now;
    publishStatus();
  }

  //do programming monolithically when commanded
  if (programming && (lastProgCheck == 0 || (now - lastProgCheck > timeBetweenProgCheck))) {
    Serial.println("we are programming!");
    lastProgCheck = now;
    //do programming
    for(int i=0;i<progVal;i++) {
      Serial.print("prog cycle: "); Serial.println(i);
      //turn transformer on and off once
      toggleRelay(250);
      delay(250);
    }

    Serial.println("done programming");
    currColor = progVal;
    progVal = 0;
    programming = false; //turn off programmingif we finished with our cycles 
    publishStatus();
  }

   //check sw state perodically
  if (!programming && (lastSwitchCheck == 0 || (now - lastSwitchCheck > timeBetweenSwitchCheck)) ) {
    lastSwitchCheck = now;
    int currSwitchState = digitalRead(switchPin);
    if(currSwitchState != switchState)  
      togglePower(currSwitchState);
  }
}
