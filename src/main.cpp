#include <Arduino.h>
#include <OneButton.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <hardware/gpio.h> // for gpio_set_drive_strength

// --- Pin Definitions ---
const int PIN_BTN_TOGGLE = 27; // D1
const int PIN_BTN_BRIGHT = 28; // D2
const int PIN_LED_FIBER = D0;  // D0 (Logic Pin 0) -> GPIO 26
const int GPIO_LED_FIBER = 26; // Hardware GPIO 26 for SDK calls

const int PIN_LED_FIBER2 = D3;  // D6 (Logic Pin 6) -> GPIO 0
const int GPIO_LED_FIBER2 = 29; // Hardware GPIO 0 for SDK calls

// --- Constants ---
// 5 levels of brightness: 20%, 40%, 60%, 80%, 100%
// PWM is 0-255.
const int BRIGHTNESS_LEVELS[] = {51, 102, 153, 204, 255};
const int NUM_LEVELS = 5;

// --- Globals ---
// Setup OLED: Controller SSD1306, 128x32 Portrait Mode
// Switching back to _F_ (full buffer) for stability - page buffer mode was
// causing issues
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R1, /* reset=*/U8X8_PIN_NONE);

// Setup Buttons
OneButton btnToggle(PIN_BTN_TOGGLE, true); // true = active low (Input Pullup)
OneButton btnBright(PIN_BTN_BRIGHT, true);

// State Variables
bool isLedOn = false;
int currentLevelIndex = 4; // Start at 100% (index 4)

// Animation & Effects
unsigned long lastFrameTime = 0;
int frameCounter = 0;
int glitchOffset = 0;
int scanlinePos = 0;
int lastScanlinePos = 0;
bool glitchActive = false;
unsigned long lastGlitchTime = 0;
bool scanlinePaused = false;
unsigned long scanlinePauseStart = 0;
const unsigned long SCANLINE_PAUSE_DURATION = 1000; // 1 second pause

// Display Health Monitoring (for tracking successful draws)
unsigned long lastSuccessfulDraw = 0;

// Screen power management
enum ScreenState {
  SCREEN_OFF,
  SCREEN_POWERING_UP,
  SCREEN_ON,
  SCREEN_POWERING_DOWN
};

ScreenState screenState = SCREEN_OFF;
unsigned long lastUserActivity = 0;
const unsigned long SCREEN_TIMEOUT = 5000; // Screen stays on for 5 seconds

// Animation timing
unsigned long animationStartTime = 0;
const unsigned long POWERUP_DURATION = 500; // Power-up animation duration in ms
const unsigned long POWERDOWN_DURATION =
    600; // Power-down animation duration in ms

// Noise burst effect variables
bool noiseBurstActive = false;
unsigned long lastNoiseBurst = 0;
unsigned long noiseBurstDuration = 150;

int noisePixelCount = 140;

// --- Cyberpunk Helper Functions ---

// Wake screen on button press
void wakeScreen() {
  if (screenState == SCREEN_OFF) {
    screenState = SCREEN_POWERING_UP;
    animationStartTime = millis();
    u8g2.setPowerSave(0);
    u8g2.setContrast(255);
  }
  lastUserActivity = millis();
}

// Check if screen should turn off after timeout
void updateScreenState() {
  unsigned long now = millis();

  switch (screenState) {
  case SCREEN_POWERING_UP:
    if (now - animationStartTime >= POWERUP_DURATION) {
      screenState = SCREEN_ON;
    }
    break;

  case SCREEN_ON:
    if (now - lastUserActivity >= SCREEN_TIMEOUT) {
      screenState = SCREEN_POWERING_DOWN;
      animationStartTime = now;
    }
    break;

  case SCREEN_POWERING_DOWN:
    if (now - animationStartTime >= POWERDOWN_DURATION) {
      screenState = SCREEN_OFF;
      u8g2.setPowerSave(1);
    }
    break;

  case SCREEN_OFF:
    // Do nothing, wait for button press
    break;
  }
}

// Draw power-up animation (expanding from center)
void drawPowerUpAnimation(float progress) {
  // Progress is 0.0 to 1.0
  int centerY = 64;
  int centerX = 16;

  // Expanding box animation
  int width = (int)(32 * progress);
  int height = (int)(128 * progress);

  u8g2.drawFrame(centerX - width / 2, centerY - height / 2, width, height);

  // Radiating lines
  if (progress > 0.3) {
    for (int i = 0; i < 4; i++) {
      float angle = (progress * 6.28f) + (i * 1.57f);
      int lineLength = (int)(20 * (progress - 0.3) / 0.7);
      int x1 = centerX + (int)(cos(angle) * 5);
      int y1 = centerY + (int)(sin(angle) * 5);
      int x2 = centerX + (int)(cos(angle) * (5 + lineLength));
      int y2 = centerY + (int)(sin(angle) * (5 + lineLength));
      u8g2.drawLine(x1, y1, x2, y2);
    }
  }

  // Flash text at end
  if (progress > 0.7) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(4, 64, "ONLINE");
  }
}

// Draw power-down animation (collapsing to center)
void drawPowerDownAnimation(float progress) {
  // Progress is 0.0 to 1.0
  int centerY = 64;
  int centerX = 16;

  // Reverse progress for collapse effect
  float reverseProgress = 1.0 - progress;

  // Collapsing scanlines
  for (int i = 0; i < 8; i++) {
    int y = (int)(i * 16 * reverseProgress);
    if (y < 128) {
      u8g2.drawHLine(0, y, 32);
    }
  }

  // Shrinking box
  int width = (int)(32 * reverseProgress);
  int height = (int)(128 * reverseProgress);
  u8g2.drawFrame(centerX - width / 2, centerY - height / 2, width, height);

  // Fading text at beginning
  if (progress < 0.3) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(2, 64, "OFFLINE");
  }
}

// Trigger noise burst at random intervals (2-5 seconds)
void updateNoiseBurst() {
  unsigned long now = millis();

  // Check if we should trigger a new burst
  if (!noiseBurstActive && (now - lastNoiseBurst > random(4000, 7000))) {
    noiseBurstActive = true;
    lastNoiseBurst = now;
    // noiseBurstDuration = random(150, 250); // Burst lasts 150-250ms
  }

  // Deactivate burst after duration
  if (noiseBurstActive && (now - lastNoiseBurst > noiseBurstDuration)) {
    noiseBurstActive = false;
  }
}

// Random glitch effect trigger
void updateGlitch() {
  unsigned long now = millis();
  // Random glitch every 2-5 seconds
  if (now - lastGlitchTime > random(2000, 3500)) {
    glitchActive = true;
    glitchOffset = random(-3, 4);
    lastGlitchTime = now;
  } else if (now - lastGlitchTime > 150) {
    glitchActive = false;
    glitchOffset = 0;
  }
}

// Draw noise burst effect (dynamic white noise)
void drawNoiseBurst() {
  if (!noiseBurstActive) {
    return; // Don't draw anything if burst is not active
  }

  // Generate random noise each frame (TV static effect)
  for (int i = 0; i < noisePixelCount; i++) {
    int x = random(0, 32);
    int y = random(0, 128);
    u8g2.drawPixel(x, y);

    // Occasional clusters for visual variety
    if (random(100) < 50) {
      u8g2.drawPixel(x + 1, y);
    }
    if (random(100) < 30) {
      u8g2.drawPixel(x, y + 1);
    }
  }
}

// Draw animated diagonal scanline (slightly diagonal, with 1-second break)
void drawScanline() {
  unsigned long now = millis();

  // Check if we're in a pause period
  if (scanlinePaused) {
    if (now - scanlinePauseStart >= SCANLINE_PAUSE_DURATION) {
      scanlinePaused = false;
      scanlinePos = 0; // Reset for next cycle
    } else {
      return; // Don't draw during pause
    }
  }

  lastScanlinePos = scanlinePos;
  scanlinePos = (scanlinePos + 12) % 512; // Faster movement

  // Detect wrap-around (animation cycle completed)
  if (scanlinePos < lastScanlinePos) {
    scanlinePaused = true;
    scanlinePauseStart = now;
    return; // Don't draw this frame during pause start
  }

  // Draw slightly diagonal scanline
  for (int i = 0; i < 32; i++) {
    int y = (scanlinePos + i / 8) % 128; // Slightly diagonal (reduced from i*1)
    u8g2.drawPixel(i, y);
    u8g2.drawPixel(i, (y + 1) % 128);
  }
}

// Draw cyberpunk status indicator
void drawStatusIndicator(int y, bool active) {
  if (active) {
    // Cyber-Heartbeat Animation (20 frames cycle ~ 600ms)
    int pulse = frameCounter % 20;
    int cx = 16;    // Center X
    int cy = y - 3; // Center Y (approx)

    u8g2.drawStr(2, y, "[");
    u8g2.drawStr(26, y, "]");

    if (pulse < 3) {
      // Beat 1: Large + Crosshairs
      u8g2.drawBox(cx - 3, cy - 3, 7, 7);
      u8g2.drawLine(cx - 5, cy, cx + 5, cy);
      u8g2.drawLine(cx, cy - 5, cx, cy + 5);
    } else if (pulse < 6) {
      // Recoil: Small
      u8g2.drawBox(cx - 1, cy - 1, 3, 3);
    } else if (pulse < 11) {
      // Beat 2: Medium + Ring
      u8g2.drawBox(cx - 2, cy - 2, 5, 5);
      u8g2.drawFrame(cx - 5, cy - 5, 11, 11);
    } else {
      // Rest: Small
      u8g2.drawBox(cx - 1, cy - 1, 3, 3);
    }
  } else {
    // Draw dim brackets
    u8g2.drawStr(2, y, "[");
    u8g2.drawStr(26, y, "]");
    u8g2.drawPixel(15, y - 3);
  }
}

// Draw vertical power bar with cyberpunk styling
// Draw vertical power bar with cyberpunk styling
void drawPowerBar(int percentage, bool active) {
  int barHeight = map(percentage, 0, 100, 0, 40);

  // Draw outer frame
  u8g2.drawFrame(2, 70, 8, 42);
  u8g2.drawFrame(22, 70, 8, 42);

  if (active) {
    // Draw filled bars
    if (barHeight > 0) {
      u8g2.drawBox(3, 111 - barHeight, 6, barHeight);
      u8g2.drawBox(23, 111 - barHeight, 6, barHeight);
    }
  } else {
    // Draw disabled "hatch" pattern (diagonal lines) in each segment
    for (int i = 0; i < 5; i++) {
      int yBase = 70 + (i * 8) + 1; // Top of segment (inside frame)
      // Left Box Hatching (x=3 to 8)
      u8g2.drawLine(3, yBase + 6, 3 + 6, yBase);         // Main diagonal
      u8g2.drawLine(3, yBase + 2, 3 + 2, yBase);         // Top-left corner
      u8g2.drawLine(3 + 4, yBase + 6, 3 + 6, yBase + 4); // Bottom-right corner

      // Right Box Hatching (x=23 to 28)
      u8g2.drawLine(23, yBase + 6, 23 + 6, yBase); // Main diagonal
      u8g2.drawLine(23, yBase + 2, 23 + 2, yBase); // Top-left corner
      u8g2.drawLine(23 + 4, yBase + 6, 23 + 6,
                    yBase + 4); // Bottom-right corner
    }
  }

  // Draw segment lines for cyberpunk look
  for (int i = 0; i < 5; i++) {
    int segY = 70 + (i * 8);
    u8g2.drawHLine(2, segY, 8);
    u8g2.drawHLine(22, segY, 8);
  }
}

// Update LED brightness
void updateLED() {
  if (isLedOn) {
    // Ensure pins are outputs and have proper drive strength when turning ON
    pinMode(PIN_LED_FIBER, OUTPUT);
    pinMode(PIN_LED_FIBER2, OUTPUT);
    gpio_set_drive_strength(GPIO_LED_FIBER, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(GPIO_LED_FIBER2, GPIO_DRIVE_STRENGTH_12MA);

    // If max brightness, use digital write for pure VCC connection on both LEDs
    if (currentLevelIndex == NUM_LEVELS - 1) {
      digitalWrite(PIN_LED_FIBER, HIGH);
      digitalWrite(PIN_LED_FIBER2, HIGH);
    } else {
      analogWrite(PIN_LED_FIBER, BRIGHTNESS_LEVELS[currentLevelIndex]);
      analogWrite(PIN_LED_FIBER2, BRIGHTNESS_LEVELS[currentLevelIndex]);
    }
  } else {
    // To ensure N-Channel MOSFETs are completely OFF, we must drive the Gate
    // LOW (to Ground). NEVER set them to INPUT (High Impedance) or the floating
    // gate will pick up noise and turn on partially.
    pinMode(PIN_LED_FIBER, OUTPUT);
    pinMode(PIN_LED_FIBER2, OUTPUT);

    digitalWrite(PIN_LED_FIBER, LOW);
    digitalWrite(PIN_LED_FIBER2, LOW);
  }
}

void drawScreen() {
  // Don't draw if screen is completely off
  if (screenState == SCREEN_OFF) {
    return;
  }

  // Update noise burst timing (pre-calculates positions outside of buffer
  // operations)
  updateNoiseBurst();

  // Full buffer mode - clear, draw, send
  u8g2.clearBuffer();

  // Handle different screen states
  if (screenState == SCREEN_POWERING_UP) {
    // Show power-up animation
    unsigned long elapsed = millis() - animationStartTime;
    float progress = (float)elapsed / POWERUP_DURATION;
    if (progress > 1.0)
      progress = 1.0;
    drawPowerUpAnimation(progress);

  } else if (screenState == SCREEN_POWERING_DOWN) {
    // Show power-down animation
    unsigned long elapsed = millis() - animationStartTime;
    float progress = (float)elapsed / POWERDOWN_DURATION;
    if (progress > 1.0)
      progress = 1.0;
    drawPowerDownAnimation(progress);

  } else {
    // Normal screen when ON
    // Update animations
    updateGlitch();

    // Draw noise burst if active
    drawNoiseBurst();

    // Set font
    u8g2.setFont(u8g2_font_5x7_tr);

    // --- HEADER SECTION (Top) ---
    u8g2.setFont(u8g2_font_4x6_tr);

    // Glitchy header text
    int headerY = (glitchActive ? glitchOffset : 0);
    u8g2.drawStr(1, 8 + headerY, "CYBR.LMP");

    // Draw decorative lines
    u8g2.drawHLine(0, 10, 32);
    if (glitchActive) {
      u8g2.drawHLine(0, 11, random(32));
    }

    // --- STATUS SECTION ---
    u8g2.setFont(u8g2_font_5x7_tr);

    // Power status with cyberpunk indicator
    drawStatusIndicator(28, isLedOn);

    u8g2.setFont(u8g2_font_4x6_tr);
    int statusY = 38;
    if (isLedOn) {
      u8g2.drawStr(6 + headerY, statusY, "PWR");
      // Draw pulsing pixels
      if ((frameCounter / 5) % 2 == 0) {
        u8g2.drawPixel(2, statusY - 3);
        u8g2.drawPixel(28, statusY - 3);
      }
    } else {
      u8g2.drawStr(4 - headerY, statusY, "SLEEP");
    }

    // Separator
    u8g2.drawHLine(0, 42, 32);

    // --- BRIGHTNESS SECTION ---
    int percentage = (currentLevelIndex + 1) * 20;

    // Draw "LVL" label
    u8g2.setFont(u8g2_font_4x6_tr);
    u8g2.drawStr(8, 54, "LVL");

    // Draw large percentage number
    u8g2.setFont(u8g2_font_6x12_tr);
    char buf[10];
    sprintf(buf, "%d", percentage);
    int numX = (percentage == 100) ? 6 : 10;
    u8g2.drawStr(numX, 66, buf);

    // Draw power bars
    // Draw power bars
    drawPowerBar(percentage, isLedOn);

    // --- BOTTOM SECTION ---
    // Draw animated scanline effect
    u8g2.setDrawColor(1);
    drawScanline();

    // Draw bottom status text
    u8g2.setFont(u8g2_font_4x6_tr);
    u8g2.drawStr(2, 122, "SYS.OK");

    // Frame counter display (hex style)
    if (frameCounter % 30 < 15) {
      sprintf(buf, "%02X", (frameCounter / 10) % 256);
      u8g2.drawStr(18, 122, buf);
    }
  }

  u8g2.sendBuffer();

  // Track successful draw for health monitoring
  lastSuccessfulDraw = millis();
  frameCounter++;
}

// --- Button Callbacks ---

void onToggleClick() {
  wakeScreen(); // Wake screen on button press
  isLedOn = !isLedOn;
  updateLED();
  drawScreen();

  // Trigger glitch effect on state change
  glitchActive = true;
  lastGlitchTime = millis();
}

void onBrightClick() {
  wakeScreen(); // Wake screen on button press

  if (!isLedOn) {
    return;
  }

  // Increment index, wrap around if needed
  currentLevelIndex++;
  if (currentLevelIndex >= NUM_LEVELS) {
    currentLevelIndex = 0; // Go back to 20%
  }

  updateLED();
  drawScreen();

  // Trigger glitch effect on brightness change
  glitchActive = true;
  lastGlitchTime = millis();
}

// --- Main Setup & Loop ---

void setup() {
  // Initialize Pins
  pinMode(PIN_LED_FIBER, OUTPUT);
  pinMode(PIN_LED_FIBER2, OUTPUT);
  // Increase drive strength to maximum (12mA) to drive MOSFETs hard
  gpio_set_drive_strength(GPIO_LED_FIBER, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_drive_strength(GPIO_LED_FIBER2, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_slew_rate(GPIO_LED_FIBER, GPIO_SLEW_RATE_FAST);
  gpio_set_slew_rate(GPIO_LED_FIBER2, GPIO_SLEW_RATE_FAST);

  // Initialize Display
  u8g2.begin();
  u8g2.setPowerSave(1); // Start with display OFF

  // Attach Button Events
  btnToggle.attachClick(onToggleClick);
  btnBright.attachClick(onBrightClick);

  // Seed random for glitch effects
  randomSeed(micros());

  // Initial State
  updateLED();

  // Screen starts in OFF state, will wake on button press
  screenState = SCREEN_OFF;
}

void loop() {
  unsigned long now = millis();

  // Update screen state (handle power up/down and timeouts)
  updateScreenState();

  // Keep watching the push buttons
  btnToggle.tick();
  btnBright.tick();

  // Only animate screen when it's not completely off
  // Animate at ~10fps for normal screen, 30fps for animations & noise
  int frameRate = (screenState == SCREEN_POWERING_UP ||
                   screenState == SCREEN_POWERING_DOWN || noiseBurstActive)
                      ? 33
                      : 100;

  if (screenState != SCREEN_OFF && now - lastFrameTime > frameRate) {
    lastFrameTime = now;
    drawScreen();

    // Periodic heartbeat every 20 seconds (only when screen is on)
    if (frameCounter % 100 == 0 && screenState == SCREEN_ON) {
      unsigned long elapsed = millis();
    }
  }

  delay(10);
}