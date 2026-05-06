#define WEBSOCKETS_MAX_DATA_SIZE 65536
#define WEBSOCKETS_MAX_FRAME_SIZE 65536

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

// ================= WIFI =================
const char* ssid       = "Tecnologia";
const char* password   = "11213141";
const char* serverHost = "10.36.215.22";
const int   serverPort = 8765;
const char* serverPath = "/";

// ================= PINS =================
#define MIC_WS   1
#define MIC_SCK  2
#define MIC_SD   41

#define SPK_BCLK 42
#define SPK_LRC  40
#define SPK_DIN  39

#define BTN_PIN  48
//#define LED_PIN  48

// ================= AUDIO =================
#define MIC_SAMPLE_RATE    16000
#define SPK_SAMPLE_RATE    24000
#define CHUNK_SIZE         512
#define MAX_RECORD_MS      10000

// ================= RING BUFFER =================
#define RING_BUFFER_SIZE   (1024 * 1024)
#define PLAYBACK_START_PCT 5
#define I2S_DRAIN_MS       800

uint8_t* ringBuf    = nullptr;
size_t   ringWrite  = 0;
size_t   ringRead   = 0;
size_t   ringFilled = 0;
bool     streamEnded = false;

SemaphoreHandle_t  ringMutex;
EventGroupHandle_t playEvents;

#define EVT_START_PLAY  (1 << 0)

// ================= STATE =================
WebSocketsClient webSocket;
bool speakerInstalled = false;
bool wsConnected      = false;
bool playing          = false;
bool recording        = false;

// ================= LED =================
//void ledOn()  { digitalWrite(LED_PIN, HIGH); }
//void ledOff() { digitalWrite(LED_PIN, LOW);  }

// ================= RING BUFFER =================
void ringWrite_safe(const uint8_t* data, size_t len)
{
    xSemaphoreTake(ringMutex, portMAX_DELAY);
    for (size_t i = 0; i < len; i++)
    {
        if (ringFilled >= RING_BUFFER_SIZE)
        {
            ringRead = (ringRead + 1) % RING_BUFFER_SIZE;
            ringFilled--;
        }
        ringBuf[ringWrite] = data[i];
        ringWrite = (ringWrite + 1) % RING_BUFFER_SIZE;
        ringFilled++;
    }
    xSemaphoreGive(ringMutex);
}

size_t ringRead_safe(uint8_t* out, size_t len)
{
    xSemaphoreTake(ringMutex, portMAX_DELAY);
    size_t toRead = min(len, ringFilled);
    for (size_t i = 0; i < toRead; i++)
    {
        out[i] = ringBuf[ringRead];
        ringRead = (ringRead + 1) % RING_BUFFER_SIZE;
    }
    ringFilled -= toRead;
    xSemaphoreGive(ringMutex);
    return toRead;
}

size_t ringAvailable()
{
    xSemaphoreTake(ringMutex, portMAX_DELAY);
    size_t f = ringFilled;
    xSemaphoreGive(ringMutex);
    return f;
}

void ringReset()
{
    xSemaphoreTake(ringMutex, portMAX_DELAY);
    ringWrite = ringRead = ringFilled = 0;
    xSemaphoreGive(ringMutex);
}

// ================= I2S MIC =================
void setupMic()
{
    i2s_config_t config = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = MIC_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 512,
        .use_apll             = false
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = MIC_SCK,
        .ws_io_num    = MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = MIC_SD
    };
    i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pins);
    i2s_zero_dma_buffer(I2S_NUM_0);
}

// ================= I2S SPEAKER =================
void setupSpeaker()
{
    if (speakerInstalled)
        i2s_driver_uninstall(I2S_NUM_1);

    i2s_config_t config = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = SPK_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 64,
        .dma_buf_len          = 512,
        .use_apll             = false,
        .tx_desc_auto_clear   = true
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = SPK_BCLK,
        .ws_io_num    = SPK_LRC,
        .data_out_num = SPK_DIN,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(I2S_NUM_1, &config, 0, NULL);
    i2s_set_pin(I2S_NUM_1, &pins);
    i2s_zero_dma_buffer(I2S_NUM_1);
    speakerInstalled = true;
}

// ================= PLAYBACK TASK =================
void playbackTask(void* param)
{
    static uint8_t silence[1024] = {0};
    uint8_t        pcmChunk[2048];

    while (true)
    {
        xEventGroupWaitBits(playEvents, EVT_START_PLAY, pdTRUE, pdTRUE, portMAX_DELAY);
        playing = true;

        while (true)
        {
            size_t avail = ringAvailable();
            if (avail == 0)
            {
                if (streamEnded) break;
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            size_t got = ringRead_safe(pcmChunk, min(sizeof(pcmChunk), avail));
            if (got > 0)
            {
                size_t written;
                i2s_write(I2S_NUM_1, pcmChunk, got, &written, portMAX_DELAY);
            }
        }

        // Drain DMA FIFOs with silence so last syllable is never clipped
        {
            uint32_t drainStart = millis();
            while (millis() - drainStart < I2S_DRAIN_MS)
            {
                size_t written;
                i2s_write(I2S_NUM_1, silence, sizeof(silence), &written, portMAX_DELAY);
            }
        }

        i2s_zero_dma_buffer(I2S_NUM_1);
        playing = false;
    }
}

// ================= WEBSOCKET LOOP TASK =================
// Runs on core 0 continuously so heartbeat never times out
// even while recording or playback is active on core 1.
void wsLoopTask(void* param)
{
    while (true)
    {
        webSocket.loop();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ================= WEBSOCKET EVENT =================
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length)
{
    switch (type)
    {
        case WStype_CONNECTED:
            wsConnected = true;
            Serial.println("WebSocket connected");
            break;

        case WStype_DISCONNECTED:
            wsConnected = false;
            Serial.println("WebSocket disconnected");
            break;

        case WStype_BIN:
            ringWrite_safe(payload, length);
            if (!playing)
            {
                size_t filled    = ringAvailable();
                size_t threshold = (RING_BUFFER_SIZE * PLAYBACK_START_PCT) / 100;
                if (filled >= threshold)
                    xEventGroupSetBits(playEvents, EVT_START_PLAY);
            }
            break;

        case WStype_TEXT:
            if (String((char*)payload).indexOf("end") >= 0)
            {
                streamEnded = true;
                if (!playing && ringAvailable() > 0)
                    xEventGroupSetBits(playEvents, EVT_START_PLAY);
            }
            break;
    }
}

// ================= RECORD + SEND =================
void doOneTurn()
{
    if (!wsConnected) return;

    while (playing) delay(100);

    streamEnded = false;
    ringReset();

    recording = true;
    //ledOn();

    uint8_t  chunk[CHUNK_SIZE];
    size_t   bytesRead;
    uint32_t start = millis();

    while (digitalRead(BTN_PIN) == LOW)
    {
        if (millis() - start > MAX_RECORD_MS) break;
        if (!wsConnected) break;

        i2s_read(I2S_NUM_0, chunk, CHUNK_SIZE, &bytesRead, portMAX_DELAY);
        webSocket.sendBIN(chunk, bytesRead);
        delay(1);
    }

    recording = false;
    //ledOff();

    if (wsConnected)
        webSocket.sendTXT("{\"type\":\"end\"}");

    while (playing) delay(10);
}

// ================= SETUP =================
void setup()
{
    Serial.begin(115200);

    pinMode(BTN_PIN, INPUT_PULLUP);
    //pinMode(LED_PIN, OUTPUT);

    ringBuf    = (uint8_t*)heap_caps_malloc(RING_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (ringBuf == nullptr)
    {
        Serial.println("FATAL: PSRAM alloc failed! Enable OPI PSRAM in Tools menu.");
        while (true) delay(1000);
    }

    ringMutex  = xSemaphoreCreateMutex();
    playEvents = xEventGroupCreate();

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("WiFi Connected");

    setupMic();
    setupSpeaker();

    // Playback task on core 1
    xTaskCreatePinnedToCore(playbackTask, "playback", 4096, NULL, 2, NULL, 1);

    // WebSocket loop task on core 0 — keeps heartbeat alive at all times
    xTaskCreatePinnedToCore(wsLoopTask, "wsLoop", 4096, NULL, 1, NULL, 0);

    webSocket.begin(serverHost, serverPort, serverPath);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000);
    webSocket.enableHeartbeat(15000, 3000, 2);
}

// ================= LOOP =================
void loop()
{
    if (playing || recording) return;

    if (digitalRead(BTN_PIN) == LOW)
    {
        delay(20);
        if (digitalRead(BTN_PIN) == LOW)
            doOneTurn();
    }
}
