/* ============================================================
 * process2.c - I2C Communication Process (BeagleBone)
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

#include "common.h"

#define PIPE_PATH "/tmp/gps_fifo"
#define I2C_BUS "/dev/i2c-2"
#define STM32_I2C_ADDR 0x30
#define LOG_FILE "process2.log"

/* ============================================================
 * Logger Function
 * ============================================================ */
void write_log(const char *level, const char *format, ...) {
    FILE *file = fopen(LOG_FILE, "a");
    if (!file) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);

    fprintf(file, "[%s] [%s] ", time_str, level);

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);

    fprintf(file, "\n");
    fclose(file);
}

/* ============================================================
 * Main
 * ============================================================ */
int main(void) {
    write_log("INFO", "Starting Process 2 (I2C Manager)...");

    /* 1. יצירת ופתיחת ה-FIFO Pipe */
    if (mkfifo(PIPE_PATH, 0666) < 0) {
        if (errno != EEXIST) {
            perror("[Process 2 Error] Failed to create FIFO");
            write_log("ERROR", "Failed to create FIFO (%s): %s", PIPE_PATH, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    printf("[Process 2] Opening FIFO pipe at %s...\n", PIPE_PATH);
    int pipe_fd = open(PIPE_PATH, O_RDWR);
    if (pipe_fd < 0) {
        perror("[Process 2 Error] Failed to open PIPE");
        write_log("ERROR", "Failed to open FIFO pipe (%s): %s", PIPE_PATH, strerror(errno));
        exit(EXIT_FAILURE);
    }
    printf("[Process 2] FIFO opened successfully!\n");
    write_log("INFO", "FIFO pipe opened successfully.");

    /* 2. פתיחת ערוץ ה-I2C */
    int i2c_fd = open(I2C_BUS, O_RDWR);
    if (i2c_fd < 0) {
        perror("[Process 2 Error] Failed to open I2C bus");
        write_log("ERROR", "Failed to open I2C bus (%s): %s", I2C_BUS, strerror(errno));
        close(pipe_fd);
        exit(EXIT_FAILURE);
    }

    if (ioctl(i2c_fd, I2C_SLAVE, STM32_I2C_ADDR) < 0) {
        perror("[Process 2 Error] Failed to acquire bus access / talk to slave");
        write_log("ERROR", "Failed to set I2C slave address 0x%02X: %s", STM32_I2C_ADDR, strerror(errno));
        close(i2c_fd);
        close(pipe_fd);
        exit(EXIT_FAILURE);
    }
    write_log("INFO", "I2C bus initialized at address 0x%02X.", STM32_I2C_ADDR);

    ParkingData_t data;

    /* 3. לולאה ראשית לקריאה מ-I2C וכתיבה ל-PIPE */
    while (1) {
        memset(&data, 0, sizeof(ParkingData_t));

        /* קריאה רציפה של המבנה מה-STM32 */
        ssize_t bytes_read = read(i2c_fd, &data, sizeof(ParkingData_t));

        if (bytes_read == sizeof(ParkingData_t)) {
            if (data.msg_type == 0) {   /* MSG_NONE - no new data */
              usleep(200000);
              continue;
              }  
            printf("[Process 2] Read from STM32 -> ID: %u, MsgType: %u, Lat: %.6f, Lon: %.6f, Time: %u\n",
                   data.client_id, data.msg_type, data.latitude, data.longitude, data.timestamp);
            write_log("INFO", "Read I2C data (Client ID: %u, Lat: %.6f, Lon: %.6f)",
                      data.client_id, data.latitude, data.longitude);

            /* שליחה ל-Process 1 דרך ה-FIFO */
            ssize_t bytes_written = write(pipe_fd, &data, sizeof(ParkingData_t));
            if (bytes_written == sizeof(ParkingData_t)) {
                printf("[Process 2] Data successfully sent to Process 1.\n");
                write_log("INFO", "Data successfully written to FIFO pipe.");
            } else {
                perror("[Process 2 Error] Failed to write complete data to PIPE");
                write_log("ERROR", "Failed to write data to FIFO pipe: %s", strerror(errno));
            }
        } else {
            perror("[Process 2 Error] Read error from I2C");
            write_log("WARN", "I2C read error or incomplete packet size received (%zd bytes).", bytes_read);
        }

       usleep(200000); /* השהייה בין קריאות */
    }

    close(i2c_fd);
    close(pipe_fd);
    return 0;
}
