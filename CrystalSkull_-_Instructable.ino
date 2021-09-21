/*
  Orbis Terrae (c) 2021 - Smoking Crystal Skull

  The code controls the ultrasonic mist maker, the fan and the pulsing led strip inside the crystal skull.

  These elements can be controlled or watched thru different interfaces, including different level of intensity.
    Physical Buttons on the brain control box (on/off)
    internal web server to control the intensity (percentage)

  Several other outputs have been integrated for logging or debug purpose, entirely optional:
    OLED screen, hidden in the brain box, for run time debugging.
    ESP8266 serial port, to debug on the host during the development.
    internal EEPROM to save settings upon restart.

  The main loop handle these interfaces every second.
  The frequency may be increased for a better reactivity, but will have an impact on the level of power used.

  The brain box rocker switches are handled under interruption for better reactivity.

  Simply update the code with the #TOBEUPDATED comment to adapt it to your needs.
*/

// Import required libraries
#include <ESP8266WiFi.h>
#include <aREST.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

#define LOCATION  "BedRoom" // #TOBEUPDATED 
#define FWREV     "v0.1.5"
#define FWNAME    "CrystalSkull IoT"
#define IOT_ID    "6"

#define SWITCH_BUTTON_1  9 // SSD2 - no PWM output but interrupt on input
#define SWITCH_BUTTON_2 15 // D8 - Boot fails if pulled HIGH
#define SWITCH_BUTTON_3 10 // SSD3 - no PWM but interrupt
#define FAN_INB         12 // D6 - FAN_INA is set to LOW directly without a pin, as we don't go in reverse - SSD2 doesn't include a PWM
#define LED_STRIP       13 // D7
#define FOG_PIN         14 // D5

#define EE_LED   1    // EEPROM MAP -> LED status
#define EE_FAN   2    // EEPROM MAP -> Fan status
#define EE_FOG   3    // EEPROM MAP -> Fog status
#define EE_PLED  4    // EEPROM MAP -> previous LED Status
#define EE_PFAN  5    // EEPROM MAP -> previous Fan status
#define EE_PFOG  6    // EEPROM MAP -> previous Fog status

// define if there is an OLED display connected
// OLED SLK -> ESP8266 D1/GPIO5/SCL ; OLED SDA -> ESP8266 D2/GPIO4/SDA
#define OLEDCONNECTED 1
Adafruit_SSD1306 display = Adafruit_SSD1306(128, 64, &Wire);

#define INTERVAL_SERVER   1000      //interval to update the web server in ms -> 1s
#define INTERVAL_RESET    60*60*24  //reset once a day...
#define LEDSTATUS 1                 //ON
#define MAX_OUTPUT 255  // on ESP8266 V3 CH340, max output is 255         #TOBEUPDATED 
//#define MAX_OUTPUT 1023  // on ESP8266 V2 CP2102, max output is 1023    #TOBEUPDATED 
// see instructions

// WiFi parameters
const char* ssid = "xxxxxxxxx";     // #TOBEUPDATED
const char* password = "xxxxxxxxx"; // #TOBEUPDATED
// Create aREST instance
aREST rest = aREST();
// The port to listen for incoming TCP connections
#define LISTEN_PORT           80 // this is where the interval webserver display variables
// Create an instance of the server
WiFiServer server(LISTEN_PORT);

// Variables to be exposed to the API on the internal web server
int fogStatus                = 100; // where the current value of the fog intensity % is stored
int fanStatus                = 100; // current value of the fan speed in %
int ledStatus                = 100; // current value of the LED strip in %
int OLEDStatus               = OLEDCONNECTED; // do we have an OLED display connected?
// other global variables
int previousLedIntensity     = 0; // value of the previous ledStatus% before it was switched off
int previousFogIntensity     = 0; // value of the previous fogStatus% before it was switched off
int previousFanIntensity     = 0; // value of the previous fanStatus% before it was switched off
int EEPROMchanged            = 0; // eeprom commit is done once per loop at maximum to prevent crash
volatile int isInterrupted   = 0;
int interTime                = 0;
long runningTime             = 0;

/*  -------------------------------------------------------------------------------*/
/*  -- fogControl -----------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
void fogControl(int percent) {
  fogStatus = percent;
  if (fogStatus > 0) { // save the previous state in EEPROM
    previousFogIntensity = fogStatus;
  }
  EEPROM.write(EE_FOG, fogStatus); // only can store a value between 0 to 255, not the full scale - so we store the % only
  EEPROM.write(EE_PFOG, previousFogIntensity);
  Serial.printf("Set fog intensity: %d%%\n", fogStatus);
  EEPROMchanged = 1; // commit will be done in main loop - prevent crash under interrupts
  analogWrite(FOG_PIN, fogStatus * MAX_OUTPUT / 100); // convert the percentage of intensity into a full scale value
}

/*  -------------------------------------------------------------------------------*/
/*  -- fogOn ----------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int fogOn (String command) {
  // Get fan intensity from command - this is a percentage between 1 and 100%
  //  http://192.168.X.X/fanOn?param=75
  int percent = command.toInt();
  if (percent == 0 || percent > 100) { // 0 means no argument was provided to the web rest server
    percent = 100; // so we default to 100%
  }
  fogControl(percent);
  return percent;
}

/*  -------------------------------------------------------------------------------*/
/*  -- fogOff ---------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int fogOff (String command) {
  fogStatus = 0;
  fogControl(fogStatus);
  return fogStatus;
}

/*  -------------------------------------------------------------------------------*/
/*  -- fanControl -----------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
void fanControl(int percent) {
  fanStatus = percent;
  if (fanStatus > 0) {
    previousFanIntensity = fanStatus;
  }
  EEPROM.write(EE_FAN, fanStatus);
  EEPROM.write(EE_PFAN, previousFanIntensity);
  Serial.printf("Set fan intensity: %d%%\n", fanStatus);
  EEPROMchanged = 1; // commit will be done in main loop - prevent crash under interrupts
  analogWrite(FAN_INB, fanStatus * MAX_OUTPUT / 100);
}

/*  -------------------------------------------------------------------------------*/
/*  -- fanOn ----------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int fanOn (String command) {
  // Get fan intensity from command
  //  http://192.168.X.X/fanOn?param=75

  int percent = command.toInt();
  if (percent == 0 || percent > 100) {
    percent = 100;
  }
  fanControl(percent);
  return percent;
}

/*  -------------------------------------------------------------------------------*/
/*  -- fanOff ---------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int fanOff (String command) {
  fanStatus = 0;
  fanControl(fanStatus);
  return fanStatus;
}

/*  -------------------------------------------------------------------------------*/
/*  -- ledOn ----------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int ledOn(String command) {

  // Get led strip intensity from command
  //  http://192.168.XX.XX/ledOn?param=75
  int percent = command.toInt();
  if (percent == 0 || percent > 100) {
    percent = 100;
  }
  int intense = percent * MAX_OUTPUT / 100; // parameter is a percentage, as we can only store up to 255 in the EEPROM

  ledStatus = percent;
  if (ledStatus > 0) {
    previousLedIntensity = ledStatus;
  }
  EEPROM.write(EE_LED, ledStatus);
  EEPROM.write(EE_PLED, previousLedIntensity);
  EEPROMchanged = 1;
  Serial.printf("Set LED intensity: %d%%\n", ledStatus);
  for (int i = 0 ; i < intense ; i++) { // ramp up intensity
    analogWrite(LED_STRIP, i);
    delay(1);
  }
  return ledStatus;
}

/*  -------------------------------------------------------------------------------*/
/*  -- ledOff ---------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int ledOff(String command) {
  ledStatus = 0;
  EEPROM.write(EE_LED, ledStatus);
  EEPROM.write(EE_PLED, previousLedIntensity);
  EEPROMchanged = 1;
  for (int i = previousLedIntensity ; i >= 0 ; i--) {  // ramp down intensity
    analogWrite(LED_STRIP, i);
    delay(1);
  }
  return ledStatus;
}

/*  -------------------------------------------------------------------------------*/
/*  -- homebridgeStatus -----------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int homebridgeStatus(String command) {
  // direct control from HomeBridge -> return status
  if (ledStatus > 0 || fanStatus > 0 || fogStatus > 0) {
    // at least one activity on one of the peripheral -> the crystal skull is running
    return 1;
  }
  else {
    // no activity, everything is off
    return 0;
  }
}

/*  -------------------------------------------------------------------------------*/
/*  -- homebridgeOn ---------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int homebridgeOn(String command) {
  // direct control from HomeBridge -> turn On all peripherals based on previously saved intensity
  if (ledStatus >= 1) {
    ledOn(String(ledStatus));
    //    Serial.println("engaging leds");
  }
  else {
    ledOn(String(previousLedIntensity));
  }
  if (fanStatus >= 1) {
    fanOn(String(fanStatus));
    //    Serial.println("engaging fans");
  }
  else {
    fanOn(String(previousFanIntensity));
  }
  if (fogStatus >= 1) {
    fogOn(String(fogStatus));
    //    Serial.println("engaging fog");
  }
  else {
    fogOn(String(previousFogIntensity));
  }
  return 1;
}

/*  -------------------------------------------------------------------------------*/
/*  -- homebridgeOff --------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int homebridgeOff(String command) {
  // direct control from HomeBridge -> turn Off all peripherals
  ledOff("");
  fanOff("");
  fogOff("");
  return 1;
}

/*  -------------------------------------------------------------------------------*/
/*  -- display_header -------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int display_header() {
  // handle the display on the OLED screen
  if (OLEDStatus == 1) {
    display.clearDisplay();
    display.display();

    // text display tests
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("Orbis Terrae - IoT");
    display.println(IOT_ID);
    display.println(FWNAME);
    display.print("IP: ");
    display.println(WiFi.localIP());
    display.print("running: ");
    display.print(runningTime);
    display.print("s\n");
    display.println();

    display.setTextSize(2);
    display.setCursor(0, 34);
    display.print("Fog:  ");
    display.print(fogStatus);
    display.print("\n");
    display.print("Fan:  ");
    display.print(fanStatus);
    display.print("\n");

    display.setCursor(0, 0);
    display.display(); // actually display all of the above
  }
  return OLEDStatus;
}

/*  -------------------------------------------------------------------------------*/
/*  -- heartBeat ------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int heartBeat() {
  if (ledStatus > 0) { // only activate if LED strip is on
    int i;
    // simple heart beat scheme, pulsing gently from 100% to 50% and back
    for (i = ledStatus ; i >= (ledStatus / 2) ; i--) {
      analogWrite(LED_STRIP, i * MAX_OUTPUT / 100);
      delay(25);
    }
    for (i = (ledStatus / 2) ; i <= ledStatus ; i++) {
      analogWrite(LED_STRIP, i * MAX_OUTPUT / 100);
      delay(25);
    }
  }
  return ledStatus;
}

/*  -------------------------------------------------------------------------------*/
/*  -- dumpEEPROM -----------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int dumpEEPROM(String command) {
  // dump EEPROM content on the serial port - debug only
  int lled, llog, lfan, lfog, lpled, lpfan, lpfog;
  lled     = EEPROM.read(EE_LED); // read the last led status from EEPROM
  Serial.printf("current led strip status stored in EEPROM: %d\n", lled);
  lfan      = EEPROM.read(EE_FAN); // read the last fan status from EEPROM
  Serial.printf("current fan intensity stored in EEPROM: %d\n", lfan);
  lfog      = EEPROM.read(EE_FOG); // read the last fog status from EEPROM
  Serial.printf("current fog intensity stored in EEPROM: %d\n", lfog);
  lpled     = EEPROM.read(EE_PLED); // read the last led status from EEPROM
  Serial.printf("previous led strip status stored in EEPROM: %d\n", lpled);
  lpfan      = EEPROM.read(EE_PFAN); // read the last fan status from EEPROM
  Serial.printf("previous fan intensity stored in EEPROM: %d\n", lpfan);
  lpfog      = EEPROM.read(EE_PFOG); // read the last fog status from EEPROM
  Serial.printf("previous fog intensity stored in EEPROM: %d\n", lpfog);
  return 1;
}

/*  -------------------------------------------------------------------------------*/
/*  -- toggleLed ------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
IRAM_ATTR void toggleLed() {
  // function called when interupt detected on pin BUTTON_SWITCH
  if (isInterrupted == 0) { // prevent the switch to create multiple event - just wait for the flag to be cleared in the main loop
    //Serial.println("BUTTON 1 ACTIVATED!!!"); // may create crash under interrupt
    isInterrupted = 1;
  }
}
/*  -------------------------------------------------------------------------------*/
/*  -- toggleFog ------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
IRAM_ATTR void toggleFog() {
  // function called when interupt detected on pin BUTTON_SWITCH
  if (isInterrupted == 0) {
    // Serial.println("BUTTON 2 ACTIVATED!!!");
    isInterrupted = 2;
  }
}
/*  -------------------------------------------------------------------------------*/
/*  -- toggleFan ------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
IRAM_ATTR void toggleFan() {
  // function called when interupt detected on pin BUTTON_SWITCH
  if (isInterrupted == 0) {
    // Serial.println("BUTTON 3 ACTIVATED!!!");
    isInterrupted = 3;
  }
}

/*  -------------------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
/*  -- setup ----------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
void setup(void)
{
  // Start Serial
  Serial.begin(115200);
  EEPROM.begin(512);

  Serial.println();
  Serial.printf("\nOrbis Terrae (c) 2021 %s IoT %s\n", FWNAME, IOT_ID );

  String thisBoard = ARDUINO_BOARD;
  Serial.println(thisBoard);

  // read the last current and previous status / intensity % from EEPROM
  ledStatus                 = EEPROM.read(EE_LED);
  fanStatus                 = EEPROM.read(EE_FAN);
  fogStatus                 = EEPROM.read(EE_FOG);
  previousLedIntensity      = EEPROM.read(EE_PLED);
  previousFanIntensity      = EEPROM.read(EE_PFAN);
  previousFogIntensity      = EEPROM.read(EE_PFOG);

  // Init variables and expose them to REST API on the internal web server
  rest.variable("ledStatus", &ledStatus);
  rest.variable("fanStatus", &fanStatus);
  rest.variable("fogStatus", &fogStatus);
  rest.variable("OLEDStatus", &OLEDStatus);
  rest.variable("runTime", &runningTime);

  rest.function("ledOn", ledOn); // turn led strip on with specified intensity (in %)
  rest.function("ledOff", ledOff); // turn led strip off
  rest.function("fanOn", fanOn); // turn fan on with specified intensity (in %)
  rest.function("fanOff", fanOff); // turn fan off
  rest.function("fogOn", fogOn); // turn the ultrasonic humidifier with specified intensity (in %)
  rest.function("fogOff", fogOff); // turn fog off
  rest.function("dumpEEPROM", dumpEEPROM); // display the content of EEPROM to the serial port (debug)
  rest.function("homebridgeStatus", homebridgeStatus); // answer to the homebridge status query
  rest.function("homebridgeOn", homebridgeOn); // turn on all peripherals (fan, fog, led)
  rest.function("homebridgeOff", homebridgeOff); // turn off all peripherals (fan, fog, led)

  // Give name & ID to the device (ID should be 6 characters long) for the internal webserver
  rest.set_id(IOT_ID);
  rest.set_name("Orbis Terrae IoT");

  if (OLEDStatus == 1) {
    bool noerror = false;
    noerror = display.begin(SSD1306_SWITCHCAPVCC, 0x3C); // Address 0x3C for 128x32
    if (noerror == true) {
      Serial.println("OLED Connected");
      display.display();
    }
    else {
      Serial.println("no OLED display connected");
      OLEDStatus = 0;
    }
  }

  // Connect to WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  // Start the server
  server.begin();
  Serial.println("Server started");

  // Print the IP address
  Serial.println(WiFi.localIP());

  pinMode(SWITCH_BUTTON_1, INPUT);
  pinMode(SWITCH_BUTTON_2, INPUT);
  pinMode(SWITCH_BUTTON_3, INPUT);
  attachInterrupt(digitalPinToInterrupt(SWITCH_BUTTON_1), toggleLed, FALLING);
  attachInterrupt(digitalPinToInterrupt(SWITCH_BUTTON_2), toggleFog, FALLING);
  attachInterrupt(digitalPinToInterrupt(SWITCH_BUTTON_3), toggleFan, FALLING);

  pinMode(FOG_PIN, OUTPUT);
  pinMode(FAN_INB, OUTPUT);
  pinMode(LED_STRIP, OUTPUT);

  display_header();
  if (ledStatus >= 1) {
    ledOn(String(ledStatus));
    //    Serial.println("engaging leds");
  }
  if (fanStatus >= 1) {
    fanOn(String(fanStatus));
    //    Serial.println("engaging fans");
  }
  if (fogStatus >= 1) {
    fogOn(String(fogStatus));
    //    Serial.println("engaging fog");
  }

  Serial.println("#########################");
}

/*  -------------------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
/*  -- loop -----------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
void loop() {

  // handle the 3x on/off buttons to control the 3 key values (led strip, fog, fan)
  if (isInterrupted >= 1) { // a button has been activated under interrupt? 
    if (isInterrupted == 1) {
      Serial.println("BUTTON 1 LED ACTIVATED!!!");
      isInterrupted = 0; // clear the flag
      if (ledStatus >= 1) { // was On just before - maybe at 20%
        previousLedIntensity = ledStatus; // so saving the previous value (20%) to go back to it on the next button activation
        ledOff("");
      }
      else { // was Off just before (ie 0%)
        ledOn(String(previousLedIntensity)); // going back to the previously saved intensity (20%) since the current is 0%;
      }
    }
    else {
      if (isInterrupted == 2) {
        Serial.println("BUTTON 2 FOG ACTIVATED!!!");
        isInterrupted = 0;
        if (fogStatus >= 1) {
          previousFogIntensity = fogStatus;
          fogOff("");
        }
        else {
          fogOn(String(previousFogIntensity));
        }
      }
      else {
        if (isInterrupted == 3) {
          Serial.println("BUTTON 3 FAN ACTIVATED!!!");
          isInterrupted = 0;
          if (fanStatus >= 1) {
            previousFanIntensity = fanStatus;
            fanOff("");
          }
          else {
            fanOn(String(previousFanIntensity));
          }
        }
      }
    }
    isInterrupted = 0;
  }

  // handle the storage of key values into EEPROM
  if (EEPROMchanged == 1) { // means there has been a change in the key values of the fan or fog, via interrupt or web interface
    if (OLEDStatus == 1) { // so we update the OLED display on top of commiting the EEPROM
      display_header();
    }
    EEPROMchanged = 0; // clear the flag
    if (EEPROM.commit()) {
      Serial.println("Saving to EEPROM...");
    }
    else {
      Serial.println("ERROR! EEPROM commit failed");
    }
  }
  else {
    if (OLEDStatus == 1) {
      display_header();
    }
  }

  // sleep a bit...
  delay(INTERVAL_SERVER);
  interTime += INTERVAL_SERVER;
  runningTime += INTERVAL_SERVER / 1000;

  // add some robustness
  if (runningTime > (INTERVAL_RESET)) {   // reseting after 1 day of runntime
    Serial.printf("Orbis Terrae IoT - running time: %ds - Rebooting\n", runningTime);
    runningTime = 0;
    interTime = 0;
    ESP.restart();
  }

  // Handle REST calls
  WiFiClient client = server.available();
  if (client) {
    rest.handle(client);
  }

  // pulse the led strip
  heartBeat();
}
