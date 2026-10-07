/**
 * @file    rtg.c
 * @brief   GPS emulator - STM32 acts as I2C slave (Listen mode) and serves
 *          ParkingData_t packets to the BeagleBone master.
 * @author  may1
 * @date    Sep 22, 2026
 */
#include "rtg.h"
#include "i2c.h"
#include <time.h>

extern RTC_HandleTypeDef hrtc;

static ParkingData_t gps_data;            /**< Latest message, written by the main loop */
static ParkingData_t tx_buf;              /**< Snapshot being transmitted to the master */
static uint8_t       rx_dummy[8];         /**< Sink for unexpected master writes */
static volatile uint32_t data_version;    /**< Incremented on every new message */
static uint32_t      tx_version;          /**< Version of the snapshot in tx_buf */

/**
 * @brief  Read the RTC and convert to epoch seconds.
 * @return Epoch timestamp (seconds since 1970-01-01).
 */
static uint32_t get_rtc_timestamp(void)
{
    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};
    struct tm t = {0};

    /* GetTime must be called before GetDate (unlocks shadow registers) */
    HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);

    t.tm_sec   = sTime.Seconds;
    t.tm_min   = sTime.Minutes;
    t.tm_hour  = sTime.Hours;
    t.tm_mday  = sDate.Date;
    t.tm_mon   = sDate.Month - 1;
    t.tm_year  = sDate.Year + 100;
    t.tm_isdst = -1;

    return (uint32_t)mktime(&t);
}

/**
 * @brief Initialize the emulator data. No message is pending at start.
 */
void GPS_Emulator_Init(void)
{
    gps_data.client_id = 101;
    gps_data.msg_type  = MSG_NONE;
    gps_data.latitude  = 32.085300;
    gps_data.longitude = 34.781800;
    gps_data.timestamp = get_rtc_timestamp();
    data_version = 0;
}

/**
 * @brief Put the I2C peripheral in Listen mode (ACKs its own address forever).
 * @param hi2c I2C handle (hi2c2).
 */
void GPS_Emulator_Start_Listening(I2C_HandleTypeDef *hi2c)
{
    if (HAL_I2C_EnableListen_IT(hi2c) != HAL_OK) {
        Error_Handler();
    }
}

/**
 * @brief Publish a new message. Thread-safe against the I2C interrupt.
 * @param type MsgType_t (MSG_START / MSG_END)
 * @param lat  Latitude
 * @param lon  Longitude
 */
void GPS_Emulator_Update_Data(uint8_t type, double lat, double lon)
{
    uint32_t ts = get_rtc_timestamp();   /* outside the critical section */

    HAL_NVIC_DisableIRQ(I2C2_EV_IRQn);   /* critical section vs AddrCallback */
    gps_data.msg_type  = type;
    gps_data.latitude  = lat;
    gps_data.longitude = lon;
    gps_data.timestamp = ts;
    data_version++;
    HAL_NVIC_EnableIRQ(I2C2_EV_IRQn);
}

/**
 * @brief Call from the main loop. Emulates a parking session (START/END
 *        alternately) and re-arms Listen mode if an error left it idle.
 */
void GPS_Emulator_Task(void)
{
    static uint32_t last_tick = 0;
    static uint8_t  parked = 0;

    if (HAL_GetTick() - last_tick >= PARK_TOGGLE_PERIOD_MS) {
        last_tick = HAL_GetTick();
        parked = !parked;
        GPS_Emulator_Update_Data(parked ? MSG_START : MSG_END, 32.085300, 34.781800);
    }

    /* Safety net: if the peripheral dropped out of Listen mode, re-arm it */
    if (HAL_I2C_GetState(&hi2c2) == HAL_I2C_STATE_READY) {
        HAL_I2C_EnableListen_IT(&hi2c2);
    }
}

/* ===================== HAL I2C callbacks ===================== */

/**
 * @brief Address matched. Master read -> send snapshot; master write -> drain.
 */
void HAL_I2C_AddrCallback(I2C_HandleTypeDef *hi2c, uint8_t TransferDirection, uint16_t AddrMatchCode)
{
    (void)AddrMatchCode;
    if (hi2c->Instance != I2C2) return;

    if (TransferDirection == I2C_DIRECTION_RECEIVE) {        /* master reads */
        tx_buf     = gps_data;
        tx_version = data_version;
        HAL_I2C_Slave_Seq_Transmit_IT(hi2c, (uint8_t *)&tx_buf, sizeof(tx_buf),
                                      I2C_FIRST_AND_LAST_FRAME);
    } else {                                                  /* master writes */
        HAL_I2C_Slave_Seq_Receive_IT(hi2c, rx_dummy, sizeof(rx_dummy),
                                     I2C_FIRST_AND_LAST_FRAME);
    }
}

/**
 * @brief Full packet sent -> mark message as consumed (prevents duplicates),
 *        unless the main loop already published a newer one.
 */
void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C2 && tx_version == data_version) {
        gps_data.msg_type = MSG_NONE;
    }
}

/**
 * @brief STOP received -> go back to Listen mode.
 */
void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C2) {
        HAL_I2C_EnableListen_IT(hi2c);
    }
}

/**
 * @brief Error (e.g. master NACKed early / read fewer bytes) -> recover.
 */
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C2) return;
    if (HAL_I2C_GetState(hi2c) == HAL_I2C_STATE_READY) {
        HAL_I2C_EnableListen_IT(hi2c);
    }
}
