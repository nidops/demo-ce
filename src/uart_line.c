/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file uart_line.c
 * @brief Async UART line reader backed by buffer-pool DMA buffers.
 */

#include <stdbool.h>
#include <string.h>
#include <zephyr/sys/util.h>
#include "buffer.h"
#include "uart_line.h"

/*----------------------------------------------------------------------------------------------
 * Local config
 *---------------------------------------------------------------------------------------------*/

/**
 * @brief Maximum number of uart_line instances that can be allocated.
 */
#ifndef UART_LINE_MAX_INSTANCES
#define UART_LINE_MAX_INSTANCES       (2u)
#endif

/**
 * @brief Size of each RX DMA buffer handed to the UART driver.
 *
 * This is independent from UART_LINE_MAX_LEN (logical line length).
 */
#ifndef UART_LINE_DMA_CHUNK
#define UART_LINE_DMA_CHUNK           (128u)
#endif

/**
 * @brief Number of DMA buffers per instance.
 *
 * Two buffers are the minimum useful async UART setup: one active buffer and
 * one next buffer supplied when the driver raises UART_RX_BUF_REQUEST.
 */
#ifndef UART_LINE_BUFFER_COUNT
#define UART_LINE_BUFFER_COUNT        (2u)
#endif

/**
 * @brief Finite idle timeout so RX_RDY fires without filling the whole DMA buffer.
 */
#ifndef UART_LINE_IDLE_TIMEOUT_US
#define UART_LINE_IDLE_TIMEOUT_US     (50000u) /* 50 ms */
#endif

#if (UART_LINE_BUFFER_COUNT < 2u)
#error "UART_LINE_BUFFER_COUNT must be at least 2 for async UART RX"
#endif

/*----------------------------------------------------------------------------------------------
 * Private async RX buffer pool
 *---------------------------------------------------------------------------------------------*/

/**
 * @brief Private async/DMA state for one uart_line instance.
 */
typedef struct
{
    uint8_t rx_memory[UART_LINE_BUFFER_COUNT * UART_LINE_DMA_CHUNK]; /**< DMA-capable storage handed to UART. */
    buffer_st rx_buffers[UART_LINE_BUFFER_COUNT];                    /**< Descriptors used by buffer-pool. */
    buffer_array_ctx_st rx_pool;                                     /**< buffer-pool context for UART RX buffers. */
} uart_line_priv_st;

#define UART_LINE_SLAB_ALIGN      (sizeof(void *))
#define UART_LINE_SLAB_BLOCK_SIZE ROUND_UP(sizeof(uart_line_priv_st), UART_LINE_SLAB_ALIGN)

K_MEM_SLAB_DEFINE(uart_line_priv_slab,
                  UART_LINE_SLAB_BLOCK_SIZE,
                  UART_LINE_MAX_INSTANCES,
                  UART_LINE_SLAB_ALIGN);

/*----------------------------------------------------------------------------------------------
 * Default instance
 *---------------------------------------------------------------------------------------------*/

/**
 * @brief Default UART line instance used by uart_line_transmit().
 */
static uart_line_st *default_uart_line = NULL;

/*----------------------------------------------------------------------------------------------
 * Local helpers: RX pool
 *---------------------------------------------------------------------------------------------*/

static void uart_line_rx_pool_init(uart_line_priv_st *pv)
{
    if (NULL == pv)
    {
        return;
    }

    (void)memset(pv, 0, sizeof(*pv));
    buffer_array_ctx_init(&pv->rx_pool,
                          pv->rx_buffers,
                          pv->rx_memory,
                          UART_LINE_BUFFER_COUNT,
                          UART_LINE_DMA_CHUNK);
}

static buffer_st *uart_line_rx_pool_acquire(buffer_array_ctx_st *pool)
{
    if (NULL == pool)
    {
        return NULL;
    }

    return buffer_array_acquire(pool);
}

static bool uart_line_rx_pool_release(buffer_array_ctx_st *pool, const uint8_t *buf)
{
    if ((NULL == pool) || (NULL == buf))
    {
        return false;
    }

    return buffer_array_release_by_ptr(pool, (uint8_t *)buf);
}

/*----------------------------------------------------------------------------------------------
 * Local helpers: line assembly
 *---------------------------------------------------------------------------------------------*/

/**
 * @brief Push a completed line into the message queue.
 *
 * Null-terminates the current buffer and enqueues it into the bound msgq.
 * If the queue is full, sets uart_line_st::dropped.
 *
 * @param uline UART line context.
 */
static inline void uart_line_push(uart_line_st *uline)
{
    if ((NULL == uline) || (NULL == uline->msgq))
    {
        return;
    }

    if (0u == uline->pos)
    {
        return;
    }

    /* Null-terminate & enqueue (pos is always < UART_LINE_MAX_LEN by construction). */
    uline->buf[uline->pos] = '\0';

    if (0 != k_msgq_put(uline->msgq, uline->buf, K_NO_WAIT))
    {
        uline->dropped = true;
    }

    uline->pos = 0u;
}

/**
 * @brief Consume a single received character.
 *
 * Handles CR-LF normalization, CRLF/LFCR collapse, line overflow, and line completion.
 * On newline, calls uart_line_push().
 *
 * @param uline UART line context.
 * @param rx    Received character byte.
 */
static inline void uart_line_consume_char(uart_line_st *uline, uint8_t rx)
{
    char c;

    if (NULL == uline)
    {
        return;
    }

    c = (char)rx;

    /* Normalize CR to LF. */
    if ('\r' == c)
    {
        c = '\n';
    }

    if ('\n' == c)
    {
        /* Collapse double newline (covers CRLF/LFCR after normalization). */
        if ('\n' == uline->last_char)
        {
            uline->last_char = c;
            return;
        }

        uart_line_push(uline);
    }
    else
    {
        if (uline->pos < (UART_LINE_MAX_LEN - 1u))
        {
            uline->buf[uline->pos] = c;
            uline->pos++;
        }
        else
        {
            /* Overflow: discard current line. */
            uline->pos        = 0u;
            uline->overflowed = true;
        }
    }

    uline->last_char = c;
}

/**
 * @brief Flush a partially assembled RX line into the message queue.
 */
static inline void uart_line_flush(uart_line_st *uline)
{
    if ((NULL != uline) && (uline->pos > 0u))
    {
        uart_line_push(uline);
    }
}

/*----------------------------------------------------------------------------------------------
 * Local helpers: async RX
 *---------------------------------------------------------------------------------------------*/

/**
 * @brief Give one available buffer to the UART driver.
 *
 * @param dev UART device.
 * @param pv  Private async/DMA state.
 *
 * @return 0 on success, negative errno on failure.
 */
static int uart_line_start_rx(const struct device *dev, uart_line_priv_st *pv)
{
    buffer_st *slot;
    uint8_t *buf;
    size_t cap_bytes;
    int rc;

    if ((NULL == dev) || (NULL == pv))
    {
        return -EINVAL;
    }

    slot = uart_line_rx_pool_acquire(&pv->rx_pool);
    if (NULL == slot)
    {
        return -ENOMEM;
    }

    buf = buffer_data(slot, &cap_bytes);
    if ((NULL == buf) || (0u == cap_bytes))
    {
        buffer_mark_free(slot);
        return -ENOMEM;
    }

    rc = uart_rx_enable(dev, buf, cap_bytes, UART_LINE_IDLE_TIMEOUT_US);
    if (0 != rc)
    {
        buffer_mark_free(slot);
    }

    return rc;
}

/**
 * @brief Respond to a UART_RX_BUF_REQUEST event with one pool buffer.
 */
static void uart_line_rsp_next_buffer(const struct device *dev, uart_line_priv_st *pv)
{
    buffer_st *slot;
    uint8_t *buf;
    size_t cap_bytes;

    if ((NULL == dev) || (NULL == pv))
    {
        return;
    }

    slot = uart_line_rx_pool_acquire(&pv->rx_pool);
    if (NULL == slot)
    {
        return;
    }

    buf = buffer_data(slot, &cap_bytes);
    if ((NULL == buf) || (0u == cap_bytes))
    {
        buffer_mark_free(slot);
        return;
    }

    if (0 != uart_rx_buf_rsp(dev, buf, cap_bytes))
    {
        buffer_mark_free(slot);
    }
}

/*----------------------------------------------------------------------------------------------
 * UART async callback
 *---------------------------------------------------------------------------------------------*/

/**
 * @brief UART async callback handler.
 *
 * Handles RX ready events, buffer requests/releases, and overflow/stop conditions.
 * Forwards received characters to uart_line_consume_char().
 *
 * @param dev       UART device.
 * @param evt       UART event from Zephyr async API.
 * @param user_data Pointer to @ref uart_line_st context (set in uart_callback_set()).
 */
static void uart_async_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
    uart_line_st      *uline;
    uart_line_priv_st *pv;

    if ((NULL == dev) || (NULL == evt) || (NULL == user_data))
    {
        return;
    }

    uline = (uart_line_st *)user_data;
    pv    = (uart_line_priv_st *)uline->priv;

    if (NULL == pv)
    {
        return;
    }

    switch (evt->type)
    {
        case UART_RX_RDY:
        {
            const uint8_t *buf = evt->data.rx.buf + evt->data.rx.offset;
            const size_t   len = evt->data.rx.len;

            for (size_t i = 0u; i < len; i++)
            {
                uart_line_consume_char(uline, buf[i]);
            }
            break;
        }

        case UART_RX_BUF_REQUEST:
        {
            uart_line_rsp_next_buffer(dev, pv);
            break;
        }

        case UART_RX_BUF_RELEASED:
        {
            (void)uart_line_rx_pool_release(&pv->rx_pool, evt->data.rx_buf.buf);
            break;
        }

        case UART_RX_STOPPED:
        {
            /* Flush any partial line, warn in thread mode, and recover RX. */
            uart_line_flush(uline);
            uline->overflowed = true;
            (void)uart_line_start_rx(dev, pv);
            break;
        }

        case UART_RX_DISABLED:
        {
            /* Flush any partial line and restart RX for continuous reception. */
            uart_line_flush(uline);
            (void)uart_line_start_rx(dev, pv);
            break;
        }

        default:
        {
            /* Ignore unhandled events. */
            break;
        }
    }
}

/*----------------------------------------------------------------------------------------------
 * Public APIs
 *---------------------------------------------------------------------------------------------*/

int uart_line_init(const struct device *dev,
                   uart_line_st *uline,
                   struct k_msgq *queue)
{
    uart_line_priv_st *pv = NULL;
    int rc;

    if ((NULL == dev) || (NULL == uline) || (NULL == queue))
    {
        return -EINVAL;
    }

    if (0 == device_is_ready(dev))
    {
        return -ENODEV;
    }

    (void)memset(uline, 0, sizeof(*uline));
    uline->uart_dev = dev;
    uline->msgq     = queue;

    if (0 != k_mem_slab_alloc(&uart_line_priv_slab, (void **)&pv, K_NO_WAIT))
    {
        return -ENOMEM;
    }

    uart_line_rx_pool_init(pv);
    uline->priv = pv;

    rc = uart_callback_set(dev, uart_async_cb, uline);
    if (0 != rc)
    {
        printk("uart_line: uart_callback_set failed (rc=%d)\n", rc);
        k_mem_slab_free(&uart_line_priv_slab, pv);
        uline->priv = NULL;
        return rc;
    }

    rc = uart_line_start_rx(dev, pv);
    if (0 != rc)
    {
        printk("uart_line: uart_rx_enable failed (rc=%d)\n", rc);
        (void)uart_callback_set(dev, NULL, NULL);
        k_mem_slab_free(&uart_line_priv_slab, pv);
        uline->priv = NULL;
        return rc;
    }

    return 0;
}

void uart_line_deinit(uart_line_st *uline)
{
    uart_line_priv_st *pv;

    if ((NULL == uline) || (NULL == uline->uart_dev))
    {
        return;
    }

    pv = (uart_line_priv_st *)uline->priv;

    /* First detach callback to avoid restart-on-DISABLED during teardown. */
    (void)uart_callback_set(uline->uart_dev, NULL, NULL);

    /* Now disable RX; no callback will run. */
    (void)uart_rx_disable(uline->uart_dev);

    /* Flush any partial line. */
    uart_line_flush(uline);

    if (NULL != pv)
    {
        k_mem_slab_free(&uart_line_priv_slab, pv);
    }

    uline->priv = NULL;

    if (uline == default_uart_line)
    {
        default_uart_line = NULL;
    }
}

void uart_line_rx_poll_warnings(uart_line_st *uline,
                                void (*warn_cb)(uart_line_st *uline, const char *msg))
{
    if ((NULL == uline) || (NULL == warn_cb))
    {
        return;
    }

    if (uline->overflowed)
    {
        warn_cb(uline, "\r\n[WARN] Line too long or RX error; recovered\r\n");
        uline->overflowed = false;
    }

    if (uline->dropped)
    {
        warn_cb(uline, "\r\n[WARN] RX queue full, line dropped\r\n");
        uline->dropped = false;
    }
}

bool uart_line_rx_get(uart_line_st *uline,
                      char *dst,
                      size_t dst_len,
                      k_timeout_t timeout)
{
    char   tmp[UART_LINE_MAX_LEN];
    size_t n;

    if ((NULL == uline) || (NULL == uline->msgq) || (NULL == dst) || (0u == dst_len))
    {
        return false;
    }

    if (0 != k_msgq_get(uline->msgq, tmp, timeout))
    {
        return false; /* timeout/no line */
    }

    n = strnlen(tmp, UART_LINE_MAX_LEN);
    if (n >= dst_len)
    {
        n = dst_len - 1u;
    }

    (void)memcpy(dst, tmp, n);
    dst[n] = '\0';

    return true;
}

void uart_line_tx(uart_line_st *uline, const char *msg)
{
    size_t i;

    if ((NULL == uline) || (NULL == uline->uart_dev) || (NULL == msg))
    {
        return;
    }

    for (i = 0u; '\0' != msg[i]; i++)
    {
        uart_poll_out(uline->uart_dev, msg[i]);
    }
}

void uart_line_set_default(uart_line_st *uline)
{
    default_uart_line = uline;
}

void uart_line_transmit(const char *msg)
{
    if ((NULL == default_uart_line) || (NULL == msg))
    {
        return;
    }

    uart_line_tx(default_uart_line, msg);
}
