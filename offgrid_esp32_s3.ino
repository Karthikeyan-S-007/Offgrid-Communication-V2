#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>              // Sandeep Mistry LoRa library
#include <U8g2lib.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// =====================================================
// ESP32 OFF-GRID COMMUNICATION
// OLED: Waveshare 2.42in SSD1309, 128x64, software SPI
// LoRa: SX1278 RA-02, 433 MHz
// SOS: persistent badge, SOS packet, and on-demand heartbeat/RSSI monitoring.
// =====================================================

// ESP32-S3-WROOM-1 MCN16R8 pin allocation
#define OLED_CLK 5
#define OLED_DIN 4
#define OLED_CS  6
#define OLED_DC  7
#define OLED_RST 8
U8G2_SSD1309_128X64_NONAME0_F_4W_SW_SPI oled(
  U8G2_R0, OLED_CLK, OLED_DIN, OLED_CS, OLED_DC, OLED_RST
);

// Buttons (existing wiring; each button connects GPIO to GND)
#define BTN_UP     10
#define BTN_DOWN   11
#define BTN_LEFT   12
#define BTN_RIGHT  13
#define BTN_SELECT 14
#define BTN_BACK   15
#define BUZZER_PIN 40  // Active buzzer module

// LoRa pins (conflict-free with the OLED and buttons)
#define LORA_SCK  16
#define LORA_MISO 17
#define LORA_MOSI 18
#define LORA_SS   21
#define LORA_RST  38
#define LORA_DIO0 39
#define LORA_FREQ 433E6

// BLE UART service (ESP32-S3 does not support Bluetooth Classic SPP)
const char* BT_NAME = "OFFGRID-ESP32";
static const char* BLE_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* BLE_RX_UUID      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // phone writes here
static const char* BLE_TX_UUID      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // device notifies here
BLEServer* bleServer = nullptr;
BLECharacteristic* bleTxCharacteristic = nullptr;
volatile bool bleClientConnected = false;
bool bluetoothEnabled = false;
String bluetoothBuffer = "";
String bleIncomingBuffer = "";


// Menus
const char* menuItems[] = {"Bluetooth", "Custom Message", "SOS Message"};
const int MENU_COUNT = 3;
int selectedItem = 0;
enum Screen { MAIN_MENU, BLUETOOTH_MENU, CUSTOM_MESSAGE_MENU, SOS_MENU };
Screen currentScreen = MAIN_MENU;

// Radio state and display text
bool loraReady = false;
String nodeId = "NODE";
bool sosAlertActive = false;
bool sosAlertAcknowledged = false;
bool sosEnded = false;
bool sosOutgoingActive = false;
bool sosPaired = false;
bool sosConnectionLostReported = false;
String sosSessionId = "";
String pairedReceiverId = "";
unsigned long lastSosBroadcastMs = 0;
unsigned long lastSosHeartbeatMs = 0;
const unsigned long SOS_BROADCAST_INTERVAL_MS = 1000;
const unsigned long SOS_HEARTBEAT_INTERVAL_MS = 1000;
bool sosMonitoring = false;
String sosSenderId = "";
String sosMessage = "";
int sosRssi = -127;
unsigned long lastSosPingMs = 0;
unsigned long lastSosPongMs = 0;
const unsigned long SOS_PACKET_INTERVAL_MS = 1000;
const unsigned long SOS_TIMEOUT_MS = 5000;
String latestRXMessage = "";
String latestPhoneMessage = "";
String currentTXMessage = "";

// History contains LoRa messages only: RX or TX
const int MAX_MESSAGES = 30;
String messageHistory[MAX_MESSAGES];
bool messageIsRX[MAX_MESSAGES];
int messageCount = 0;
int historyOffset = 0; // 0 = newest
bool historyViewActive = false;

// Morse composer
String morseBuffer = "";
String morseMessage = "";
char decodedChar = '\0';
bool showInvalidMorse = false;

#define MENU_FONT  u8g2_font_6x10_tf
#define SMALL_FONT u8g2_font_5x7_tf

// Function declarations
void showMainMenu();
void openSelectedMenu();
void showBluetoothMenu();
void enableBluetooth();
void disableBluetooth();
void showCustomMessageMenu();
void showChatScreen();
void showMorseScreen();
void showHistoryEntry();
void showSOSMenu();
void showSOSReady();
void checkBluetooth();
void startBLE();
void bleSendLine(const String& line);
bool bleHasClient();
void processBluetoothMessage(String message);
void handleBluetoothMode();
void handleMorseMode();
void addLoRaHistory(String message, bool incoming);
void transmitLoRaMessage(String message);
void checkLoRaReceive();
void processLoRaReceivedMessage(String message);
void drawWrappedText(String text, int x, int y, int maxChars, int maxLines);
void drawSosBadge();
void drawSignalBars(int rssi);
void sendSosPacket(String packet);
void handleSosPackets(String packet);
void serviceSosMonitor();
void beepTone(unsigned int frequency, unsigned long durationMs);
void beepSosReceived();
void beepPaired();
void beepSosEnded();
void beepConnectionLost();
void stopOutgoingSOS();
String makeNodeId();
char decodeMorse(String code);
bool buttonPressed(int pin);

void setup() {
  Serial.begin(115200);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);

  oled.begin();
  nodeId = makeNodeId();
  showMainMenu();

  Serial.println("\n=== OFF-GRID COMMUNICATION ===");
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  LoRa.setSPIFrequency(4000000);

  Serial.println("Starting SX1278 at 433 MHz...");
  if (LoRa.begin(LORA_FREQ)) {
    loraReady = true;
    LoRa.setTxPower(17);             // within common RA-02 range; obey local regulations
    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(5);
    LoRa.enableCrc();
    Serial.println("LoRa initialized.");
  } else {
    loraReady = false;
    Serial.println("LoRa initialization failed. Check wiring, 3.3V supply, and antenna.");
  }
  Serial.println("BLE starts from the Bluetooth menu. Use a BLE UART terminal app.");
}

void loop() {
  checkBluetooth();
  checkLoRaReceive();

  if (currentScreen == MAIN_MENU) {
    if (buttonPressed(BTN_UP)) {
      selectedItem = (selectedItem - 1 + MENU_COUNT) % MENU_COUNT;
      showMainMenu();
    } else if (buttonPressed(BTN_DOWN)) {
      selectedItem = (selectedItem + 1) % MENU_COUNT;
      showMainMenu();
    } else if (buttonPressed(BTN_RIGHT) || buttonPressed(BTN_SELECT)) {
      openSelectedMenu();
    } else if (buttonPressed(BTN_BACK)) {
      showMainMenu();
    }
  } else if (currentScreen == BLUETOOTH_MENU) {
    if (buttonPressed(BTN_SELECT) || buttonPressed(BTN_RIGHT)) {
      if (!bluetoothEnabled) enableBluetooth();
      else disableBluetooth();
    } else if (buttonPressed(BTN_BACK)) {
      showMainMenu();
    }
  } else if (currentScreen == CUSTOM_MESSAGE_MENU) {
    if (bluetoothEnabled && bleHasClient()) handleBluetoothMode();
    else handleMorseMode();

    if (buttonPressed(BTN_BACK)) {
      historyViewActive = false;
      showMainMenu();
    }
  } else if (currentScreen == SOS_MENU) {
    if (buttonPressed(BTN_SELECT)) {
      if (!sosOutgoingActive && !sosAlertActive) showSOSReady();
      else if (sosAlertActive) {
        sosMonitoring = true;
        lastSosPingMs = 0;
        lastSosPongMs = 0;
        sosConnectionLostReported = false;
        showSOSMenu();
      }
    } else if (buttonPressed(BTN_RIGHT)) {
      if (sosOutgoingActive) stopOutgoingSOS();
      else if (sosMonitoring) { sosMonitoring = false; showSOSMenu(); }
    } else if (buttonPressed(BTN_LEFT)) {
      if (sosAlertActive) {
        if (sosAlertAcknowledged && (sosEnded || sosConnectionLostReported)) {
          sosAlertActive = false; sosAlertAcknowledged = false; sosEnded = false;
          sosSenderId = ""; sosSessionId = ""; sosMonitoring = false;
        } else {
          sosAlertAcknowledged = true;
        }
        showSOSMenu();
      }
    } else if (buttonPressed(BTN_BACK)) {
      if (sosOutgoingActive) stopOutgoingSOS();
      sosMonitoring = false;
      showMainMenu();
    }
  }
  serviceSosMonitor();
}

// Phone BLE UART app should append newline after each message.
void checkBluetooth() {
  if (!bluetoothEnabled) return;
  while (bleIncomingBuffer.length() > 0) {
    char c = bleIncomingBuffer[0];
    bleIncomingBuffer.remove(0, 1);
    if (c == '\n' || c == '\r') {
      if (bluetoothBuffer.length() > 0) {
        String completed = bluetoothBuffer;
        bluetoothBuffer = "";
        processBluetoothMessage(completed);
      }
    } else if (bluetoothBuffer.length() < 120) {
      bluetoothBuffer += c;
    }
  }
}

void processBluetoothMessage(String message) {
  message.trim();
  if (message.length() == 0) return;
  latestPhoneMessage = message;
  Serial.print("Bluetooth input (no RX/TX prefix): ");
  Serial.println(message);
  if (currentScreen == CUSTOM_MESSAGE_MENU && bluetoothEnabled && bleHasClient()) {
    historyViewActive = false;
    showChatScreen();
  }
  // Automatically send to LoRa; SELECT is not needed.
  transmitLoRaMessage(message);
}

bool buttonPressed(int pin) {
  if (digitalRead(pin) == LOW) {
    delay(35);
    if (digitalRead(pin) == LOW) {
      while (digitalRead(pin) == LOW) delay(3);
      return true;
    }
  }
  return false;
}

class OffgridBleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { bleClientConnected = true; }
  void onDisconnect(BLEServer*) override {
    bleClientConnected = false;
    if (bluetoothEnabled) BLEDevice::startAdvertising();
  }
};
class OffgridBleRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    String value = characteristic->getValue();
    for (size_t i = 0; i < value.length(); ++i) {
      if (bleIncomingBuffer.length() < 240) bleIncomingBuffer += value[i];
    }
  }
};

void startBLE() {
  static bool initialized = false;
  if (initialized) {
    BLEDevice::startAdvertising();
    return;
  }
  BLEDevice::init(BT_NAME);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new OffgridBleServerCallbacks());
  BLEService* service = bleServer->createService(BLE_SERVICE_UUID);
  bleTxCharacteristic = service->createCharacteristic(BLE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  bleTxCharacteristic->addDescriptor(new BLE2902());
  BLECharacteristic* rx = service->createCharacteristic(BLE_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new OffgridBleRxCallbacks());
  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  initialized = true;
  Serial.println("BLE UART ready. Name: OFFGRID-ESP32");
}

bool bleHasClient() { return bluetoothEnabled && bleClientConnected; }

void bleSendLine(const String& line) {
  if (!bleHasClient() || bleTxCharacteristic == nullptr) return;
  String payload = line + "\n";
  bleTxCharacteristic->setValue(payload.c_str());
  bleTxCharacteristic->notify();
}

void showMainMenu() {
  currentScreen = MAIN_MENU;
  oled.clearBuffer();
  oled.setFont(MENU_FONT);
  oled.drawStr(20, 10, "OFF-GRID COMMS");
  oled.drawLine(0, 14, 127, 14);
  oled.drawStr(20, 29, "Bluetooth");
  oled.drawStr(20, 43, "Custom Message");
  oled.drawStr(20, 57, "SOS Message");
  int y = selectedItem == 0 ? 29 : (selectedItem == 1 ? 43 : 57);
  oled.drawStr(5, y, ">");
  drawSosBadge();
  oled.sendBuffer();
}

void openSelectedMenu() {
  if (selectedItem == 0) {
    currentScreen = BLUETOOTH_MENU;
    showBluetoothMenu();
  } else if (selectedItem == 1) {
    currentScreen = CUSTOM_MESSAGE_MENU;
    historyOffset = 0;
    historyViewActive = false;
    showCustomMessageMenu();
  } else {
    currentScreen = SOS_MENU;
    sosMonitoring = sosAlertActive;
    lastSosPingMs = 0;
    lastSosPongMs = millis();
    showSOSMenu();
  }
}

void showBluetoothMenu() {
  oled.clearBuffer();
  oled.setFont(MENU_FONT);
  oled.drawStr(35, 10, "BLUETOOTH");
  oled.drawLine(0, 14, 127, 14);
  if (bluetoothEnabled) {
    oled.drawStr(5, 30, "Status: ENABLED");
    oled.drawStr(5, 43, bleHasClient() ? "Phone: CONNECTED" : "Phone: WAITING");
    oled.drawStr(5, 58, "> Disconnect");
  } else {
    oled.drawStr(5, 30, "Status: DISABLED");
    oled.drawStr(5, 48, "> Enable Bluetooth");
  }
  drawSosBadge();
  oled.sendBuffer();
}

void enableBluetooth() {
  Serial.println("Starting BLE UART service...");
  startBLE();
  bluetoothEnabled = true;
  bluetoothBuffer = "";
  showBluetoothMenu();
}

void disableBluetooth() {
  // BLE advertising is stopped logically; service remains initialized to avoid
  // unsafe controller teardown. Re-enable by selecting Enable Bluetooth.
  bluetoothEnabled = false;
  bluetoothBuffer = "";
  if (BLEDevice::getAdvertising()) BLEDevice::getAdvertising()->stop();
  Serial.println("BLE disabled (advertising stopped).");
  showBluetoothMenu();
}

void showCustomMessageMenu() {
  if (bluetoothEnabled && bleHasClient()) showChatScreen();
  else showMorseScreen();
}

// Bluetooth screen: phone input has no prefix; history uses LoRa RX/TX labels.
void showChatScreen() {
  oled.clearBuffer();
  oled.setFont(MENU_FONT);
  oled.drawStr(28, 9, "OFF-GRID CHAT");
  oled.drawLine(0, 12, 127, 12);
  oled.setFont(SMALL_FONT);
  oled.drawStr(2, 20, "PHONE INPUT:");
  if (latestPhoneMessage.length() > 0) drawWrappedText(latestPhoneMessage, 2, 28, 21, 2);
  else oled.drawStr(2, 29, "Waiting for phone text...");
  oled.drawLine(0, 44, 127, 44);
  if (messageCount > 0) {
    int idx = messageCount - 1;
    String entry = String(messageIsRX[idx] ? "RX: " : "TX: ") + messageHistory[idx];
    oled.drawStr(2, 53, entry.substring(0, 21).c_str());
  } else {
    oled.drawStr(2, 53, loraReady ? "LoRa ready; no history" : "LoRa: NOT READY");
  }
  oled.drawStr(2, 62, "UP/DN: History  BACK: Menu");
  drawSosBadge();
  oled.sendBuffer();
}

void handleBluetoothMode() {
  if (buttonPressed(BTN_UP)) {
    if (historyOffset < messageCount - 1) historyOffset++;
    historyViewActive = true;
    showHistoryEntry();
  } else if (buttonPressed(BTN_DOWN)) {
    if (historyOffset > 0) historyOffset--;
    if (historyOffset == 0) {
      historyViewActive = false;
      showChatScreen();
    } else {
      historyViewActive = true;
      showHistoryEntry();
    }
  }
  // No SELECT action for sending a Bluetooth message.
}

// Morse: LEFT dot, RIGHT dash. SELECT once decodes; SELECT again commits.
// With an empty code buffer, SELECT sends the completed draft.
void handleMorseMode() {
  if (historyViewActive) {
    if (buttonPressed(BTN_LEFT) || buttonPressed(BTN_RIGHT) || buttonPressed(BTN_SELECT)) {
      historyViewActive = false;
      showMorseScreen();
    } else if (buttonPressed(BTN_UP)) {
      if (historyOffset < messageCount - 1) historyOffset++;
      showHistoryEntry();
    } else if (buttonPressed(BTN_DOWN)) {
      if (historyOffset > 0) historyOffset--;
      if (historyOffset == 0) {
        historyViewActive = false;
        showMorseScreen();
      } else showHistoryEntry();
    }
    return;
  }

  if (buttonPressed(BTN_LEFT)) {
    if (morseBuffer.length() < 5) morseBuffer += ".";
    decodedChar = '\0'; showInvalidMorse = false; showMorseScreen();
  } else if (buttonPressed(BTN_RIGHT)) {
    if (morseBuffer.length() < 5) morseBuffer += "-";
    decodedChar = '\0'; showInvalidMorse = false; showMorseScreen();
  } else if (buttonPressed(BTN_UP)) {
    if (messageCount > 0) {
      historyOffset = min(historyOffset + 1, messageCount - 1);
      historyViewActive = true;
      showHistoryEntry();
    }
  } else if (buttonPressed(BTN_DOWN)) {
    if (historyOffset > 0) historyOffset--;
    if (historyOffset == 0) showMorseScreen();
    else { historyViewActive = true; showHistoryEntry(); }
  } else if (buttonPressed(BTN_SELECT)) {
    if (morseBuffer.length() > 0 && decodedChar == '\0') {
      decodedChar = decodeMorse(morseBuffer);
      showInvalidMorse = (decodedChar == '#');
      showMorseScreen();
    } else if (decodedChar != '\0') {
      if (decodedChar != '#' && morseMessage.length() < 100) morseMessage += decodedChar;
      morseBuffer = ""; decodedChar = '\0'; showInvalidMorse = false;
      showMorseScreen();
    } else if (morseBuffer.length() == 0 && morseMessage.length() > 0) {
      String completed = morseMessage;
      transmitLoRaMessage(completed);
      morseMessage = ""; morseBuffer = ""; decodedChar = '\0';
      showMorseScreen();
    } else {
      showMorseScreen();
    }
  }
}

// Morse display: top is latest RX; bottom is the current draft.
// RX redraws never clear morseBuffer, decodedChar, or morseMessage.
void showMorseScreen() {
  oled.clearBuffer();
  oled.setFont(MENU_FONT);
  oled.drawStr(35, 9, "MORSE CHAT");
  oled.drawLine(0, 12, 127, 12);
  oled.setFont(SMALL_FONT);
  oled.drawStr(2, 20, "LATEST LORA RX:");
  if (latestRXMessage.length() == 0) {
    oled.drawStr(2, 28, loraReady ? "No received message" : "LoRa not initialized");
  } else {
    drawWrappedText(latestRXMessage, 2, 28, 21, 2);
  }
  oled.drawLine(0, 44, 127, 44);
  String codeLine = "Code: " + (morseBuffer.length() ? morseBuffer : String("-"));
  oled.drawStr(2, 52, codeLine.c_str());
  String charText = "-";
  if (showInvalidMorse) charText = "INVALID";
  else if (decodedChar == ' ') charText = "SPACE";
  else if (decodedChar != '\0') charText = String(decodedChar);
  String charLine = "Char: " + charText;
  oled.drawStr(68, 52, charLine.c_str());
  String composed = morseMessage;
  if (composed.length() > 13) composed = composed.substring(composed.length() - 13);
  String msgLine = "MSG: " + (composed.length() ? composed : String("-"));
  oled.drawStr(2, 61, msgLine.c_str());
  drawSosBadge();
  oled.sendBuffer();
}

void showHistoryEntry() {
  if (currentScreen != CUSTOM_MESSAGE_MENU) return;
  oled.clearBuffer();
  oled.setFont(MENU_FONT);
  oled.drawStr(35, 9, "HISTORY");
  oled.drawLine(0, 12, 127, 12);
  oled.setFont(SMALL_FONT);
  if (messageCount == 0) {
    oled.drawStr(2, 28, "No LoRa history yet");
  } else {
    int idx = constrain(messageCount - 1 - historyOffset, 0, messageCount - 1);
    String prefix = messageIsRX[idx] ? "RX: " : "TX: ";
    oled.drawStr(2, 23, prefix.c_str());
    drawWrappedText(messageHistory[idx], 2, 32, 21, 3);
  }
  oled.drawStr(2, 62, "UP/DN browse; L/R/SEL return");
  drawSosBadge();
  oled.sendBuffer();
}

void addLoRaHistory(String message, bool incoming) {
  message.trim();
  if (message.length() == 0) return;
  if (messageCount >= MAX_MESSAGES) {
    for (int i = 0; i < MAX_MESSAGES - 1; i++) {
      messageHistory[i] = messageHistory[i + 1];
      messageIsRX[i] = messageIsRX[i + 1];
    }
    messageCount = MAX_MESSAGES - 1;
  }
  messageHistory[messageCount] = message;
  messageIsRX[messageCount] = incoming;
  messageCount++;
  historyOffset = 0;
}

// Real LoRa transmit. Both devices must use matching radio settings.
void transmitLoRaMessage(String message) {
  message.trim();
  if (message.length() == 0) return;
  if (!loraReady) {
    Serial.println("LoRa TX unavailable: radio not initialized.");
    oled.clearBuffer();
    oled.setFont(MENU_FONT);
    oled.drawStr(35, 10, "TX FAILED");
    oled.drawLine(0, 13, 127, 13);
    oled.setFont(SMALL_FONT);
    oled.drawStr(2, 24, "LoRa not initialized");
    drawWrappedText(message, 2, 34, 21, 2);
    drawSosBadge();
  oled.sendBuffer();
    return;
  }

  currentTXMessage = message;
  Serial.println("\n========== LORA TX ==========");
  Serial.print("TX: "); Serial.println(message);
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(message);
  int result = LoRa.endPacket();

  if (result == 1) {
    addLoRaHistory(message, false);
    Serial.println("LoRa packet transmitted.");
  } else {
    Serial.println("LoRa packet transmission failed.");
  }

  oled.clearBuffer();
  oled.setFont(MENU_FONT);
  oled.drawStr(48, 10, result == 1 ? "TX SENT" : "TX FAILED");
  oled.drawLine(0, 13, 127, 13);
  oled.setFont(SMALL_FONT);
  oled.drawStr(2, 23, "LoRa TX:");
  drawWrappedText(message, 2, 32, 21, 2);
  oled.drawStr(2, 58, result == 1 ? "Transmission complete" : "Transmission failed");
  drawSosBadge();
  oled.sendBuffer();
  delay(300);

  if (currentScreen == CUSTOM_MESSAGE_MENU) {
    if (bluetoothEnabled && bleHasClient()) showChatScreen();
    else showMorseScreen();
  }
}

// Real LoRa receive polling.
void checkLoRaReceive() {
  if (!loraReady) return;
  int packetSize = LoRa.parsePacket();
  if (packetSize <= 0) return;
  String incoming = "";
  while (LoRa.available()) {
    char c = (char)LoRa.read();
    if (incoming.length() < 120) incoming += c;
  }
  if (incoming.length() > 0) processLoRaReceivedMessage(incoming);
}

void processLoRaReceivedMessage(String message) {
  message.trim();
  if (message.length() == 0) return;
  if (message.startsWith("SOS|") || message.startsWith("PING|") || message.startsWith("PONG|") || message.startsWith("ACK|") || message.startsWith("END|") || message.startsWith("ENDACK|")) {
    handleSosPackets(message);
    return;
  }
  latestRXMessage = message;
  addLoRaHistory(message, true);
  Serial.print("RX: "); Serial.println(message);
  Serial.print("RSSI: "); Serial.println(LoRa.packetRssi());
  Serial.print("SNR: "); Serial.println(LoRa.packetSnr());

  // Forward LoRa-received text to the phone when connected.
  if (bluetoothEnabled && bleHasClient()) {
    bleSendLine(message);
    Serial.println("Forwarded LoRa RX to phone over Bluetooth.");
  }

  // Refresh only the screen, never reset Morse composition state.
  if (currentScreen == CUSTOM_MESSAGE_MENU) {
    if (historyViewActive) showHistoryEntry();
    else if (bluetoothEnabled && bleHasClient()) showChatScreen();
    else showMorseScreen();
  }
}

// Wrap text to the 5x7 font (about 21 characters per line).
void drawWrappedText(String text, int x, int y, int maxChars, int maxLines) {
  int start = 0;
  int line = 0;
  while (start < (int)text.length() && line < maxLines) {
    int end = min(start + maxChars, (int)text.length());
    if (end < (int)text.length()) {
      int spaceAt = text.lastIndexOf(' ', end);
      if (spaceAt >= start && spaceAt > start) end = spaceAt;
    }
    String part = text.substring(start, end);
    part.trim();
    oled.drawStr(x, y + line * 8, part.c_str());
    start = end;
    while (start < (int)text.length() && text.charAt(start) == ' ') start++;
    line++;
  }
}

// Morse decoder: A-Z, digits 0-9; custom ..-- means SPACE.
char decodeMorse(String code) {
  if (code == "..--") return ' ';
  if (code == ".-") return 'A'; if (code == "-...") return 'B';
  if (code == "-.-.") return 'C'; if (code == "-..") return 'D';
  if (code == ".") return 'E'; if (code == "..-.") return 'F';
  if (code == "--.") return 'G'; if (code == "....") return 'H';
  if (code == "..") return 'I'; if (code == ".---") return 'J';
  if (code == "-.-") return 'K'; if (code == ".-..") return 'L';
  if (code == "--") return 'M'; if (code == "-.") return 'N';
  if (code == "---") return 'O'; if (code == ".--.") return 'P';
  if (code == "--.-") return 'Q'; if (code == ".-.") return 'R';
  if (code == "...") return 'S'; if (code == "-") return 'T';
  if (code == "..-") return 'U'; if (code == "...-") return 'V';
  if (code == ".--") return 'W'; if (code == "-..-") return 'X';
  if (code == "-.--") return 'Y'; if (code == "--..") return 'Z';
  if (code == "-----") return '0'; if (code == ".----") return '1';
  if (code == "..---") return '2'; if (code == "...--") return '3';
  if (code == "....-") return '4'; if (code == ".....") return '5';
  if (code == "-....") return '6'; if (code == "--...") return '7';
  if (code == "---..") return '8'; if (code == "----.") return '9';
  return '#';
}

// SOS menu: when an SOS is active, SELECT starts packet monitoring.
// If no SOS is active, SELECT broadcasts this node's SOS.
void showSOSMenu() {
  oled.clearBuffer();
  oled.setFont(MENU_FONT);
  oled.drawStr(32, 10, "SOS MONITOR");
  oled.drawLine(0, 14, 127, 14);
  oled.setFont(SMALL_FONT);
  if (sosOutgoingActive) {
    oled.drawStr(2, 24, "SOS: TRANSMITTING");
    String idLine = "ID: " + (sosSessionId.length() ? sosSessionId : String("N/A"));
    oled.drawStr(2, 34, idLine.substring(0, 21).c_str());
    String peer = sosPaired ? "CONNECTED: " + pairedReceiverId : "Waiting for receiver";
    oled.drawStr(2, 44, peer.substring(0, 21).c_str());
    oled.drawStr(2, 55, "RIGHT: Stop  BACK: Exit");
  } else if (sosAlertActive) {
    String sender = "FROM: " + sosSenderId;
    oled.drawStr(2, 24, sender.substring(0, 21).c_str());
    drawSignalBars(sosRssi);
    String rssiText = String(sosRssi) + " dBm";
    oled.drawStr(35, 43, rssiText.c_str());
    bool connected = sosMonitoring && (millis() - lastSosPongMs <= SOS_TIMEOUT_MS) && lastSosPongMs != 0;
    oled.drawStr(2, 53, connected ? "Connection: ACTIVE" : (sosEnded ? "SOS ENDED" : "Connection: LOST/WAIT"));
    oled.drawStr(2, 63, "LEFT: Ack/Clear BACK: Exit");
  } else {
    oled.drawStr(2, 27, "No SOS received");
    oled.drawStr(2, 42, "SELECT: Send SOS");
    oled.drawStr(2, 59, "BACK: Main menu");
  }
  drawSosBadge();
  oled.sendBuffer();
}

void showSOSReady() {
  if (!loraReady) {
    Serial.println("Cannot send SOS: LoRa not initialized.");
    showSOSMenu();
    return;
  }
  sosOutgoingActive = true;
  sosPaired = false;
  pairedReceiverId = "";
  sosSessionId = String((uint32_t)esp_random(), HEX);
  sosSessionId.toUpperCase();
  sosMessage = "Emergency assistance needed";
  lastSosBroadcastMs = 0;
  sendSosPacket("SOS|" + nodeId + "|" + sosSessionId + "|" + sosMessage);
  lastSosBroadcastMs = millis();
  Serial.println("SOS transmission started.");
  showSOSMenu();
}

void stopOutgoingSOS() {
  if (!sosOutgoingActive) return;
  String endPacket = "END|" + nodeId + "|" + sosSessionId + "|" + (sosPaired ? pairedReceiverId : String("*"));
  for (int i = 0; i < 3; ++i) { sendSosPacket(endPacket); delay(80); }
  sosOutgoingActive = false;
  sosPaired = false;
  pairedReceiverId = "";
  beepSosEnded();
  Serial.println("SOS transmission stopped.");
  showSOSMenu();
}

String makeNodeId() {
  uint64_t mac = ESP.getEfuseMac();
  char id[12];
  snprintf(id, sizeof(id), "%04X%04X", (uint16_t)(mac >> 32), (uint32_t)mac);
  return String(id);
}

void sendSosPacket(String packet) {
  if (!loraReady) return;
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();
  LoRa.receive();
  Serial.print("SOS protocol TX: "); Serial.println(packet);
}

void handleSosPackets(String packet) {
  int rssi = LoRa.packetRssi();
  unsigned long now = millis();

  if (packet.startsWith("SOS|")) {
    int a = packet.indexOf('|');
    int b = packet.indexOf('|', a + 1);
    int c = packet.indexOf('|', b + 1);
    if (b < 0 || c < 0) return;
    String sender = packet.substring(a + 1, b);
    String sid = packet.substring(b + 1, c);
    String body = packet.substring(c + 1);
    if (sender == nodeId) return;
    bool isNew = !sosAlertActive || sender != sosSenderId || sid != sosSessionId;
    if (isNew) {
      sosAlertActive = true; sosAlertAcknowledged = false; sosEnded = false;
      sosSenderId = sender; sosSessionId = sid; sosMessage = body;
      sosMonitoring = false; lastSosPongMs = 0; sosConnectionLostReported = false;
      beepSosReceived();
      // First valid receiver acknowledgement is tied to sender + SOS session ID.
      sendSosPacket("ACK|" + nodeId + "|" + sender + "|" + sid);
      Serial.printf("SOS received from %s, ID %s, RSSI %d dBm\n", sender.c_str(), sid.c_str(), rssi);
    } else {
      sosRssi = rssi;
      sendSosPacket("ACK|" + nodeId + "|" + sender + "|" + sid);
    }
    sosRssi = rssi;
    if (currentScreen == SOS_MENU) showSOSMenu();
    else if (currentScreen == MAIN_MENU) showMainMenu();
    else if (currentScreen == BLUETOOTH_MENU) showBluetoothMenu();
    else if (currentScreen == CUSTOM_MESSAGE_MENU) {
      if (historyViewActive) showHistoryEntry();
      else if (bluetoothEnabled && bleHasClient()) showChatScreen();
      else showMorseScreen();
    }
    return;
  }

  if (packet.startsWith("ACK|")) {
    int a = packet.indexOf('|'); int b = packet.indexOf('|', a + 1); int c = packet.indexOf('|', b + 1);
    if (a < 0 || b < 0 || c < 0) return;
    String receiver = packet.substring(a + 1, b);
    String target = packet.substring(b + 1, c);
    String sid = packet.substring(c + 1);
    if (sosOutgoingActive && target == nodeId && sid == sosSessionId && !sosPaired) {
      sosPaired = true; pairedReceiverId = receiver;
      sendSosPacket("PONG|" + nodeId + "|" + receiver + "|" + sid);
      beepPaired();
      Serial.printf("Paired with first receiver %s\n", receiver.c_str());
      if (currentScreen == SOS_MENU) showSOSMenu();
    }
    return;
  }

  if (packet.startsWith("PING|")) {
    int a = packet.indexOf('|'); int b = packet.indexOf('|', a + 1);
    if (a < 0 || b < 0) return;
    String targetSender = packet.substring(a + 1, b);
    String sid = packet.substring(b + 1);
    if (sosOutgoingActive && targetSender == nodeId && sid == sosSessionId && sosPaired) {
      // Only the first paired receiver can maintain the session.
      sendSosPacket("PONG|" + nodeId + "|" + pairedReceiverId + "|" + sid);
    } else if (sosAlertActive && targetSender == sosSenderId && sid == sosSessionId) {
      sendSosPacket("PONG|" + nodeId + "|" + sosSenderId + "|" + sid);
    }
    return;
  }

  if (packet.startsWith("PONG|")) {
    int a = packet.indexOf('|'); int b = packet.indexOf('|', a + 1); int c = packet.indexOf('|', b + 1);
    if (a < 0 || b < 0 || c < 0) return;
    String sender = packet.substring(a + 1, b);
    String receiver = packet.substring(b + 1, c);
    String sid = packet.substring(c + 1);
    if (sosMonitoring && sender == sosSenderId && sid == sosSessionId && receiver == sosSenderId) {
      sosRssi = rssi; lastSosPongMs = now; sosConnectionLostReported = false;
      if (currentScreen == SOS_MENU) showSOSMenu();
    }
    if (sosOutgoingActive && sosPaired && sender == nodeId && receiver == pairedReceiverId && sid == sosSessionId) {
      lastSosHeartbeatMs = now;
    }
    return;
  }

  if (packet.startsWith("END|")) {
    int a = packet.indexOf('|'); int b = packet.indexOf('|', a + 1); int c = packet.indexOf('|', b + 1);
    if (a < 0 || b < 0 || c < 0) return;
    String sender = packet.substring(a + 1, b);
    String sid = packet.substring(b + 1, c);
    String receiver = packet.substring(c + 1);
    if (sosAlertActive && sender == sosSenderId && sid == sosSessionId && (receiver == "*" || receiver == nodeId)) {
      sosEnded = true; sosMonitoring = false; sosRssi = rssi;
      sendSosPacket("ENDACK|" + nodeId + "|" + sender + "|" + sid);
      beepSosEnded();
      if (currentScreen == SOS_MENU) showSOSMenu();
      else if (currentScreen == MAIN_MENU) showMainMenu();
    }
    return;
  }
  if (packet.startsWith("ENDACK|")) {
    if (sosOutgoingActive) Serial.println("SOS_END acknowledged by receiver.");
  }
}

void serviceSosMonitor() {
  if (!loraReady) return;
  unsigned long now = millis();
  if (sosOutgoingActive && now - lastSosBroadcastMs >= SOS_BROADCAST_INTERVAL_MS) {
    lastSosBroadcastMs = now;
    if (!sosPaired) sendSosPacket("SOS|" + nodeId + "|" + sosSessionId + "|" + sosMessage);
    else if (now - lastSosHeartbeatMs >= SOS_HEARTBEAT_INTERVAL_MS) {
      sendSosPacket("PING|" + nodeId + "|" + sosSessionId);
    }
  }
  if (sosMonitoring && sosAlertActive) {
    if (now - lastSosPingMs >= SOS_PACKET_INTERVAL_MS) {
      lastSosPingMs = now;
      sendSosPacket("PING|" + sosSenderId + "|" + sosSessionId);
    }
    if (lastSosPongMs != 0 && now - lastSosPongMs > SOS_TIMEOUT_MS && !sosConnectionLostReported) {
      sosConnectionLostReported = true;
      beepConnectionLost();
      Serial.println("SOS connection lost (timeout).");
      if (currentScreen == SOS_MENU) showSOSMenu();
    }
  }
}

// Active buzzer: it makes its own tone, so use simple ON/OFF control.
// 'frequency' is retained in the function signature to avoid changing callers.
void beepTone(unsigned int frequency, unsigned long durationMs) {
  (void)frequency;
  if (durationMs == 0) return;
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
}
void beepSosReceived() { beepTone(2200, 300); }
void beepPaired() { beepTone(2500, 120); delay(100); beepTone(2500, 120); }
void beepSosEnded() { beepTone(1600, 500); }
void beepConnectionLost() { for (int i = 0; i < 3; ++i) { beepTone(900, 150); if (i < 2) delay(100); } }

void drawSignalBars(int rssi) {
  int bars = 1;
  if (rssi >= -55) bars = 5;
  else if (rssi >= -67) bars = 4;
  else if (rssi >= -80) bars = 3;
  else if (rssi >= -95) bars = 2;
  // Phone-style display; thresholds only map measured RSSI to visual bars.
  for (int i = 0; i < 5; i++) {
    int h = 4 + i * 3;
    int x = 5 + i * 8;
    if (i < bars) oled.drawBox(x, 39 - h, 5, h);
    else oled.drawFrame(x, 39 - h, 5, h);
  }
}

void drawSosBadge() {
  if (!sosAlertActive) return;
  // Keep the notification at the top-right without changing the current screen.
  oled.setFont(SMALL_FONT);
  oled.drawFrame(111, 0, 16, 11);
  oled.drawStr(112, 8, sosAlertAcknowledged ? "SOS" : "S!");
}
