/*
 * Copyright 2017, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <autoconf.h>
#include <camkes.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <sel4/sel4.h>
#include <utils/attribute.h>
#include <utils/ansi.h>
#include <camkes.h>
#include <camkes/io.h>
#include <camkes/irq.h>
#include <camkes/virtqueue.h>
#include <platsupport/chardev.h>
#include <platsupport/irq.h>
#include "serial.h"
#include "plat.h"

#define ESCAPE_CHAR '@'
#define MAX_CLIENTS 12
#define BUFSIZE 4088

typedef struct getchar_client {
    virtqueue_device_t *recv_queue;
    virtqueue_driver_t *send_queue;
} client_t;

static client_t *clients = NULL;
static int num_clients = 0;
char gather_buf[BUFSIZE];

static ps_io_ops_t io_ops;

static int last_out = -1;
static uint8_t output_buffers[MAX_CLIENTS][BUFSIZE];
static int output_buffers_used[MAX_CLIENTS] = { 0 };
static uint16_t output_buffer_bitmask = 0;
static int done_output = 0;
static int has_data = 0;

/* We predefine output colours for clients */
const char *all_output_colours[MAX_CLIENTS] = {
    /* Processed streams */
    ANSI_COLOR(RED),
    ANSI_COLOR(GREEN),
    ANSI_COLOR(BLUE),
    ANSI_COLOR(MAGENTA),
    ANSI_COLOR(YELLOW),
    ANSI_COLOR(CYAN),
    ANSI_COLOR(RED, BOLD)
    ANSI_COLOR(GREEN, BOLD),
    ANSI_COLOR(BLUE, BOLD),
    ANSI_COLOR(MAGENTA, BOLD),
    ANSI_COLOR(YELLOW, BOLD),
    ANSI_COLOR(CYAN, BOLD)
};

static void flush_buffer(int b)
{
    const char *col = all_output_colours[b];
    int i;
    if (output_buffers_used[b] == 0) {
        return;
    }
    if (b != last_out) {
        printf("%s%s", COLOR_RESET, col);
        last_out = b;
    }
    for (i = 0; i < output_buffers_used[b]; i++) {
        printf("%c", output_buffers[b][i]);
    }
    done_output = 1;
    output_buffers_used[b] = 0;
    output_buffer_bitmask &= ~BIT(b);
    fflush(stdout);
}

static int debug = 0;

/* Try to flush up to the end of the line. */
static bool flush_buffer_line(int b)
{
    if (output_buffers_used[b] == 0) {
        return 0;
    }
    uint8_t *nlptr = memchr(output_buffers[b], '\r', output_buffers_used[b]);
    if (nlptr == NULL) {
        nlptr = memchr(output_buffers[b], '\n', output_buffers_used[b]);
    }
    if (nlptr == NULL) {
        if (debug == 2) {
            ZF_LOGD("newline not found!\r\n");
        }
        return 0;
    }
    size_t length = (nlptr - &output_buffers[b][0]) + 1;
    if (length < output_buffers_used[b] && (output_buffers[b][length] == '\n' || output_buffers[b][length] == '\r')) {
        length++;               /* Include \n after \r if present */
    }
    if (length == 0) {
        if (debug == 2) {
            ZF_LOGD("0-length!\r\n");
        }
        return 0;
    }
    if (b != last_out) {
        printf("%s%s", COLOR_RESET, all_output_colours[b]);
        last_out = b;
    }
    int i;
    for (i = 0; i < length; i++) {
        printf("%c", output_buffers[b][i]);
    }
    for (i = length; i < output_buffers_used[b]; i++) {
        output_buffers[b][i - length] = output_buffers[b][i];
    }
    output_buffers_used[b] -= length;
    if (output_buffers_used[b] == 0) {
        output_buffer_bitmask &= ~BIT(b);
    }
    return 1;
}

static int is_newline(const uint8_t *c)
{
    return (c[0] == '\r' && c[1] == '\n') || (c[0] == '\n' && c[1] == '\r');
}

static int active_client = 0;
static int active_multiclients = 0;

/* Try coalescing client output. This is intended for use with
 * multi-input mode to all clients. */

/* (XXX) CAVEATS:
 *
 * - Has not been tested with more than 2 clients
 *
 * - Has not been tested with multi-input mode set not matching client
 *   set
 *
 * - Does not handle ANSI codes, UTF-8 or other multibytes specially;
     may break them when coalescing starts/stops.
 *
 * - Still "fails" due to some timing/buffering issues, but these
 *   failures are sufficiently rare that this is still useful. */
static int try_coalesce_output()
{
    size_t length = 0;
    size_t used[MAX_CLIENTS] = { 0 };
    size_t n_used = 0;
    for (size_t i = 0; i < MAX_CLIENTS; i++) {
        if (output_buffers_used[i] > 0) {
            used[n_used++] = i;
            if (length == 0 || length > output_buffers_used[i]) {
                length = output_buffers_used[i];
            }
            if (n_used > 1) {
                if (memcmp(output_buffers[used[0]], output_buffers[i], length) != 0) {
                    if (debug == 1) {
                        ZF_LOGD("\r\nDifferent contents '");
                        for (int j = 0; j < length; ++j) {
                            printf("%0hhx", output_buffers[used[0]][j]);
                        }
                        printf("' vs '");
                        for (int j = 0; j < length; ++j) {
                            printf("%0hhx", output_buffers[i][j]);
                        }
                        printf("'\r\n");
                    }
                    return -1; /* different contents, don't special-case */
                }
            }
        }
    }
    if (n_used > 1 && length > 0) {
        if (last_out != -1) {
            printf("%s", COLOR_RESET);
        }
        for (int i = 0; i < length; i++) {
            printf("%c", output_buffers[used[0]][i]);
        }
        last_out = -1;
        fflush(stdout);
        for (int i = 0; i < n_used; i++) {
            output_buffers_used[used[i]] -= length;
            for (int j = 0; j < output_buffers_used[used[i]]; ++j) {
                output_buffers[used[i]][j] = output_buffers[used[i]][j + length];
            }
            if (output_buffers_used[used[i]] == 0) {
                output_buffer_bitmask &= ~BIT(used[i]);
            }
        }
        if (output_buffer_bitmask != 0) {
            has_data = 1;
        }
        return 0;               /* coalesced */
    }
    return 1;                   /* buffering */
}

static void internal_putchar(int b, int c)
{
    int UNUSED error;
    error = serial_lock();
    /* Add to buffer */
    int index = output_buffers_used[b];
    uint8_t *buffer = output_buffers[b];
    buffer[index] = (uint8_t)c;
    output_buffers_used[b]++;
    int coalesce_status = -1;

    if (active_client == -1) {
        /* Test for special case: multiple clients outputting the EXACT SAME THING. */
        coalesce_status = try_coalesce_output();
    }
    if (output_buffers_used[b] == BUFSIZE) {
        /* Since we're violating contract anyway (flushing in the
         * middle of someone else's line), flush all buffers, so the
         * fastpath can be used again. */
        char is_done = 0;
        int i;
        int prev_client = last_out;
        if (prev_client != -1) {
            flush_buffer_line(prev_client);
        }
        while (!is_done) {
            is_done = 1;
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (flush_buffer_line(i)) {
                    is_done = 0;
                }
            }
        }
        /* Flush the rest, if necessary. */
        if (output_buffers_used[b] == BUFSIZE) {
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (i != b) {
                    flush_buffer(i);
                }
            }
        }
        /* then set the colors back. If this clients's buffer overflowed,
         * it's probably going to overflow again, so let's avoid
         * that. */
        if (last_out != b) {
            printf("%s%s", COLOR_RESET, all_output_colours[b]);
            last_out = b;
        }
    } else if ((index >= 1 && is_newline(buffer + index - 1) && coalesce_status == -1)
               || (last_out == b && output_buffer_bitmask == 0 && coalesce_status == -1)) {
        /* Allow fast output (newline or same-as-last-client) if
         * multi-input is not enabled OR last coalescing attempt
         * failed due to a mismatch. This is important as client output
         * may be delayed; coalescing failure due to empty buffer
         * should lead to further buffering rather than early flush,
         * in case we can coalesce later. */
        flush_buffer(b);
    } else {
        output_buffer_bitmask |= BIT(b);
    }
    has_data = 1;
    error = serial_unlock();
}

static void serial_notify_free_send(virtqueue_driver_t *queue)
{
    void *buf = NULL;
    uint32_t buf_size = 0, wr_len = 0;
    vq_flags_t flag;
    virtqueue_ring_object_t handle;
    while (virtqueue_get_used_buf(queue, &handle, &wr_len)) {
        while (camkes_virtqueue_driver_gather_buffer(queue, &handle, &buf, &buf_size, &flag) >= 0) {
            /* Clean up and free the buffer we allocated */
            camkes_virtqueue_buffer_free(queue, buf);
        }
    }
}

static int serial_notify_recv(int client, virtqueue_device_t *queue)
{
    int err;
    void *buf = NULL;
    size_t buf_size = 0;
    vq_flags_t flag;
    virtqueue_ring_object_t handle;

    while (virtqueue_get_available_buf(queue, &handle)) {
        size_t len = virtqueue_scattered_available_size(queue, &handle);
        if (camkes_virtqueue_device_gather_copy_buffer(queue, &handle, gather_buf, len) < 0) {
            ZF_LOGW("Dropping data for client %d: Can't gather vq buffer.", client);
            continue;
        }

        /* Print to serial */
        for (size_t i = 0; i < len; i++) {
            char ch = gather_buf[i];
            if (ch == '\n') {
                internal_putchar(client, '\r');
            }
            internal_putchar(client, ch);
        }
        queue->notify();
    }
}

/* This is called whenever one of our virtqueues get notified */
void serial_wait_callback(void)
{
    /* Walk through all recv virtqueues and poll for data */
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].recv_queue && VQ_DEV_POLL(clients[i].recv_queue)) {
            serial_notify_recv(i, clients[i].recv_queue);
        }
        
        if (clients[i].send_queue && VQ_DRV_POLL(clients[i].send_queue)) {
            serial_notify_free_send(clients[i].send_queue);
        }
    }
}

static void internal_raw_putchar(int id, int c)
{
    /* This is so overkill we need a better solution than this */
    int err = camkes_virtqueue_driver_scatter_send_buffer(clients[id].send_queue, (void *) &c, 1);
    ZF_LOGE_IF(err < 0, "Unknown error while enqueuing char for client %d", id);
    clients[id].send_queue->notify();
}

static void give_client_char(uint8_t c)
{
    if (active_client >= 0) {
        internal_raw_putchar(active_client, c);
    } else if (active_client == -1) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if ((active_multiclients & BIT(i)) == BIT(i)) {
                internal_raw_putchar(i, c);
            }
        }
    }
}

static int statemachine = 1;
static void handle_char(uint8_t c)
{
    /* If there are no clients, then we return immediately */
    if (!num_clients) {
        return;
    }

    /* some manually written state machine magic to detect switching of input direction */
    switch (statemachine) {
    case 0:
        if (c == '\r' || c == '\n') {
            statemachine = 1;
        }
        give_client_char(c);
        break;
    case 1:
        if (c == ESCAPE_CHAR) {
            statemachine = 2;
        } else {
            statemachine = 0;
            give_client_char(c);
        }
        break;
    case 2:
        switch (c) {
        case ESCAPE_CHAR:
            statemachine = 0;
            give_client_char(c);
            break;
        case 'm':
            statemachine = 3;
            active_multiclients = 0;
            active_client = -1;
            last_out = -1;
            printf(COLOR_RESET "\r\nMulti-client input to clients: ");
            fflush(stdout);
            break;
        case 'd':
            debug = (debug + 1) % 3;
            printf(COLOR_RESET "\r\nDebug: %i\r\n", debug);
            last_out = -1;
            statemachine = 1;
            break;
        case '?':
            last_out = -1;
            printf(COLOR_RESET "\r\n --- SerialServer help ---"
                   "\r\n Escape char: %c"
                   "\r\n 0 - %-2d switches input to that client"
                   "\r\n ?      shows this help"
                   "\r\n m      simultaneous multi-client input"
                   "\r\n d      switch between debugging modes"
                   "\r\n          0: no debugging"
                   "\r\n          1: debug multi-input mode output coalescing"
                   "\r\n          2: debug flush_buffer_line"
                   "\r\n", ESCAPE_CHAR, num_clients - 1);
            statemachine = 1;
            break;
        default:
            if (c >= '0' && c < '0' + num_clients) {
                last_out = -1;
                int client = c - '0';
                printf(COLOR_RESET "\r\nSwitching input to %d\r\n", client);
                active_client = client;
                statemachine = 1;
            } else {
                statemachine = 0;
                give_client_char(ESCAPE_CHAR);
                give_client_char(c);
            }
        }
        break;
    case 3:
        if (c >= '0' && c < '0' + num_clients) {
            printf(COLOR_RESET "%s%d", (active_multiclients != 0 ? "," : ""), (c - '0'));
            active_multiclients |= BIT(c - '0');
            last_out = -1;
            fflush(stdout);
        } else if (c == 'm' || c == 'M' || c == '\r' || c == '\n') {
            last_out = -1;
            printf(COLOR_RESET "\r\nSwitching input to multi-client. Output will be best-effort coalesced (colored white).\r\n");
            statemachine = 1;
        }
        break;
    }
}

static void timer_callback(void *data)
{
    int UNUSED error;
    error = serial_lock();
    if (done_output) {
        done_output = 0;
    } else if (has_data) {
        /* flush everything if no writes since last callback */
        int i;
        char is_done = 0;
        char succeeded = 0;
        while (!is_done) {
            is_done = 1;
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (flush_buffer_line(i)) {
                    succeeded = 1;
                    is_done = 0;
                }
            }
        }
        if (!succeeded) {
            for (i = 0; i < MAX_CLIENTS; i++) {
                flush_buffer(i);
            }
        }
    }
    error = serial_unlock();
}

seL4_CPtr timeout_notification(void);
int run(void)
{
    seL4_CPtr notification = timeout_notification();
    while (1) {
        seL4_Wait(notification, NULL);
        timer_callback(NULL);
    }
    return 0;
}

void serial_server_irq_handle(void *data, ps_irq_acknowledge_fn_t acknowledge_fn, void *ack_data)
{
    int error = serial_lock();
    ZF_LOGF_IF(error, "Failed to lock serial server");

    plat_serial_interrupt(handle_char);

    error = acknowledge_fn(ack_data);
    ZF_LOGF_IF(error, "Failed to acknowledge IRQ");

    error = serial_unlock();
    ZF_LOGF_IF(error, "Failed to unlock serial server");
}

void serial_putchar(int c)
{
    plat_serial_putchar(c);
}

void pre_init(void)
{
    int error;
    error = serial_lock();

    error = camkes_io_ops(&io_ops);
    ZF_LOGF_IF(error, "Failed to initialise IO ops");

    /* Initialize the serial port */
    plat_pre_init(&io_ops);
    set_putchar(serial_putchar);

    plat_post_init(&(io_ops.irq_ops));

    /* Start regular heartbeat of 500ms */
    timeout_periodic(0, 500000000);
    error = serial_unlock();
    ZF_LOGF_IF(error, "Failed to unlock serial");
}

int virtqueue_init(void)
{
    int err;
    /* Query what clients exist */
    num_clients = ARRAY_SIZE(serial_layout);
    clients = calloc(num_clients, sizeof(client_t));

    for (int i = 0; i < num_clients; i++) {
        virtqueue_driver_t *vq_send;
        virtqueue_device_t *vq_recv;

        vq_recv = malloc(sizeof(*vq_recv));
        if (!vq_recv) {
            ZF_LOGE("Unable to alloc recv camkes-virtqueue for client: %d", i);
            err = -1;
            goto cleanup;
        }

        vq_send = malloc(sizeof(*vq_send));
        if (!vq_send) {
            ZF_LOGE("Unable to alloc send camkes-virtqueue for client: %d", i);
            err = -1;
            goto cleanup;
        }

        /* Initialise read virtqueue */
        err = camkes_virtqueue_device_init(vq_recv, serial_layout[i].recv_id);
        if (err) {
            ZF_LOGE("Unable to initialise serial server recv virtqueue for client %d", i);
            goto cleanup;
        }

        /* Initialise write virtqueue */
        err = camkes_virtqueue_driver_init(vq_send, serial_layout[i].send_id);
        if (err) {
            ZF_LOGE("Unable to initialise serial server send virtqueue for client %d", i);
            goto cleanup;
        }

        clients[i].recv_queue = vq_recv;
        clients[i].send_queue = vq_send;       
    }

    return 0;

cleanup:
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].recv_queue) {
            free(clients[i].recv_queue);
        }
        if (clients[i].send_queue) {
            free(clients[i].send_queue);
        }
    }

    return err;
}

void post_init(void)
{
    if (num_registered_virtqueue_channels > 0) {
        int res = virtqueue_init();
        ZF_LOGE_IF(res, "Serial server does not support read and write virtqueues");
    }
}