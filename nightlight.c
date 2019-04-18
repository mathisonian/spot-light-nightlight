

/**
 * This code is a combination of my own work and code adapted from demos online.
 * The FFT code is taken from the Piccolo music visualizer demo:
 *  - https://learn.adafruit.com/piccolo/code
 * with adaptations made to enable reading of other serial ports based on this
 * arduino forum post:
 *  https://forum.arduino.cc/index.php?topic=295773.0
 *
 * The FFFT library must be installed (file can be found in the Piccolo project). 
 */


#include <ffft.h>
#include <math.h>

#define ADC_CHANNEL 0


int16_t       capture[FFT_N];    // Audio capture buffer
complex_t     bfly_buff[FFT_N];  // FFT "butterfly" buffer
uint16_t      spectrum[FFT_N/2]; // Spectrum output buffer
volatile byte samplePos = 0;     // Buffer position counter

byte
  dotCount = 0, // Frame counter for delaying dot-falling speed
  colCount = 0; // Frame counter for storing past column data
int
  col[8][10],   // Column levels for the prior 10 frames
  minLvlAvg[8], // For dynamic adjustment of low & high ends of graph,
  maxLvlAvg[8], // pseudo rolling averages for the prior few frames.
  colDiv[8];    // Used when filtering FFT output to 8 columns

/*
These tables were arrived at through testing, modeling and trial and error,
exposing the unit to assorted music and sounds.  But there's no One Perfect
EQ Setting to Rule Them All, and the graph may respond better to some
inputs than others.  The software works at making the graph interesting,
but some columns will always be less lively than others, especially
comparing live speech against ambient music of varying genres.
*/
static const uint8_t PROGMEM
  // This is low-level noise that's subtracted from each FFT output column:
  noise[64]={ 8,8,8,7,5,4,4,4,3,4,4,3,2,3,3,4,
              2,1,2,1,3,2,3,2,1,2,3,1,2,3,4,4,
              3,2,2,2,2,2,2,1,3,2,2,2,2,2,2,2,
              2,2,2,2,2,2,2,2,2,2,2,2,2,3,3,4 },
  // These are scaling quotients for each FFT output column, sort of a
  // graphic EQ in reverse.  Most music is pretty heavy at the bass end.
  eq[64]={
    255, 175,218,225,220,198,147, 99, 68, 47, 33, 22, 14,  8,  4,  2,
      0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },
  // When filtering down to 8 columns, these tables contain indexes
  // and weightings of the FFT spectrum output values to use.  Not all
  // buckets are used -- the bottom-most and several at the top are
  // either noisy or out of range or generally not good for a graph.
  col0data[] = {  2,  1,  // # of spectrum bins to merge, index of first
    111,   8 },           // Weights for each bin
  col1data[] = {  4,  1,  // 4 bins, starting at index 1
     19, 186,  38,   2 }, // Weights for 4 bins.  Got it now?
  col2data[] = {  5,  2,
     11, 156, 118,  16,   1 },
  col3data[] = {  8,  3,
      5,  55, 165, 164,  71,  18,   4,   1 },
  col4data[] = { 11,  5,
      3,  24,  89, 169, 178, 118,  54,  20,   6,   2,   1 },
  col5data[] = { 17,  7,
      2,   9,  29,  70, 125, 172, 185, 162, 118, 74,
     41,  21,  10,   5,   2,   1,   1 },
  col6data[] = { 25, 11,
      1,   4,  11,  25,  49,  83, 121, 156, 180, 185,
    174, 149, 118,  87,  60,  40,  25,  16,  10,   6,
      4,   2,   1,   1,   1 },
  col7data[] = { 37, 16,
      1,   2,   5,  10,  18,  30,  46,  67,  92, 118,
    143, 164, 179, 185, 184, 174, 158, 139, 118,  97,
     77,  60,  45,  34,  25,  18,  13,   9,   7,   5,
      3,   2,   2,   1,   1,   1,   1 },
  // And then this points to the start of the data for each of the columns:
  * const colData[]  = {
    col0data, col1data, col2data, col3data,
    col4data, col5data, col6data, col7data };


const int SWITCH_PIN = 2;

boolean ledOn = false;
int lastConnectionRead = HIGH;

unsigned long lastToggledSwitch = 0;

int redPin = 9;
int greenPin = 10;
int bluePin = 11;
int redPin2 = 3;
int greenPin2 = 5;
int bluePin2 = 6;

volatile bool isPetting = false;
volatile bool wasPetting = false;

//const unsigned int LIGHT_SENSOR_PIN = A5;
//const unsigned int SOUND_PIN = A0;
int lightSensorValue = 0;

#define LIGHT_SENSOR_PIN  5
#define SOUND_PIN 0

volatile byte current_pin = SOUND_PIN;
volatile int ready_pin = -1;



/*
 * Setup ADC free-run mode
 */
int oldADMUX = ADMUX, oldADCSRA = ADCSRA, oldADCSRB = ADCSRB, oldDIDR0 = DIDR0, oldTIMSK0 = TIMSK0;
void setupFreeRun() {

  // Init ADC free-run mode; f = ( 16MHz/prescaler ) / 13 cycles/conversion 
  ADMUX  = ADC_CHANNEL; // Channel sel, right-adj, use AREF pin
  ADCSRA = _BV(ADEN)  | // ADC enable
           _BV(ADSC)  | // ADC start
           _BV(ADATE) | // Auto trigger
           _BV(ADIE)  | // Interrupt enable
           _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0); // 128:1 / 13 = 9615 Hz
  ADCSRB = 0;                // Free run mode, no high MUX bit
  DIDR0  = 1 << SOUND_PIN | 1 << LIGHT_SENSOR_PIN; // Turn off digital input for ADC pin
  TIMSK0 = 0;                // Timer0 off

  interrupts(); // Enable interrupts
}

void setup() {
  uint8_t i, j, nBins, binNum, *data;
  
  pinMode(SWITCH_PIN, INPUT_PULLUP);

  Serial.begin(9600);
  memset(col , 0, sizeof(col));

  for(i=0; i<8; i++) {
    minLvlAvg[i] = 0;
    maxLvlAvg[i] = 512;
    data         = (uint8_t *)pgm_read_word(&colData[i]);
    nBins        = pgm_read_byte(&data[0]) + 2;
    binNum       = pgm_read_byte(&data[1]);
    for(colDiv[i]=0, j=2; j<nBins; j++)
      colDiv[i] += pgm_read_byte(&data[j]);
  }

  setupFreeRun();
  //  pinMode(A5, INPUT);
}


int loopCount = 0;
int r = 0, g = 255, b = 0;
int curR = 0, curG = 255, curB = 0;

void loop() {
  uint8_t  i, x, L, *data, nBins, binNum, weighting, c;
  uint16_t minLvl, maxLvl;
  int      level, y, sum;
  
  while(ADCSRA & _BV(ADIE)); // Wait for audio sampling to finish

//  Serial.println("hi");
  int sensorVal = digitalRead(SWITCH_PIN);
  
  loopCount++;

  // overflow?
  if (loopCount > 2000) {
    loopCount = 1000;
  }

  if (sensorVal == LOW && (loopCount > 100)) {

    ledOn = !ledOn;
    if (ledOn) {
//      Serial.println("LED ON");
      // LED ON
      isPetting = false;
      wasPetting = false;
      r = 0;
      g = 255;
      b = 0;
      curR = 0;
      curG = 255;
      curB = 0;
    } else {
//      Serial.println("LED OFF");
    }
    loopCount = 0;
  }


  lastConnectionRead = sensorVal;

  

  fft_input(capture, bfly_buff);   // Samples -> complex #s
  samplePos = 0;                   // Reset sample counter
  ADCSRA |= _BV(ADIE);             // Resume sampling interrupt
  fft_execute(bfly_buff);          // Process complex data
  fft_output(bfly_buff, spectrum); // Complex -> spectrum

  // Remove noise and apply EQ levels
  for(x=0; x<FFT_N/2; x++) {
    L = pgm_read_byte(&noise[x]);
    spectrum[x] = (spectrum[x] <= L) ? 0 :
      (((spectrum[x] - L) * (256L - pgm_read_byte(&eq[x]))) >> 8);
  }


  // Downsample spectrum output to 8 columns:
  int maxIdx = -1;
  int maxVal = -1;
  for(x=0; x<8; x++) {
    data   = (uint8_t *)pgm_read_word(&colData[x]);
    nBins  = pgm_read_byte(&data[0]) + 2;
    binNum = pgm_read_byte(&data[1]);
    for(sum=0, i=2; i<nBins; i++)
      sum += spectrum[binNum++] * pgm_read_byte(&data[i]); // Weighted
    col[x][colCount] = sum / colDiv[x];                    // Average
    minLvl = maxLvl = col[x][0];
    for(i=1; i<10; i++) { // Get range of prior 10 frames
      if(col[x][i] < minLvl)      minLvl = col[x][i];
      else if(col[x][i] > maxLvl) maxLvl = col[x][i];
    }
    // minLvl and maxLvl indicate the extents of the FFT output, used
    // for vertically scaling the output graph (so it looks interesting
    // regardless of volume level).  If they're too close together though
    // (e.g. at very low volume levels) the graph becomes super coarse
    // and 'jumpy'...so keep some minimum distance between them (this
    // also lets the graph go to zero when no sound is playing):
    if((maxLvl - minLvl) < 8) maxLvl = minLvl + 8;
    minLvlAvg[x] = (minLvlAvg[x] * 7 + minLvl) >> 3; // Dampen min/max levels
    maxLvlAvg[x] = (maxLvlAvg[x] * 7 + maxLvl) >> 3; // (fake rolling average)

    // Second fixed-point scale based on dynamic min/max levels:
    level = 10L * (col[x][colCount] - minLvlAvg[x]) /
      (long)(maxLvlAvg[x] - minLvlAvg[x]);

    // Clip output and convert to byte:
    if(level < 0L)      c = 0;
    else if(level > 10) c = 10; // Allow dot to go a couple pixels off top
    else                c = (uint8_t)level;

    Serial.print(col[x][colCount]);
    Serial.print(',');
    if (x > 0 && col[x][colCount] > maxVal && col[x][colCount] > 5) {
      maxIdx = x;
      maxVal = col[x][colCount];
    }
  }
  Serial.print('\n');

  if (maxIdx != -1) {
    if (maxIdx == 1) {
      r = 255;
      g = 0;
      b = 0;
    }
    if (maxIdx == 2) {
      r = 213;
      g = 43;
      b = 0;
    }
    if (maxIdx == 3) {
      r = 170;
      g = 85;
      b = 0;
    }
    if (maxIdx == 4) {
      r = 128;
      g = 128;
      b = 0;
    }
    if (maxIdx == 5) {
      r = 85;
      g = 170;
      b = 0;
    }
    if (maxIdx == 6) {
      r = 43;
      g = 213;
      b = 0;
    }
    if (maxIdx == 7) {
      r = 0;
      g = 255;
      b = 0;
    }
  }
//
//  Serial.print("Max IDX ");
//  Serial.print(maxIdx);
//  Serial.print('\n');

  
  if (isPetting) {
    r = 0;
    g = 0;
    b = 255;
//    Serial.println("Is petting");
  } 
  
  if (isPetting == false && wasPetting == true) {
    r = 0;
    g = 255;
    b = 0;
    wasPetting = false;
//    Serial.println("was petting");
  }

  int lightFadeSpeed = 2;
  if (ledOn && loopCount > 30) {
    if (r > curR) {
      curR += lightFadeSpeed;
    } else {
      curR -= lightFadeSpeed;
    }
    if (g > curG) {
      curG += lightFadeSpeed;
    } else {
      curG -= lightFadeSpeed;
    }
    if (b > curB) {
      curB += lightFadeSpeed;
    } else {
      curB -= lightFadeSpeed;
    }
  
    curR = min(max(0, curR), 255);
    curG = min(max(0, curG), 255);
    curB = min(max(0, curB), 255);  
  }
  

//  Serial.print(curR);
//  Serial.print(",");
//  Serial.print(curG);
//  Serial.print(",");
//  Serial.print(curB);
//  Serial.print("\n");
  
  if (ledOn) {
    if (loopCount < 30) {
      setColor(0, int(loopCount / 30.0 * 255.0), 0);
    } else {
      setColor(curR, curG, curB);  
    }
    
  } else {
    if (loopCount < 30) {
      setColor(int((30 - loopCount) / 30.0 * curR), int((30 - loopCount) / 30.0 * curG), int((30 - loopCount) / 30.0 * curB));
    } else {
      setColor(0, 0, 0);  
    }
  }

//  double red, green, blue;
//  ColorConverter::HslToRgb(hue, saturation, lightness, red, green, blue);
//  setColor(int(red), int(green), int(blue));

  if(++colCount >= 10) colCount = 0;
//  delay(10);
}

/*
 * Sampling interupt
 *   - Cycles through the sensor pins, taking FFT_N audio samples and a single
 *     sample of all others.
 *     https://forum.arduino.cc/index.php?topic=295773.0
// */
ISR(ADC_vect) {
  int16_t sample = ADC; // 0-1023
  boolean done = false;

//  Serial.println("here");
  if (current_pin == SOUND_PIN) {
//    isPetting = false;
    static const int16_t noiseThreshold = 4;

    capture[samplePos] =
      ((sample > (512 - noiseThreshold)) &&
       (sample < (512 + noiseThreshold))) ? 0 :
      sample - 512; // Sign-convert for FFT; -512 to +511

    if (++samplePos >= FFT_N) done = true;
  } else if (current_pin == LIGHT_SENSOR_PIN) {
    lightSensorValue = sample;
//    Serial.println(lightSensorValue);
    if (lightSensorValue < 200) {
      isPetting = true;
      wasPetting = true;
    } else {
      if (isPetting) {
        wasPetting = true;
      } else {
        wasPetting = false;
      }
      isPetting = false;
    }
    done = true;
  }

  if (done) {
    // Record which pin is done sampling
    ready_pin = current_pin;

    // Turn off interrupts to report back and switch pins
    ADCSRA &= ~_BV(ADIE);
   
    // Switch pins
    switch (current_pin) {
      case SOUND_PIN: current_pin = LIGHT_SENSOR_PIN; break;
      case LIGHT_SENSOR_PIN: current_pin = SOUND_PIN; break;
    }
    ADMUX  = current_pin;
  }
}


    void setColor(int red, int green, int blue)
    {
      red = 255 - red;
      green = 255 - green;
      blue = 255 - blue;
      analogWrite(redPin, red);
      analogWrite(greenPin, green);
      analogWrite(bluePin, blue);  
      analogWrite(redPin2, red);
      analogWrite(greenPin2, green);
      analogWrite(bluePin2, blue);  
    }


    unsigned long microToSeconds(unsigned long m) {
      return m / 1000000;
    }
    
