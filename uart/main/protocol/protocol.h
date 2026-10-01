#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROTOCOL_SYNC_0       0xAA
#define PROTOCOL_SYNC_1       0x55
#define PROTOCOL_VERSION      1
#define PROTOCOL_MAX_PAYLOAD  256

/*
 * Tipos de mensajes.
 * 0x00 - ACK
 * 0x01 - TEMPERATURE
 * 0x02 - ALARM
 * 0x03 ... 0xFF para mas tipos de mensajes
 */
#define MSG_TYPE_ACK          0x00
#define MSG_TYPE_TEMPERATURE  0x01
#define MSG_TYPE_ALARM        0x02

/*
 * ACK status.
 */
#define ACK_ERROR             0x00
#define ACK_OK                0x01


/*
 * =============
 * Metadata
 * =============
 */
typedef struct {
    uint8_t sender;
    uint8_t receiver;
    uint64_t timestamp; // Unix epoch seg
} protocol_metadata_t;


/*
 * ==============================
 * Generic message
 * ==============================
 */
typedef struct {
    protocol_metadata_t metadata;
    uint8_t type;
    const uint8_t *payload;
    size_t payload_len;
} protocol_message_t;


/*
 * ========
 * ACK
 * ========
 */
typedef struct {
    protocol_metadata_t metadata;
    protocol_metadata_t acknowledged_message;
    /*
     * 0 = error
     * 1 = OK
     */
    uint8_t status;
} protocol_ack_t;


/*
 * ==========
 * Parser
 * ==========
 */
#define PROTOCOL_PARSER_BUFFER_SIZE (2 + 1 + 1 + 10 + 1 + PROTOCOL_MAX_PAYLOAD + 2)

typedef struct {
    uint8_t buffer[PROTOCOL_PARSER_BUFFER_SIZE];
    size_t index;
    size_t expected_length;
} protocol_parser_t;


/*
 * =======
 * API
 * =======
 */
void protocol_parser_init(
    protocol_parser_t *parser
);

bool protocol_encode_message(
    const protocol_message_t *message,
    uint8_t *output,
    size_t output_size,
    size_t *encoded_size
);

bool protocol_encode_ack(
    const protocol_ack_t *ack,
    uint8_t *output,
    size_t output_size,
    size_t *encoded_size
);

bool protocol_parser_feed(
    protocol_parser_t *parser,
    uint8_t byte,
    protocol_message_t *message
);

#endif
