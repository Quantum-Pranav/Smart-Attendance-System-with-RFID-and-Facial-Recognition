// ==== LIBRARIES ====
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ==== Wi-Fi & Telegram Configuration ====
const char* ssid = "YOURSSID";
const char* password = "YOURPASS"; 

#define BOTtoken "YOUR_BOT_TOKEN"

// Note: A default CHAT_ID is no longer used, we use the student-specific ones.

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// ==== Hardware Pin Definitions for ESP32 ====
// --- MFRC522 RFID Reader ---
#define SS_PIN  5
#define RST_PIN 4
MFRC522 rfid(SS_PIN, RST_PIN);

// --- I2C LCD Display (SDA=GPIO 21, SCL=GPIO 22 on many ESP32 boards) ---
// If it conflicts with RFID, you may need to check your board's pinout.
// We will use standard pins SDA=21, SCL=22. Let's adjust RFID pins.
// Let's re-assign RFID pins to avoid conflict with default I2C

// Note: Standard SPI pins for ESP32 are SCK=18, MISO=19, MOSI=23

// --- I2C LCD ---
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- Serial Communication with ESP32-CAM ---
#define RXD2 16  // ESP32 RX2 (Connect to ESP32-CAM TX)
#define TXD2 17  // ESP32 TX2 (Connect to ESP32-CAM RX)
HardwareSerial SerialCam(2);

// ==== Attendance Data ====
const int classCount = 20; // total number of classes
const int numStudents = 4;
String studentNames[numStudents] = {"PRANAV", "GANGA", "ASHHAD", "SWARUP"};
String UIDs[numStudents] = {
  "3D D4 8E XX",  // Pranav
  "57 CD 8C XX",  // Ganga
  "87 C3 93 XX",  // Ashhad
  "83 04 D6 XX"   // Swarup
};
String chatIDs[numStudents] = {
  "1460578XXX",   // Pranav
  "5728949XXX",   // Ganga
  "7558227XXX",   // Ashhad
  "1885855XXX"    // Swarup
};
int attended[numStudents] = {0, 0, 0, 0};
String recognizedFaceName = ""; // Stores the name from the camera

// ==== State Management ====
enum State { WAITING_FOR_FACE, WAITING_FOR_CARD };
State currentState = WAITING_FOR_FACE;
unsigned long cardScanTimeout = 0;

void setup() {
  Serial.begin(115200); // For debugging
  SerialCam.begin(115200, SERIAL_8N1, RXD2, TXD2);

  SPI.begin();
  rfid.PCD_Init();
  lcd.init();
  lcd.backlight();

  // Connect to WiFi
  lcd.print("Connecting WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  client.setInsecure();
  Serial.println("\nWiFi connected!");

  // Initial State
  lcd.clear();
  lcd.print("System Ready");
  lcd.setCursor(0, 1);
  lcd.print("Waiting for Face...");
}

void loop() {
  // --- STATE 1: WAITING FOR FACE DATA FROM CAMERA ---
  if (currentState == WAITING_FOR_FACE) {
    if (SerialCam.available()) {
      String msgFromCam = SerialCam.readStringUntil('\n');
      msgFromCam.trim();
      if (msgFromCam.startsWith("FACE:")) {
        recognizedFaceName = msgFromCam.substring(5);
        Serial.println("Face received: " + recognizedFaceName);

        // Transition to next state
        currentState = WAITING_FOR_CARD;
        cardScanTimeout = millis() + 10000; // 10-second timeout to scan card

        lcd.clear();
        lcd.print("Face: " + recognizedFaceName);
        lcd.setCursor(0, 1);
        lcd.print("Tap your card...");
      }
    }
  }

  // --- STATE 2: WAITING FOR RFID CARD SCAN ---
  if (currentState == WAITING_FOR_CARD) {
    // Check for timeout
    if (millis() > cardScanTimeout) {
      Serial.println("Card scan timed out.");
      recognizedFaceName = "";
      currentState = WAITING_FOR_FACE;
      lcd.clear();
      lcd.print("Timeout!");
      lcd.setCursor(0, 1);
      lcd.print("Waiting for Face...");
      delay(2000);
      return;
    }

    // Look for new cards
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
      return; // Nothing to do if no card is present
    }

    // A card has been detected, get its UID
    String tag = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      tag += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
      tag += String(rfid.uid.uidByte[i], HEX);
      if (i < rfid.uid.size - 1) tag += " ";
    }
    tag.toUpperCase();
    Serial.println("Card Scanned: " + tag);

    bool studentFound = false;
    for (int i = 0; i < numStudents; i++) {
      if (tag == UIDs[i]) {
        studentFound = true;
        String rfidName = studentNames[i];

        // *** THE CORE MATCHING LOGIC ***
        if (rfidName == recognizedFaceName) {
          // SUCCESS! Face and Card match.
          attended[i]++;

          // 1. Send confirmation back to ESP32-CAM
          SerialCam.println("MATCH:" + rfidName);
          Serial.println("SUCCESS: Match confirmed for " + rfidName + ". Sent confirmation to CAM.");

          // 2. Update LCD
          lcd.clear();
          lcd.print("Welcome " + rfidName);
          lcd.setCursor(0, 1);
          lcd.print("Attendance OK!");
          
          // 3. Send personalized Telegram message
          String message = "🎓 Attendance Marked!\n\nHello " + rfidName + 
                           ",\nYour attendance has been recorded successfully.";
          bot.sendMessage(chatIDs[i], message, "");
          Serial.println("Sent Telegram notification to " + rfidName);

        } else {
          // MISMATCH! Card does not belong to the person seen by the camera.
          Serial.println("MISMATCH: Face was " + recognizedFaceName + ", but card belongs to " + rfidName);
          lcd.clear();
          lcd.print("Mismatch!");
          lcd.setCursor(0, 1);
          lcd.print(rfidName + "'s card?");
        }
        break; // Exit student loop
      }
    }

    if (!studentFound) {
      Serial.println("Unknown Card!");
      lcd.clear();
      lcd.print("Unknown Card");
      lcd.setCursor(0, 1);
      lcd.print("Not Registered.");
    }

    // Cleanup and reset state
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    recognizedFaceName = "";
    currentState = WAITING_FOR_FACE;
    delay(3000); // Display result for 3 seconds
    lcd.clear();
    lcd.print("System Ready");
    lcd.setCursor(0, 1);
    lcd.print("Waiting for Face...");
  }
}
