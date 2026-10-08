/**
 * @file    process1.c
 * @brief   BeagleBone process 1 - Ethernet (TCP client) communication.
 *
 * Reads ParkingData_t packets that process 2 writes to the FIFO and forwards
 * each one to the central TCP server over Ethernet. After every packet it
 * waits for a 1-byte reply from the server (0x06 = ACK, 0x15 = NACK).
 * If the connection drops, the process reconnects automatically.
 *
 * Data flow: process2 --(FIFO /tmp/gps_fifo)--> process1 --(TCP)--> server
 *
 * The server address is read at startup from a CONFIG file
 * (default "process1.conf", or the path given as the first argument).
 *
 * Run: ./process1 [config_file]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

#include "common.h"

#define PIPE_PATH    "/tmp/gps_fifo"   /**< FIFO shared with process 2 */
#define DEFAULT_CONFIG  "process1.conf"  /**< Config file used when no argument is given */
#define LOG_FILE        "process1.log"   /**< Log file of this process */

/* Configuration (defaults, overridden by the CONFIG file) */
static char g_server_ip[64] = "192.168.10.1";  /**< Server IP address */
static int  g_server_port   = 8080;            /**< Server TCP port */

/**
 * @brief  Append a timestamped line to the log file.
 *
 * Opens and closes the file on every call so the log is always flushed,
 * even if the process is killed.
 *
 * @param  level   Severity string: "INFO", "WARN" or "ERROR"
 * @param  format  printf-style format string, followed by its arguments
 */
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

/**
 * @brief  Remove leading and trailing whitespace in place.
 * @param  s  String to trim
 * @return Pointer to the first non-space character of @p s
 */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

/**
 * @brief  Load SERVER_IP and SERVER_PORT from a KEY=VALUE config file.
 *
 * Empty lines and lines starting with '#' are ignored. If the file does not
 * exist the built-in defaults are kept.
 *
 * @param  path  Config file path
 * @return 0 if the file was read, -1 if it could not be opened
 */
static int load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("[Config] %s not found - using defaults\n", path);
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *value = trim(eq + 1);

        if (strcmp(key, "SERVER_IP") == 0) {
            snprintf(g_server_ip, sizeof(g_server_ip), "%s", value);
        } else if (strcmp(key, "SERVER_PORT") == 0) {
            int port = atoi(value);
            if (port > 0 && port < 65536) g_server_port = port;
        }
    }
    fclose(f);
    return 0;
}

/**
 * @brief  Create a TCP socket and connect it to the configured server.
 *
 * @return Connected socket descriptor on success, -1 on failure
 *         (the socket is closed on failure, nothing leaks).
 */
int connect_to_server(void) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        write_log("ERROR", "Socket creation failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(g_server_port);

    if (inet_pton(AF_INET, g_server_ip, &serv_addr.sin_addr) <= 0) {
        write_log("ERROR", "Invalid server address: %s", g_server_ip);
        close(sock_fd);
        return -1;
    }

    if (connect(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

/**
 * @brief  Entry point: load config, open the FIFO, connect to the server,
 *         then forward packets forever.
 *
 * Steps:
 *  -# Load the server address from the config file.
 *  -# Create the FIFO if needed (so start order does not matter) and open it
 *     O_RDWR so open() does not block while process 2 is not running yet.
 *  -# Connect to the server, retrying every 3 seconds until it succeeds.
 *  -# Loop: read one ParkingData_t from the FIFO, send it to the server,
 *     wait for the ACK byte. On send failure, reconnect.
 *
 * @param  argc  Argument count
 * @param  argv  argv[1] (optional) = config file path
 * @return EXIT_FAILURE if the FIFO cannot be opened; otherwise never returns.
 */
int main(int argc, char *argv[]) {
    write_log("INFO", "Starting Process 1 (Ethernet Manager)...");

    load_config((argc > 1) ? argv[1] : DEFAULT_CONFIG);
    printf("[Config] SERVER_IP=%s SERVER_PORT=%d\n", g_server_ip, g_server_port);
    write_log("INFO", "Config: server %s:%d", g_server_ip, g_server_port);

    /* Create the FIFO if process 2 has not created it yet */
    if (mkfifo(PIPE_PATH, 0666) < 0 && errno != EEXIST) {
        perror("[Process 1 Error] Failed to create FIFO");
        write_log("ERROR", "Failed to create FIFO (%s): %s", PIPE_PATH, strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* 1. Open the FIFO (O_RDWR prevents blocking until a writer exists) */
    printf("[Process 1] Opening FIFO pipe at %s...\n", PIPE_PATH);
    int pipe_fd = open(PIPE_PATH, O_RDWR);
    if (pipe_fd < 0) {
        perror("[Process 1 Error] Failed to open PIPE");
        write_log("ERROR", "Failed to open FIFO pipe (%s): %s", PIPE_PATH, strerror(errno));
        exit(EXIT_FAILURE);
    }
    printf("[Process 1] FIFO opened successfully!\n");
    write_log("INFO", "FIFO pipe opened successfully.");

    /* 2. Connect to the TCP server (retry until success) */
    int sock_fd = -1;
    while (sock_fd < 0) {
        printf("[Process 1] Attempting to connect to TCP Server at %s:%d...\n", g_server_ip, g_server_port);
        write_log("INFO", "Attempting connection to TCP Server at %s:%d", g_server_ip, g_server_port);

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

    /* 3. Main loop: FIFO -> TCP server */
    while (1) {
        ssize_t bytes_read = read(pipe_fd, &data, sizeof(ParkingData_t));

        if (bytes_read == sizeof(ParkingData_t)) {
            printf("[Process 1] Read %zd bytes from Pipe (Client ID: %u). Sending to Server...\n",
                   bytes_read, data.client_id);
            write_log("INFO", "Read packet from Pipe (Client ID: %u). Forwarding to Server...", data.client_id);

            /* Send the packet to the server */
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

            /* Wait for the server reply (0x06 = ACK, 0x15 = NACK) */
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
            /* FIFO empty - wait 100 ms and try again */
            usleep(100000);
        }
    }

    close(pipe_fd);
    if (sock_fd >= 0) close(sock_fd);
    return 0;
}
