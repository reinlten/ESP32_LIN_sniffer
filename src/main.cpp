#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <stdio.h>
#include <string.h>

// LIN bus level after divider is read on this pin.
constexpr uint8_t LIN_RX_PIN = 4;
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t LIN_BAUD = 19200;
constexpr uint8_t TARGET_LIN_ID = 0x04;
constexpr uint8_t BATTERY_ADC_PIN = 34;

// Battery divider: VIN -- 22k -- ADC -- 5.6k -- GND
constexpr float BATTERY_R_TOP_KOHM = 21.7f;
constexpr float BATTERY_R_BOTTOM_KOHM = 5.5f;
constexpr float BATTERY_DIVIDER_FACTOR =
  (BATTERY_R_TOP_KOHM + BATTERY_R_BOTTOM_KOHM) / BATTERY_R_BOTTOM_KOHM;

// ESP32 VSPI default pins: SCK=18, MISO=19, MOSI=23, CS configurable.
constexpr uint8_t SD_CS_PIN = 5;
constexpr uint32_t WINDOW_MS = 2000;
constexpr uint16_t MAX_LOG_FILE_INDEX = 9999;
constexpr uint8_t BATTERY_AVG_SAMPLES = 8;

constexpr uint32_t BIT_US = 1000000UL / LIN_BAUD;
constexpr uint32_t BREAK_MIN_US = BIT_US * 13;       // LIN break >= 13 dominant bits
constexpr uint32_t BREAK_MAX_US = BIT_US * 40;
constexpr uint32_t START_TIMEOUT_US = BIT_US * 20;
constexpr uint8_t MAX_LIN_BYTES_AFTER_SYNC = 10;     // PID + 8 data + checksum

struct LinFrame {
  uint8_t pid = 0;
  uint8_t data[8]{};
  uint8_t dataLen = 0;
  uint8_t checksum = 0;
  bool checksumPresent = false;
};

bool g_sdOk = false;
uint32_t g_windowStartMs = 0;
bool g_linSeenInWindow = false;
bool g_lightSeenInWindow = false;
uint8_t g_lastLightB0 = 0;
uint8_t g_lastLightB1 = 0;
uint32_t g_logLineCounter = 0;
char g_logFilePath[20] = "/lin_0000.txt";

void waitUntil(uint32_t targetUs) {
  while ((int32_t)(micros() - targetUs) < 0) {
    // busy wait for precise sampling points
  }
}

uint8_t linIdFromPid(uint8_t pid) {
  return pid & 0x3F;
}

bool linPidParityValid(uint8_t pid) {
  const uint8_t id0 = (pid >> 0) & 0x01;
  const uint8_t id1 = (pid >> 1) & 0x01;
  const uint8_t id2 = (pid >> 2) & 0x01;
  const uint8_t id3 = (pid >> 3) & 0x01;
  const uint8_t id4 = (pid >> 4) & 0x01;
  const uint8_t id5 = (pid >> 5) & 0x01;

  const uint8_t p0 = id0 ^ id1 ^ id2 ^ id4;
  const uint8_t p1 = ~(id1 ^ id3 ^ id4 ^ id5) & 0x01;

  return (((pid >> 6) & 0x01) == p0) && (((pid >> 7) & 0x01) == p1);
}

uint8_t linChecksum(const uint8_t* data, uint8_t len, bool enhanced, uint8_t pid) {
  uint16_t sum = 0;
  if (enhanced) {
    sum += pid;
  }

  for (uint8_t i = 0; i < len; ++i) {
    sum += data[i];
    if (sum > 0xFF) {
      sum = (sum & 0xFF) + 1;
    }
  }

  return static_cast<uint8_t>(~sum);
}

bool waitForStartBit(uint32_t timeoutUs) {
  const uint32_t t0 = micros();
  while (digitalRead(LIN_RX_PIN) == HIGH) {
    if ((micros() - t0) > timeoutUs) {
      return false;
    }
  }
  return true;
}

bool readLinByte(uint8_t& outByte, uint32_t startTimeoutUs) {
  if (!waitForStartBit(startTimeoutUs)) {
    return false;
  }

  const uint32_t startUs = micros();
  uint8_t value = 0;

  // Start bit should still be dominant at half bit time.
  waitUntil(startUs + (BIT_US / 2));
  if (digitalRead(LIN_RX_PIN) != LOW) {
    return false;
  }

  // Sample each data bit in the center of its bit time.
  for (uint8_t bit = 0; bit < 8; ++bit) {
    waitUntil(startUs + BIT_US + (BIT_US / 2) + (BIT_US * bit));
    const uint8_t level = static_cast<uint8_t>(digitalRead(LIN_RX_PIN));
    value |= (level << bit);
  }

  // Stop bit should be recessive.
  waitUntil(startUs + BIT_US + (BIT_US * 8) + (BIT_US / 2));
  if (digitalRead(LIN_RX_PIN) != HIGH) {
    return false;
  }

  outByte = value;
  return true;
}

bool detectBreak(uint32_t& breakLenUs) {
  if (digitalRead(LIN_RX_PIN) == HIGH) {
    return false;
  }

  const uint32_t lowStart = micros();
  while (digitalRead(LIN_RX_PIN) == LOW) {
    if ((micros() - lowStart) > BREAK_MAX_US) {
      return false;
    }
  }

  breakLenUs = micros() - lowStart;
  if (breakLenUs < BREAK_MIN_US) {
    return false;
  }

  return true;
}

bool captureLinFrame(LinFrame& frame, uint32_t& breakLenUs, uint8_t& syncByte) {
  if (!detectBreak(breakLenUs)) {
    return false;
  }

  if (!readLinByte(syncByte, START_TIMEOUT_US)) {
    return false;
  }

  uint8_t bytes[MAX_LIN_BYTES_AFTER_SYNC]{};
  uint8_t count = 0;

  while (count < MAX_LIN_BYTES_AFTER_SYNC) {
    uint8_t b = 0;
    if (!readLinByte(b, BIT_US * 20)) {
      break;
    }
    bytes[count++] = b;
  }

  if (count == 0) {
    return false;
  }

  frame.pid = bytes[0];
  if (count >= 2) {
    frame.checksumPresent = true;
    frame.checksum = bytes[count - 1];
    frame.dataLen = static_cast<uint8_t>(count - 2);
    for (uint8_t i = 0; i < frame.dataLen; ++i) {
      frame.data[i] = bytes[i + 1];
    }
  } else {
    frame.checksumPresent = false;
    frame.dataLen = 0;
  }

  return true;
}

void printHexByte(uint8_t value) {
  if (value < 0x10) {
    Serial.print('0');
  }
  Serial.print(value, HEX);
}

void printFrame(const LinFrame& frame, uint32_t breakLenUs, uint8_t syncByte) {
  Serial.print('[');
  Serial.print(millis());
  Serial.print(" ms] BREAK=");
  Serial.print(breakLenUs);
  Serial.print(" us SYNC=0x");
  printHexByte(syncByte);

  Serial.print(" PID=0x");
  printHexByte(frame.pid);
  Serial.print(" ID=0x");
  printHexByte(linIdFromPid(frame.pid));
  Serial.print(" PARITY=");
  Serial.print(linPidParityValid(frame.pid) ? "OK" : "BAD");

  Serial.print(" DATA(");
  Serial.print(frame.dataLen);
  Serial.print(")=");
  if (frame.dataLen == 0) {
    Serial.print("-");
  } else {
    for (uint8_t i = 0; i < frame.dataLen; ++i) {
      if (i) {
        Serial.print(' ');
      }
      printHexByte(frame.data[i]);
    }
  }

  if (frame.checksumPresent) {
    const uint8_t classic = linChecksum(frame.data, frame.dataLen, false, frame.pid);
    const uint8_t enhanced = linChecksum(frame.data, frame.dataLen, true, frame.pid);
    const bool classicOk = (classic == frame.checksum);
    const bool enhancedOk = (enhanced == frame.checksum);

    Serial.print(" CHK=0x");
    printHexByte(frame.checksum);
    Serial.print(" (");
    if (classicOk && enhancedOk) {
      Serial.print("OK classic/enhanced");
    } else if (classicOk) {
      Serial.print("OK classic");
    } else if (enhancedOk) {
      Serial.print("OK enhanced");
    } else {
      Serial.print("BAD");
    }
    Serial.print(')');
  }

  Serial.println();
}

void hexByteToText(uint8_t value, char* out) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  out[0] = HEX_DIGITS[(value >> 4) & 0x0F];
  out[1] = HEX_DIGITS[value & 0x0F];
  out[2] = '\0';
}

const char* interpretLightCode(uint8_t b0, uint8_t b1) {
  if (b0 == 0xEF && b1 == 0xCA) return "Plinks";
  if (b0 == 0xED && b1 == 0xCB) return "Prechts";
  if (b0 == 0xEF && b1 == 0xC9) return "0";
  if (b0 == 0x2F && b1 == 0xCB) return "Auto";
  if (b0 == 0xE7 && b1 == 0xCB) return "St";
  if (b0 == 0xE6 && b1 == 0xCB) return "St_N1";
  if (b0 == 0xE2 && b1 == 0xCB) return "St_N1_N2";
  if (b0 == 0x47 && b1 == 0xCB) return "Abbl";
  if (b0 == 0x46 && b1 == 0xCB) return "Abbl_N1";
  if (b0 == 0x42 && b1 == 0xCB) return "Abbl_N1_N2";
  if (b0 == 0xEF && b1 == 0xC2) return "Plinks (Entsp.)";
  if (b0 == 0xED && b1 == 0xC3) return "Prechts (Entsp.)";
  if (b0 == 0xEF && b1 == 0xC1) return "0 (Entsp.)";
  if (b0 == 0x2F && b1 == 0xC3) return "Auto (Entsp.)";
  if (b0 == 0xE7 && b1 == 0xC3) return "St (Entsp.)";
  if (b0 == 0xE6 && b1 == 0xC3) return "St_N1 (Entsp.)";
  if (b0 == 0xE2 && b1 == 0xC3) return "St_N1_N2 (Entsp.)";
  if (b0 == 0x47 && b1 == 0xC3) return "Abbl (Entsp.)";
  if (b0 == 0x46 && b1 == 0xC3) return "Abbl_N1 (Entsp.)";
  if (b0 == 0x42 && b1 == 0xC3) return "Abbl_N1_N2 (Entsp.) [?? KI GEN!]";
  return "Unbekannt";
}

void appendLogLine(bool linPresent, bool lightPresent, uint8_t b0, uint8_t b1) {
  if (!g_sdOk) {
    return;
  }

  File file = SD.open(g_logFilePath, FILE_APPEND);
  if (!file) {
    Serial.println("SD Fehler: Datei konnte nicht geoeffnet werden");
    return;
  }

  char h0[3];
  char h1[3];
  hexByteToText(b0, h0);
  hexByteToText(b1, h1);

  ++g_logLineCounter;
  file.print(g_logLineCounter);
  file.print(' ');
  file.print(linPresent ? 1 : 0);
  file.print(' ');
  file.print(lightPresent ? 1 : 0);
  file.print(' ');
  file.print(h0);
  file.print(' ');
  file.print(h1);
  file.print(' ');
  file.println(interpretLightCode(b0, b1));
  file.close();
}

float readBatteryVoltage() {
  uint32_t adcMvSum = 0;
  for (uint8_t i = 0; i < BATTERY_AVG_SAMPLES; ++i) {
    adcMvSum += analogReadMilliVolts(BATTERY_ADC_PIN);
  }

  const float adcMvAvg = static_cast<float>(adcMvSum) / BATTERY_AVG_SAMPLES;
  const float adcV = adcMvAvg / 1000.0f;
  return adcV * BATTERY_DIVIDER_FACTOR;
}

void appendWindowLogLine(bool linPresent,
                         bool lightPresent,
                         bool hasLightData,
                         uint8_t b0,
                         uint8_t b1,
                         float batteryV) {
  if (!g_sdOk) {
    return;
  }

  File file = SD.open(g_logFilePath, FILE_APPEND);
  if (!file) {
    Serial.println("SD Fehler: Datei konnte nicht geoeffnet werden");
    return;
  }

  ++g_logLineCounter;
  file.print(g_logLineCounter);
  file.print(' ');
  file.print(linPresent ? 1 : 0);
  file.print(' ');
  file.print(lightPresent ? 1 : 0);
  file.print(' ');

  if (hasLightData) {
    char h0[3];
    char h1[3];
    hexByteToText(b0, h0);
    hexByteToText(b1, h1);
    file.print(h0);
    file.print(' ');
    file.print(h1);
    file.print(' ');
    file.print(interpretLightCode(b0, b1));
  } else {
    file.print("-- -- --");
  }

  file.print(' ');
  file.println(batteryV, 2);
  file.close();
}

void ensureLogHeader() {
  if (!g_sdOk) {
    return;
  }

  for (uint16_t idx = 1; idx <= MAX_LOG_FILE_INDEX; ++idx) {
    char candidate[20];
    snprintf(candidate, sizeof(candidate), "/lin_%04u.txt", idx);

    if (SD.exists(candidate)) {
      continue;
    }

    strncpy(g_logFilePath, candidate, sizeof(g_logFilePath) - 1);
    g_logFilePath[sizeof(g_logFilePath) - 1] = '\0';

    File file = SD.open(g_logFilePath, FILE_WRITE);
    if (!file) {
      Serial.println("SD Fehler: neue Logdatei konnte nicht erstellt werden");
      return;
    }

    file.println("Nr LIN Licht Data0 Data1 Interpr Batt_V");
    file.close();

    g_logLineCounter = 0;
    Serial.print("Logdatei: ");
    Serial.println(g_logFilePath);
    return;
  }

  Serial.println("SD Fehler: keine freie Logdatei mehr gefunden");
  g_sdOk = false;
}

void processFrameForWindow(const LinFrame& frame) {
  g_linSeenInWindow = true;

  const uint8_t id = linIdFromPid(frame.pid);
  if (id == TARGET_LIN_ID && frame.dataLen >= 2) {
    g_lightSeenInWindow = true;
    g_lastLightB0 = frame.data[0];
    g_lastLightB1 = frame.data[1];
  }
}

void processWindowIfDue() {
  const uint32_t now = millis();
  if ((now - g_windowStartMs) < WINDOW_MS) {
    return;
  }

  const float batteryV = readBatteryVoltage();

  const bool linPresent = g_linSeenInWindow;
  const bool lightPresent = g_lightSeenInWindow;
  const bool hasLightData = g_linSeenInWindow && g_lightSeenInWindow;

  appendWindowLogLine(linPresent,
                      lightPresent,
                      hasLightData,
                      g_lastLightB0,
                      g_lastLightB1,
                      batteryV);

  Serial.print("LOG: LIN=");
  Serial.print(linPresent ? 1 : 0);
  Serial.print(" Licht=");
  Serial.print(lightPresent ? 1 : 0);
  Serial.print(" Data=");
  if (hasLightData) {
    printHexByte(g_lastLightB0);
    Serial.print(' ');
    printHexByte(g_lastLightB1);
    Serial.print(" Interpr=");
    Serial.print(interpretLightCode(g_lastLightB0, g_lastLightB1));
  } else {
    Serial.print("-- -- Interpr=--");
  }
  Serial.print(" Batt_V=");
  Serial.println(batteryV, 2);

  g_linSeenInWindow = false;
  g_lightSeenInWindow = false;
  g_windowStartMs = now;
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  pinMode(LIN_RX_PIN, INPUT);

  Serial.println();
  Serial.println("ESP32 LIN sniffer started");
  Serial.print("RX pin: GPIO ");
  Serial.println(LIN_RX_PIN);
  Serial.print("LIN baud: ");
  Serial.println(LIN_BAUD);
  Serial.print("Target ID: 0x");
  printHexByte(TARGET_LIN_ID);
  Serial.println();
  Serial.print("Battery ADC pin: GPIO ");
  Serial.println(BATTERY_ADC_PIN);
  Serial.print("Battery divider factor: ");
  Serial.println(BATTERY_DIVIDER_FACTOR, 4);
  Serial.print("Battery average samples: ");
  Serial.println(BATTERY_AVG_SAMPLES);

  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);

  g_sdOk = SD.begin(SD_CS_PIN);
  if (g_sdOk) {
    Serial.print("SD bereit an CS GPIO ");
    Serial.println(SD_CS_PIN);
    ensureLogHeader();
  } else {
    Serial.println("SD Init fehlgeschlagen, Logging deaktiviert");
  }

  g_windowStartMs = millis();
  Serial.println("Mode: alle 2s loggen, inkl. LIN-Status und Batteriespannung");
}

void loop() {
  LinFrame frame;
  uint32_t breakLenUs = 0;
  uint8_t syncByte = 0;

  if (captureLinFrame(frame, breakLenUs, syncByte)) {
    // Valid LIN frames should start with sync byte 0x55 after break.
    if (syncByte == 0x55) {
      processFrameForWindow(frame);
    }
  }

  processWindowIfDue();
}