/**
 * @file  price_cli.c
 * @brief Terminal utility for managing city prices.
 *
 * Writes a command line to PRICE_FILE (append-only audit trail) and sends
 * SIGUSR1 to the server, which applies it to the DB and shared memory.
 * Current prices are read directly from shared memory.
 *
 * Usage:
 * @code
 *   ./price_cli                                  interactive menu
 *   ./price_cli show
 *   ./price_cli add    "Haifa" 12 4
 *   ./price_cli update "Tel Aviv" 18 6
 *   ./price_cli remove "Haifa"
 * @endcode
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/file.h>

#include "prices_shared.h"

static PriceTable_t *g_prices;

/* ============================================================
 * Helpers
 * ============================================================ */
/** @brief Attach to the server's shared-memory price table. */
static int shm_attach(void)
{
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd < 0) return -1;
    g_prices = mmap(NULL, sizeof(PriceTable_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return (g_prices == MAP_FAILED) ? -1 : 0;
}

/** @brief Read the server PID from PID_FILE and check it is alive. */
static pid_t get_server_pid(void)
{
    FILE *f = fopen(PID_FILE, "r");
    int pid = -1;
    if (f) {
        if (fscanf(f, "%d", &pid) != 1) pid = -1;
        fclose(f);
    }
    if (pid <= 0 || kill(pid, 0) != 0) return -1;
    return pid;
}

/** @brief Copy the shared table locally (consistent snapshot). */
static int snapshot(CityPrice_t *out, uint32_t *version)
{
    pthread_mutex_lock(&g_prices->lock);
    int n = g_prices->count;
    memcpy(out, g_prices->cities, sizeof(CityPrice_t) * n);
    if (version) *version = g_prices->version;
    pthread_mutex_unlock(&g_prices->lock);
    return n;
}

/** @brief Print the price table with row numbers. */
static int show_prices(CityPrice_t *list)
{
    int n = snapshot(list, NULL);
    printf("\n  #  %-20s %10s %10s\n", "City", "Hourly", "Base");
    printf("  -------------------------------------------\n");
    for (int i = 0; i < n; i++) {
        printf(" %2d  %-20s %10.2f %10.2f\n", i + 1, list[i].city,
               list[i].hourly_rate, list[i].base_rate);
    }
    if (n == 0) printf("  (no cities)\n");
    return n;
}

/** @brief Read a line from stdin, strip newline and surrounding spaces. */
static int read_line(const char *prompt, char *buf, size_t size)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int)size, stdin)) return -1;
    buf[strcspn(buf, "\r\n")] = '\0';

    char *s = buf;
    while (*s == ' ') s++;
    memmove(buf, s, strlen(s) + 1);
    size_t len = strlen(buf);
    while (len > 0 && buf[len - 1] == ' ') buf[--len] = '\0';
    return (int)len;
}

/** @brief Validate a city name for the file format. */
static int valid_city(const char *city)
{
    if (city[0] == '\0' || strlen(city) >= CITY_NAME_LEN) {
        printf("City name must be 1-%d characters.\n", CITY_NAME_LEN - 1);
        return 0;
    }
    if (strchr(city, '|')) {
        printf("City name cannot contain '|'.\n");
        return 0;
    }
    return 1;
}

/**
 * @brief Read a non-negative rate. Empty input keeps current (if allowed).
 * @return 0 ok, -1 invalid
 */
static int read_rate(const char *prompt, double current, int allow_keep, double *out)
{
    char buf[64], *end;
    if (read_line(prompt, buf, sizeof(buf)) < 0) return -1;
    if (buf[0] == '\0' && allow_keep) {
        *out = current;
        return 0;
    }
    double v = strtod(buf, &end);
    if (end == buf || *end != '\0' || v < 0) {
        printf("Invalid rate '%s'.\n", buf);
        return -1;
    }
    *out = v;
    return 0;
}

/** @brief Find a city in a snapshot. @return index or -1 */
static int find_city(const CityPrice_t *list, int n, const char *city)
{
    for (int i = 0; i < n; i++)
        if (strcmp(list[i].city, city) == 0) return i;
    return -1;
}

/**
 * @brief Append a command to PRICE_FILE (exclusive lock) and signal the server.
 *        Waits up to 2 seconds for the server to reload shared memory.
 */
static int send_command(const char *fmt, ...)
{
    pid_t pid = get_server_pid();
    if (pid < 0) {
        printf("Server is not running (no valid %s).\n", PID_FILE);
        return -1;
    }

    int fd = open(PRICE_FILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) {
        printf("Cannot open %s: %s\n", PRICE_FILE, strerror(errno));
        return -1;
    }

    char line[256];
    int len = snprintf(line, sizeof(line), "%ld ", (long)time(NULL));
    va_list args;
    va_start(args, fmt);
    len += vsnprintf(line + len, sizeof(line) - len, fmt, args);
    va_end(args);
    len += snprintf(line + len, sizeof(line) - len, "\n");

    flock(fd, LOCK_EX);
    ssize_t w = write(fd, line, len);
    flock(fd, LOCK_UN);
    close(fd);
    if (w != len) {
        printf("Write to %s failed.\n", PRICE_FILE);
        return -1;
    }

    uint32_t before;
    CityPrice_t tmp[MAX_CITIES];
    snapshot(tmp, &before);

    if (kill(pid, SIGUSR1) != 0) {
        printf("Cannot signal server (PID %d): %s\n", pid, strerror(errno));
        return -1;
    }

    for (int i = 0; i < 20; i++) {          /* wait for reload */
        usleep(100000);
        uint32_t now;
        snapshot(tmp, &now);
        if (now != before) {
            printf("Done - server reloaded prices.\n");
            return 0;
        }
    }
    printf("Command written, but server did not confirm reload yet.\n");
    return 0;
}

/* ============================================================
 * Menu actions
 * ============================================================ */
static void menu_add(void)
{
    char city[64];
    double hourly, base;
    CityPrice_t list[MAX_CITIES];
    int n = snapshot(list, NULL);

    if (read_line("New city name: ", city, sizeof(city)) < 0 || !valid_city(city)) return;
    if (find_city(list, n, city) >= 0) {
        printf("'%s' already exists - use Update.\n", city);
        return;
    }
    if (read_rate("Hourly rate (NIS): ", 0, 0, &hourly) < 0) return;
    if (read_rate("Base rate   (NIS): ", 0, 0, &base) < 0) return;

    send_command("ADD %s|%.2f|%.2f", city, hourly, base);
}

static void menu_update(void)
{
    CityPrice_t list[MAX_CITIES];
    char buf[16];
    int n = show_prices(list);
    if (n == 0) return;

    if (read_line("\nSelect city # to update: ", buf, sizeof(buf)) < 0) return;
    int idx = atoi(buf) - 1;
    if (idx < 0 || idx >= n) {
        printf("Invalid selection.\n");
        return;
    }

    CityPrice_t *c = &list[idx];
    printf("Updating '%s' (Enter = keep current value)\n", c->city);

    char prompt[64];
    double hourly, base;
    snprintf(prompt, sizeof(prompt), "Hourly rate [%.2f]: ", c->hourly_rate);
    if (read_rate(prompt, c->hourly_rate, 1, &hourly) < 0) return;
    snprintf(prompt, sizeof(prompt), "Base rate   [%.2f]: ", c->base_rate);
    if (read_rate(prompt, c->base_rate, 1, &base) < 0) return;

    if (hourly == c->hourly_rate && base == c->base_rate) {
        printf("No change.\n");
        return;
    }
    send_command("UPDATE %s|%.2f|%.2f", c->city, hourly, base);
}

static void menu_remove(void)
{
    CityPrice_t list[MAX_CITIES];
    char buf[16];
    int n = show_prices(list);
    if (n == 0) return;

    if (read_line("\nSelect city # to remove: ", buf, sizeof(buf)) < 0) return;
    int idx = atoi(buf) - 1;
    if (idx < 0 || idx >= n) {
        printf("Invalid selection.\n");
        return;
    }
    char confirm[8];
    printf("Remove '%s'? (y/n): ", list[idx].city);
    if (read_line("", confirm, sizeof(confirm)) < 0) return;
    if (confirm[0] == 'y' || confirm[0] == 'Y') {
        send_command("REMOVE %s", list[idx].city);
    }
}

/* ============================================================
 * Main
 * ============================================================ */
static int usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s                         interactive menu\n"
            "  %s show\n"
            "  %s add    <city> <hourly> <base>\n"
            "  %s update <city> <hourly> <base>\n"
            "  %s remove <city>\n", prog, prog, prog, prog, prog);
    return EXIT_FAILURE;
}

int main(int argc, char *argv[])
{
    if (shm_attach() < 0) {
        fprintf(stderr, "Cannot attach to shared memory %s - is the server running?\n", SHM_NAME);
        return EXIT_FAILURE;
    }

    /* ---------- command-line mode ---------- */
    if (argc > 1) {
        CityPrice_t list[MAX_CITIES];
        const char *cmd = argv[1];

        if (strcmp(cmd, "show") == 0) {
            show_prices(list);
            return EXIT_SUCCESS;
        }
        if ((strcmp(cmd, "add") == 0 || strcmp(cmd, "update") == 0) && argc == 5) {
            char *e1, *e2;
            double h = strtod(argv[3], &e1), b = strtod(argv[4], &e2);
            if (!valid_city(argv[2]) || *e1 || *e2 || h < 0 || b < 0) return usage(argv[0]);
            return send_command("%s %s|%.2f|%.2f", cmd[0] == 'a' ? "ADD" : "UPDATE",
                                argv[2], h, b) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        if (strcmp(cmd, "remove") == 0 && argc == 3) {
            if (!valid_city(argv[2])) return usage(argv[0]);
            return send_command("REMOVE %s", argv[2]) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        return usage(argv[0]);
    }

    /* ---------- interactive mode ---------- */
    char buf[16];
    for (;;) {
        printf("\n==== City Price Manager ====\n"
               "1. Add city\n"
               "2. Update city prices\n"
               "3. Remove city\n"
               "4. Show prices\n"
               "0. Exit\n");
        if (read_line("Select option: ", buf, sizeof(buf)) < 0) break;

        switch (atoi(buf)) {
        case 1: menu_add();    break;
        case 2: menu_update(); break;
        case 3: menu_remove(); break;
        case 4: { CityPrice_t list[MAX_CITIES]; show_prices(list); } break;
        case 0: return EXIT_SUCCESS;
        default: printf("Invalid option.\n");
        }
    }
    return EXIT_SUCCESS;
}
