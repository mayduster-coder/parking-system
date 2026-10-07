/**
 * @file    common.h
 * @brief   Data packet shared by all parts of the Parking System.
 *
 * The same structure travels through the whole system:
 * STM32 --(I2C)--> process2 --(FIFO)--> process1 --(TCP)--> server.
 *
 * The structure is packed (no padding), so its size is identical on the
 * STM32 (Cortex-M7), the BeagleBone (ARM Linux) and the PC (x86 Linux).
 * All three are little-endian and use IEEE-754 doubles, so the raw bytes
 * can be sent as-is.
 *
 * @warning Any change here must also be made in RTG/Inc/rtg.h on the STM32.
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

/**
 * @brief One GPS/parking message (23 bytes, packed).
 */
typedef struct __attribute__((packed)) {
    uint16_t client_id;  /**< Unique client (car) identifier */
    uint8_t  msg_type;   /**< 0 = no new data, 1 = START parking, 2 = END parking */
    double   latitude;   /**< Latitude in degrees (-90 .. 90) */
    double   longitude;  /**< Longitude in degrees (-180 .. 180) */
    uint32_t timestamp;  /**< Epoch seconds taken from the STM32 RTC */
} ParkingData_t;

#endif /* COMMON_H */

