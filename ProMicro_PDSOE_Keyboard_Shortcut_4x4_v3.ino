#include <Wire.h>
#include <PCF8575.h>
#include <Keyboard.h>
#include <EEPROM.h>

// ─── Configuration ────────────────────────────────────────────────────────────

#define INT_PIN   7
#define PCF_ADDR  0x20

#define NUM_KEYS        16
#define MAX_COMBO_KEYS  4     // Max simultaneous keys per button (e.g. CTRL+SHIFT+ALT+K)
#define EEPROM_MAGIC    0xAB  // Sentinel byte — if missing, EEPROM is uninitialised
#define EEPROM_VERSION  0x01  // Bump this if the layout format changes

// EEPROM layout:
//   [0]        = magic byte (0xAB)
//   [1]        = version   (0x01)
//   [2..N]     = 16 entries × MAX_COMBO_KEYS bytes each
//                Each byte is a HID keycode (0x00 = unused slot)
#define EEPROM_MAGIC_ADDR   0
#define EEPROM_VERSION_ADDR 1
#define EEPROM_DATA_ADDR    2

const int ledPin = 5;          // PWM pin
const float frequency = 0.2;   // cycles per second
const int minBrightness = 02;
const int maxBrightness = 100;

unsigned long startTime;

// ─── HID modifier / special-key name table ────────────────────────────────────

struct KeyAlias {
  const char* name;
  uint8_t     code;
};

// Names are case-insensitive during parsing (input is uppercased before lookup)
const KeyAlias KEY_ALIASES[] = {
  { "CTRL",       KEY_LEFT_CTRL   },
  { "LCTRL",      KEY_LEFT_CTRL   },
  { "RCTRL",      KEY_RIGHT_CTRL  },
  { "SHIFT",      KEY_LEFT_SHIFT  },
  { "LSHIFT",     KEY_LEFT_SHIFT  },
  { "RSHIFT",     KEY_RIGHT_SHIFT },
  { "ALT",        KEY_LEFT_ALT    },
  { "LALT",       KEY_LEFT_ALT    },
  { "RALT",       KEY_RIGHT_ALT   },
  { "GUI",        KEY_LEFT_GUI    },
  { "WIN",        KEY_LEFT_GUI    },
  { "CMD",        KEY_LEFT_GUI    },
  { "ENTER",      KEY_RETURN      },
  { "RETURN",     KEY_RETURN      },
  { "ESC",        KEY_ESC         },
  { "BACKSPACE",  KEY_BACKSPACE   },
  { "TAB",        KEY_TAB         },
  { "DELETE",     KEY_DELETE      },
  { "DEL",        KEY_DELETE      },
  { "INSERT",     KEY_INSERT      },
  { "HOME",       KEY_HOME        },
  { "END",        KEY_END         },
  { "PAGEUP",     KEY_PAGE_UP     },
  { "PAGEDOWN",   KEY_PAGE_DOWN   },
  { "UP",         KEY_UP_ARROW    },
  { "DOWN",       KEY_DOWN_ARROW  },
  { "LEFT",       KEY_LEFT_ARROW  },
  { "RIGHT",      KEY_RIGHT_ARROW },
  { "CAPSLOCK",   KEY_CAPS_LOCK   },
  { "F1",         KEY_F1          },
  { "F2",         KEY_F2          },
  { "F3",         KEY_F3          },
  { "F4",         KEY_F4          },
  { "F5",         KEY_F5          },
  { "F6",         KEY_F6          },
  { "F7",         KEY_F7          },
  { "F8",         KEY_F8          },
  { "F9",         KEY_F9          },
  { "F10",        KEY_F10         },
  { "F11",        KEY_F11         },
  { "F12",        KEY_F12         },
  { "SPACE",      ' '             },
  { "NONE",       0x00            },
};

const uint8_t NUM_ALIASES = sizeof(KEY_ALIASES) / sizeof(KEY_ALIASES[0]);

// ─── Globals ──────────────────────────────────────────────────────────────────

PCF8575 pcf8575(PCF_ADDR);

volatile bool keyPressed = false;
volatile unsigned long lastInterruptTime = 0;
const unsigned long debounceDelay = 50;

// In-RAM keymap: [button index][key slot] — loaded from EEPROM at boot
uint8_t keymap[NUM_KEYS][MAX_COMBO_KEYS];

void updateHeartbeatLED(int pin, float freq, int minB, int maxB) {
  static unsigned long startTime = millis();

  unsigned long currentTime = millis();
  float t = (currentTime - startTime) / 1000.0;

  float wave = sin(2 * PI * freq * t);
  float normalized = (wave + 1.0) / 2.0;

  int brightness = minB + normalized * (maxB - minB);
  analogWrite(pin, brightness);
}

// ─── EEPROM helpers ───────────────────────────────────────────────────────────

void loadKeymapFromEEPROM() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR)   != EEPROM_MAGIC ||
      EEPROM.read(EEPROM_VERSION_ADDR) != EEPROM_VERSION) {
    // First boot — write defaults (bare ASCII characters 1-9, *, 0, #, A-D)
    Serial.println(F("[EEPROM] Not initialised — writing defaults"));

    const char defaults[NUM_KEYS] = {
      '1','2','3','A',
      '4','5','6','B',
      '7','8','9','C',
      '*','0','#','D'
    };

    for (int i = 0; i < NUM_KEYS; i++) {
      keymap[i][0] = (uint8_t)defaults[i];
      for (int j = 1; j < MAX_COMBO_KEYS; j++) keymap[i][j] = 0x00;
    }
    saveKeymapToEEPROM();
    return;
  }

  int addr = EEPROM_DATA_ADDR;
  for (int i = 0; i < NUM_KEYS; i++)
    for (int j = 0; j < MAX_COMBO_KEYS; j++)
      keymap[i][j] = EEPROM.read(addr++);

  Serial.println(F("[EEPROM] Keymap loaded"));
}

void saveKeymapToEEPROM() {
  EEPROM.update(EEPROM_MAGIC_ADDR,   EEPROM_MAGIC);
  EEPROM.update(EEPROM_VERSION_ADDR, EEPROM_VERSION);

  int addr = EEPROM_DATA_ADDR;
  for (int i = 0; i < NUM_KEYS; i++)
    for (int j = 0; j < MAX_COMBO_KEYS; j++)
      EEPROM.update(addr++, keymap[i][j]);   // update() only writes if changed → saves write cycles

  Serial.println(F("[EEPROM] Keymap saved"));
}

// ─── Key-name parsing ─────────────────────────────────────────────────────────

// Convert a token like "CTRL", "A", "F5", "SHIFT" to a HID keycode.
// Returns 0x00 if unrecognised.
uint8_t tokenToKeycode(const char* token) {
  // Single printable ASCII character?
  if (token[1] == '\0' && token[0] >= 0x20 && token[0] <= 0x7E)
    return (uint8_t)token[0];

  // Look up in alias table (already uppercased by caller)
  for (uint8_t i = 0; i < NUM_ALIASES; i++)
    if (strcmp(token, KEY_ALIASES[i].name) == 0)
      return KEY_ALIASES[i].code;

  return 0x00;  // unknown
}

// Parse a combo string like "CTRL+SHIFT+I" into up to MAX_COMBO_KEYS keycodes.
// Returns the number of valid keys parsed.
uint8_t parseCombo(const char* combo, uint8_t* out) {
  char buf[64];
  strncpy(buf, combo, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  // Uppercase the whole buffer for alias matching
  for (int i = 0; buf[i]; i++) buf[i] = toupper((unsigned char)buf[i]);

  uint8_t count = 0;
  char* token = strtok(buf, "+");
  while (token && count < MAX_COMBO_KEYS) {
    // Strip leading/trailing spaces
    while (*token == ' ') token++;
    char* end = token + strlen(token) - 1;
    while (end > token && *end == ' ') *end-- = '\0';

    uint8_t code = tokenToKeycode(token);
    if (code != 0x00) {
      out[count++] = code;
    } else {
      Serial.print(F("[Parse] Unknown token: "));
      Serial.println(token);
    }
    token = strtok(nullptr, "+");
  }
  return count;
}

// ─── Serial command handler ───────────────────────────────────────────────────
//
// Commands (sent with newline terminator):
//
//   SET <pin> <combo>    — assign a combo to a pin (0-15)
//                          e.g.  SET 0 CTRL+ALT+DEL
//                          e.g.  SET 3 A
//                          e.g.  SET 14 CTRL+SHIFT+I
//
//   GET <pin>            — print the current mapping for a pin
//
//   LIST                 — print all 16 mappings
//
//   SAVE                 — persist current RAM keymap to EEPROM
//
//   LOAD                 — reload keymap from EEPROM (discards unsaved changes)
//
//   RESET                — restore factory defaults and save to EEPROM

void printPinMapping(uint8_t pin) {
  Serial.print(F("  P"));
  Serial.print(pin);
  Serial.print(F(": "));
  bool any = false;
  for (int j = 0; j < MAX_COMBO_KEYS; j++) {
    if (keymap[pin][j] == 0x00) continue;
    if (any) Serial.print('+');

    // Try to find a friendly name
    bool found = false;
    for (uint8_t k = 0; k < NUM_ALIASES; k++) {
      if (KEY_ALIASES[k].code == keymap[pin][j]) {
        Serial.print(KEY_ALIASES[k].name);
        found = true;
        break;
      }
    }
    if (!found) {
      // Fall back to the raw ASCII character if printable
      if (keymap[pin][j] >= 0x20 && keymap[pin][j] <= 0x7E)
        Serial.print((char)keymap[pin][j]);
      else {
        Serial.print(F("0x"));
        Serial.print(keymap[pin][j], HEX);
      }
    }
    any = true;
  }
  if (!any) Serial.print(F("<none>"));
  Serial.println();
}

void handleSerialCommand(const char* line) {
  char buf[80];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  // Tokenise on first space only
  char* cmd = strtok(buf, " ");
  if (!cmd) return;

  // Uppercase the command word
  for (int i = 0; cmd[i]; i++) cmd[i] = toupper((unsigned char)cmd[i]);

  // ── SET <pin> <combo> ──
  if (strcmp(cmd, "SET") == 0) {
    char* pinStr   = strtok(nullptr, " ");
    char* comboStr = strtok(nullptr, "");   // rest of line
    if (!pinStr || !comboStr) {
      Serial.println(F("[ERR] Usage: SET <pin 0-15> <combo>  e.g. SET 0 CTRL+ALT+DEL"));
      return;
    }
    int pin = atoi(pinStr);
    if (pin < 0 || pin >= NUM_KEYS) {
      Serial.println(F("[ERR] Pin must be 0-15"));
      return;
    }

    uint8_t codes[MAX_COMBO_KEYS] = {0};
    uint8_t n = parseCombo(comboStr, codes);
    if (n == 0) {
      Serial.println(F("[ERR] No valid keys found in combo string"));
      return;
    }

    for (int j = 0; j < MAX_COMBO_KEYS; j++)
      keymap[pin][j] = (j < n) ? codes[j] : 0x00;

    Serial.print(F("[OK] P"));
    Serial.print(pin);
    Serial.print(F(" set to: "));
    Serial.println(comboStr);
    Serial.println(F("  (type SAVE to persist)"));

  // ── GET <pin> ──
  } else if (strcmp(cmd, "GET") == 0) {
    char* pinStr = strtok(nullptr, " ");
    if (!pinStr) { Serial.println(F("[ERR] Usage: GET <pin 0-15>")); return; }
    int pin = atoi(pinStr);
    if (pin < 0 || pin >= NUM_KEYS) { Serial.println(F("[ERR] Pin must be 0-15")); return; }
    printPinMapping(pin);

  // ── LIST ──
  } else if (strcmp(cmd, "LIST") == 0) {
    Serial.println(F("Current keymap (RAM):"));
    for (int i = 0; i < NUM_KEYS; i++) printPinMapping(i);

  // ── SAVE ──
  } else if (strcmp(cmd, "SAVE") == 0) {
    saveKeymapToEEPROM();

  // ── LOAD ──
  } else if (strcmp(cmd, "LOAD") == 0) {
    loadKeymapFromEEPROM();
    Serial.println(F("[OK] Keymap reloaded from EEPROM"));

  // ── RESET ──
  } else if (strcmp(cmd, "RESET") == 0) {
    // Invalidate magic so loadKeymapFromEEPROM() rewrites defaults
    EEPROM.write(EEPROM_MAGIC_ADDR, 0x00);
    loadKeymapFromEEPROM();
    Serial.println(F("[OK] Factory defaults restored and saved"));

  } else {
    Serial.println(F("[ERR] Unknown command. Available: SET, GET, LIST, SAVE, LOAD, RESET"));
  }
}

// ─── Interrupt ────────────────────────────────────────────────────────────────

void handleInterrupt() {
  unsigned long now = millis();
  if (now - lastInterruptTime > debounceDelay) {
    keyPressed = true;
    lastInterruptTime = now;
  }
}

// ─── Setup ────────────────────────────────────────────────────────────────────

void setup() {

  pinMode(ledPin, OUTPUT);
  startTime = millis();

  

  Serial.begin(115200);
  delay(3000);
  Serial.println(F("=== Pro Micro HID Keypad ==="));
  Serial.println(F("Commands: SET <pin> <combo> | GET <pin> | LIST | SAVE | LOAD | RESET"));

  Keyboard.begin();

  for (int i = 0; i < NUM_KEYS; i++)
    pcf8575.pinMode(i, INPUT_PULLUP);

  pinMode(INT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(INT_PIN), handleInterrupt, FALLING);

  if (pcf8575.begin()) Serial.println(F("[OK] PCF8575 ready"));
  else                  Serial.println(F("[ERR] PCF8575 init failed!"));

  loadKeymapFromEEPROM();
  Serial.println(F("Ready."));
}

// ─── Main loop ────────────────────────────────────────────────────────────────

static char serialBuf[80];
static uint8_t serialLen = 0;

void loop() {
  // ── Serial command reader (newline-terminated) ──
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLen > 0) {
        serialBuf[serialLen] = '\0';
        handleSerialCommand(serialBuf);
        serialLen = 0;
      }
    } else if (serialLen < sizeof(serialBuf) - 1) {
      serialBuf[serialLen++] = c;
    }
  }

  // ── Key scan ──
  if (keyPressed) {
    scanKeypad();
    keyPressed = false;
  }

  updateHeartbeatLED(ledPin, frequency, minBrightness, maxBrightness);
}

// ─── Keypad scan & HID send ───────────────────────────────────────────────────

void scanKeypad() {
  PCF8575::DigitalInput values = pcf8575.digitalReadAll();

  bool pinStates[NUM_KEYS] = {
    values.p0,  values.p1,  values.p2,  values.p3,
    values.p4,  values.p5,  values.p6,  values.p7,
    values.p8,  values.p9,  values.p10, values.p11,
    values.p12, values.p13, values.p14, values.p15
  };

  for (int i = 0; i < NUM_KEYS; i++) {
    if (pinStates[i] != LOW) continue;

    Serial.print(F("[KEY] P"));
    Serial.print(i);
    Serial.print(F(" -> "));

    // Press all keys in the combo
    for (int j = 0; j < MAX_COMBO_KEYS; j++) {
      if (keymap[i][j] == 0x00) continue;
      Keyboard.press(keymap[i][j]);
      Serial.print(keymap[i][j], HEX);
      Serial.print(' ');
    }

    delay(10);
    Keyboard.releaseAll();   // Release everything at once
    Serial.println();
  }
}