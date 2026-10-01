#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "string.h"
#include "driver/gpio.h"
#include "protocol/protocol.h"

#define TXD_PIN 17
#define RXD_PIN 18

static const int RX_BUF_SIZE = 1024;
protocol_parser_t uart_parser;
protocol_message_t rx_message;


void init(void) {
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM_1, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}


static void rx_task(void *arg) {
    static const char *RX_TASK_TAG = "RX_TASK";
    esp_log_level_set(RX_TASK_TAG, ESP_LOG_INFO);

    uint8_t rx_buffer[128];

    while (1) {
        const int rx_bytes_count = uart_read_bytes(UART_NUM_1, rx_buffer, sizeof(rx_buffer), 1000 / portTICK_PERIOD_MS);
        for (int i = 0; i < rx_bytes_count; i++) {
            if (protocol_parser_feed(&uart_parser, rx_buffer[i], &rx_message)) {
                ESP_LOGI(RX_TASK_TAG, "Trama valida recibida por UART");
                if (rx_message.type == MSG_TYPE_TEMPERATURE) {
                    int16_t encoded = (int16_t)(((uint16_t)rx_message.payload[0] << 8) |
                                                 (uint16_t)rx_message.payload[1]);
                    float temperature = (float)encoded / 100.0f;
                    ESP_LOGI(RX_TASK_TAG, "sensor origen: %d", rx_message.metadata.sender);
                    ESP_LOGI(RX_TASK_TAG, "temperatura decodificada: %.2f C", temperature);
                }
            }
        }
    }
}


void app_main(void) {
    init();
    protocol_parser_init(&uart_parser);
    xTaskCreate(rx_task, "uart_rx_task", 4096, NULL, configMAX_PRIORITIES - 1, NULL);
}
