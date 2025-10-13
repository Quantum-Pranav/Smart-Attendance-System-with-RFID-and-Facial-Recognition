/*
 * Edge Impulse Arduino examples
 * Copyright (c) 2022 EdgeImpulse Inc.
 *
 * This code has been modified to include WiFi, a robust NTP time sync,
 * SD Card logging (using the faster SD_MMC library), UART communication, 
 * Telegram reporting, and an LED status indicator.
 */

// =================== LIBRARIES & INCLUDES ===================
#include <ganga_13-project-1_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "esp_camera.h"

// NEW: Libraries for new features
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "time.h"
#include "FS.h"
#include "SD_MMC.h" // Using the faster SD_MMC library

// =================== USER CONFIGURATION ===================
// --- WiFi Credentials ---
const char* ssid = "YOURSSID";
const char* password = "YOURPASS";

// --- Telegram Bot Settings ---
#define BOTtoken "7814906387:AAFmNdS" 
#define CHAT_ID "14600"

// --- NTP Time Settings ---
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800; // India Standard Time (UTC +5:30)
const int daylightOffset_sec = 0;

// --- End-of-Day (EOD) Report Settings ---
const unsigned long eod_interval_ms = 60000UL; // 1 minute for demo

// --- On-board LED Pin ---
#define FLASH_GPIO_PIN 4

// =================== CAMERA & EI SETUP (Original) ===================

#define CAMERA_MODEL_AI_THINKER // Has PSRAM

#if defined(CAMERA_MODEL_AI_THINKER)
    #define PWDN_GPIO_NUM   32
    #define RESET_GPIO_NUM  -1
    #define XCLK_GPIO_NUM   0
    #define SIOD_GPIO_NUM   26
    #define SIOC_GPIO_NUM   27
    #define Y9_GPIO_NUM     35
    #define Y8_GPIO_NUM     34
    #define Y7_GPIO_NUM     39
    #define Y6_GPIO_NUM     36
    #define Y5_GPIO_NUM     21
    #define Y4_GPIO_NUM     19
    #define Y3_GPIO_NUM     18
    #define Y2_GPIO_NUM     5
    #define VSYNC_GPIO_NUM  25
    #define HREF_GPIO_NUM   23
    #define PCLK_GPIO_NUM   22
#else
    #error "Camera model not selected"
#endif

/* Constant defines */
#define EI_CAMERA_RAW_FRAME_BUFFER_COLS   320
#define EI_CAMERA_RAW_FRAME_BUFFER_ROWS   240
#define EI_CAMERA_FRAME_BYTE_SIZE         3

/* Private variables */
static bool debug_nn = false; 
static bool is_initialised = false;
uint8_t *snapshot_buf; 

static camera_config_t camera_config = {
    .pin_pwdn = PWDN_GPIO_NUM, .pin_reset = RESET_GPIO_NUM, .pin_xclk = XCLK_GPIO_NUM,
    .pin_sscb_sda = SIOD_GPIO_NUM, .pin_sscb_scl = SIOC_GPIO_NUM, .pin_d7 = Y9_GPIO_NUM,
    .pin_d6 = Y8_GPIO_NUM, .pin_d5 = Y7_GPIO_NUM, .pin_d4 = Y6_GPIO_NUM, .pin_d3 = Y5_GPIO_NUM,
    .pin_d2 = Y4_GPIO_NUM, .pin_d1 = Y3_GPIO_NUM, .pin_d0 = Y2_GPIO_NUM, .pin_vsync = VSYNC_GPIO_NUM,
    .pin_href = HREF_GPIO_NUM, .pin_pclk = PCLK_GPIO_NUM, .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0, .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_QVGA, .jpeg_quality = 12, .fb_count = 1,
    .fb_location = CAMERA_FB_IN_PSRAM, .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

// =================== GLOBAL VARIABLES & OBJECTS ===================
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);
unsigned long last_eod_send_time = 0;
String uart_buffer = "";
bool face_sent = false;
unsigned long face_sent_time = 0;
const unsigned long face_cooldown_ms = 10000; // 10 second cooldown

// =================== HELPER FUNCTIONS ===================

// --- WiFi & Time Initialization ---
void initWiFi() {
    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(500);
    }
    Serial.println("\nWiFi connected.");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
}

void initTime() {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.print("Synchronizing time");
    
    struct tm timeinfo;
    int max_retries = 10;
    while (!getLocalTime(&timeinfo) && max_retries-- > 0) {
        Serial.print(".");
        delay(1000); 
    }

    if(max_retries <= 0) {
        Serial.println("\nFailed to obtain time. Proceeding without time sync.");
        return;
    }
    
    Serial.println("\nTime synchronized successfully.");
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
}

// --- SD Card Functions ---
void initSDCard() {
    Serial.println("Initializing SD card using MMC interface...");
    if (!SD_MMC.begin("/sdcard", true)) { // Use 1-bit mode for compatibility
        Serial.println("Card Mount Failed. Check formatting (FAT32) and connection.");
        return;
    }
    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("No SD card attached");
        return;
    }
    Serial.println("SD Card initialized via MMC.");
}

String getCurrentDate() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return "nodate";
    }
    char buffer[11];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
    return String(buffer);
}

String getCurrentDateTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return "notime";
    }
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buffer);
}

void logAttendance(String name) {
    String date = getCurrentDate();
    String filePath = "/" + date + ".csv";
    
    Serial.printf("Logging attendance for %s to %s\n", name.c_str(), filePath.c_str());

    File file = SD_MMC.open(filePath, FILE_APPEND);
    if (!file) {
        file = SD_MMC.open(filePath, FILE_WRITE);
        if(file){
            file.println("DateTime,Name"); 
            file.close();
            file = SD_MMC.open(filePath, FILE_APPEND);
        } else {
             Serial.println("Failed to create or open file.");
            return;
        }
    }
    
    String log_entry = getCurrentDateTime() + "," + name;
    if (file.println(log_entry)) {
        Serial.println("Log entry saved.");
    } else {
        Serial.println("Failed to write log entry.");
    }
    file.close();
}

// --- UART & Telegram Functions ---
void handleUART() {
    while (Serial.available() > 0) {
        char receivedChar = Serial.read();
        uart_buffer += receivedChar;
        if (receivedChar == '\n') {
            uart_buffer.trim(); 
            Serial.print("Received from main MCU: ");
            Serial.println(uart_buffer);

            if (uart_buffer.startsWith("MATCH:")) {
                String name = uart_buffer.substring(6); 
                logAttendance(name);
            }
            uart_buffer = ""; 
        }
    }
}

void sendTelegramReport() {
    String date = getCurrentDate();
    String filePath = "/" + date + ".csv";

    File file = SD_MMC.open(filePath);
    if (!file || file.size() == 0) {
        Serial.println("Attendance file not found or is empty for today.");
        bot.sendMessage(CHAT_ID, "Attendance report for " + date + ":\nNo entries found.", "");
        file.close();
        return;
    }

    String report_message = "Attendance Report for " + date + ":\n\n";
    while (file.available()) {
        report_message += file.readStringUntil('\n') + "\n";
    }
    file.close();

    Serial.println("Sending daily report to Telegram...");
    if (bot.sendMessage(CHAT_ID, report_message, "")) {
        Serial.println("Report sent successfully.");
    } else {
        Serial.println("Failed to send report.");
    }
}

void handleEOD() {
    if (millis() - last_eod_send_time > eod_interval_ms) {
        sendTelegramReport();
        last_eod_send_time = millis(); 
    }
}

// =================== ARDUINO SETUP & LOOP ===================

void setup()
{
    Serial.begin(115200);
    pinMode(FLASH_GPIO_PIN, OUTPUT);

    initWiFi();
    initTime();
    initSDCard();
    client.setInsecure();
    bot.sendMessage(CHAT_ID, "Attendance System CAM is online!", "");

    if (ei_camera_init() == false) {
        ei_printf("Failed to initialize Camera!\r\n");
    } else {
        ei_printf("Camera initialized\r\n");
    }

    ei_printf("\nStarting continuous inference...\n");
}

void loop()
{
    handleUART();
    handleEOD();

    if (face_sent && (millis() - face_sent_time > face_cooldown_ms)) {
        face_sent = false; 
    }
    
    if (!face_sent) {
        digitalWrite(FLASH_GPIO_PIN, HIGH); 

        snapshot_buf = (uint8_t*)malloc(EI_CAMERA_RAW_FRAME_BUFFER_COLS * EI_CAMERA_RAW_FRAME_BUFFER_ROWS * EI_CAMERA_FRAME_BYTE_SIZE);
        if(snapshot_buf == nullptr) {
            ei_printf("ERR: Failed to allocate snapshot buffer!\n");
            digitalWrite(FLASH_GPIO_PIN, LOW); 
            return;
        }

        ei::signal_t signal;
        signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
        signal.get_data = &ei_camera_get_data;

        if (ei_camera_capture((size_t)EI_CLASSIFIER_INPUT_WIDTH, (size_t)EI_CLASSIFIER_INPUT_HEIGHT, snapshot_buf) == false) {
            ei_printf("Failed to capture image\r\n");
            free(snapshot_buf);
            digitalWrite(FLASH_GPIO_PIN, LOW);
            return;
        }

        ei_impulse_result_t result = { 0 };
        EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);
        if (err != EI_IMPULSE_OK) {
            ei_printf("ERR: Failed to run classifier (%d)\n", err);
            free(snapshot_buf);
            digitalWrite(FLASH_GPIO_PIN, LOW);
            return;
        }
        
        bool face_detected_this_cycle = false;
        #if EI_CLASSIFIER_OBJECT_DETECTION == 1
            for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
                auto bb = result.bounding_boxes[i];
                if (bb.value > 0.80) {
                    Serial.printf("FACE:%s\n", bb.label);
                    ei_printf("Sent %s to main MCU for verification.\n", bb.label);
                    face_sent = true;
                    face_sent_time = millis();
                    face_detected_this_cycle = true;
                    break;
                }
            }
        #else
            for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
                if (result.classification[i].value > 0.80) {
                    Serial.printf("FACE:%s\n", ei_classifier_inferencing_categories[i]);
                    ei_printf("Sent %s to main MCU for verification.\n", ei_classifier_inferencing_categories[i]);
                    face_sent = true;
                    face_sent_time = millis();
                    face_detected_this_cycle = true;
                    break; 
                }
            }
        #endif

        free(snapshot_buf);

        if (face_detected_this_cycle) {
            digitalWrite(FLASH_GPIO_PIN, LOW);
        }
    }
    delay(10);
}


// =================== ORIGINAL CAMERA FUNCTIONS (Unchanged) ===================

bool ei_camera_init(void) {
    if (is_initialised) return true;
    #if defined(CAMERA_MODEL_ESP_EYE)
        pinMode(13, INPUT_PULLUP);
        pinMode(14, INPUT_PULLUP);
    #endif
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed with error 0x%x\n", err);
        return false;
    }
    sensor_t * s = esp_camera_sensor_get();
    if (s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1); s->set_brightness(s, 1); s->set_saturation(s, 0);
    }
    is_initialised = true;
    return true;
}

void ei_camera_deinit(void) {
    esp_camera_deinit();
    is_initialised = false;
}

bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) {
    if (!is_initialised) {
        ei_printf("ERR: Camera is not initialized\r\n");
        return false;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ei_printf("Camera capture failed\n");
        return false;
    }
    bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, snapshot_buf);
    esp_camera_fb_return(fb);
    if(!converted){
        ei_printf("Conversion failed\n");
        return false;
    }
    if ((img_width != EI_CAMERA_RAW_FRAME_BUFFER_COLS) || (img_height != EI_CAMERA_RAW_FRAME_BUFFER_ROWS)) {
        ei::image::processing::crop_and_interpolate_rgb888(
            out_buf, EI_CAMERA_RAW_FRAME_BUFFER_COLS, EI_CAMERA_RAW_FRAME_BUFFER_ROWS,
            out_buf, img_width, img_height);
    }
    return true;
}

static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr) {
    size_t pixel_ix = offset * 3;
    size_t pixels_left = length;
    size_t out_ptr_ix = 0;
    while (pixels_left != 0) {
        out_ptr[out_ptr_ix] = (snapshot_buf[pixel_ix + 2] << 16) + (snapshot_buf[pixel_ix + 1] << 8) + snapshot_buf[pixel_ix];
        out_ptr_ix++;
        pixel_ix+=3;
        pixels_left--;
    }
    return 0;
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "Invalid model for current sensor"
#endif
