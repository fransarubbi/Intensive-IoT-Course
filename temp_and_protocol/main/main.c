#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bmp280.h"
#include "esp_err.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "protocol/protocol.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "driver/gpio.h"


static const int RX_BUF_SIZE = 1024;

#define TXD_PIN 5
#define RXD_PIN 4
#define NODE_ID_TX  0x00
#define NODE_ID_RX  0x01

// UNICO CAMBIO ENTRE PLACAS: NODE_ID_TX o NODE_ID_RX
#define NODE_ID     NODE_ID_RX
#define PEER_ID     ((NODE_ID == NODE_ID_TX) ? NODE_ID_RX : NODE_ID_TX)
#define NETWORK     0x0005
#define RADIO_CHANNEL 15
#define SDA_GPIO    6
#define SCL_GPIO    7
#define I2C_PORT    0
#define TEMPERATURE_PERIOD_MS 5000
#define MAC_HEADER_SIZE      9
#define FCS_SIZE             2
#define RADIO_MAX_PSDU       127
#define RADIO_MAX_PROTO_FRAME (RADIO_MAX_PSDU - MAC_HEADER_SIZE - FCS_SIZE) /* 116 */

typedef struct {
    uint8_t len;
    uint8_t data[RADIO_MAX_PROTO_FRAME];
} rx_packet_t;

static const char *TAG = "APP";
static const char *TX_TASK_TAG = "TX_TASK";
static bmp280_t dev = { 0 };

static QueueHandle_t     rx_queue;
static SemaphoreHandle_t tx_mutex;
static SemaphoreHandle_t tx_done_sem;
static volatile bool     tx_ok;
static uint8_t           tx_buf[RADIO_MAX_PSDU + 1];
static uint8_t           mac_seq;


/* ---------- Callbacks del driver  ---------- */
IRAM_ATTR void esp_ieee802154_receive_done(
    uint8_t *frame,
    esp_ieee802154_frame_info_t *info
) {
    BaseType_t woken = pdFALSE;
    uint8_t total = frame[0];   /* incluye MAC header y FCS */

    if (total >= (MAC_HEADER_SIZE + FCS_SIZE + 1)) {
        rx_packet_t pkt;
        pkt.len = total - MAC_HEADER_SIZE - FCS_SIZE;
        if (pkt.len > RADIO_MAX_PROTO_FRAME) {
            pkt.len = RADIO_MAX_PROTO_FRAME;
        }
        memcpy(pkt.data, &frame[1 + MAC_HEADER_SIZE], pkt.len);
        xQueueSendFromISR(rx_queue, &pkt, &woken);
    }
    esp_ieee802154_receive_handle_done(frame);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

IRAM_ATTR void esp_ieee802154_transmit_done(
    const uint8_t *frame,
    const uint8_t *ack,
    esp_ieee802154_frame_info_t *ack_frame_info
) {
    BaseType_t woken = pdFALSE;

    if (ack != NULL) {
        esp_ieee802154_receive_handle_done(ack);
    }
    tx_ok = true;
    xSemaphoreGiveFromISR(tx_done_sem, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

IRAM_ATTR void esp_ieee802154_transmit_failed(
    const uint8_t *frame,
    esp_ieee802154_tx_error_t error
) {
    BaseType_t woken = pdFALSE;

    tx_ok = false;
    xSemaphoreGiveFromISR(tx_done_sem, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}


/**
 * @brief Envia un frame del protocolo por la radio (bloquea hasta que termina).
 */
static esp_err_t radio_send(const uint8_t *data, size_t len, uint8_t dest_id)
{
    if (data == NULL || len == 0 || len > RADIO_MAX_PROTO_FRAME) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (xSemaphoreTake(tx_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t i = 0;
    tx_buf[i++] = (uint8_t)(MAC_HEADER_SIZE + len + FCS_SIZE);  /* longitud PSDU */
    tx_buf[i++] = 0x41;
    tx_buf[i++] = 0x88;
    tx_buf[i++] = mac_seq++;
    tx_buf[i++] = (uint8_t)(NETWORK & 0xFF);   /* PAN (little endian) */
    tx_buf[i++] = (uint8_t)(NETWORK >> 8);
    tx_buf[i++] = dest_id;  /* destino corto */
    tx_buf[i++] = 0x00;
    tx_buf[i++] = NODE_ID;  /* origen corto */
    tx_buf[i++] = 0x00;

    memcpy(&tx_buf[i], data, len);
    xSemaphoreTake(tx_done_sem, 0);   /* limpiar señal vieja */
    tx_ok = false;

    esp_err_t err = esp_ieee802154_transmit(tx_buf, true);
    if (err == ESP_OK) {
        if (xSemaphoreTake(tx_done_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
        } else if (!tx_ok) {
            err = ESP_FAIL;
        }
    }

    xSemaphoreGive(tx_mutex);
    return err;
}


void init_uart(void) {
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

int sendData(const char* logName, const float* data) {
    const int len = sizeof(data);
    const int txBytes = uart_write_bytes(UART_NUM_1, data, len);
    ESP_LOGI(logName, "Wrote %d bytes", txBytes);
    return txBytes;
}

static uint64_t get_time(void) {
    return (uint64_t)(esp_timer_get_time() / 1000000);
}

static uint64_t read_u64_be(const uint8_t *b) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | b[i];
    }
    return v;
}


static void send_temperature(void) {
    uint8_t frame[RADIO_MAX_PROTO_FRAME];
    float t, p, h;

    esp_err_t r = bmp280_read_float(&dev, &t, &p, &h);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "Lectura fallida del sensor");
        return;
    }

    int16_t encoded = (int16_t)lroundf(t * 100.0f);
    uint8_t payload[2] = {
        (uint8_t)(((uint16_t)encoded) >> 8),
        (uint8_t)(((uint16_t)encoded) & 0xFF),
    };

    protocol_message_t message = {
        .metadata = {
            .sender    = NODE_ID,
            .receiver  = PEER_ID,
            .timestamp = get_time(),
        },
        .type        = MSG_TYPE_TEMPERATURE,
        .payload     = payload,
        .payload_len = sizeof(payload),
    };

    size_t frame_size;
    if (!protocol_encode_message(&message, frame, sizeof(frame), &frame_size)) {
        ESP_LOGE(TAG, "Error codificando mensaje de temperatura");
        return;
    }

    esp_err_t err = radio_send(frame, frame_size, PEER_ID);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo enviar: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Temperatura enviada: %.2f C, timestamp=%" PRIu64,
             t, message.metadata.timestamp);
}

static void send_ack(const protocol_message_t *rx, uint8_t status) {
    protocol_ack_t ack = {
        .metadata = {
            .sender    = NODE_ID,
            .receiver  = rx->metadata.sender,
            .timestamp = get_time(),
        },
        .acknowledged_message = rx->metadata,
        .status = status,
    };

    uint8_t frame[RADIO_MAX_PROTO_FRAME];
    size_t size;

    if (!protocol_encode_ack(&ack, frame, sizeof(frame), &size)) {
        ESP_LOGE(TAG, "Error codificando ACK");
        return;
    }

    esp_err_t err = radio_send(frame, size, rx->metadata.sender);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo enviar ACK: %s", esp_err_to_name(err));
    }
}


static uint8_t process_message(const protocol_message_t *message) {
    ESP_LOGI(TAG,
             "Mensaje recibido: type=0x%02X sender=%u receiver=%u timestamp=%" PRIu64,
             message->type,
             message->metadata.sender,
             message->metadata.receiver,
             message->metadata.timestamp);

    switch (message->type) {

    case MSG_TYPE_TEMPERATURE: {
        if (message->payload_len != 2) {
            ESP_LOGW(TAG, "Payload de temperatura invalido");
            return ACK_ERROR;
        }

        int16_t encoded = (int16_t)(((uint16_t)message->payload[0] << 8) |
                                     (uint16_t)message->payload[1]);
        float temperature = (float)encoded / 100.0f;

        ESP_LOGI(TAG, "Temperatura = %.2f C", temperature);
        sendData(TX_TASK_TAG, &temperature);
        return ACK_OK;
    }

    case MSG_TYPE_ACK: {
        /* 10 bytes de metadata reconocida + 1 byte de status */
        if (message->payload_len != 11) {
            ESP_LOGW(TAG, "Payload de ACK invalido");
            return ACK_ERROR;
        }

        uint64_t acked_ts = read_u64_be(&message->payload[2]);
        uint8_t  status   = message->payload[10];

        if (status == ACK_OK) {
            ESP_LOGI(TAG, "ACK OK (mensaje con timestamp=%" PRIu64 ")", acked_ts);
        } else {
            ESP_LOGW(TAG, "ACK con ERROR (mensaje con timestamp=%" PRIu64 ")", acked_ts);
        }
        return ACK_OK;
    }

    case MSG_TYPE_ALARM:
        ESP_LOGW(TAG, "ALARMA recibida");
        return ACK_OK;

    default:
        ESP_LOGW(TAG, "Tipo de mensaje desconocido: 0x%02X", message->type);
        return ACK_ERROR;
    }
}

static void handle_message(const protocol_message_t *message) {
    /* Ignorar mensajes que no son para mi */
    if (message->metadata.receiver != NODE_ID) {
        ESP_LOGD(TAG, "Mensaje para otro nodo (%u), ignorado",
                 message->metadata.receiver);
        return;
    }

    uint8_t status = process_message(message);

    if (message->type != MSG_TYPE_ACK) {
        send_ack(message, status);
    }
}

static void receiver_task(void *argument) {
    protocol_parser_t parser;
    protocol_parser_init(&parser);

    rx_packet_t pkt;
    protocol_message_t message;

    while (true) {
        if (xQueueReceive(rx_queue, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        for (int i = 0; i < pkt.len; i++) {
            if (protocol_parser_feed(&parser, pkt.data[i], &message)) {
                handle_message(&message);
            }
        }
    }
}

static void temperature_task(void *argument) {
    while (true) {
        send_temperature();
        vTaskDelay(pdMS_TO_TICKS(TEMPERATURE_PERIOD_MS));
    }
}



void app_main(void) {

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    /* Recursos de la radio */
    rx_queue    = xQueueCreate(8, sizeof(rx_packet_t));
    tx_mutex    = xSemaphoreCreateMutex();
    tx_done_sem = xSemaphoreCreateBinary();
    configASSERT(rx_queue && tx_mutex && tx_done_sem);

    /* Radio 802.15.4 (igual en ambas placas) */
    ESP_ERROR_CHECK(esp_ieee802154_enable());
    ESP_ERROR_CHECK(esp_ieee802154_set_channel(RADIO_CHANNEL));
    ESP_ERROR_CHECK(esp_ieee802154_set_panid(NETWORK));
    ESP_ERROR_CHECK(esp_ieee802154_set_short_address(NODE_ID));
    ESP_ERROR_CHECK(esp_ieee802154_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_ieee802154_set_rx_when_idle(true));
    ESP_ERROR_CHECK(esp_ieee802154_receive());

    /* Ambos nodos reciben (el TX recibe los ACK) */
    xTaskCreate(receiver_task, "protocol_receiver", 5120, NULL, 5, NULL);

    if (NODE_ID == NODE_ID_RX) {
        init_uart();
        static const char *TX_TASK_TAG = "TX_TASK";
        esp_log_level_set(TX_TASK_TAG, ESP_LOG_INFO);
    }

    /* Solo el TX lee el sensor y envia temperatura */
    if (NODE_ID == NODE_ID_TX) {
        ESP_ERROR_CHECK(i2cdev_init());

        bmp280_params_t params;
        bmp280_init_default_params(&params);

        ESP_ERROR_CHECK(bmp280_init_desc(&dev, BMP280_I2C_ADDRESS_0, I2C_PORT,
                                         SDA_GPIO, SCL_GPIO));   /* 0x76 */

        /* IMPORTANTE: pull-ups y 100 kHz ANTES de bmp280_init */
        dev.i2c_dev.cfg.sda_pullup_en = true;
        dev.i2c_dev.cfg.scl_pullup_en = true;
        dev.i2c_dev.cfg.master.clk_speed = 100000;

        ESP_ERROR_CHECK(bmp280_init(&dev, &params));
        ESP_LOGI(TAG, "Sensor: %s",
                 dev.id == BME280_CHIP_ID ? "BME280 (con humedad)"
                                          : "BMP280 (sin humedad)");

        xTaskCreate(temperature_task, "temperature_sender", 4096, NULL, 5, NULL);
    }

    ESP_LOGI(TAG, "Nodo %u listo (canal %d, PAN 0x%04X)",
             NODE_ID, RADIO_CHANNEL, NETWORK);
}
