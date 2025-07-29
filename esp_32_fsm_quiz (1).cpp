#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>

// --- Display & Touch Settings ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
#define TOUCH_CS  16
#define TOUCH_IRQ 17
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// --- Pin assignments for MUX/ADC ---
const int MUX_S0 = 14;
const int MUX_S1 = 27;
const int MUX_S2 = 26;
const int MUX_S3 = 25;
const int MUX_EN = 33;
const int ADC_PIN = 35;
const unsigned int SETTLE_US = 6000;

// --- FSM States ---
enum State {
  STATE_CALIBRATION,
  STATE_SELECT_PLAYERS,
  STATE_ASK_QUESTION,
  STATE_WAIT_FOR_PEN,
  STATE_MATCH_ANSWER,
  STATE_DISPLAY_SCORE,
  STATE_GAME_OVER
};
State gameState = STATE_CALIBRATION;

// --- Quiz Parameters ---
const int MAX_PLAYERS = 4;
const int Q_PER_PLAYER = 10;
int numPlayers = 2;
int scores[MAX_PLAYERS];
int questionCount[MAX_PLAYERS];
int currentPlayer = 0;
int qIndices[MAX_PLAYERS][Q_PER_PLAYER];

// --- Sensor Data ---
uint16_t rawReadings[16];
float baseline[16];
uint16_t filteredReadings[16];
// 16 points on surface of 15cm sphere
float sensorCoords[16][3] = {
  {  5.219779f,   0.000000f,  14.062500f},
  { -6.447862f,  -5.906769f,  12.187500f},
  {  0.952308f,  10.851058f,  10.312500f},
  {  7.545834f,  -9.842204f,   8.437500f},
  {-13.282087f,   2.349414f,   6.562500f},
  { 12.022472f,   7.647713f,   4.687500f},
  { -3.825002f, -14.228816f,   2.812500f},
  { -6.900089f,  13.285702f,   0.937500f},
  { 14.062273f,  -5.135520f,  -0.937500f},
  {-13.619279f,  -5.621840f,  -2.812500f},
  {  6.039283f,  12.905596f,  -4.687500f},
  {  4.036823f, -12.870029f,  -6.562500f},
  {-10.730314f,   6.218436f,  -8.437500f},
  { 10.638700f,   2.338888f, -10.312500f},
  { -5.029172f,  -7.153480f, -12.187500f},
  { -0.670797f,   5.176497f, -14.062500f}
};

// --- Question Database with angle ranges ---
struct Question {
  const char* text;
  float thetaMin, thetaMax; // inclination range in degrees
  float phiMin,   phiMax;   // azimuth range in degrees
};
const int TOTAL_Q = 500;
Question questions[TOTAL_Q]; // fill at startup or from PROGMEM

// --- Touch Button Helpers ---
struct Button { int x,y,w,h; const char* label; };
Button btnPlus  = {90,20,30,30,"+"};
Button btnMinus = {10,20,30,30,"-"};
Button btnOK    = {44,50,40,12,"OK"};
bool touchHit(int tx,int ty,const Button &b) { return tx>=b.x && tx<=b.x+b.w && ty>=b.y && ty<=b.y+b.h; }

// --- Polar reading container ---
float lastPolar[3]; // {r, theta_deg, phi_deg}

// --- Prototypes ---
void doCalibration();
void drawSelectPlayers();
void handleSelectPlayers();
void startGame();
void drawAskQuestion();
void handleWaitForPen() {
  // Poll sensors and compute polar coordinates
  getPolar(lastPolar);

  // If r is essentially zero, no valid reading yet → stay in this state
  if (lastPolar[0] <= 0.001f) {
    return;
  }

  // Valid coordinate received → move to matching state
  gameState = STATE_MATCH_ANSWER;
}

void loop() {
  switch(gameState) {
    case STATE_CALIBRATION:    doCalibration();        break;
    case STATE_SELECT_PLAYERS: handleSelectPlayers();  break;
    case STATE_ASK_QUESTION:   drawAskQuestion();      break;
    case STATE_WAIT_FOR_PEN:   handleWaitForPen();     break;
    case STATE_MATCH_ANSWER:   handleMatchAnswer();    break;
    case STATE_DISPLAY_SCORE:  drawScoreboard();       break;
    case STATE_GAME_OVER:
      // Show final standings
      display.clearDisplay();
      display.setTextSize(2);
      display.setCursor(0,0);
      display.println("Game Over");
      display.setTextSize(1);
      for (int p = 0; p < numPlayers; p++) {
        display.setCursor(0, 20 + p*10);
        display.printf("P%d: %d", p+1, scores[p]);
      }
      display.setCursor(0, 20 + numPlayers*10);
      display.println("Tap to restart");
      display.display();
      // Wait for touch to restart
      if (ts.touched()) {
        TS_Point p = ts.getPoint();
        // simple debounce
        delay(200);
        drawSelectPlayers();
        gameState = STATE_SELECT_PLAYERS;
      }
      break;
  }
}

void doCalibration() {
  const int CAL_SAMPLES = 100;
  for(uint8_t ch=0;ch<16;ch++){
    digitalWrite(MUX_S0,(ch>>0)&1);
    digitalWrite(MUX_S1,(ch>>1)&1);
    digitalWrite(MUX_S2,(ch>>2)&1);
    digitalWrite(MUX_S3,(ch>>3)&1);
    uint32_t sum=0;
    for(int i=0;i<CAL_SAMPLES;i++){
      delayMicroseconds(SETTLE_US);
      sum+=analogRead(ADC_PIN);
    }
    baseline[ch] = sum/(float)CAL_SAMPLES;
  }
  drawSelectPlayers();
  gameState = STATE_SELECT_PLAYERS;
}

void drawSelectPlayers(){
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10,0);
  display.print("Players: "); display.print(numPlayers);
  display.drawRect(btnPlus.x,btnPlus.y,btnPlus.w,btnPlus.h,WHITE);
  display.setCursor(btnPlus.x+8,btnPlus.y+8); display.print(btnPlus.label);
  display.drawRect(btnMinus.x,btnMinus.y,btnMinus.w,btnMinus.h,WHITE);
  display.setCursor(btnMinus.x+8,btnMinus.y+8); display.print(btnMinus.label);
  display.drawRect(btnOK.x,btnOK.y,btnOK.w,btnOK.h,WHITE);
  display.setCursor(btnOK.x+8,btnOK.y+2); display.print(btnOK.label);
  display.display();
}

void handleSelectPlayers(){
  if(!ts.touched()) return;
  TS_Point p = ts.getPoint();
  int tx = map(p.x,200,3900,0,SCREEN_WIDTH);
  int ty = map(p.y,200,3900,0,SCREEN_HEIGHT);
  if(touchHit(tx,ty,btnPlus) && numPlayers<MAX_PLAYERS) numPlayers++;
  if(touchHit(tx,ty,btnMinus) && numPlayers>1) numPlayers--;
  if(touchHit(tx,ty,btnOK)) { startGame(); return; }
  drawSelectPlayers();
}

void startGame(){
  memset(scores,0,sizeof(scores));
  memset(questionCount,0,sizeof(questionCount));
  static int allIdx[TOTAL_Q];
  for(int i=0;i<TOTAL_Q;i++) allIdx[i]=i;
  for(int i=TOTAL_Q-1;i>0;i--){int j=random(i+1);int t=allIdx[i];allIdx[i]=allIdx[j];allIdx[j]=t;}
  for(int p=0;p<numPlayers;p++){
    for(int q=0;q<Q_PER_PLAYER;q++){
      qIndices[p][q] = allIdx[p*Q_PER_PLAYER + q];
    }
  }
  currentPlayer = 0;
  gameState = STATE_ASK_QUESTION;
}

void drawAskQuestion(){
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);
  display.printf("P%d Q%d/%d", currentPlayer+1, questionCount[currentPlayer]+1, Q_PER_PLAYER);
  display.setCursor(0,16);
  int qi = qIndices[currentPlayer][questionCount[currentPlayer]];
  display.println(questions[qi].text);
  display.display();
  gameState = STATE_WAIT_FOR_PEN;
}

void handleWaitForPen() {
  getPolar(lastPolar);

  // If polar coordinates are still invalid (e.g., all zero)
  if (lastPolar[0] == 0 && lastPolar[1] == 0) {
    // Stay in WAIT_FOR_PEN state, do nothing
    return;
  }

  // Valid coordinate received → move to next state
  gameState = ANSWER_MATCHING;
}


void handleMatchAnswer(){
  int qi = qIndices[currentPlayer][questionCount[currentPlayer]];
  Question &Q = questions[qi];
  float th = lastPolar[1], ph = lastPolar[2];
  bool correct = (th>=Q.thetaMin && th<=Q.thetaMax && ph>=Q.phiMin && ph<=Q.phiMax);
  if(correct) scores[currentPlayer]++;
  questionCount[currentPlayer]++;
  gameState = STATE_DISPLAY_SCORE;
}

void drawScoreboard(){
  display.clearDisplay();
  display.setTextSize(1);
  for(int p=0;p<numPlayers;p++){
    display.setCursor(0,p*12);
    display.printf("P%d: %d", p+1, scores[p]);
  }
  display.display();
  delay(1000);
  // next
  if(questionCount[currentPlayer]<Q_PER_PLAYER) gameState = STATE_ASK_QUESTION;
  else {
    bool allDone=true;
    for(int p=0;p<numPlayers;p++) if(questionCount[p]<Q_PER_PLAYER){allDone=false; currentPlayer=p; break;}
    gameState = allDone ? STATE_GAME_OVER : STATE_ASK_QUESTION;
  }
}

void getPolar(float *pol) {
  // 1) Read all sensors
  for (uint8_t ch = 0; ch < 16; ch++) {
    digitalWrite(MUX_S0, (ch >> 0) & 0x1);
    digitalWrite(MUX_S1, (ch >> 1) & 0x1);
    digitalWrite(MUX_S2, (ch >> 2) & 0x1);
    digitalWrite(MUX_S3, (ch >> 3) & 0x1);
    delayMicroseconds(SETTLE_US);
    rawReadings[ch] = analogRead(ADC_PIN);
  }
  // 2) Filter readings
  for (uint8_t ch = 0; ch < 16; ch++) {
    filteredReadings[ch] = (rawReadings[ch] > baseline[ch]) ? rawReadings[ch] : 0;
  }
  // 3) Compute weighted average in Cartesian
  float sumW = 0, xW = 0, yW = 0, zW = 0;
  for (uint8_t i = 0; i < 16; i++) {
    float w = (float)filteredReadings[i];
    sumW += w;
    xW += w * sensorCoords[i][0];
    yW += w * sensorCoords[i][1];
    zW += w * sensorCoords[i][2];
  }
  if (sumW <= 0) {
    // no valid readings
    pol[0] = pol[1] = pol[2] = 0;
    return;
  }
  // 4) Normalize vector
  float ux = xW / sumW;
  float uy = yW / sumW;
  float uz = zW / sumW;
  float mag = sqrtf(ux*ux + uy*uy + uz*uz);
  ux /= mag;
  uy /= mag;
  uz /= mag;
  // 5) Scale to sphere radius
  float rx = ux * 15.0f;
  float ry = uy * 15.0f;
  float rz = uz * 15.0f;
  // 6) Convert to spherical coordinates
  float radius = sqrtf(rx*rx + ry*ry + rz*rz);
  float theta = acosf(rz / radius); // inclination in radians
  float phi   = atan2f(ry, rx);      // azimuth in radians
  // 7) Store in pol array (degrees)
  pol[0] = radius;
  pol[1] = theta * 180.0f / PI;
  pol[2] = phi   * 180.0f / PI;
}
