/**
 * @file  common.h
 * @brief Packet shared by STM32, BBG processes and the server.
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

/* Message types (must match rtg.h on the STM32) */
#define MSG_NONE   0   /**< No new data */
#define MSG_START  1   /**< Parking started */
#define MSG_END    2   /**< Parking ended */

typedef struct __attribute__((packed)) {
    uint16_t client_id;  /**< Unique client identifier */
    uint8_t  msg_type;   /**< MSG_NONE / MSG_START / MSG_END */
    double   latitude;   /**< Degrees */
    double   longitude;  /**< Degrees */
    uint32_t timestamp;  /**< Epoch seconds */
} ParkingData_t;

_Static_assert(sizeof(ParkingData_t) == 23, "ParkingData_t size mismatch");

#endif /* COMMON_H */
