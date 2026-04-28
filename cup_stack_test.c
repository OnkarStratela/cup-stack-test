// Cup Stack Test
// -----------------------------------------------------------------------------
// Discrete event test for the CAEN UHF RFID reader / antenna combo.
// The operator throws either ONE mug or a STACK OF TWO mugs (each with a
// printed RFID label) into the bin per event. The reader inventories tags
// inside a fixed scan window and the program records what was thrown vs what
// was actually detected.
//
// Default schedule: 100 single-mug throws + 100 two-mug-stack throws = 200
// events. The order is randomised (Fisher-Yates) so the operator cannot bias
// the antenna by predicting the next event type.
//
// Output:
//   cup_stack_test_<YYYYMMDD_HHMMSS>.csv  (one row per event, autosaved every
//   event so a Ctrl+C / power loss never destroys completed work).
//
// CSV columns (see README for the full schema):
//   event_id, timestamp, throw_type, expected_count, detected_count, result,
//   total_reads, scan_duration_ms, avg_rssi, max_rssi, min_rssi,
//   unique_tag_count, tag_ids, tag_read_counts, tag_avg_rssi, tag_min_rssi,
//   tag_max_rssi, first_seen_ms, last_seen_ms
//
// CLI usage:
//   ./cup_stack_test [single_throws] [stack_throws] [scan_window_ms]
//   defaults: 100 100 3000
// -----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <sys/time.h>
#include <termios.h>

#include "CAENRFIDLib_Light.h"
#include "host.h"

#define MAX_ID_LENGTH        64
#define MAX_TAGS_PER_EVENT   64
#define MAX_TOTAL_EVENTS     2000

// ANSI colours
#define GREEN   "\033[0;32m"
#define YELLOW  "\033[0;33m"
#define RED     "\033[0;31m"
#define CYAN    "\033[0;36m"
#define MAGENTA "\033[0;35m"
#define BOLD    "\033[1m"
#define RESET   "\033[0m"

// Defaults (overridable via CLI)
#define DEFAULT_SINGLE_THROWS   100
#define DEFAULT_STACK_THROWS    100
#define DEFAULT_SCAN_WINDOW_MS  3000
#define DEFAULT_POWER           316
#define INVENTORY_GAP_US        10000   // 10 ms between inventories
#define READER_PORT             "/dev/ttyACM0"
#define READER_BAUD             921600

typedef enum { THROW_SINGLE = 1, THROW_STACK = 2 } ThrowType;

typedef struct {
    char       epc[2 * MAX_ID_LENGTH + 1];
    int        read_count;
    long long  rssi_sum;        // running sum for averaging
    int        rssi_min;
    int        rssi_max;
    long long  first_seen_ms;   // ms since the event scan window started
    long long  last_seen_ms;
} TagAggregate;

typedef struct {
    int          event_id;
    char         timestamp[32];
    ThrowType    type;
    int          expected_count;
    int          detected_count;
    int          total_reads;
    int          scan_ms;
    int          avg_rssi;
    int          max_rssi;
    int          min_rssi;
    TagAggregate tags[MAX_TAGS_PER_EVENT];
    int          tag_count;
    char         result[16];
} EventResult;

// Global flags driven by SIGINT handler.
//   interrupt_event = 1 -> finish current event, then stop cleanly.
//   abort_run       = 1 -> stop immediately (set on second Ctrl+C).
volatile sig_atomic_t interrupt_event = 0;
volatile sig_atomic_t abort_run       = 0;

static FILE* g_csv = NULL;
static char  g_csv_path[256] = {0};

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
}

static void make_timestamp(char* buf, size_t bufsize) {
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    strftime(buf, bufsize, "%Y-%m-%d %H:%M:%S", tm);
}

static void make_filename_timestamp(char* buf, size_t bufsize) {
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    strftime(buf, bufsize, "%Y%m%d_%H%M%S", tm);
}

static bool onError(CAENRFIDErrorCodes ec) {
    if (ec != CAENRFID_StatusOK) {
        printf("ERROR (%d)\n", ec);
        return true;
    }
    return false;
}

static void printHex(uint8_t* vect, uint16_t length, char* result) {
    for (int i = 0; i < length; i++) {
        sprintf(result + (i * 2), "%02X", vect[i]);
    }
    result[length * 2] = '\0';
}

static void handle_sigint(int sig) {
    (void)sig;
    if (interrupt_event) {
        abort_run = 1;
    } else {
        interrupt_event = 1;
    }
}

// Drain any queued stdin bytes (e.g. ENTER hits the operator made while the
// scan window was running) and then block until the next Enter press.
static void wait_for_enter(void) {
    tcflush(STDIN_FILENO, TCIFLUSH);  // safe on non-tty stdin (returns -1, ignored)
    int c;
    while ((c = getchar()) != EOF && c != '\n') {}
    if (c == EOF) {
        abort_run = 1;
    }
}

// Fisher-Yates shuffle over an array of throw types.
static void shuffle(ThrowType* arr, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        ThrowType tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

static int read_int_arg(const char* s, int fallback) {
    if (s == NULL) return fallback;
    int v = atoi(s);
    return v > 0 ? v : fallback;
}

// -----------------------------------------------------------------------------
// Per-event tag aggregation
// -----------------------------------------------------------------------------

static int find_tag_index(EventResult* ev, const char* epc) {
    for (int i = 0; i < ev->tag_count; i++) {
        if (strcmp(ev->tags[i].epc, epc) == 0) return i;
    }
    return -1;
}

static void record_read(EventResult* ev, const char* epc, int rssi, long long elapsed_ms) {
    ev->total_reads++;

    int idx = find_tag_index(ev, epc);
    if (idx < 0) {
        if (ev->tag_count >= MAX_TAGS_PER_EVENT) return;  // out of slots, count read but skip detail
        idx = ev->tag_count++;
        TagAggregate* t = &ev->tags[idx];
        strncpy(t->epc, epc, sizeof(t->epc) - 1);
        t->epc[sizeof(t->epc) - 1] = '\0';
        t->read_count    = 0;
        t->rssi_sum      = 0;
        t->rssi_min      = INT_MAX;
        t->rssi_max      = INT_MIN;
        t->first_seen_ms = elapsed_ms;
    }
    TagAggregate* t = &ev->tags[idx];
    t->read_count++;
    t->rssi_sum    += rssi;
    if (rssi < t->rssi_min) t->rssi_min = rssi;
    if (rssi > t->rssi_max) t->rssi_max = rssi;
    t->last_seen_ms = elapsed_ms;
}

// Run inventories back-to-back for `scan_ms` milliseconds, recording every
// read into `ev`. Aborts early if abort_run is set.
static void run_event_scan(CAENRFIDReader* reader, const char* source,
                           EventResult* ev, int scan_ms) {
    long long start    = now_ms();
    long long deadline = start + scan_ms;

    long long rssi_sum = 0;
    int rssi_n         = 0;
    int rssi_min       = INT_MAX;
    int rssi_max       = INT_MIN;

    while (now_ms() < deadline && !abort_run) {
        CAENRFIDTagList *tags = NULL, *aux;
        uint16_t numTags = 0;

        CAENRFIDErrorCodes ec = CAENRFID_InventoryTag(
            reader, (char*)source, 0, 0, 0, NULL, 0, RSSI, &tags, &numTags);

        if (ec == CAENRFID_StatusOK && numTags > 0 && tags != NULL) {
            aux = tags;
            while (aux != NULL) {
                char epc[2 * MAX_ID_LENGTH + 1];
                printHex(aux->Tag.ID, aux->Tag.Length, epc);

                long long elapsed = now_ms() - start;
                int r = aux->Tag.RSSI;
                record_read(ev, epc, r, elapsed);

                rssi_sum += r;
                rssi_n++;
                if (r < rssi_min) rssi_min = r;
                if (r > rssi_max) rssi_max = r;

                CAENRFIDTagList* next = aux->Next;
                free(aux);
                aux = next;
            }
        } else if (tags != NULL) {
            // Free any leftover list nodes the API may have allocated.
            aux = tags;
            while (aux != NULL) {
                CAENRFIDTagList* next = aux->Next;
                free(aux);
                aux = next;
            }
        }

        usleep(INVENTORY_GAP_US);
    }

    ev->scan_ms        = (int)(now_ms() - start);
    ev->detected_count = ev->tag_count;
    if (rssi_n > 0) {
        ev->avg_rssi = (int)(rssi_sum / rssi_n);
        ev->min_rssi = rssi_min;
        ev->max_rssi = rssi_max;
    } else {
        ev->avg_rssi = 0;
        ev->min_rssi = 0;
        ev->max_rssi = 0;
    }
}

static const char* result_label(int expected, int detected) {
    if (detected == expected) return "correct";
    if (detected <  expected) return "under";
    return "over";
}

// -----------------------------------------------------------------------------
// CSV output
// -----------------------------------------------------------------------------

static void write_csv_header(FILE* csv) {
    fprintf(csv,
        "event_id,timestamp,throw_type,expected_count,detected_count,result,"
        "total_reads,scan_duration_ms,avg_rssi,max_rssi,min_rssi,"
        "unique_tag_count,tag_ids,tag_read_counts,tag_avg_rssi,"
        "tag_min_rssi,tag_max_rssi,first_seen_ms,last_seen_ms\n");
    fflush(csv);
}

// Append one ";"-joined token into `dst`; safely no-op if buffer is full.
static void append_token(char* dst, size_t dst_size, const char* token, bool first) {
    size_t cur = strlen(dst);
    size_t need = strlen(token) + (first ? 0 : 1) + 1;
    if (cur + need >= dst_size) return;
    if (!first) dst[cur++] = ';';
    strcpy(dst + cur, token);
}

static void write_csv_row(FILE* csv, const EventResult* ev) {
    char tag_ids[2048]    = {0};
    char tag_reads[1024]  = {0};
    char tag_avg[1024]    = {0};
    char tag_min[1024]    = {0};
    char tag_max[1024]    = {0};
    char first_seen[1024] = {0};
    char last_seen[1024]  = {0};

    for (int i = 0; i < ev->tag_count; i++) {
        const TagAggregate* t = &ev->tags[i];
        int avg = t->read_count > 0 ? (int)(t->rssi_sum / t->read_count) : 0;
        bool first = (i == 0);
        char buf[64];

        append_token(tag_ids, sizeof(tag_ids), t->epc, first);

        snprintf(buf, sizeof(buf), "%d", t->read_count);
        append_token(tag_reads, sizeof(tag_reads), buf, first);

        snprintf(buf, sizeof(buf), "%d", avg);
        append_token(tag_avg, sizeof(tag_avg), buf, first);

        snprintf(buf, sizeof(buf), "%d", t->rssi_min);
        append_token(tag_min, sizeof(tag_min), buf, first);

        snprintf(buf, sizeof(buf), "%d", t->rssi_max);
        append_token(tag_max, sizeof(tag_max), buf, first);

        snprintf(buf, sizeof(buf), "%lld", t->first_seen_ms);
        append_token(first_seen, sizeof(first_seen), buf, first);

        snprintf(buf, sizeof(buf), "%lld", t->last_seen_ms);
        append_token(last_seen, sizeof(last_seen), buf, first);
    }

    fprintf(csv,
        "%d,%s,%s,%d,%d,%s,%d,%d,%d,%d,%d,%d,"
        "\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"\n",
        ev->event_id,
        ev->timestamp,
        ev->type == THROW_SINGLE ? "single" : "stack",
        ev->expected_count,
        ev->detected_count,
        ev->result,
        ev->total_reads,
        ev->scan_ms,
        ev->avg_rssi,
        ev->max_rssi,
        ev->min_rssi,
        ev->tag_count,
        tag_ids,
        tag_reads,
        tag_avg,
        tag_min,
        tag_max,
        first_seen,
        last_seen);
    fflush(csv);
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(int argc, char** argv) {
    int single_throws  = read_int_arg(argc > 1 ? argv[1] : NULL, DEFAULT_SINGLE_THROWS);
    int stack_throws   = read_int_arg(argc > 2 ? argv[2] : NULL, DEFAULT_STACK_THROWS);
    int scan_window_ms = read_int_arg(argc > 3 ? argv[3] : NULL, DEFAULT_SCAN_WINDOW_MS);
    int total_events   = single_throws + stack_throws;

    if (total_events <= 0 || total_events > MAX_TOTAL_EVENTS) {
        printf("Error: invalid total event count (%d). Allowed range: 1..%d\n",
               total_events, MAX_TOTAL_EVENTS);
        return -1;
    }

    srand((unsigned)time(NULL));
    signal(SIGINT, handle_sigint);

    // Build randomised schedule.
    ThrowType* sequence = (ThrowType*)calloc((size_t)total_events, sizeof(ThrowType));
    if (sequence == NULL) {
        printf("Error: failed to allocate event schedule.\n");
        return -1;
    }
    for (int i = 0; i < single_throws; i++) sequence[i] = THROW_SINGLE;
    for (int i = 0; i < stack_throws;  i++) sequence[single_throws + i] = THROW_STACK;
    shuffle(sequence, total_events);

    // Reader setup.
    CAENRFIDErrorCodes ec;
    CAENRFIDReader reader = {
        .connect       = _connect,
        .disconnect    = _disconnect,
        .tx            = _tx,
        .rx            = _rx,
        .clear_rx_data = _clear_rx_data,
        .enable_irqs   = _enable_irqs,
        .disable_irqs  = _disable_irqs
    };
    RS232_params port_params = {
        .com         = READER_PORT,
        .baudrate    = READER_BAUD,
        .dataBits    = 8,
        .stopBits    = 1,
        .parity      = 0,
        .flowControl = 0,
    };
    char model[64]  = {0};
    char serial[64] = {0};
    char source[32] = "Source_0";
    int  power      = DEFAULT_POWER;

    printf("%s%s===== CUP STACK TEST =====%s\n", BOLD, CYAN, RESET);
    printf("Single mug throws : %d\n", single_throws);
    printf("Stacked mug throws: %d (stack of 2 mugs)\n", stack_throws);
    printf("Total events      : %d\n", total_events);
    printf("Scan window       : %d ms per event\n", scan_window_ms);
    printf("Reader power      : %d\n", power);
    printf("Reader port       : %s @ %d baud\n\n", port_params.com, port_params.baudrate);

    printf("[CUP-STACK] Connecting to reader...\n");
    ec = CAENRFID_Connect(&reader, CAENRFID_RS232, &port_params);
    if (onError(ec)) {
        printf("[CUP-STACK] Failed to connect!\n");
        printf("  - Check USB connection (%s)\n", port_params.com);
        printf("  - Try: sudo chmod 666 %s\n", port_params.com);
        free(sequence);
        return -1;
    }
    if (CAENRFID_GetReaderInfo(&reader, model, serial) == CAENRFID_StatusOK) {
        printf("[CUP-STACK] Reader: %s, Serial: %s\n", model, serial);
    }
    CAENRFID_SetPower(&reader, power);
    printf("[CUP-STACK] Power set to %d mW\n", power);

    // Open CSV (timestamped, autosaved per event).
    char ts[32];
    make_filename_timestamp(ts, sizeof(ts));
    snprintf(g_csv_path, sizeof(g_csv_path), "cup_stack_test_%s.csv", ts);
    g_csv = fopen(g_csv_path, "w");
    if (g_csv == NULL) {
        printf("ERROR: cannot open CSV file %s\n", g_csv_path);
        CAENRFID_Disconnect(&reader);
        free(sequence);
        return -1;
    }
    write_csv_header(g_csv);
    printf("[CUP-STACK] Logging to %s%s%s\n\n", GREEN, g_csv_path, RESET);

    printf("%sBefore each event, make sure the bin is empty.%s\n", YELLOW, RESET);
    printf("%sPress ENTER to start the test (Ctrl+C once = finish current event then stop;%s\n", YELLOW, RESET);
    printf("%s Ctrl+C twice = abort immediately).%s", YELLOW, RESET);
    fflush(stdout);
    wait_for_enter();
    if (abort_run) {
        printf("\n[CUP-STACK] Aborted before any events ran.\n");
        fclose(g_csv);
        CAENRFID_Disconnect(&reader);
        free(sequence);
        return 0;
    }

    // Live counters.
    int correct = 0, under = 0, over = 0;
    int correct_single = 0, correct_stack = 0;
    int events_done    = 0;

    for (int i = 0; i < total_events && !abort_run; i++) {
        EventResult ev;
        memset(&ev, 0, sizeof(ev));
        ev.event_id       = i + 1;
        ev.type           = sequence[i];
        ev.expected_count = (ev.type == THROW_SINGLE) ? 1 : 2;
        make_timestamp(ev.timestamp, sizeof(ev.timestamp));

        const char* type_str = ev.type == THROW_SINGLE ? "SINGLE MUG" : "STACK OF 2 MUGS";
        const char* type_col = ev.type == THROW_SINGLE ? GREEN : MAGENTA;

        printf("\n%s───────────── Event %d / %d ─────────────%s\n",
               CYAN, ev.event_id, total_events, RESET);
        printf("Throw type: %s%s%s%s   (expected unique tags: %d)\n",
               BOLD, type_col, type_str, RESET, ev.expected_count);
        printf("%sClear bin, press ENTER, throw immediately...%s",
               YELLOW, RESET);
        fflush(stdout);

        wait_for_enter();
        if (abort_run) break;

        printf("%s[SCANNING %d ms]%s ", CYAN, scan_window_ms, RESET);
        fflush(stdout);
        run_event_scan(&reader, source, &ev, scan_window_ms);

        strncpy(ev.result, result_label(ev.expected_count, ev.detected_count),
                sizeof(ev.result) - 1);

        const char* result_col;
        if (strcmp(ev.result, "correct") == 0)      result_col = GREEN;
        else if (strcmp(ev.result, "under") == 0)   result_col = RED;
        else                                        result_col = YELLOW;

        printf("→ detected %s%d%s tag(s) [%s%s%s]   reads=%d  avgRSSI=%d  scan=%dms\n",
               BOLD, ev.detected_count, RESET, result_col, ev.result, RESET,
               ev.total_reads, ev.avg_rssi, ev.scan_ms);

        for (int t = 0; t < ev.tag_count; t++) {
            int avg = ev.tags[t].read_count > 0
                        ? (int)(ev.tags[t].rssi_sum / ev.tags[t].read_count) : 0;
            printf("    • %s   reads=%d  avgRSSI=%d  min=%d  max=%d  firstMs=%lld  lastMs=%lld\n",
                   ev.tags[t].epc, ev.tags[t].read_count, avg,
                   ev.tags[t].rssi_min, ev.tags[t].rssi_max,
                   ev.tags[t].first_seen_ms, ev.tags[t].last_seen_ms);
        }

        write_csv_row(g_csv, &ev);
        events_done++;

        if (strcmp(ev.result, "correct") == 0) {
            correct++;
            if (ev.type == THROW_SINGLE) correct_single++;
            else                         correct_stack++;
        } else if (strcmp(ev.result, "under") == 0) {
            under++;
        } else {
            over++;
        }

        // Running tallies after each event.
        printf("%s   running: correct=%d  under=%d  over=%d   "
               "correct_single=%d/%d  correct_stack=%d/%d%s\n",
               CYAN, correct, under, over,
               correct_single, single_throws,
               correct_stack,  stack_throws,
               RESET);

        if (interrupt_event && !abort_run) {
            printf("\n%s[Ctrl+C] Stopping gracefully after event %d.%s\n",
                   YELLOW, ev.event_id, RESET);
            break;
        }
    }

    fclose(g_csv);
    CAENRFID_Disconnect(&reader);
    free(sequence);

    printf("\n%s%s===== TEST SUMMARY =====%s\n", BOLD, CYAN, RESET);
    printf("Events completed       : %d / %d\n", events_done, total_events);
    printf("Correct detections     : %d\n", correct);
    printf("Under-detected events  : %d\n", under);
    printf("Over-detected events   : %d\n", over);
    printf("Correct (single mug)   : %d / %d\n", correct_single, single_throws);
    printf("Correct (stack of 2)   : %d / %d\n", correct_stack,  stack_throws);
    printf("Results saved to       : %s%s%s\n", GREEN, g_csv_path, RESET);

    return 0;
}
