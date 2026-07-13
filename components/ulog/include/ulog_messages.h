/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ULog binary message struct definitions.
 * Ported from PX4-Autopilot src/modules/logger/messages.h.
 *
 * ULog File Format (PX4): https://docs.px4.io/main/en/dev_log/ulog_file_format.html
 *
 * File layout:
 *   [File Header]      8 bytes magic "ULog\x01\x12\x35\x01" + 8 bytes timestamp
 *   [Flag Bits]        B message (compatibility flags)
 *   [Info Messages]    I message(s): key-value metadata
 *   [Formats]          F message(s): topic schema definitions
 *   [Subscriptions]    A message(s): msg_id ↔ topic_name mapping
 *   [Data Section]     D message(s): topic data (repeating)
 *                      S message(s): sync markers (every ~500ms)
 *                      O message(s): dropout marks
 */

#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  ULog message type identifiers                                      */
/* ------------------------------------------------------------------ */

#define ULOG_MSG_TYPE_FORMAT           'F'
#define ULOG_MSG_TYPE_DATA             'D'
#define ULOG_MSG_TYPE_INFO             'I'
#define ULOG_MSG_TYPE_INFO_MULTIPLE    'M'
#define ULOG_MSG_TYPE_PARAMETER        'P'
#define ULOG_MSG_TYPE_PARAMETER_DEFAULT 'Q'
#define ULOG_MSG_TYPE_ADD_LOGGED_MSG   'A'
#define ULOG_MSG_TYPE_REMOVE_LOGGED_MSG 'R'
#define ULOG_MSG_TYPE_SYNC             'S'
#define ULOG_MSG_TYPE_DROPOUT          'O'
#define ULOG_MSG_TYPE_LOGGING          'L'
#define ULOG_MSG_TYPE_LOGGING_TAGGED   'C'
#define ULOG_MSG_TYPE_FLAG_BITS        'B'

/* ------------------------------------------------------------------ */
/*  Packed structs — ULog binary message formats                       */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)

/** Magic bytes at the start of every ULog file. */
#define ULOG_MAGIC  "ULog\x01\x12\x35\x01"
#define ULOG_MAGIC_LEN 8

/** File header (first 16 bytes of the file). */
typedef struct {
    uint8_t  magic[8];      /**< ULOG_MAGIC */
    uint64_t timestamp;     /**< File creation timestamp (µs since boot) */
} ulog_file_header_s;

/** Common message header (every message starts with these 3 bytes). */
#define ULOG_MSG_HEADER_LEN 3  /**< msg_size (2) + msg_type (1) */

typedef struct {
    uint16_t msg_size;  /**< Size of the message excluding this header */
    uint8_t  msg_type;  /**< One of ULOG_MSG_TYPE_* */
} ulog_message_header_s;

/** Format message — describes a topic's schema. */
typedef struct {
    uint16_t msg_size;   /**< = strlen(format) (no NUL per ULog spec) */
    uint8_t  msg_type;   /**< ULOG_MSG_TYPE_FORMAT */
    char     format[1600]; /**< "topic_name:type0 field0;type1 field1;..." (no NUL terminator) */
} ulog_message_format_s;

/** Add logged subscription — maps msg_id to topic name. */
typedef struct {
    uint16_t msg_size;     /**< = 3 + strlen(message_name) (no NUL per ULog spec) */
    uint8_t  msg_type;     /**< ULOG_MSG_TYPE_ADD_LOGGED_MSG */
    uint8_t  multi_id;     /**< Multi-instance ID (0 for single-instance) */
    uint16_t msg_id;       /**< Logger-internal message ID */
    char     message_name[255]; /**< Topic name (no NUL terminator per ULog spec) */
} ulog_message_add_logged_s;

/** Remove logged subscription. */
typedef struct {
    uint16_t msg_size;   /**< = sizeof(uint16_t) = 2 */
    uint8_t  msg_type;   /**< ULOG_MSG_TYPE_REMOVE_LOGGED_MSG */
    uint16_t msg_id;
} ulog_message_remove_logged_s;

/** Data message — topic payload. The actual payload follows msg_id. */
typedef struct {
    uint16_t msg_size;   /**< = sizeof(uint16_t) + payload_size */
    uint8_t  msg_type;   /**< ULOG_MSG_TYPE_DATA */
    uint16_t msg_id;     /**< Matches msg_id from ADD_LOGGED_MSG */
    /* uint8_t data[] follows immediately */
} ulog_message_data_s;

/** Sync message — periodic marker for crash recovery. */
typedef struct {
    uint16_t msg_size;   /**< = 8 */
    uint8_t  msg_type;   /**< ULOG_MSG_TYPE_SYNC */
    uint8_t  sync_magic[8]; /**< = {0x2F, 0x73, 0x13, 0x26, 0xDD, 0x7A, 0x79, 0x4F} */
} ulog_message_sync_s;

#define ULOG_SYNC_MAGIC  {0x2F, 0x73, 0x13, 0x26, 0xDD, 0x7A, 0x79, 0x4F}

/** Dropout message — data loss due to buffer overflow. */
typedef struct {
    uint16_t msg_size;   /**< = sizeof(uint16_t) = 2 */
    uint8_t  msg_type;   /**< ULOG_MSG_TYPE_DROPOUT */
    uint16_t duration;   /**< Dropout duration in ms */
} ulog_message_dropout_s;

/** Info message — key-value metadata. */
typedef struct {
    uint16_t msg_size;        /**< = 1 + key_len + value_len (no NUL per ULog spec) */
    uint8_t  msg_type;        /**< ULOG_MSG_TYPE_INFO */
    uint8_t  key_len;         /**< Length of the key */
    char     key_value_str[255]; /**< "type key_name" followed by binary value (no NUL) */
} ulog_message_info_s;

/** Flag bits message — compatibility flags. */
typedef struct {
    uint16_t msg_size;   /**< = 24 */
    uint8_t  msg_type;   /**< ULOG_MSG_TYPE_FLAG_BITS */
    uint8_t  compat_flags[8];     /**< Compatible flags (readers must OR) */
    uint8_t  incompat_flags[8];   /**< Incompatible flags (readers must check) */
    uint64_t appended_offsets[3]; /**< Offsets for appended data sections */
} ulog_message_flag_bits_s;

/** Logged string message. */
typedef struct {
    uint16_t msg_size;   /**< = 9 + strlen(message) (no NUL per ULog spec) */
    uint8_t  msg_type;   /**< ULOG_MSG_TYPE_LOGGING */
    uint8_t  log_level;  /**< Same as Linux kernel log levels */
    uint64_t timestamp;
    char     message[128]; /**< Log text (no NUL terminator per ULog spec) */
} ulog_message_logging_s;

/** Tagged log message. */
typedef struct {
    uint16_t msg_size;
    uint8_t  msg_type;   /**< ULOG_MSG_TYPE_LOGGING_TAGGED */
    uint8_t  log_level;
    uint16_t tag;
    uint64_t timestamp;
    char     message[128];
} ulog_message_logging_tagged_s;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif
