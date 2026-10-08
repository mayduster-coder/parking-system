/**
 * @file    server.c
 * @brief   Central TCP parking server with SQLite database and price CLI.
 *
 * Responsibilities:
 *  - Accept TCP connections from BeagleBone boards (one thread per client).
 *  - Receive ParkingData_t packets, reply ACK (0x06) or NACK (0x15).
 *  - Track parking sessions: START opens a session, END closes it and the
 *    fee is computed as  base_rate + hourly_rate * (seconds / 3600)
 *    using the tariff of the city detected from the GPS coordinates.
 *  - Store every packet and its fee in the customer_data table (SQLite).
 *  - Manage city prices from an interactive console menu
 *    (add / update / remove / show).
 *  - On SIGUSR1, reload prices from the prices file (lines: city,hourly,base).
 *  - Settings (port, file names) are read at startup from a CONFIG file
 *    (default "server.conf", or the path given as the first argument).
 *
 * Threads:
 *  - main thread    : accept() loop
 *  - client threads : one per connected client
 *  - console thread : price management menu on stdin
 *  - signal thread  : waits for SIGUSR1 with sigwait()
 *
 * Synchronization: db_lock protects the SQLite connection, sessions_lock
 * protects the session table, log_lock protects the log file.
 *
 * Build: gcc -Wall -pthread server.c -o server -lsqlite3
 * Run:   ./server [config_file]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sqlite3.h>

#include "common.h"

#define DEFAULT_CONFIG "server.conf" /**< Config file used when no argument is given */
#define PATH_LEN       128           /**< Max length of a file path in the config */
#define MAX_CITIES    64            /**< Max cities listed in the CLI */
#define MAX_SESSIONS  128           /**< Max concurrent open parking sessions */
#define MSG_START     1             /**< msg_type: parking started */
#define MSG_END       2             /**< msg_type: parking ended */
#define ACK_BYTE      0x06          /**< Reply: packet accepted */
#define NACK_BYTE     0x15          /**< Reply: packet rejected */

/* ============================================================
 * Configuration (defaults, overridden by the CONFIG file)
 * ============================================================ */
static int  g_port = 8080;                          /**< TCP listening port */
static char g_log_file[PATH_LEN]    = "server.log"; /**< Server log file */
static char g_db_file[PATH_LEN]     = "parking.db"; /**< SQLite database file */
static char g_prices_file[PATH_LEN] = "prices.txt"; /**< Prices reloaded on SIGUSR1 */

static sqlite3 *db = NULL;                                    /**< SQLite connection */
static pthread_mutex_t db_lock  = PTHREAD_MUTEX_INITIALIZER;  /**< Protects db */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;  /**< Protects the log file */

/* ============================================================
 * Logger
 * ============================================================ */

/**
 * @brief  Thread-safe: append a timestamped line to the log file.
 * @param  level   Severity string: "INFO", "WARN" or "ERROR"
 * @param  format  printf-style format string, followed by its arguments
 */
void write_log(const char *level, const char *format, ...) {
    pthread_mutex_lock(&log_lock);
    FILE *file = fopen(g_log_file, "a");
    if (file) {
        time_t now = time(NULL);
        struct tm t;
        char time_str[32];
        localtime_r(&now, &t);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &t);
        fprintf(file, "[%s] [%s] ", time_str, level);

        va_list args;
        va_start(args, format);
        vfprintf(file, format, args);
        va_end(args);

        fprintf(file, "\n");
        fclose(file);
    }
    pthread_mutex_unlock(&log_lock);
}

/* ============================================================
 * Configuration file
 * ============================================================ */

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
 * @brief  Load settings from a KEY=VALUE config file.
 *
 * Supported keys: PORT, DB_FILE, LOG_FILE, PRICES_FILE.
 * Empty lines and lines starting with '#' are ignored. Unknown keys and
 * invalid values are reported and ignored. If the file does not exist the
 * built-in defaults are kept.
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
    int line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) {
            printf("[Config] %s:%d ignored (no '=')\n", path, line_no);
            continue;
        }
        *eq = '\0';
        char *key = trim(p);
        char *value = trim(eq + 1);

        if (strcmp(key, "PORT") == 0) {
            int port = atoi(value);
            if (port > 0 && port < 65536) g_port = port;
            else printf("[Config] %s:%d invalid PORT '%s'\n", path, line_no, value);
        } else if (strcmp(key, "DB_FILE") == 0) {
            snprintf(g_db_file, PATH_LEN, "%s", value);
        } else if (strcmp(key, "LOG_FILE") == 0) {
            snprintf(g_log_file, PATH_LEN, "%s", value);
        } else if (strcmp(key, "PRICES_FILE") == 0) {
            snprintf(g_prices_file, PATH_LEN, "%s", value);
        } else {
            printf("[Config] %s:%d unknown key '%s'\n", path, line_no, key);
        }
    }
    fclose(f);
    return 0;
}

/* ============================================================
 * Dynamic City & Price Management Functions
 * ============================================================ */

/**
 * @brief  Run a parameterized SQL statement on the prices table.
 *
 * Uses a prepared statement, so city names with spaces or quotes are safe.
 * Binds ?1 = city and, if bind_rates is set, ?2 = hourly, ?3 = base.
 *
 * @param  sql          SQL text with ?1 (and optionally ?2, ?3) placeholders
 * @param  city         City name bound to ?1
 * @param  bind_rates   Non-zero to also bind hourly_rate and base_rate
 * @param  hourly_rate  Value bound to ?2
 * @param  base_rate    Value bound to ?3
 * @return Number of changed rows, or -1 on SQL error
 */
static int exec_price_sql(const char *sql, const char *city, int bind_rates,
                          double hourly_rate, double base_rate) {
    sqlite3_stmt *stmt = NULL;
    int changes = -1;

    pthread_mutex_lock(&db_lock);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, city, -1, SQLITE_TRANSIENT);
        if (bind_rates) {
            sqlite3_bind_double(stmt, 2, hourly_rate);
            sqlite3_bind_double(stmt, 3, base_rate);
        }
        if (sqlite3_step(stmt) == SQLITE_DONE)
            changes = sqlite3_changes(db);
    }
    if (changes < 0)
        write_log("ERROR", "SQL failed for city '%s': %s", city, sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_lock);
    return changes;
}

/**
 * @brief  Add a new city to the prices table.
 * @param  city_name    City name
 * @param  hourly_rate  Price per hour (NIS)
 * @param  base_rate    Fixed price per parking session (NIS)
 * @note   Does nothing (prints a message) if the city already exists.
 */
void add_city_price(const char *city_name, double hourly_rate, double base_rate) {
    int n = exec_price_sql("INSERT OR IGNORE INTO prices (city, hourly_rate, base_rate) VALUES (?1, ?2, ?3);",
                           city_name, 1, hourly_rate, base_rate);
    if (n == 1) {
        printf("[DB] City '%s' added -> Hourly: %.2f | Base: %.2f\n", city_name, hourly_rate, base_rate);
        write_log("INFO", "Added city '%s'. Hourly: %.2f, Base: %.2f", city_name, hourly_rate, base_rate);
    } else if (n == 0) {
        printf("[DB] City '%s' already exists - use Update.\n", city_name);
    }
}

/**
 * @brief  Update the prices of an existing city.
 * @param  city_name    City name (must exist)
 * @param  hourly_rate  New price per hour (NIS)
 * @param  base_rate    New fixed price per session (NIS)
 */
void update_city_price(const char *city_name, double hourly_rate, double base_rate) {
    int n = exec_price_sql("UPDATE prices SET hourly_rate = ?2, base_rate = ?3 WHERE city = ?1;",
                           city_name, 1, hourly_rate, base_rate);
    if (n == 1) {
        printf("[DB] Price updated for '%s' -> Hourly: %.2f | Base: %.2f\n", city_name, hourly_rate, base_rate);
        write_log("INFO", "Updated city '%s'. Hourly: %.2f, Base: %.2f", city_name, hourly_rate, base_rate);
    } else if (n == 0) {
        printf("[DB] City '%s' not found.\n", city_name);
    }
}

/**
 * @brief  Remove a city from the prices table.
 * @param  city_name  City name
 */
void remove_city(const char *city_name) {
    int n = exec_price_sql("DELETE FROM prices WHERE city = ?1;", city_name, 0, 0, 0);
    if (n == 1) {
        printf("[DB] City '%s' removed successfully.\n", city_name);
        write_log("INFO", "City '%s' removed from database.", city_name);
    } else if (n == 0) {
        printf("[DB] City '%s' not found.\n", city_name);
    }
}

/**
 * @brief  Read the tariff of a city.
 * @param  city         City name
 * @param[out] hourly_rate  Price per hour
 * @param[out] base_rate    Fixed price per session
 * @return 0 if the city was found, -1 otherwise (outputs unchanged)
 */
static int get_city_price(const char *city, double *hourly_rate, double *base_rate) {
    sqlite3_stmt *stmt = NULL;
    int found = -1;

    pthread_mutex_lock(&db_lock);
    if (sqlite3_prepare_v2(db, "SELECT hourly_rate, base_rate FROM prices WHERE city = ?1;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, city, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            *hourly_rate = sqlite3_column_double(stmt, 0);
            *base_rate   = sqlite3_column_double(stmt, 1);
            found = 0;
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_lock);
    return found;
}

/**
 * @brief  Print the price table as a numbered list.
 * @param[out] cities  If not NULL, receives the city names in display order
 *                     (array of MAX_CITIES strings of 64 chars)
 * @return Number of cities printed
 */
int show_all_prices(char cities[][64]) {
    sqlite3_stmt *stmt = NULL;
    int count = 0;

    pthread_mutex_lock(&db_lock);
    printf("\n--- Current City Pricing Table ---\n");
    if (sqlite3_prepare_v2(db, "SELECT city, hourly_rate, base_rate FROM prices ORDER BY city;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW && count < MAX_CITIES) {
            const char *city = (const char *)sqlite3_column_text(stmt, 0);
            printf("%2d. City: %-15s | Hourly Rate: %6.2f | Base Rate: %6.2f\n", count + 1, city,
                   sqlite3_column_double(stmt, 1), sqlite3_column_double(stmt, 2));
            if (cities) snprintf(cities[count], 64, "%s", city);
            count++;
        }
    }
    if (count == 0) printf("(no cities)\n");
    printf("----------------------------------\n");
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_lock);
    return count;
}

/**
 * @brief  Map GPS coordinates to a city name.
 * @param  lat  Latitude in degrees
 * @param  lon  Longitude in degrees
 * @return "Tel Aviv" inside its bounding box, otherwise "DefaultCity"
 */
const char* get_city_from_coordinates(double lat, double lon) {
    /* Tel Aviv bounding box (center 32.0853, 34.7818) */
    if (lat >= 32.00 && lat <= 32.15 && lon >= 34.70 && lon <= 34.85) {
        return "Tel Aviv";
    }
    return "DefaultCity";
}

/**
 * @brief  Load prices from the prices file into the database (called on SIGUSR1).
 *
 * Each line has the form  city,hourly_rate,base_rate  (e.g. "Haifa,20,3").
 * Existing cities are updated, new cities are added (upsert).
 * Invalid lines are ignored.
 */
static void reload_prices_from_file(void) {
    FILE *f = fopen(g_prices_file, "r");
    if (!f) {
        printf("\n[Signal] Cannot open %s\n", g_prices_file);
        write_log("WARN", "SIGUSR1: cannot open %s: %s", g_prices_file, strerror(errno));
        return;
    }

    char line[128], city[64];
    double hourly, base;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " %63[^,],%lf,%lf", city, &hourly, &base) == 3 && hourly >= 0 && base >= 0) {
            exec_price_sql("INSERT INTO prices (city, hourly_rate, base_rate) VALUES (?1, ?2, ?3) "
                           "ON CONFLICT(city) DO UPDATE SET hourly_rate = excluded.hourly_rate, "
                           "base_rate = excluded.base_rate;", city, 1, hourly, base);
            printf("[Signal] %s -> Hourly: %.2f | Base: %.2f\n", city, hourly, base);
            write_log("INFO", "Reloaded from file: %s hourly=%.2f base=%.2f", city, hourly, base);
        }
    }
    fclose(f);
}

/* ============================================================
 * Database
 * ============================================================ */

/**
 * @brief  Open the SQLite database and create the tables if needed.
 *
 * Tables:
 *  - prices(city PK, hourly_rate, base_rate)
 *  - customer_data(id, client_id, msg_type, latitude, longitude,
 *                  timestamp, calculated_fee, created_at)
 *
 * Default prices are inserted only if missing, so prices changed by the
 * user are kept across restarts.
 *
 * @return 0 on success, -1 on error
 */
int init_database(void) {
    if (sqlite3_open(g_db_file, &db) != SQLITE_OK) {
        write_log("ERROR", "Cannot open database: %s", sqlite3_errmsg(db));
        return -1;
    }

    const char *sql =
        /* prices table */
        "CREATE TABLE IF NOT EXISTS prices ("
        "city TEXT PRIMARY KEY, hourly_rate REAL, base_rate REAL);"
        /* parking records table */
        "CREATE TABLE IF NOT EXISTS customer_data ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, client_id INTEGER, msg_type INTEGER, "
        "latitude REAL, longitude REAL, timestamp INTEGER, calculated_fee REAL, "
        "created_at DATETIME DEFAULT (datetime('now', 'localtime')));"
        /* default prices - only if missing */
        "INSERT OR IGNORE INTO prices VALUES ('Tel Aviv', 15.00, 5.00);"
        "INSERT OR IGNORE INTO prices VALUES ('DefaultCity', 10.00, 5.00);";

    char *err_msg = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err_msg) != SQLITE_OK) {
        write_log("ERROR", "SQL error during initialization: %s", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    printf("[DB] Database and tables initialized successfully.\n");
    write_log("INFO", "Database and tables initialized successfully.");
    return 0;
}

/* ============================================================
 * Parking Sessions (START -> END)
 * ============================================================ */
/** @brief An open parking session (between START and END). */
typedef struct {
    int      used;       /**< 1 if this slot holds an open session */
    uint16_t client_id;  /**< Client that started parking */
    uint32_t start_ts;   /**< Timestamp of the START message */
} Session_t;

static Session_t sessions[MAX_SESSIONS];                          /**< Open sessions */
static pthread_mutex_t sessions_lock = PTHREAD_MUTEX_INITIALIZER; /**< Protects sessions */

/**
 * @brief  Open (or restart) the parking session of a client.
 * @param  id  Client ID
 * @param  ts  START timestamp
 * @note   If the table is full the START is silently dropped.
 */
static void session_start(uint16_t id, uint32_t ts) {
    pthread_mutex_lock(&sessions_lock);
    int slot = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].used && sessions[i].client_id == id) { slot = i; break; }
        if (!sessions[i].used && slot < 0) slot = i;
    }
    if (slot >= 0) {
        sessions[slot].used = 1;
        sessions[slot].client_id = id;
        sessions[slot].start_ts = ts;
    }
    pthread_mutex_unlock(&sessions_lock);
}

/**
 * @brief  Close the parking session of a client.
 * @param  id  Client ID
 * @param[out] start_ts  Timestamp of the matching START
 * @return 0 if a session was found and closed, -1 if there was no START
 */
static int session_end(uint16_t id, uint32_t *start_ts) {
    int rc = -1;
    pthread_mutex_lock(&sessions_lock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].used && sessions[i].client_id == id) {
            *start_ts = sessions[i].start_ts;
            sessions[i].used = 0;
            rc = 0;
            break;
        }
    }
    pthread_mutex_unlock(&sessions_lock);
    return rc;
}

/**
 * @brief  Process one parking packet: update sessions, compute the fee and
 *         store the record in customer_data.
 *
 *  - msg_type 0 (no data): ignored, ACK.
 *  - START: opens a session, record stored with fee 0.
 *  - END:   closes the session, fee = base + hourly * seconds / 3600.
 *  - Other msg_type: rejected (NACK).
 *
 * @param  data  Packet received from the client
 * @return 0 -> reply ACK, -1 -> reply NACK
 */
int save_customer_parking(const ParkingData_t *data) {
    if (data->msg_type == 0) return 0;   /* no new data */
    if (data->msg_type != MSG_START && data->msg_type != MSG_END) {
        write_log("WARN", "Unknown msg_type %u from client %u", data->msg_type, data->client_id);
        return -1;
    }

    /* 1. Detect the city from the GPS coordinates */
    const char *city_name = get_city_from_coordinates(data->latitude, data->longitude);

    /* 2. Fee: START = 0, END = base + hourly * hours */
    double calculated_fee = 0.0;
    if (data->msg_type == MSG_START) {
        session_start(data->client_id, data->timestamp);
        printf("[Server] START client %u in %s\n", data->client_id, city_name);
    } else {
        uint32_t start_ts;
        if (session_end(data->client_id, &start_ts) == 0) {
            double hourly_rate = 10.0, base_rate = 5.0;
            if (get_city_price(city_name, &hourly_rate, &base_rate) != 0)
                get_city_price("DefaultCity", &hourly_rate, &base_rate);

            uint32_t seconds = (data->timestamp > start_ts) ? data->timestamp - start_ts : 0;
            calculated_fee = base_rate + hourly_rate * (seconds / 3600.0);
            printf("[Server] END client %u in %s: %u sec -> %.2f NIS\n",
                   data->client_id, city_name, seconds, calculated_fee);
        } else {
            printf("[Server] END client %u without START\n", data->client_id);
            write_log("WARN", "END without START for client %u", data->client_id);
        }
    }

    /* 3. Store the record */
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    pthread_mutex_lock(&db_lock);
    if (sqlite3_prepare_v2(db,
            "INSERT INTO customer_data (client_id, msg_type, latitude, longitude, timestamp, calculated_fee) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6);", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, data->client_id);
        sqlite3_bind_int(stmt, 2, data->msg_type);
        sqlite3_bind_double(stmt, 3, data->latitude);
        sqlite3_bind_double(stmt, 4, data->longitude);
        sqlite3_bind_int64(stmt, 5, data->timestamp);
        sqlite3_bind_double(stmt, 6, calculated_fee);
        if (sqlite3_step(stmt) == SQLITE_DONE) rc = 0;
    }
    if (rc == 0)
        write_log("INFO", "Logged parking record for Client ID: %u (City: %s, Type: %u, Fee: %.2f)",
                  data->client_id, city_name, data->msg_type, calculated_fee);
    else
        write_log("ERROR", "Failed to insert customer parking data: %s", sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_lock);
    return rc;
}

/* ============================================================
 * Interactive Console Thread (CLI)
 * ============================================================ */

/**
 * @brief  Print a prompt and read one line from stdin (spaces allowed).
 * @param  prompt  Text to print
 * @param[out] buf  Destination buffer (newline removed)
 * @param  size    Buffer size
 * @return Length of the line, or -1 on EOF
 */
static int read_line(const char *prompt, char *buf, size_t size) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int)size, stdin)) return -1;
    buf[strcspn(buf, "\r\n")] = '\0';
    return (int)strlen(buf);
}

/**
 * @brief  Read a non-negative price from stdin.
 * @param  prompt        Text to print
 * @param  current       Value returned when the user presses Enter (if allowed)
 * @param  keep_allowed  Non-zero: empty input keeps @p current
 * @param[out] out       The price entered
 * @return 0 on success, -1 on invalid input or EOF
 */
static int read_rate(const char *prompt, double current, int keep_allowed, double *out) {
    char buf[32], *end;
    if (read_line(prompt, buf, sizeof(buf)) < 0) return -1;
    if (buf[0] == '\0' && keep_allowed) { *out = current; return 0; }
    double v = strtod(buf, &end);
    if (end == buf || *end != '\0' || v < 0) {
        printf("Invalid value.\n");
        return -1;
    }
    *out = v;
    return 0;
}

/**
 * @brief  Show the numbered price list and let the user pick a city.
 * @param[out] city_out  Selected city name (at least 64 chars)
 * @return 0 on valid selection, -1 otherwise
 */
static int choose_city(char *city_out) {
    char cities[MAX_CITIES][64], buf[16];
    int n = show_all_prices(cities);
    if (n == 0) return -1;
    if (read_line("Select city number: ", buf, sizeof(buf)) < 0) return -1;
    int idx = atoi(buf) - 1;
    if (idx < 0 || idx >= n) {
        printf("Invalid selection.\n");
        return -1;
    }
    strcpy(city_out, cities[idx]);
    return 0;
}

/**
 * @brief  Console thread: price management menu
 *         (1 Add, 2 Update, 3 Remove, 4 Show).
 * @param  arg  Unused
 * @return Never returns
 */
void *console_thread_handler(void *arg) {
    (void)arg;
    char buf[16], city[64];
    double hourly, base;

    while (1) {
        printf("\n==== Parking Server Management CLI ====\n");
        printf("1. Add City\n");
        printf("2. Update City Price\n");
        printf("3. Remove City\n");
        printf("4. Show All City Prices\n");
        if (read_line("Select option: ", buf, sizeof(buf)) < 0) {
            sleep(1);       /* stdin closed (e.g. running in background) */
            continue;
        }

        switch (atoi(buf)) {
        case 1:
            if (read_line("Enter city name: ", city, sizeof(city)) <= 0) break;
            if (read_rate("Enter hourly rate: ", 0, 0, &hourly) < 0) break;
            if (read_rate("Enter base rate: ", 0, 0, &base) < 0) break;
            add_city_price(city, hourly, base);
            break;

        case 2: {
            double cur_hourly, cur_base;
            char prompt[64];
            if (choose_city(city) < 0) break;
            if (get_city_price(city, &cur_hourly, &cur_base) < 0) break;
            printf("Updating '%s' (press Enter to keep current value)\n", city);
            snprintf(prompt, sizeof(prompt), "New hourly rate [%.2f]: ", cur_hourly);
            if (read_rate(prompt, cur_hourly, 1, &hourly) < 0) break;
            snprintf(prompt, sizeof(prompt), "New base rate [%.2f]: ", cur_base);
            if (read_rate(prompt, cur_base, 1, &base) < 0) break;
            update_city_price(city, hourly, base);
            break;
        }

        case 3:
            if (choose_city(city) < 0) break;
            if (read_line("Are you sure? (y/n): ", buf, sizeof(buf)) < 0) break;
            if (buf[0] == 'y' || buf[0] == 'Y') remove_city(city);
            break;

        case 4:
            show_all_prices(NULL);
            break;

        default:
            printf("Invalid option.\n");
        }
    }
    return NULL;
}

/* ============================================================
 * Signal thread
 * ============================================================ */

/**
 * @brief  Signal thread: waits for SIGUSR1 and reloads prices from file.
 *
 * SIGUSR1 is blocked in all threads and received here synchronously with
 * sigwait(), so it is safe to call printf() and SQLite functions.
 *
 * @param  arg  Pointer to the sigset_t containing SIGUSR1
 * @return Never returns
 */
void *signal_thread_handler(void *arg) {
    sigset_t *set = arg;
    int sig;
    while (1) {
        if (sigwait(set, &sig) == 0 && sig == SIGUSR1) {
            printf("\n[Signal] SIGUSR1 received. Reloading prices from %s...\n", g_prices_file);
            reload_prices_from_file();
        }
    }
    return NULL;
}

/* ============================================================
 * Client Thread
 * ============================================================ */

/**
 * @brief  Receive exactly @p len bytes from a TCP socket.
 *
 * TCP is a byte stream, so one recv() may return only part of a packet.
 * This function loops until the whole packet has arrived.
 *
 * @param  fd   Connected socket
 * @param[out] buf  Destination buffer (at least @p len bytes)
 * @param  len  Number of bytes to read
 * @return 1 = full packet, 0 = peer closed cleanly, -1 = error / partial close
 */
static int recv_all(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, (char *)buf + got, len - got, 0);
        if (n == 0) return (got == 0) ? 0 : -1;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 1;
}

/** @brief Arguments passed to a client thread. */
typedef struct {
    int  fd;                     /**< Client socket */
    char ip[INET_ADDRSTRLEN];    /**< Client IP address (for logs) */
} ClientInfo_t;

/**
 * @brief  Client thread: receive packets, process them, reply ACK/NACK.
 * @param  arg  Heap-allocated ClientInfo_t (freed by this thread)
 * @return NULL when the client disconnects
 */
void *client_thread_handler(void *arg) {
    ClientInfo_t client = *(ClientInfo_t *)arg;
    free(arg);

    ParkingData_t data;
    while (1) {
        int r = recv_all(client.fd, &data, sizeof(ParkingData_t));
        if (r == 0) {
            printf("\n[Server] Client disconnected (%s)\n", client.ip);
            write_log("INFO", "Client disconnected (%s)", client.ip);
            break;
        }
        if (r < 0) {
            write_log("ERROR", "Recv error from client %s: %s", client.ip, strerror(errno));
            break;
        }

        printf("\n[Server] Received -> ID: %u, Type: %u, Lat: %.6f, Lon: %.6f, Time: %u\n",
               data.client_id, data.msg_type, data.latitude, data.longitude, data.timestamp);

        /* Store in DB and reply ACK/NACK to process 1 */
        uint8_t reply = (save_customer_parking(&data) == 0) ? ACK_BYTE : NACK_BYTE;
        if (send(client.fd, &reply, 1, MSG_NOSIGNAL) != 1) {
            write_log("ERROR", "Send ACK to %s failed", client.ip);
            break;
        }
    }

    close(client.fd);
    return NULL;
}

/* ============================================================
 * Main
 * ============================================================ */

/**
 * @brief  Entry point: load config, init DB, start signal and console
 *         threads, then accept clients forever (one thread per client).
 * @param  argc  Argument count
 * @param  argv  argv[1] (optional) = config file path
 * @return EXIT_FAILURE on initialization error; otherwise never returns.
 */
int main(int argc, char *argv[]) {
    const char *config_path = (argc > 1) ? argv[1] : DEFAULT_CONFIG;
    load_config(config_path);
    printf("[Config] PORT=%d DB_FILE=%s LOG_FILE=%s PRICES_FILE=%s\n",
           g_port, g_db_file, g_log_file, g_prices_file);

    /* Block SIGUSR1 in all threads - only the signal thread receives it */
    static sigset_t sig_set;
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &sig_set, NULL);
    signal(SIGPIPE, SIG_IGN);   /* a disconnected client must not kill the server */

    write_log("INFO", "Starting Server Application...");

    if (init_database() < 0) {
        exit(EXIT_FAILURE);
    }

    pthread_t console_tid, signal_tid;
    if (pthread_create(&signal_tid, NULL, signal_thread_handler, &sig_set) != 0 ||
        pthread_create(&console_tid, NULL, console_thread_handler, NULL) != 0) {
        perror("pthread_create");
        exit(EXIT_FAILURE);
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        write_log("ERROR", "Socket creation failed.");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(g_port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        write_log("ERROR", "Bind failed on port %d", g_port);
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("Listen failed");
        write_log("ERROR", "Listen failed.");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("[Server PID: %d] Ready.\n", getpid());
    printf("Server listening on port %d...\n", g_port);
    write_log("INFO", "Server listening on port %d with PID %d (config: %s)", g_port, getpid(), config_path);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen);
        if (client_fd < 0) {
            write_log("ERROR", "Accept failed: %s", strerror(errno));
            continue;
        }

        ClientInfo_t *client = malloc(sizeof(ClientInfo_t));
        if (!client) {
            close(client_fd);
            continue;
        }
        client->fd = client_fd;
        inet_ntop(AF_INET, &client_addr.sin_addr, client->ip, sizeof(client->ip));
        printf("\n[Server] New connection from %s\n", client->ip);
        write_log("INFO", "Client connected from IP: %s", client->ip);

        /* One thread per client - main thread keeps accepting */
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread_handler, client) != 0) {
            write_log("ERROR", "Failed to create client thread");
            close(client_fd);
            free(client);
            continue;
        }
        pthread_detach(tid);
    }

    close(server_fd);
    sqlite3_close(db);
    return 0;
}
