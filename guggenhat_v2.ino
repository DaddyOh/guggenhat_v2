/*--------------------------------------------------------------------------
 GUGGENHAT: a Bluefruit LE-enabled wearable NeoPixel marquee.
 
 Requires:
 - Arduino Micro or Leonardo microcontroller board.  An Arduino Uno will
 NOT work -- Bluetooth plus the large NeoPixel array requires the extra
 512 bytes available on the Micro/Leonardo boards.
 - Adafruit Bluefruit LE nRF8001 breakout: www.adafruit.com/products/1697
 - 4 Meters 60 NeoPixel LED strip: www.adafruit.com/product/1461
 - 3xAA alkaline cells, 4xAA NiMH or a beefy (e.g. 1200 mAh) LiPo battery.
 - Late-model Android or iOS phone or tablet running nRF UART or
 Bluefruit LE Connect app.
 - BLE_UART, NeoPixel, NeoMatrix and GFX libraries for Arduino.
 
 Written by Phil Burgess / Paint Your Dragon for Adafruit Industries.
 MIT license.  All text above must be included in any redistribution.
 
 Extended  by Eric F. Palmer
 - added default message capability
 - added a few eye candy affects that only display when the hat is started
 - fixed a color setting defect that caused memory corrpution
 - added a command to the bluetooth commands to redisplay the default messages
 
 Desired Enhancements
 - more eye candy
 - ability to interrupt eye candy with a bluetooth sent message
 - ability to get battery level via bluetooth, maybe even display it on the hat
 
 --------------------------------------------------------------------------*/

#include <SPI.h>
#include <Adafruit_BLE_UART.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_GFX.h>

// NEOPIXEL STUFF ----------------------------------------------------------

// 4 meters of NeoPixel strip is coiled around a top hat; the result is
// not a perfect grid.  My large-ish 61cm circumference hat accommodates
// 37 pixels around...a 240 pixel reel isn't quite enough for 7 rows all
// around, so there's 7 rows at the front, 6 at the back; a smaller hat
// will fare better.
#define NEO_PIN     6 // Arduino pin to NeoPixel data input
#define NEO_WIDTH  37 // Hat circumference in pixels
#define NEO_HEIGHT  7 // Number of pixel rows (round up if not equal)
#define NEO_OFFSET  (((NEO_WIDTH * NEO_HEIGHT) - 240) / 2)

// Pixel strip must be coiled counterclockwise, top to bottom, due to
// custom remap function (not a regular grid).
Adafruit_NeoMatrix matrix(NEO_WIDTH, NEO_HEIGHT, NEO_PIN,
NEO_MATRIX_TOP  + NEO_MATRIX_LEFT +
NEO_MATRIX_ROWS + NEO_MATRIX_PROGRESSIVE,
NEO_GRB         + NEO_KHZ800);

char          msg[21]       = {
  0};            // BLE 20 char limit + NUL
uint8_t       msgLen        = 0;              // Empty message
int           msgX          = matrix.width(); // Start off right edge
unsigned long prevFrameTime = 0L;             // For animation timing
#define FPS 20                                // Scrolling speed

// EFP Custom stuff  ----------------------------------------------------------------
boolean displayEyeCandy = true; // only at the startup of the hat, can't send bluretooth lessage t
boolean displayDefaultMsg = true;  
boolean thisDefaultMsgAlreadyDisplayed = false;
int currentDefaultMsg = 0;  // start at the first message
unsigned long startDefaultMsgMs = 0;

#define START_DELAY_MS 5000
#define MSGCT 3
uint8_t defaultMsgCt = MSGCT;     // set this to the count of default messages
/* edit the array lengths to show the default message count */
char *default_msgs[MSGCT];        // the default messages in an array, must be 20 character long or less
uint8_t msg_len[MSGCT];           // array of default message lengths
char *msg_color[MSGCT];           // array of msg colors, each default message can have a different color
unsigned long msg_ms[MSGCT];      // the length of time in MS to display each message, hand tweak this.

// this is where you set your default messages
void setUpDefaultMsg() {
  /*default_msgs[0] = "1086"; //4
  default_msgs[1] = "BLUE CHEESE"; //11
  default_msgs[2] = "FIRST ROBOTICS"; //14
  default_msgs[3] = "RVA MAKERFEST"; //13 
  default_msgs[4] = "SCIENCE MATTERS"; //15 
  msg_len[0] = 4;
  msg_len[1] = 11;
  msg_len[2] = 14;
  msg_len[3] = 14;
  msg_len[4] = 15; 
  msg_color[0] = "#FFFFFF";
  msg_color[1] = "#0000FF";
  msg_color[2] = "#FFFF00";
  msg_color[3] = "#00FF00";
  msg_color[4] = "#FFFFFF";  
  msg_ms[0] = 3000;
  msg_ms[1] = 5000;
  msg_ms[2] = 6000;
  msg_ms[3] = 5500;
  msg_ms[4] = 6200; */
  
  default_msgs[0] = "1086"; //4
  default_msgs[1] = "BLUE CHEESE"; //11
  default_msgs[2] = "RVA MAKERFEST"; //13 
  msg_len[0] = 4;
  msg_len[1] = 11;
  msg_len[2] = 14; 
  msg_color[0] = "#FFFFFF";
  msg_color[1] = "#0000FF";
  msg_color[2] = "#00FF00";  
  msg_ms[0] = 3000;
  msg_ms[1] = 5000;
  msg_ms[2] = 5500; 
  
}

// BLUEFRUIT LE STUFF-------------------------------------------------------

// CLK, MISO, MOSI connect to hardware SPI.  Other pins are configrable:
#define ADAFRUITBLE_REQ 10
#define ADAFRUITBLE_RST  9
#define ADAFRUITBLE_RDY  2 // Must be an interrupt pin

Adafruit_BLE_UART BTLEserial = Adafruit_BLE_UART(
ADAFRUITBLE_REQ, ADAFRUITBLE_RDY, ADAFRUITBLE_RST);
aci_evt_opcode_t  prevState  = ACI_EVT_DISCONNECTED;

// STATUS LED STUFF --------------------------------------------------------

// The Arduino's onboard LED indicates BTLE status.  Fast flash = waiting
// for connection, slow flash = connected, off = disconnected.
#define LED 13                   // Onboard LED (not NeoPixel) pin
int           LEDperiod   = 0;   // Time (milliseconds) between LED toggles
boolean       LEDstate    = LOW; // LED flashing state HIGH/LOW
unsigned long prevLEDtime = 0L;  // For LED timing

// UTILITY FUNCTIONS -------------------------------------------------------

// Because the NeoPixel strip is coiled and not a uniform grid, a special
// remapping function is used for the NeoMatrix library.  Given an X and Y
// grid position, this returns the corresponding strip pixel number.
// Any off-strip pixels are automatically clipped by the NeoPixel library.
uint16_t remapXY(uint16_t x, uint16_t y) {
  return y * NEO_WIDTH + x - NEO_OFFSET;
}

// Given hexadecimal character [0-9,a-f], return decimal value (0 if invalid)
uint8_t unhex(char c) {
  return ((c >= '0') && (c <= '9')) ?      c - '0' :
  ((c >= 'a') && (c <= 'f')) ? 10 + c - 'a' :
  ((c >= 'A') && (c <= 'F')) ? 10 + c - 'A' : 0;
}

// Read from BTLE into buffer, up to maxlen chars (remainder discarded).
// Does NOT append trailing NUL.  Returns number of bytes stored.
uint8_t readStr(char dest[], uint8_t maxlen) {
  int     c;
  uint8_t len = 0;
  while((c = BTLEserial.read()) >= 0) {
    if(len < maxlen) dest[len++] = c;
  }
  return len;
}
// -------------------------------------------------------------
// EFP custom functions   eye Candy and supporting functions 
// -------------------------------------------------------------
void displayGraphics_1(unsigned long delayMs){
  unsigned int ctRow = 0;

  matrix.fillScreen(0);
  while (ctRow < NEO_HEIGHT ){
    matrix.drawPixel(18, ctRow, matrix.Color(255, 0, 0)),
    ++ctRow;
  }
  matrix.show();
  delay(delayMs);
  matrix.fillScreen(0);
  ctRow = 0;
  while (ctRow < NEO_HEIGHT ){
    matrix.drawPixel(19, ctRow, matrix.Color(0, 0, 255));
    matrix.drawPixel(17, ctRow, matrix.Color(0, 0, 255));
    ++ctRow;
  }
  matrix.show();
  delay(delayMs);

}

void displayColsAround(unsigned long delayMs){
  for (unsigned int col=0; col < NEO_WIDTH; ++col){
    matrix.fillScreen(0);
    pixelCol(col, 0, 7, 255, 255, 0);
    matrix.show();
    delay(delayMs);
  }
}

void displayColsAround_2(unsigned long delayMs){
  for (unsigned int col=0; col < NEO_WIDTH; ++col){
    matrix.fillScreen(0);
    pixelCol(col, 0, 7, 255, 255, 0);
    pixelCol(NEO_WIDTH - col, 0, 7, 0, 0, 255);
    matrix.show();
    delay(delayMs);
  }
}

void pixelCol(unsigned int col, unsigned int startRow, unsigned int endRow, byte red, byte green, byte blue){
  for (unsigned int r = startRow; r <= endRow; ++r){
    matrix.drawPixel(col, r, matrix.Color(red, green, blue));
  }
}

void displayRowsAround(unsigned long delayMs){
  for (unsigned int row=0; row < NEO_HEIGHT; ++row){
    matrix.fillScreen(0);
    pixelRow(row, 0, NEO_WIDTH-1, 255, 0, 0);
    matrix.show();
    delay(delayMs);
  }
}

void pixelRow(unsigned int row, unsigned int startCol, unsigned int endCol, byte red, byte green, byte blue){
  for (unsigned int col = startCol; col <= endCol; ++col){
    matrix.drawPixel(col, row, matrix.Color(red, green, blue));
  }  
}

// -------------------------------------------------------------
// MEAT, POTATOES ----------------------------------------------
// -------------------------------------------------------------
void setup() {
  matrix.begin();
  matrix.setRemapFunction(remapXY);
  matrix.setTextWrap(false);   // Allow scrolling off left
  matrix.setTextColor(matrix.Color(255, 0, 0)); // Red by default
  matrix.setBrightness(31);    // Batteries have limited sauce

  BTLEserial.begin();

  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW);

  // EFP
  setUpDefaultMsg();

  delay(START_DELAY_MS);
}

void loop() {


  unsigned long t = millis(); // Current elapsed time, milliseconds.
  // millis() comparisons are used rather than delay() so that animation
  // speed is consistent regardless of message length & other factors.

  BTLEserial.pollACI(); // Handle BTLE operations
  aci_evt_opcode_t state = BTLEserial.getState();

  if(state != prevState) { // BTLE state change?
    switch(state) {        // Change LED flashing to show state
    case ACI_EVT_DEVICE_STARTED: 
      LEDperiod = 1000L / 10; 
      break;
    case ACI_EVT_CONNECTED:      
      LEDperiod = 1000L / 2;  
      break;
    case ACI_EVT_DISCONNECTED:   
      LEDperiod = 0L;         
      break;
    }
    prevState   = state;
    prevLEDtime = t;
    LEDstate    = LOW; // Any state change resets LED
    digitalWrite(LED, LEDstate);
  }

  if(LEDperiod && ((t - prevLEDtime) >= LEDperiod)) { // Handle LED flash
    prevLEDtime = t;
    LEDstate    = !LEDstate;
    digitalWrite(LED, LEDstate);
  }

  // If connected, check for input from BTLE...
  if((state == ACI_EVT_CONNECTED) && BTLEserial.available()) {
    int peekChar;
    peekChar = BTLEserial.peek();
    if(peekChar == '#') { // Color commands start with '#'
      char color[7];
      switch(readStr(color, sizeof(color))) {
      case 4:                  // #RGB    4/4/4 RGB
        matrix.setTextColor(matrix.Color(
        unhex(color[1]) * 17, // Expand to 8/8/8
        unhex(color[2]) * 17,
        unhex(color[3]) * 17));
        break;
      case 5:                  // #XXXX   5/6/5 RGB
        matrix.setTextColor(
        (unhex(color[1]) << 12) +
          (unhex(color[2]) <<  8) +
          (unhex(color[3]) <<  4) +
          unhex(color[4]));
        break;
      case 7:                  // #RRGGBB 8/8/8 RGB
        matrix.setTextColor(matrix.Color(
        (unhex(color[1]) << 4) + unhex(color[2]),
        (unhex(color[3]) << 4) + unhex(color[4]),
        (unhex(color[5]) << 4) + unhex(color[6])));
        break;
      }
    } 
    else if (peekChar == ':'){
      char colon[1];
      // clear the str in bluetooth , is this necessary?
      readStr(colon, 1);
      displayDefaultMsg = true;
      thisDefaultMsgAlreadyDisplayed = false; 
      currentDefaultMsg = 0; 
    }
    else { // Not color, must be message string
      displayDefaultMsg = false;
      thisDefaultMsgAlreadyDisplayed = false;  
      msgLen      = readStr(msg, sizeof(msg)-1);
      msg[msgLen] = 0;
      msgX        = matrix.width(); // Reset scrolling
    }
  }

  if (displayEyeCandy == true) { 
    // Eye Candy at start of default messages
    // curently limited, bluetooth commands can't be captured when eye candy is displayed    
    for (int i=0; i<3; ++i) {
      displayRowsAround(30);
    }

    for (int i=0; i<3; ++i) {
      displayColsAround_2(30);
    }

    for (int i=0; i<5; ++i) {
      displayGraphics_1(80);
    } 
   
   displayEyeCandy = false; 
  }

  if (displayDefaultMsg == true) {
    if (thisDefaultMsgAlreadyDisplayed == false){
      startDefaultMsgMs = millis();
      char *color = msg_color[currentDefaultMsg];
      matrix.setTextColor(matrix.Color(
      (unhex(color[1]) << 4) + unhex(color[2]),
      (unhex(color[3]) << 4) + unhex(color[4]),
      (unhex(color[5]) << 4) + unhex(color[6])));

      strncpy(msg, default_msgs[currentDefaultMsg], msg_len[currentDefaultMsg]);
      msgLen      = msg_len[currentDefaultMsg];
      msg[msgLen] = 0;
      msgX        = matrix.width(); // Reset scrolling 

      thisDefaultMsgAlreadyDisplayed = true; 
    } 
    else {
      // time to move to the next default message?
      if ((millis() - startDefaultMsgMs) > msg_ms[currentDefaultMsg]) {
        ++currentDefaultMsg;
        thisDefaultMsgAlreadyDisplayed = false;
        if (currentDefaultMsg>(defaultMsgCt-1)){
          currentDefaultMsg = 0;
        }
      }
    } 
  }


  if((t - prevFrameTime) >= (1000L / FPS)) { // Handle scrolling
    matrix.fillScreen(0);
    matrix.setCursor(msgX, 0);
    matrix.print(msg);
    if(--msgX < (msgLen * -6)) msgX = matrix.width(); // We must repeat!
    matrix.show();
    prevFrameTime = t;
  }
}



