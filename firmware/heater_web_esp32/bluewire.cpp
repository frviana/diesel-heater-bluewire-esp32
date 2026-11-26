#include "bluewire.h"
#include "bluewire_rx.h"

#include <driver/gpio.h>
#include <driver/rmt_tx.h>
#include <esp_timer.h>
#include <esp_intr_alloc.h>
#include <hal/gpio_ll.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace BlueWire {
namespace {
constexpr gpio_num_t busPin = static_cast<gpio_num_t>(DATA_PIN);
using Line = char[120];

struct Command {
    char text[24];
    uint32_t queuedMs;
};

QueueHandle_t commands;
QueueHandle_t status;
QueueHandle_t events;
rmt_channel_handle_t txChannel;
rmt_encoder_handle_t txEncoder;
rmt_symbol_word_t txSymbols[9];
portMUX_TYPE rxLock = portMUX_INITIALIZER_UNLOCKED;
PulseCapture rx;
bool sending = false;
bool txFailed = false;

class BridgeOutput : public Print {
    char line[sizeof(Line)]{};
    size_t length = 0;

public:
    size_t write(uint8_t byte) override {
        if (byte == '\n') {
            line[length] = '\0';
            if (line[0] == 'S' && line[1] == ',') xQueueOverwrite(status, line);
            else if (length) xQueueSend(events, line, 0);
            length = 0;
        } else if (byte != '\r' && length < sizeof(line) - 1) {
            line[length++] = static_cast<char>(byte);
        }
        return 1;
    }
} output;

void IRAM_ATTR onEdge(void *) {
    const uint32_t now = esp_timer_get_time();
    const bool level = gpio_ll_get_level(&GPIO, busPin);
    portENTER_CRITICAL_ISR(&rxLock);
    if (!sending) rx.edge(now, level);
    portEXIT_CRITICAL_ISR(&rxLock);
}

bool IRAM_ATTR onTxDone(rmt_channel_handle_t, const rmt_tx_done_event_data_t *, void *) {
    portENTER_CRITICAL_ISR(&rxLock);
    rx.lastEdge = esp_timer_get_time();
    rx.high = gpio_ll_get_level(&GPIO, busPin);
    sending = false;
    portEXIT_CRITICAL_ISR(&rxLock);
    return false;
}

Frame takeFrame() {
    portENTER_CRITICAL(&rxLock);
    if (rx.high && static_cast<uint32_t>(micros() - rx.lastEdge) >= FRAME_GAP_US) rx.finish();
    const Frame frame = rx.pending;
    rx.pending.bits = 0;
    portEXIT_CRITICAL(&rxLock);
    return frame;
}

bool takeCommand(char (&text)[24]) {
    Command cmd;
    if (xQueueReceive(commands, &cmd, 0) != pdTRUE) return false;
    if (millis() - cmd.queuedMs >= 8000) {
        output.println(F("ERR command expired in queue"));
        text[0] = '\0';
    } else {
        memcpy(text, cmd.text, sizeof(text));
    }
    return true;
}

bool sendFrame(uint8_t byte) {
    if (txFailed) return false;
    portENTER_CRITICAL(&rxLock);
    const bool quiet = !sending && rx.high && !rx.active && !rx.pending.bits &&
                       static_cast<uint32_t>(micros() - rx.lastEdge) >= FRAME_GAP_US &&
                       gpio_ll_get_level(&GPIO, busPin);
    if (quiet) sending = true;
    portEXIT_CRITICAL(&rxLock);
    if (!quiet) return false;

    // RMT is inverted at the GPIO matrix. Its idle zero releases the
    // open-drain output, including before the first transaction.
    txSymbols[0].duration0 = 1;
    txSymbols[0].level0 = 0;
    txSymbols[0].duration1 = START_LOW_US;
    txSymbols[0].level1 = 1;
    for (size_t i = 0; i < 8; ++i) {
        const uint16_t low = (byte & (0x80 >> i)) ? LONG_LOW_US : SHORT_LOW_US;
        txSymbols[i + 1].duration0 = BIT_PERIOD_US - low;
        txSymbols[i + 1].level0 = 0;
        txSymbols[i + 1].duration1 = low;
        txSymbols[i + 1].level1 = 1;
    }
    rmt_transmit_config_t config{};
    if (rmt_transmit(txChannel, txEncoder, txSymbols, sizeof(txSymbols), &config) == ESP_OK) return true;

    portENTER_CRITICAL(&rxLock);
    sending = false;
    portEXIT_CRITICAL(&rxLock);
    txFailed = true;
    output.println(F("ERR blue-wire transmitter failed"));
    return false;
}

struct Startup {
    SemaphoreHandle_t done;
    int pollSeconds;
    bool ok;
};
}
}

#include "bluewire_bridge.h"

namespace BlueWire {
namespace {
bool initializeBus() {
    gpio_config_t gpio{};
    gpio.pin_bit_mask = 1ULL << DATA_PIN;
    gpio.mode = GPIO_MODE_INPUT;
    if (gpio_config(&gpio) != ESP_OK) return false;
    gpio_set_level(busPin, 1);
    if (gpio_set_direction(busPin, GPIO_MODE_INPUT_OUTPUT_OD) != ESP_OK) return false;

    rmt_tx_channel_config_t tx{};
    tx.gpio_num = busPin;
    tx.clk_src = RMT_CLK_SRC_DEFAULT;
    tx.resolution_hz = 1000000;
    tx.mem_block_symbols = 64;
    tx.trans_queue_depth = 1;
    tx.flags.invert_out = true;
    tx.flags.io_loop_back = true;
    tx.flags.io_od_mode = true;
    if (rmt_new_tx_channel(&tx, &txChannel) != ESP_OK) {
        gpio_set_direction(busPin, GPIO_MODE_INPUT);
        return false;
    }

    rmt_copy_encoder_config_t copy{};
    rmt_tx_event_callbacks_t callbacks{};
    callbacks.on_trans_done = onTxDone;
    bool enabled = false;
    if (rmt_new_copy_encoder(&copy, &txEncoder) == ESP_OK &&
        rmt_tx_register_event_callbacks(txChannel, &callbacks, nullptr) == ESP_OK &&
        rmt_enable(txChannel) == ESP_OK) {
        enabled = true;
        const esp_err_t service = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        rx.lastEdge = micros();
        rx.high = gpio_get_level(busPin);
        if ((service == ESP_OK || service == ESP_ERR_INVALID_STATE) &&
            gpio_set_intr_type(busPin, GPIO_INTR_ANYEDGE) == ESP_OK &&
            gpio_isr_handler_add(busPin, onEdge, nullptr) == ESP_OK) return true;
    }

    if (enabled) rmt_disable(txChannel);
    if (txEncoder) rmt_del_encoder(txEncoder);
    rmt_del_channel(txChannel);
    gpio_set_direction(busPin, GPIO_MODE_INPUT);
    return false;
}

void busTask(void *arg) {
    auto &startup = *static_cast<Startup *>(arg);
    const bool ok = initializeBus();
    if (ok) startBridge(startup.pollSeconds);
    startup.ok = ok;
    xSemaphoreGive(startup.done);
    if (!ok) {
        vTaskDelete(nullptr);
        return;
    }
    for (;;) {
        tickBridge();
        vTaskDelay(1);
    }
}
}

bool begin(int pollSeconds) {
    commands = xQueueCreate(8, sizeof(Command));
    status = xQueueCreate(1, sizeof(Line));
    events = xQueueCreate(24, sizeof(Line));
    Startup startup{xSemaphoreCreateBinary(), pollSeconds, false};
    // Keep GPIO edge interrupts off the Arduino loop core. The DHT22
    // library disables interrupts on its core while reading the sensor.
    const BaseType_t core = portNUM_PROCESSORS > 1 ? 1 - xPortGetCoreID() : 0;
    if (commands && status && events && startup.done &&
        xTaskCreatePinnedToCore(busTask, "bluewire", 4096, &startup, 3, nullptr, core) == pdPASS) {
        xSemaphoreTake(startup.done, portMAX_DELAY);
    }
    if (startup.done) vSemaphoreDelete(startup.done);
    if (startup.ok) return true;

    if (commands) vQueueDelete(commands);
    if (status) vQueueDelete(status);
    if (events) vQueueDelete(events);
    commands = status = events = nullptr;
    return false;
}

bool command(const String &text) {
    Command cmd{};
    if (!commands || text.isEmpty() || text.length() >= sizeof(cmd.text)) return false;
    text.toCharArray(cmd.text, sizeof(cmd.text));
    cmd.queuedMs = millis();
    return xQueueSend(commands, &cmd, 0) == pdTRUE;
}

bool readStatus(char (&line)[120]) {
    return status && xQueueReceive(status, line, 0) == pdTRUE;
}

bool readEvent(char (&line)[120]) {
    return events && xQueueReceive(events, line, 0) == pdTRUE;
}

uint32_t badFrames() {
    portENTER_CRITICAL(&rxLock);
    const uint32_t rejected = rx.rejected;
    portEXIT_CRITICAL(&rxLock);
    return rejected;
}
}
