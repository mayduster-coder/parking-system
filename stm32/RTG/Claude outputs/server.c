/**
 * @file  server.c
 * @brief Central TCP parking server.
 *
 *  - One thread per client; receives ParkingData_t packets and answers ACK/NACK.
 *  - START opens a session, END closes it and computes the fee from the
 *    elapsed time and the city's tariff.
 *  - Prices are stored in SQLite (persistent) and mirrored in POSIX shared
 *    memory (fast lookup, readable by price_cli).
 *  - Price changes arrive through PRICE_FILE + SIGUSR1 (sent by price_cli).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sqlite3.h>

#include "common.h"
#include "prices_shared.h"

#define PORT          8080
#define LOG_FILE      "server.log"
#define DB_FILE       "parking.db"
#define MAX_SESSIONS  128
#define ACK_BYTE      0x06
#define NACK_BYTE     0x15
#define DEFAULT_CITY  "DefaultCity"

/* ============================================================
 * Globals
 * ============================================================ */
static sqlite3        *db;
static pthread_mutex_t db_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static PriceTable_t   *g_prices;               /**< Shared memory table */
static long            g_price_file_offset;    /**< Bytes of PRICE_FILE already applied */

/** @brief Open parking session (between START and END) */
typedef struct {
    int      used;
    uint16_t client_id;
    uint32_t start_ts;
    char     city[CITY_NAME_LEN];
} Session_t;

static Session_t       sessions[MAX_SESSIONS];
static pthread_mutex_t sessions_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * Logger
 * ============================================================ */
/**
 * @brief Thread-safe timestamped logger.
 * @param level  "INFO" / "WARN" / "ERROR"
 * @param format printf-style format
 */
static void write_log(const char *level, const char *format, ...)
{
    pthread_mutex_lock(&log_lock);
    FILE *file = fopen(LOG_FILE, "a");
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
 * Database - prices
 * ============================================================ */
/**
 * @brief Run a prepared statement on the prices table.
 * @param sql   SQL with ?1 = city, ?2 = hourly, ?3 = base (as needed)
 * @param city  City name
 * @param nrates 0 = only city is bound, 2 = hourly and base are bound too
 * @return number of changed rows, or -1 on SQL error
 */
static int exec_city_stmt(const char *sql, const char *city,
                          int nrates, double hourly, double base)
{
    sqlite3_stmt *stmt = NULL;
    int changes = -1;

    pthread_mutex_lock(&db_lock);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, city, -1, SQLITE_TRANSIENT);
        if (nrates == 2) {
            sqlite3_bind_double(stmt, 2, hourly);
            sqlite3_bind_double(stmt, 3, base);
        }
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            changes = sqlite3_changes(db);
        } else {
            write_log("ERROR", "SQL step failed for '%s': %s", city, sqlite3_errmsg(db));
        }
    } else {
        write_log("ERROR", "SQL prepare failed: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_lock);
    return changes;
}

/** @brief Add a new city. Fails (logs) if the city already exists. */
static void db_add_city(const char *city, double hourly, double base)
{
    int n = exec_city_stmt("INSERT OR IGNORE INTO prices (city, hourly_rate, base_rate) "
                           "VALUES (?1, ?2, ?3);", city, 2, hourly, base);
    if (n == 1) {
        printf("[Prices] ADD    '%s' -> hourly %.2f, base %.2f\n", city, hourly, base);
        write_log("INFO", "ADD city '%s' hourly=%.2f base=%.2f", city, hourly, base);
    } else if (n == 0) {
        printf("[Prices] ADD    '%s' ignored - city already exists (use UPDATE)\n", city);
        write_log("WARN", "ADD ignored, city '%s' already exists", city);
    }
}

/** @brief Update an existing city. Logs a warning if it does not exist. */
static void db_update_city(const char *city, double hourly, double base)
{
    int n = exec_city_stmt("UPDATE prices SET hourly_rate = ?2, base_rate = ?3 "
                           "WHERE city = ?1;", city, 2, hourly, base);
    if (n == 1) {
        printf("[Prices] UPDATE '%s' -> hourly %.2f, base %.2f\n", city, hourly, base);
        write_log("INFO", "UPDATE city '%s' hourly=%.2f base=%.2f", city, hourly, base);
    } else if (n == 0) {
        printf("[Prices] UPDATE '%s' ignored - city not found (use ADD)\n", city);
        write_log("WARN", "UPDATE ignored, city '%s' not found", city);
    }
}

/** @brief Remove a city. */
static void db_remove_city(const char *city)
{
    int n = exec_city_stmt("DELETE FROM prices WHERE city = ?1;", city, 0, 0, 0);
    if (n == 1) {
        printf("[Prices] REMOVE '%s'\n", city);
        write_log("INFO", "REMOVE city '%s'", city);
    } else if (n == 0) {
        printf("[Prices] REMOVE '%s' ignored - city not found\n", city);
        write_log("WARN", "REMOVE ignored, city '%s' not found", city);
    }
}

/* ============================================================
 * Shared memory
 * ============================================================ */
/**
 * @brief Create/attach the shared price table and init its process-shared mutex.
 * @return 0 on success, -1 on error
 */
static int shm_init(void)
{
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        write_log("ERROR", "shm_open failed: %s", strerror(errno));
        return -1;
    }
    if (ftruncate(fd, sizeof(PriceTable_t)) < 0) {
        write_log("ERROR", "ftruncate failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    g_prices = mmap(NULL, sizeof(PriceTable_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (g_prices == MAP_FAILED) {
        write_log("ERROR", "mmap failed: %s", strerror(errno));
        return -1;
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&g_prices->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    g_prices->count = 0;
    g_prices->version = 0;
    return 0;
}

/** @brief Reload the shared-memory table from the prices table in SQLite. */
static void shm_sync_from_db(void)
{
    CityPrice_t tmp[MAX_CITIES];
    int count = 0;
    sqlite3_stmt *stmt = NULL;

    pthread_mutex_lock(&db_lock);
    if (sqlite3_prepare_v2(db, "SELECT city, hourly_rate, base_rate FROM prices ORDER BY city;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW && count < MAX_CITIES) {
            snprintf(tmp[count].city, CITY_NAME_LEN, "%s",
                     (const char *)sqlite3_column_text(stmt, 0));
            tmp[count].hourly_rate = sqlite3_column_double(stmt, 1);
            tmp[count].base_rate   = sqlite3_column_double(stmt, 2);
            count++;
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_lock);

    pthread_mutex_lock(&g_prices->lock);
    memcpy(g_prices->cities, tmp, sizeof(CityPrice_t) * count);
    g_prices->count = count;
    g_prices->version++;
    pthread_mutex_unlock(&g_prices->lock);

    write_log("INFO", "Shared memory reloaded: %d cities (version %u)", count, g_prices->version);
}

/**
 * @brief Look up a city's tariff in shared memory.
 * @return 0 if found, -1 otherwise
 */
static int shm_lookup(const char *city, double *hourly, double *base)
{
    int found = -1;
    pthread_mutex_lock(&g_prices->lock);
    for (int i = 0; i < g_prices->count; i++) {
        if (strcmp(g_prices->cities[i].city, city) == 0) {
            *hourly = g_prices->cities[i].hourly_rate;
            *base   = g_prices->cities[i].base_rate;
            found = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_prices->lock);
    return found;
}

/* ============================================================
 * Price update file (written by price_cli)
 * ============================================================ */
/**
 * @brief Parse and apply one line of PRICE_FILE.
 * @param line e.g. "1730000000 UPDATE Tel Aviv|18.00|6.00"
 */
static void apply_price_line(char *line)
{
    long ts;
    char cmd[16];
    int consumed = 0;

    line[strcspn(line, "\r\n")] = '\0';
    if (sscanf(line, "%ld %15s %n", &ts, cmd, &consumed) != 2 || consumed == 0) {
        if (line[0] != '\0') write_log("WARN", "Bad line in %s: '%s'", PRICE_FILE, line);
        return;
    }
    char *args = line + consumed;

    if (strcmp(cmd, "REMOVE") == 0) {
        db_remove_city(args);
        return;
    }

    /* ADD / UPDATE: city|hourly|base */
    char *p1 = strchr(args, '|');
    char *p2 = p1 ? strchr(p1 + 1, '|') : NULL;
    if (!p1 || !p2) {
        write_log("WARN", "Bad %s arguments: '%s'", cmd, args);
        return;
    }
    *p1 = '\0';
    *p2 = '\0';
    double hourly = strtod(p1 + 1, NULL);
    double base   = strtod(p2 + 1, NULL);
    if (hourly < 0 || base < 0 || args[0] == '\0') {
        write_log("WARN", "Invalid values in %s line for '%s'", cmd, args);
        return;
    }

    if (strcmp(cmd, "ADD") == 0)         db_add_city(args, hourly, base);
    else if (strcmp(cmd, "UPDATE") == 0) db_update_city(args, hourly, base);
    else write_log("WARN", "Unknown command '%s' in %s", cmd, PRICE_FILE);
}

/**
 * @brief Apply all lines added to PRICE_FILE since the last call,
 *        then reload shared memory. Called on SIGUSR1.
 */
static void process_price_file(void)
{
    FILE *f = fopen(PRICE_FILE, "r");
    if (!f) {
        write_log("WARN", "SIGUSR1 received but %s cannot be opened: %s", PRICE_FILE, strerror(errno));
        return;
    }

    flock(fileno(f), LOCK_SH);              /* price_cli writes under LOCK_EX */
    fseek(f, g_price_file_offset, SEEK_SET);

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        apply_price_line(line);
    }
    g_price_file_offset = ftell(f);
    flock(fileno(f), LOCK_UN);
    fclose(f);

    shm_sync_from_db();
}

/** @brief At startup skip history already applied to the DB. */
static void init_price_file_offset(void)
{
    struct stat st;
    g_price_file_offset = (stat(PRICE_FILE, &st) == 0) ? st.st_size : 0;
}

/* ============================================================
 * Database - init and parking records
 * ============================================================ */
/** @brief Open DB, create tables, insert default prices only if missing. */
static int init_database(void)
{
    if (sqlite3_open(DB_FILE, &db) != SQLITE_OK) {
        write_log("ERROR", "Cannot open database: %s", sqlite3_errmsg(db));
        return -1;
    }

    const char *sql =
        "CREATE TABLE IF NOT EXISTS prices ("
        "  city TEXT PRIMARY KEY, hourly_rate REAL, base_rate REAL);"
        "CREATE TABLE IF NOT EXISTS customer_data ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT, client_id INTEGER, msg_type INTEGER,"
        "  latitude REAL, longitude REAL, timestamp INTEGER, calculated_fee REAL,"
        "  created_at DATETIME DEFAULT (datetime('now', 'localtime')));"
        /* Defaults only on first run - never overwrite prices changed by the user */
        "INSERT OR IGNORE INTO prices VALUES ('Tel Aviv', 15.00, 5.00);"
        "INSERT OR IGNORE INTO prices VALUES ('" DEFAULT_CITY "', 10.00, 5.00);";

    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        write_log("ERROR", "DB init failed: %s", err);
        sqlite3_free(err);
        return -1;
    }
    write_log("INFO", "Database initialized.");
    return 0;
}

/** @brief Insert one row into customer_data (prepared statement). */
static int insert_parking_record(const ParkingData_t *d, double fee)
{
    sqlite3_stmt *stmt = NULL;
    int rc = -1;

    pthread_mutex_lock(&db_lock);
    if (sqlite3_prepare_v2(db,
            "INSERT INTO customer_data (client_id, msg_type, latitude, longitude, timestamp, calculated_fee) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6);", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, d->client_id);
        sqlite3_bind_int(stmt, 2, d->msg_type);
        sqlite3_bind_double(stmt, 3, d->latitude);
        sqlite3_bind_double(stmt, 4, d->longitude);
        sqlite3_bind_int64(stmt, 5, d->timestamp);
        sqlite3_bind_double(stmt, 6, fee);
        if (sqlite3_step(stmt) == SQLITE_DONE) rc = 0;
    }
    if (rc != 0) write_log("ERROR", "Insert parking record failed: %s", sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_lock);
    return rc;
}

/* ============================================================
 * Parking logic
 * ============================================================ */
/** @brief Map GPS coordinates to a city name. */
static const char *get_city_from_coordinates(double lat, double lon)
{
    if (lat >= 32.00 && lat <= 32.15 && lon >= 34.70 && lon <= 34.85) return "Tel Aviv";
    return DEFAULT_CITY;
}

/** @brief Remember the START of a session (overwrites an unfinished one). */
static void session_start(uint16_t id, uint32_t ts, const char *city)
{
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
        snprintf(sessions[slot].city, CITY_NAME_LEN, "%s", city);
    } else {
        write_log("ERROR", "Session table full, START of client %u dropped", id);
    }
    pthread_mutex_unlock(&sessions_lock);
}

/**
 * @brief Close a session.
 * @return 0 and fills start_ts/city if a START existed, -1 otherwise
 */
static int session_end(uint16_t id, uint32_t *start_ts, char *city)
{
    int rc = -1;
    pthread_mutex_lock(&sessions_lock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].used && sessions[i].client_id == id) {
            *start_ts = sessions[i].start_ts;
            snprintf(city, CITY_NAME_LEN, "%s", sessions[i].city);
            sessions[i].used = 0;
            rc = 0;
            break;
        }
    }
    pthread_mutex_unlock(&sessions_lock);
    return rc;
}

/**
 * @brief Handle one packet from a client.
 * @return 0 -> ACK, -1 -> NACK
 */
static int handle_parking_message(const ParkingData_t *d)
{
    if (d->latitude < -90 || d->latitude > 90 || d->longitude < -180 || d->longitude > 180) {
        write_log("WARN", "Invalid coordinates from client %u", d->client_id);
        return -1;
    }

    switch (d->msg_type) {
    case MSG_NONE:
        return 0;

    case MSG_START: {
        const char *city = get_city_from_coordinates(d->latitude, d->longitude);
        session_start(d->client_id, d->timestamp, city);
        printf("[Server] START client %u in %s\n", d->client_id, city);
        write_log("INFO", "START client %u in %s (ts=%u)", d->client_id, city, d->timestamp);
        return insert_parking_record(d, 0.0);
    }

    case MSG_END: {
        uint32_t start_ts;
        char city[CITY_NAME_LEN];
        if (session_end(d->client_id, &start_ts, city) != 0) {
            write_log("WARN", "END without START for client %u", d->client_id);
            return insert_parking_record(d, 0.0);
        }

        double hourly, base;
        if (shm_lookup(city, &hourly, &base) != 0 &&
            shm_lookup(DEFAULT_CITY, &hourly, &base) != 0) {
            write_log("ERROR", "No tariff for '%s' nor %s", city, DEFAULT_CITY);
            return -1;
        }

        uint32_t seconds = (d->timestamp > start_ts) ? d->timestamp - start_ts : 0;
        double fee = base + hourly * (seconds / 3600.0);

        printf("[Server] END   client %u in %s: %u sec -> %.2f NIS (base %.2f + %.2f/h)\n",
               d->client_id, city, seconds, fee, base, hourly);
        write_log("INFO", "END client %u in %s: %u sec, fee %.2f", d->client_id, city, seconds, fee);
        return insert_parking_record(d, fee);
    }

    default:
        write_log("WARN", "Unknown msg_type %u from client %u", d->msg_type, d->client_id);
        return -1;
    }
}

/* ============================================================
 * Networking
 * ============================================================ */
/**
 * @brief Receive exactly len bytes (TCP is a stream).
 * @return 1 = full packet, 0 = peer closed cleanly, -1 = error / partial close
 */
static int recv_all(int fd, void *buf, size_t len)
{
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

typedef struct {
    int  fd;
    char ip[INET_ADDRSTRLEN];
} ClientArgs_t;

/** @brief Per-client thread: receive packets, reply ACK/NACK. */
static void *client_thread(void *arg)
{
    ClientArgs_t ca = *(ClientArgs_t *)arg;
    free(arg);

    ParkingData_t data;
    for (;;) {
        int r = recv_all(ca.fd, &data, sizeof(data));
        if (r == 0) {
            printf("[Server] Client %s disconnected\n", ca.ip);
            write_log("INFO", "Client %s disconnected", ca.ip);
            break;
        }
        if (r < 0) {
            write_log("ERROR", "Recv error from %s: %s", ca.ip, strerror(errno));
            break;
        }

        uint8_t reply = (handle_parking_message(&data) == 0) ? ACK_BYTE : NACK_BYTE;
        if (send(ca.fd, &reply, 1, MSG_NOSIGNAL) != 1) {
            write_log("ERROR", "Send ACK to %s failed: %s", ca.ip, strerror(errno));
            break;
        }
    }
    close(ca.fd);
    return NULL;
}

/* ============================================================
 * Signals (handled synchronously in a dedicated thread)
 * ============================================================ */
/** @brief Waits for SIGUSR1 (reload prices) and SIGINT/SIGTERM (shutdown). */
static void *signal_thread(void *arg)
{
    sigset_t *set = arg;
    int sig;

    for (;;) {
        if (sigwait(set, &sig) != 0) continue;

        if (sig == SIGUSR1) {
            printf("\n[Signal] SIGUSR1 - applying price updates from %s\n", PRICE_FILE);
            write_log("INFO", "SIGUSR1 received - processing %s", PRICE_FILE);
            process_price_file();
        } else {
            printf("\n[Server] Shutting down (signal %d)\n", sig);
            write_log("INFO", "Shutdown on signal %d", sig);
            unlink(PID_FILE);
            shm_unlink(SHM_NAME);
            pthread_mutex_lock(&db_lock);
            sqlite3_close(db);
            exit(EXIT_SUCCESS);
        }
    }
    return NULL;
}

/** @brief Write our PID so price_cli knows where to send SIGUSR1. */
static int write_pid_file(void)
{
    FILE *f = fopen(PID_FILE, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", getpid());
    fclose(f);
    return 0;
}

/* ============================================================
 * Main
 * ============================================================ */
int main(void)
{
    /* Block signals in all threads; signal_thread receives them with sigwait */
    static sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);   /* line-buffered even when redirected */

    write_log("INFO", "Starting server...");

    if (init_database() < 0 || shm_init() < 0) {
        fprintf(stderr, "Initialization failed - see %s\n", LOG_FILE);
        return EXIT_FAILURE;
    }
    init_price_file_offset();
    shm_sync_from_db();

    if (write_pid_file() < 0) {
        fprintf(stderr, "Cannot write %s\n", PID_FILE);
        return EXIT_FAILURE;
    }

    pthread_t sig_tid;
    if (pthread_create(&sig_tid, NULL, signal_thread, &set) != 0) {
        fprintf(stderr, "Cannot create signal thread\n");
        return EXIT_FAILURE;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(server_fd, SOMAXCONN) < 0) {
        perror("bind/listen");
        write_log("ERROR", "bind/listen on port %d failed: %s", PORT, strerror(errno));
        return EXIT_FAILURE;
    }

    printf("[Server PID %d] Listening on port %d\n", getpid(), PORT);
    printf("[Server] Update prices with: ./price_cli\n");
    write_log("INFO", "Listening on port %d, PID %d", PORT, getpid());

    for (;;) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno != EINTR) write_log("ERROR", "accept failed: %s", strerror(errno));
            continue;
        }

        ClientArgs_t *ca = malloc(sizeof(*ca));
        if (!ca) {
            close(cfd);
            continue;
        }
        ca->fd = cfd;
        inet_ntop(AF_INET, &caddr.sin_addr, ca->ip, sizeof(ca->ip));
        printf("[Server] New connection from %s\n", ca->ip);
        write_log("INFO", "Client connected from %s", ca->ip);

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, ca) != 0) {
            write_log("ERROR", "pthread_create failed for %s", ca->ip);
            close(cfd);
            free(ca);
            continue;
        }
        pthread_detach(tid);
    }
}
