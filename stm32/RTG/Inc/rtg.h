/**
 * @file    rtg.h
 * @brief   GPS emulator over I2C (STM32 side) - Parking System Project
 * @author  may1
 */
#ifndef RTG_H
#define RTG_H

#include "main.h"
#include <stdint.h>

/** @brief Message types - must match common.h on the Linux side */
typedef enum {
    MSG_NONE  = 0,  /**< No new data (already read by the master) */
    MSG_START = 1,  /**< Parking session started */
    MSG_END   = 2   /**< Parking session ended */
} MsgType_t;

/** @brief Packet sent to the BBG - must be IDENTICAL to common.h */
typedef struct __attribute__((packed)) {
    uint16_t client_id;  /**< Unique client identifier */
    uint8_t  msg_type;   /**< MsgType_t */
    double   latitude;   /**< Latitude in degrees */
    double   longitude;  /**< Longitude in degrees */
    uint32_t timestamp;  /**< Epoch seconds from the RTC */
} ParkingData_t;

_Static_assert(sizeof(ParkingData_t) == 23, "ParkingData_t must match Linux common.h");

/** @brief User button B1 on the Nucleo-144 (PC13, reads 1 when pressed) */
#define PARK_BUTTON_PORT       GPIOC
#define PARK_BUTTON_PIN        GPIO_PIN_13

/** @brief Blue LED LD2 (PB7) - ON while the car is parked */
#define PARK_LED_PORT          GPIOB
#define PARK_LED_PIN           GPIO_PIN_7

/** @brief Button must be stable this long to count as a press (debounce) */
#define BUTTON_DEBOUNCE_MS     50U

void GPS_Emulator_Init(void);
void GPS_Emulator_Start_Listening(I2C_HandleTypeDef *hi2c);
void GPS_Emulator_Task(void);
void GPS_Emulator_Update_Data(uint8_t type, double lat, double lon);

#endif /* RTG_H */
