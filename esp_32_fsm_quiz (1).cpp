#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>

// --- Display & Touch Settings ---
#define TFT_CS    5
#define TFT_DC    2
#define TFT_RST   4
#define TOUCH_CS  15
#define TOUCH_IRQ 17
#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 320

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
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

// --- Question Database ---
struct Question { const char* text; float thetaMin, thetaMax; float phiMin, phiMax; };
const int TOTAL_Q = 500;
Question questions[TOTAL_Q]; // Populate in setup or PROGMEM

// --- Touch Button ---
struct Button { int x,y,w,h; const char* label; };
Button btnPlus  = {190, 50, 40, 40, "+"};
Button btnMinus = { 10, 50, 40, 40, "-"};
Button btnOK    = { 90,280, 60, 30, "OK"};
bool touchHit(int tx,int ty,const Button &b) { return tx>=b.x && tx<=b.x+b.w && ty>=b.y && ty<=b.y+b.h; }

// --- Polar container ---
float lastPolar[3];

// --- Prototypes ---
void doCalibration();
void drawSelectPlayers();
void handleSelectPlayers();
void startGame();
void drawAskQuestion();
void handleWaitForPen();
void handleMatchAnswer();
void drawScoreboard();
void getPolar(float *pol);

void setup() {
  Serial.begin(115200);
  // SPI bus for TFT+touch
  SPI.begin(18, 19, 23);
  tft.begin(); tft.setRotation(1);
  ts.begin(); ts.setRotation(1);
  // MUX & ADC
  pinMode(MUX_S0,OUTPUT); pinMode(MUX_S1,OUTPUT);
  pinMode(MUX_S2,OUTPUT); pinMode(MUX_S3,OUTPUT);
  pinMode(MUX_EN,OUTPUT); digitalWrite(MUX_EN,LOW);
  analogReadResolution(12); analogSetAttenuation(ADC_11db);
  randomSeed(analogRead(34));
  // TODO: populate questions[] here
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
      tft.fillScreen(ILI9341_BLACK);
      tft.setTextSize(2); tft.setTextColor(ILI9341_WHITE);
      tft.setCursor(20,20); tft.println("Game Over");
      tft.setTextSize(1);
      for(int p=0;p<numPlayers;p++){
        tft.setCursor(20,60+p*20);
        tft.printf("P%d: %d",p+1,scores[p]);
      }
      tft.setCursor(20,200);
      tft.println("Tap to restart");
      if(ts.touched()){
        delay(200);
        drawSelectPlayers(); gameState=STATE_SELECT_PLAYERS;
      }
      break;
  }
}

void doCalibration() {
  const int CAL_SAMPLES=100;
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
    baseline[ch]=sum/(float)CAL_SAMPLES;
  }
  drawSelectPlayers(); gameState=STATE_SELECT_PLAYERS;
}

void drawSelectPlayers(){
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextSize(3); tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(30,10);
  tft.print("Players: "); tft.print(numPlayers);
  tft.drawRect(btnPlus.x,btnPlus.y,btnPlus.w,btnPlus.h,ILI9341_WHITE);
  tft.setCursor(btnPlus.x+12,btnPlus.y+10); tft.print(btnPlus.label);
  tft.drawRect(btnMinus.x,btnMinus.y,btnMinus.w,btnMinus.h,ILI9341_WHITE);
  tft.setCursor(btnMinus.x+12,btnMinus.y+10); tft.print(btnMinus.label);
  tft.drawRect(btnOK.x,btnOK.y,btnOK.w,btnOK.h,ILI9341_WHITE);
  tft.setCursor(btnOK.x+10,btnOK.y+8); tft.print(btnOK.label);
}

void handleSelectPlayers(){
  if(!ts.touched()) return;
  TS_Point p=ts.getPoint();
  int tx=map(p.x,200,3900,0,SCREEN_WIDTH);
  int ty=map(p.y,200,3900,0,SCREEN_HEIGHT);
  if(touchHit(tx,ty,btnPlus)&&numPlayers<MAX_PLAYERS) numPlayers++;
  if(touchHit(tx,ty,btnMinus)&&numPlayers>1) numPlayers--;
  if(touchHit(tx,ty,btnOK)){ startGame(); return; }
  drawSelectPlayers();
}

void startGame(){
  memset(scores,0,sizeof(scores)); memset(questionCount,0,sizeof(questionCount));
  static int allIdx[TOTAL_Q]; for(int i=0;i<TOTAL_Q;i++) allIdx[i]=i;
  for(int i=TOTAL_Q-1;i>0;i--){int j=random(i+1);int t=allIdx[i];allIdx[i]=allIdx[j];allIdx[j]=t;}
  for(int p=0;p<numPlayers;p++) for(int q=0;q<Q_PER_PLAYER;q++)
    qIndices[p][q]=allIdx[p*Q_PER_PLAYER+q];
  currentPlayer=0; gameState=STATE_ASK_QUESTION;
}

void drawAskQuestion(){
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextSize(2); tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(10,10);
  tft.printf("P%d Q%d/%d",currentPlayer+1,questionCount[currentPlayer]+1,Q_PER_PLAYER);
  tft.setCursor(10,60);
  int qi=qIndices[currentPlayer][questionCount[currentPlayer]];
  tft.setTextSize(1);
  tft.println(questions[qi].text);
  gameState=STATE_WAIT_FOR_PEN;
}

void handleWaitForPen(){
  getPolar(lastPolar);
  if(lastPolar[0]<=0.001f) return;
  gameState=STATE_MATCH_ANSWER;
}

void handleMatchAnswer(){
  int qi=qIndices[currentPlayer][questionCount[currentPlayer]];
  auto &Q=questions[qi];
  float th=lastPolar[1],ph=lastPolar[2];
  if(th>=Q.thetaMin&&th<=Q.thetaMax&&ph>=Q.phiMin&&ph<=Q.phiMax)
    scores[currentPlayer]++;
  questionCount[currentPlayer]++;
  gameState=STATE_DISPLAY_SCORE;
}

void drawScoreboard(){
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextSize(2); tft.setTextColor(ILI9341_WHITE);
  for(int p=0;p<numPlayers;p++){
    tft.setCursor(10,20+p*30);
    tft.printf("P%d: %d",p+1,scores[p]);
  }
  delay(1000);
  if(questionCount[currentPlayer]<Q_PER_PLAYER) gameState=STATE_ASK_QUESTION;
  else {
    bool allDone=true;
    for(int p=0;p<numPlayers;p++) if(questionCount[p]<Q_PER_PLAYER){allDone=false;currentPlayer=p;break;}
    gameState=allDone?STATE_GAME_OVER:STATE_ASK_QUESTION;
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
