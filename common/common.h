#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint16_t client_id;  // 2 bytes
    uint8_t  msg_type;   // 1 byte
    double   latitude;   // 8 bytes
    double   longitude;  // 8 bytes
    uint32_t timestamp;  // 4 bytes
} ParkingData_t;


#endif
