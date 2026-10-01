#include "protocol/protocol.h"
#include <string.h>

/*

 ┌────────┬────────┬────────┬──────────────┬──────────┬─────────┬─────────┬──────┐
 │ SYNC   │ VERSION│ FLAGS  │ METADATA     │ TYPE     │ LENGTH  │ PAYLOAD │ CRC  │
 │ 2 B    │ 1 B    │ 1 B    │ 10 B         │ 1 B      │  2 B    │   N B   │  2 B │
 └────────┴────────┴────────┴──────────────┴──────────┴─────────┴─────────┴──────┘
                                   │
                                   ├─ sender      1 B
                                   ├─ receiver    1 B
                                   └─ timestamp   8 B

SYNC: son 2 bytes (PROTOCOL_SYNC_0 y PROTOCOL_SYNC_1) que sirven para indicar donde empieza
un mensaje (trama). Si en el medio de una transmision hay ruido o bytes basura, el receptor
ignora todo hasta encontrar estos dos bytes seguidos

VERSION (1 byte): indica la version del protocolo. Permite evolucionar el protocolo y
tener retrocompatibilidad

FLAGS (1 byte): bits que sirven para encender o apagar configuraciones del mensaje (por ejemplo,
si está encriptado, si es urgente, etc.). Ahora esto no esta implementado, se manda solo 0

METADATA (10 bytes): contiene la información de enrutamiento y tiempo:
- Sender (1 byte): ID de quien envia
- Receiver (1 byte): ID de quien debe recibir
- Timestamp (8 bytes): marca de tiempo de cuando se genero

TYPE (1 byte): identifica que tipo de mensaje se esta enviando (ej. si es un comando, un ACK, un
mensaje de error o datos de un sensor).

LENGTH (2 bytes): indica el tamaño exacto en bytes que va a ocupar el campo PAYLOAD

PAYLOAD (N bytes): los datos reales que se transmiten

CRC (2 bytes): cyclic redundancy check. Es un calculo matematico que se hace sobre todos los bytes
del mensaje (desde VERSION hasta el final del PAYLOAD). El receptor hace el mismo calculo al recibir;
si el resultado no coincide con el CRC enviado, significa que el paquete se corrompio y se descarta
*/


#define FLAGS_NONE 0x00
#define HEADER_SIZE (2 + 1 + 1 + 10 + 1 + 2)  // Campos previos al payload
#define CRC_SIZE 2


/*
 * Helpers para serializar
 */
static void write_u16_be(
    uint8_t *buffer,
    uint16_t value
) {
    buffer[0] = (uint8_t)(value >> 8);
    buffer[1] = (uint8_t)(value);
}


static uint16_t read_u16_be(const uint8_t *buffer) {
    return ((uint16_t)buffer[0] << 8) | ((uint16_t)buffer[1]);
}


static void write_u64_be(
    uint8_t *buffer,
    uint64_t value
)
{
    buffer[0] = (uint8_t)(value >> 56);
    buffer[1] = (uint8_t)(value >> 48);
    buffer[2] = (uint8_t)(value >> 40);
    buffer[3] = (uint8_t)(value >> 32);
    buffer[4] = (uint8_t)(value >> 24);
    buffer[5] = (uint8_t)(value >> 16);
    buffer[6] = (uint8_t)(value >> 8);
    buffer[7] = (uint8_t)(value);
}


static uint64_t read_u64_be(
    const uint8_t *buffer
)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value <<= 8;
        value |= buffer[i];
    }
    return value;
}


/*
 * CRC-16-CCITT
 */
static uint16_t crc16_ccitt(
    const uint8_t *data,
    size_t length
)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc =
                    (crc << 1) ^
                    0x1021;
            }
            else {
                crc <<= 1;
            }
        }
    }

    return crc;
}


/*
 * Codificar los metadatos
 */
static void encode_metadata(
    const protocol_metadata_t *metadata,
    uint8_t *buffer
)
{
    buffer[0] = metadata->sender;
    buffer[1] = metadata->receiver;
    write_u64_be(
        &buffer[2],
        metadata->timestamp
    );
}


static void decode_metadata(
    const uint8_t *buffer,
    protocol_metadata_t *metadata
)
{
    metadata->sender = buffer[0];
    metadata->receiver = buffer[1];
    metadata->timestamp = read_u64_be(&buffer[2]);
}


/*
 * Codificacion de mensajes
 */
bool protocol_encode_message(
    const protocol_message_t *message,
    uint8_t *output,
    size_t output_size,
    size_t *encoded_size
)
{
    if (message == NULL ||
        output == NULL ||
        encoded_size == NULL) {
        return false;
    }

    if (message->payload_len > PROTOCOL_MAX_PAYLOAD) return false;

    // calcula el tamaño total del frame
    const size_t total_size = HEADER_SIZE + message->payload_len + CRC_SIZE;

    if (output_size < total_size) return false;
    size_t index = 0;

    output[index++] = PROTOCOL_SYNC_0;
    output[index++] = PROTOCOL_SYNC_1;
    output[index++] = PROTOCOL_VERSION;
    output[index++] = FLAGS_NONE;

    encode_metadata(
        &message->metadata,
        &output[index]
    );

    index += 10;
    output[index++] = message->type;
    write_u16_be(
        &output[index],
        (uint16_t)message->payload_len
    );

    index += 2;
    if (message->payload_len > 0) {
        memcpy(
            &output[index],
            message->payload,
            message->payload_len
        );
        index += message->payload_len;
    }

    // CRC cubre todo menos el SYNC (CRC empieza en VERSION)
    uint16_t crc =
        crc16_ccitt(
            &output[2],
            index - 2
        );

    write_u16_be(
        &output[index],
        crc
    );
    index += 2;
    *encoded_size = index;
    return true;
}


/*
 * Codificacion del ack
 */
bool protocol_encode_ack(
    const protocol_ack_t *ack,
    uint8_t *output,
    size_t output_size,
    size_t *encoded_size
)
{
    if (ack == NULL ||
        output == NULL ||
        encoded_size == NULL) {
        return false;
    }

    uint8_t payload[11];

    encode_metadata(
        &ack->acknowledged_message,
        payload
    );

    payload[10] = ack->status;
    protocol_message_t message = {
        .metadata = ack->metadata,
        .type = MSG_TYPE_ACK,
        .payload = payload,
        .payload_len = sizeof(payload)
    };

    return protocol_encode_message(
        &message,
        output,
        output_size,
        encoded_size
    );
}


/*
 * Parser
 */
void protocol_parser_init(
    protocol_parser_t *parser
)
{
    if (parser == NULL) {
        return;
    }

    memset(
        parser,
        0,
        sizeof(*parser)
    );
}


/*
 * Reset parser
 */
static void parser_reset(
    protocol_parser_t *parser
)
{
    parser->index = 0;
    parser->expected_length = 0;
}


/*
 * El parser recibe solo un byte a la vez. Esto se diseña asi
 * porque UART, SPI, radio, etc mandan data en chunks arbitrarios
 */
bool protocol_parser_feed(
    protocol_parser_t *parser,
    uint8_t byte,
    protocol_message_t *message
)
{
    if (parser == NULL ||
        message == NULL) {
        return false;
    }

    // busca el SYNC
    if (parser->index == 0) {
        if (byte == PROTOCOL_SYNC_0) {
            parser->buffer[0] = byte;
            parser->index = 1;
        }
        return false;
    }

    if (parser->index == 1) {
        if (byte == PROTOCOL_SYNC_1) {
            parser->buffer[1] = byte;
            parser->index = 2;
        }
        else if (byte == PROTOCOL_SYNC_0) {
            parser->buffer[0] = byte;
            parser->index = 1;
        }
        else {
            parser_reset(parser);
        }
        return false;
    }

    if (parser->index >=
        sizeof(parser->buffer)) {
        parser_reset(parser);
        return false;
    }

    parser->buffer[parser->index++] = byte;

    if (parser->index == 17) {
        uint16_t payload_length = read_u16_be(&parser->buffer[15]);

        if (payload_length >
            PROTOCOL_MAX_PAYLOAD) {
            parser_reset(parser);
            return false;
        }
        parser->expected_length = HEADER_SIZE + payload_length + CRC_SIZE;
    }

    if (parser->expected_length == 0 ||
        parser->index <
        parser->expected_length) {
        return false;
    }
    if (parser->index !=
        parser->expected_length) {
        parser_reset(parser);
        return false;
    }
    uint16_t received_crc = read_u16_be(&parser->buffer[parser->index - 2]);
    uint16_t calculated_crc =
        crc16_ccitt(
            &parser->buffer[2],
            parser->index - 4
        );
    if (received_crc !=
        calculated_crc) {
        parser_reset(parser);
        return false;
    }
    if (parser->buffer[2] !=
        PROTOCOL_VERSION) {
        parser_reset(parser);
        return false;
    }
    decode_metadata(
        &parser->buffer[4],
        &message->metadata
    );
    message->type = parser->buffer[14];
    uint16_t payload_length = read_u16_be(&parser->buffer[15]);
    message->payload = &parser->buffer[17];
    message->payload_len = payload_length;
    parser_reset(parser);
    return true;
}
