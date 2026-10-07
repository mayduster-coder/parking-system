/* ============================================================
 * process1.c - Ethernet Communication Process (BeagleBone)
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

#include "common.h"

#define PIPE_PATH "/tmp/gps_fifo"
#define SERVER_IP "192.168.10.1"/* IP של השרת במחשב */
#define SERVER_PORT 8080
#define LOG_FILE "process1.log"

/* ============================================================
 * Logger Function (Section 6 - Logging and Monitoring)
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
 * TCP Connection
 * ============================================================ */
int connect_to_server(void) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        write_log("ERROR", "Socket creation failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        write_log("ERROR", "Invalid server address: %s", SERVER_IP);
        close(sock_fd);
        return -1;
    }

    if (connect(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

/* ============================================================
 * Main
 * ============================================================ */
int main(void) {
    write_log("INFO", "Starting Process 1 (Ethernet Manager)...");

    /* 1. פתיחת ה-PIPE לקריאה מ-Process 2 בשיטת O_RDWR למניעת חסימות */
    printf("[Process 1] Opening FIFO pipe at %s...\n", PIPE_PATH);
    int pipe_fd = open(PIPE_PATH, O_RDWR);
    if (pipe_fd < 0) {
        perror("[Process 1 Error] Failed to open PIPE");
        write_log("ERROR", "Failed to open FIFO pipe (%s): %s", PIPE_PATH, strerror(errno));
        exit(EXIT_FAILURE);
    }
    printf("[Process 1] FIFO opened successfully!\n");
    write_log("INFO", "FIFO pipe opened successfully.");

    /* 2. התחברות לשרת TCP */
    int sock_fd = -1;
    while (sock_fd < 0) {
        printf("[Process 1] Attempting to connect to TCP Server at %s:%d...\n", SERVER_IP, SERVER_PORT);
        write_log("INFO", "Attempting connection to TCP Server at %s:%d", SERVER_IP, SERVER_PORT);
        
        sock_fd = connect_to_server();
        if (sock_fd < 0) {
            printf("[Process 1] Connection failed. Retrying in 3 seconds...\n");
            write_log("WARN", "TCP Connection failed. Retrying in 3 seconds...");
            sleep(3);
        }
    }
    printf("[Process 1] Connected to Server successfully!\n");
    write_log("INFO", "Connected to TCP Server successfully.");

    ParkingData_t data;

    /* 3. לולאה ראשית: קריאה רציפה מה-Pipe ושליחה לשרת */
    while (1) {
        ssize_t bytes_read = read(pipe_fd, &data, sizeof(ParkingData_t));
        
        if (bytes_read == sizeof(ParkingData_t)) {
            printf("[Process 1] Read %zd bytes from Pipe (Client ID: %u). Sending to Server...\n", 
                   bytes_read, data.client_id);
            write_log("INFO", "Read packet from Pipe (Client ID: %u). Forwarding to Server...", data.client_id);

            /* שליחה לשרת TCP */
            ssize_t bytes_sent = send(sock_fd, &data, sizeof(ParkingData_t), 0);
            if (bytes_sent <= 0) {
                perror("[Process 1 Error] Send to server failed, reconnecting...");
                write_log("ERROR", "Send to server failed. Attempting to reconnect...");
                
                close(sock_fd);
                sock_fd = -1;
                while (sock_fd < 0) {
                    sleep(2);
                    sock_fd = connect_to_server();
                }
                write_log("INFO", "Reconnected to TCP Server successfully.");
                continue;
            }

            /* קבלת תשובת ACK מהשרת */
            uint8_t ack_response = 0;
            ssize_t ack_bytes = recv(sock_fd, &ack_response, sizeof(ack_response), 0);
            if (ack_bytes > 0) {
                printf("[Process 1] Received ACK from Server: 0x%02X\n", ack_response);
                write_log("INFO", "Received ACK (0x%02X) from Server for Client ID: %u", ack_response, data.client_id);
            } else {
                printf("[Process 1] ACK read error or server disconnected.\n");
                write_log("WARN", "ACK response read error or server disconnected.");
            }

        } else if (bytes_read <= 0) {
            /* הצינור ריק כרגע - ממתינים 100ms וממשיכים בלולאה */
            usleep(100000);
        }
    }

    close(pipe_fd);
    if (sock_fd >= 0) close(sock_fd);
    return 0;
}
