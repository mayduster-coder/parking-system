/**
 * @file  prices_shared.h
 * @brief Definitions shared by the server and the price_cli utility:
 *        update-file format, PID file and the shared-memory price table.
 *
 * Price update file format (one command per line, append-only = audit trail):
 * @code
 *   <epoch> ADD <city>|<hourly_rate>|<base_rate>
 *   <epoch> UPDATE <city>|<hourly_rate>|<base_rate>
 *   <epoch> REMOVE <city>
 * @endcode
 * City names may contain spaces but not the '|' character.
 *
 * Flow: price_cli appends a line -> sends SIGUSR1 to the server ->
 *       server applies new lines to SQLite -> reloads shared memory.
 */
#ifndef PRICES_SHARED_H
#define PRICES_SHARED_H

#include <pthread.h>
#include <stdint.h>

#define PRICE_FILE     "prices_update.txt"  /**< Append-only command file */
#define PID_FILE       "server.pid"         /**< Server PID (for kill()) */
#define SHM_NAME       "/parking_prices"    /**< POSIX shared memory name */

#define MAX_CITIES     64
#define CITY_NAME_LEN  32

/** @brief One city's tariff */
typedef struct {
    char   city[CITY_NAME_LEN];
    double hourly_rate;   /**< NIS per hour */
    double base_rate;     /**< NIS per session */
} CityPrice_t;

/** @brief Price table in shared memory, protected by a process-shared mutex */
typedef struct {
    pthread_mutex_t lock;     /**< PTHREAD_PROCESS_SHARED */
    uint32_t        version;  /**< Incremented on every reload */
    int             count;
    CityPrice_t     cities[MAX_CITIES];
} PriceTable_t;

#endif /* PRICES_SHARED_H */
