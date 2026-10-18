// A Power Glove as a Bluetooth keyboard, on an M5StickC Plus
//
// Reads the glove's stock NES (4021 shift register) serial protocol through
// its own 9-pin plug and presents to the computer as a plain Bluetooth
// keyboard. The glove is not opened or modified.
//
// Wiring (glove plug pin -> Stick header, through a BSS138 level shifter;
// which shifter channel carries which signal does not matter):
//   pin 5 (+5V)   -> Stick 5V OUT hole, next to GND (switched on by axpEnable5V)
//   pin 2 (GND)   -> shifter GND (HV row) (shifter GNDs bridge to Stick GND)
//   pin 7 (LATCH) <- HV <- LV <- G26
//   pin 8 (CLOCK) <- HV <- LV <- G0       (G0 is also the ESP32 boot pin: see
//                                          "If the screen stays black" in the README)
//   pin 9 (DATA)  -> HV -> LV -> G36      (input-only pin)
//   Shifter LV -> Stick 3V3, shifter HV -> Stick 5V IN hole (far end of the header)
//
// The glove boots silent and sends nothing until START is pressed once.
//
// Button map:
//   Start = wakes the glove, types nothing. A = Option+Space (Claude quick entry),
//   B = Caps Lock (voice on/off), Select = Return (send),
//   Up/Down = volume up/down (repeat while held), Left/Right = prev/next track.
//   The Stick's own M5 button mutes and unmutes the glove.

#include <NimBleKeyboard.h>
#include <Wire.h>
#include <M5Unified.h>

const int PIN_LATCH = 26;
const int PIN_CLOCK = 0;
const int PIN_DATA  = 36;
const int PIN_PWR_HOLD = 4;
const int PIN_BTN_M5 = 37;   // Stick's own front button, active low: mute toggle

const unsigned long SETTLE_AFTER_CONNECT_MS = 1000;
const int STABLE_POLLS = 5;             // ~150ms at 30ms/poll: deliberate presses only
const unsigned long PRESS_COOLDOWN_MS = 250;
const unsigned long VOL_REPEAT_MS = 200;
const unsigned long VOL_REPEAT_DELAY_MS = 450;

BleKeyboard kb("PowerGlove KB", "Powerglove BT", 100);

enum { BTN_A, BTN_B, BTN_SELECT, BTN_START, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT };
const char *BTN_NAMES[8] = {"A", "B", "Select", "Start", "Up", "Down", "Left", "Right"};

uint8_t prevState = 0;                  // committed state, bit set = pressed
uint8_t pendingState = 0;
int stableCount = 0;
unsigned long lastPressMs[8] = {0};
unsigned long lockoutUntil = 0;
bool muted = false;
unsigned long connectedSince = 0;
bool wasConnected = false;
unsigned long badReads = 0;
uint8_t lastRaw = 0;                    // last raw read, for the diagnostic heartbeat
int volHeld = 0;                        // +1 up, -1 down
unsigned long volHeldSince = 0, lastVolRepeat = 0;

// StickC Plus v1: the 5V header/Grove rail is gated by the AXP192 PMU's EXTEN
// switch (off until enabled over I2C, SDA=21 SCL=22, addr 0x34).
void axpEnable5V() {
  Wire1.begin(21, 22);
  Wire1.beginTransmission(0x34);
  Wire1.write(0x12);
  Wire1.endTransmission(false);
  Wire1.requestFrom(0x34, 1);
  uint8_t reg12 = Wire1.available() ? Wire1.read() : 0x00;
  Wire1.beginTransmission(0x34);
  Wire1.write(0x12);
  Wire1.write(reg12 | 0x40);  // bit6 = EXTEN
  Wire1.endTransmission();
}

uint8_t readPadRaw() {
  // Slow timing on purpose: the BSS138 shifter + 1.5m of glove cable give
  // ~µs-scale rise times; fast edges smear bits across positions.
  digitalWrite(PIN_LATCH, HIGH);
  delayMicroseconds(12);
  digitalWrite(PIN_LATCH, LOW);
  delayMicroseconds(12);
  uint8_t v = 0;
  for (int i = 0; i < 8; i++) {
    if (digitalRead(PIN_DATA) == LOW) v |= (1 << i);  // active low
    digitalWrite(PIN_CLOCK, HIGH);
    delayMicroseconds(12);
    digitalWrite(PIN_CLOCK, LOW);
    delayMicroseconds(12);
  }
  return v;
}

bool readPad(uint8_t *out) {
  // ONE read per poll. Prior art (glovedivers, OniDaito) is unanimous:
  // back-to-back latches catch the glove's MCU mid-update and read garbage.
  uint8_t a = readPadRaw();
  lastRaw = a;
  if (a == 0xFF) { badReads++; return false; }  // floating/unplugged data line
  *out = a;
  return true;
}

void tapCombo(uint8_t mod, char key) {
  kb.press(mod);
  kb.press(key);
  delay(20);
  kb.releaseAll();
}

void tapMedia(const MediaKeyReport &k) {
  kb.press(k);
  delay(15);
  kb.release(k);
}

void onPress(int btn) {
  Serial.printf("PRESS %s\n", BTN_NAMES[btn]);
  switch (btn) {
    case BTN_A:      tapCombo(KEY_LEFT_ALT, ' '); break;  // Option+Space: summon Claude
    case BTN_B:      kb.press(KEY_CAPS_LOCK); delay(220); kb.release(KEY_CAPS_LOCK); break;  // voice toggle; macOS ignores short caps taps
    case BTN_START:  break;  // ceremonial: wakes the glove, types nothing
    case BTN_SELECT: kb.press(KEY_RETURN); break;  // send
    case BTN_UP:     tapMedia(KEY_MEDIA_VOLUME_UP);   volHeld = +1; volHeldSince = millis(); break;
    case BTN_DOWN:   tapMedia(KEY_MEDIA_VOLUME_DOWN); volHeld = -1; volHeldSince = millis(); break;
    case BTN_LEFT:   tapMedia(KEY_MEDIA_PREVIOUS_TRACK); break;
    case BTN_RIGHT:  tapMedia(KEY_MEDIA_NEXT_TRACK); break;
  }
}

void onRelease(int btn) {
  Serial.printf("RELEASE %s\n", BTN_NAMES[btn]);
  switch (btn) {
    case BTN_B:      break;
    case BTN_START:  break;
    case BTN_SELECT: kb.release(KEY_RETURN); break;
    case BTN_UP:
    case BTN_DOWN:   volHeld = 0; break;
    default: break;
  }
}

void drawScreen(bool connected) {
  auto &d = M5.Display;
  d.fillScreen(0x0000);
  d.setTextDatum(top_center);
  d.setTextSize(2);
  d.setTextColor(0x07FF, 0x0000);            // cyan title
  d.drawString("Powerglove BT", 120, 4);
  d.setTextSize(2);
  if (muted) {
    d.setTextColor(0xF800, 0x0000);          // red
    d.drawString("** MUTED **", 120, 28);
  } else if (connected) {
    d.setTextColor(0x07E0, 0x0000);          // green
    d.drawString("BT CONNECTED", 120, 28);
  } else {
    d.setTextColor(0xFD20, 0x0000);          // orange
    d.drawString("BT waiting...", 120, 28);
  }
  d.setTextSize(1);
  d.setTextColor(0xFFFF, 0x0000);
  d.setTextDatum(top_left);
  d.drawString("START  wake glove (after power)", 8, 56);
  d.drawString("A      summon Claude", 8, 68);
  d.drawString("B      talk / stop", 8, 80);
  d.drawString("SELECT send", 8, 92);
  d.drawString("UP/DN  volume   L/R track", 8, 104);
  d.drawString("M5 btn mute toggle", 8, 116);
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  cfg.internal_imu = false;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(80);
  axpEnable5V();
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);
  pinMode(PIN_LATCH, OUTPUT);
  pinMode(PIN_CLOCK, OUTPUT);
  pinMode(PIN_DATA, INPUT);
  pinMode(PIN_BTN_M5, INPUT);
  // G36 shares its header hole with G25 on the StickC Plus; per M5 docs G25
  // must be left floating or it distorts G36 reads.
  pinMode(25, INPUT);
  gpio_pulldown_dis(GPIO_NUM_25);
  gpio_pullup_dis(GPIO_NUM_25);
  gpio_pulldown_dis(GPIO_NUM_36);
  gpio_pullup_dis(GPIO_NUM_36);
  digitalWrite(PIN_LATCH, LOW);
  digitalWrite(PIN_CLOCK, LOW);
  Serial.begin(115200);
  Serial.println("PowerGlove KB v7.1 starting");
  kb.begin();
}

void loop() {
  // the Stick's own M5 button mutes the glove: nothing is typed until it is pressed again
  static bool m5Was = false;
  bool m5Now = (digitalRead(PIN_BTN_M5) == LOW);
  if (m5Now && !m5Was) {
    muted = !muted;
    volHeld = 0;
    if (kb.isConnected()) kb.releaseAll();
    Serial.println(muted ? "MUTED" : "UNMUTED");
  }
  m5Was = m5Now;

  static int lastScreen = -1;   // -1 forces first draw; encodes connected+muted
  int nowScreen = (kb.isConnected() ? 1 : 0) | (muted ? 2 : 0);
  if (nowScreen != lastScreen) {
    lastScreen = nowScreen;
    drawScreen(kb.isConnected());
  }
  if (!kb.isConnected()) {
    wasConnected = false;
    prevState = pendingState = 0;
    stableCount = 0;
    volHeld = 0;
    delay(50);
    return;
  }
  if (!wasConnected) {
    wasConnected = true;
    connectedSince = millis();
    Serial.println("BLE connected");
  }
  // settle window: no events right after (re)connecting
  if (millis() - connectedSince < SETTLE_AFTER_CONNECT_MS) {
    delay(30);
    return;
  }

  // diagnostic heartbeat: raw pad state + failed-read counter, every 2s
  static unsigned long lastDiag = 0;
  if (millis() - lastDiag > 2000) {
    lastDiag = millis();
    Serial.printf("diag raw=0x%02X fails=%lu committed=0x%02X\n", lastRaw, badReads, prevState);
  }

  uint8_t state;
  if (readPad(&state)) {
    if (state == pendingState) {
      if (stableCount < STABLE_POLLS) stableCount++;
    } else {
      pendingState = state;
      stableCount = 1;
    }
    if (stableCount >= STABLE_POLLS && pendingState != prevState) {
      uint8_t changed = pendingState ^ prevState;
      for (int i = 0; i < 8; i++) {
        if (!(changed & (1 << i))) continue;
        if (pendingState & (1 << i)) {
          if (millis() - lastPressMs[i] >= PRESS_COOLDOWN_MS) {
            lastPressMs[i] = millis();
            // chaos lockout: 3+ distinct buttons inside 700ms = dying glove, not fingers
            int recent = 0;
            for (int j = 0; j < 8; j++)
              if (lastPressMs[j] && millis() - lastPressMs[j] < 700) recent++;
            if (recent >= 3) {
              lockoutUntil = millis() + 2000;
              kb.releaseAll();
              volHeld = 0;
              Serial.println("CHAOS LOCKOUT (power flap?)");
            } else if (millis() >= lockoutUntil && !muted) {
              onPress(i);
            }
          }
        } else {
          if (millis() >= lockoutUntil && !muted) onRelease(i);
        }
      }
      prevState = pendingState;
    }
  }

  // volume auto-repeat while held
  if (volHeld != 0 && millis() - volHeldSince > VOL_REPEAT_DELAY_MS &&
      millis() - lastVolRepeat > VOL_REPEAT_MS) {
    lastVolRepeat = millis();
    tapMedia(volHeld > 0 ? KEY_MEDIA_VOLUME_UP : KEY_MEDIA_VOLUME_DOWN);
  }

  delay(30);
}
