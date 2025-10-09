/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file uart_line.c
 */

#include <string.h>
#include <zephyr/sys/util.h>
#include "uart_line.h"

/*----------------------------------------------------------------------------------------------
 * Local config
 *---------------------------------------------------------------------------------------------*/

#ifndef UART_LINE_MAX_INSTANCES
#define UART_LINE_MAX_INSTANCES   (2u)
#endif

#ifndef UART_LINE_DMA_CHUNK
#define UART_LINE_DMA_CHUNK       (128u)   /* size of each RX DMA handoff buffer */
#endif

/* Finite idle timeout so RX_RDY fires without filling the whole DMA buffer */
#ifndef UART_LINE_IDLE_TIMEOUT_US
#define UART_LINE_IDLE_TIMEOUT_US (50000u) /* 50 ms */
#endif

/**
 * @brief Private per-instance state for async/DMA mode.
 */
typedef struct
{
    uint8_t rx_a[UART_LINE_DMA_CHUNK];    /**< First RX DMA buffer. */
    uint8_t rx_b[UART_LINE_DMA_CHUNK];    /**< Second RX DMA buffer. */
    volatile bool a_free;                 /**< True if buffer A is free for reuse. */
    volatile bool b_free;                 /**< True if buffer B is free for reuse. */
} uart_line_priv_st;

/* one slab, 4-byte aligned */
#define SLAB_ALIGN      4u
#define SLAB_BLOCK_SIZE ROUND_UP(sizeof(uart_line_priv_st), SLAB_ALIGN)

/**
 * @brief Memory slab for allocating private async/DMA state.
 */
K_MEM_SLAB_DEFINE(uart_line_priv_slab,
                  SLAB_BLOCK_SIZE,
                  UART_LINE_MAX_INSTANCES,
                  SLAB_ALIGN);

/*----------------------------------------------------------------------------------------------
 * Private per-instance state
 *---------------------------------------------------------------------------------------------*/

/**
 * @brief Default UART line instance used by uart_line_transmit().
 */
static uart_line_st *default_uart_line = NULL;

/*----------------------------------------------------------------------------------------------
 * Local helpers
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
    if (0u == uline->pos)
    {
        return;
    }

    /* Null-terminate & enqueue */
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
    char c = (char)rx;

    /* Normalize CR to LF */
    if ('\r' == c)
    {
        c = '\n';
    }

    if ('\n' == c)
    {
        /* Collapse double newline (covers CRLF/LFCR after normalization) */
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
            uline->buf[uline->pos++] = c;
        }
        else
        {
            /* Overflow: discard current line */
            uline->pos = 0u;
            uline->overflowed = true;
        }
    }

    uline->last_char = c;
}

/**
 * @brief Flush a partially assembled RX line into the message queue.
 *
 * Finalizes the current assembly buffer as a complete line and enqueues it to
 * the bound message queue if at least one character has been collected
 */
static inline void uart_line_flush(uart_line_st *uline)
{
    if (NULL == uline)
    {
        return;
    }
    if  (uline->pos > 0u)
    {
        /* Push current partial line as a full line */
        uart_line_push(uline);
    }
}

/*----------------------------------------------------------------------------------------------
 * Async (DMA-capable) path
 *---------------------------------------------------------------------------------------------*/

/**
 * @brief Enable asynchronous RX (DMA).
 *
 * Starts RX using buffer A and primes buffer B as free.
 *
 * @param dev UART device.
 * @param pv  Private async/DMA state.
 *
 * @return 0  If successful
 */
static int uart_line_rx_enable_async(const struct device *dev, uart_line_priv_st *pv)
{
    pv->a_free = false;
    pv->b_free = true;
    int rc = uart_rx_enable(dev, pv->rx_a, UART_LINE_DMA_CHUNK, UART_LINE_IDLE_TIMEOUT_US);
    if (0 != rc)
    {
        printk("uart_line: uart_rx_enable failed rc=%d (DMA/async not set up)\n", rc);
    }
    return rc;
}

/**
 * @brief UART async callback handler.
 *
 * Handles RX ready events, buffer requests/releases, and overflow/stop conditions.
 * Forwards received characters to uart_line_consume_char().
 *
 * @param dev       UART device.
 * @param evt       UART event from Zephyr async API.
 * @param user_data Pointer to @ref uart_line_st context.
 */
static void uart_async_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
    uart_line_st *uline = (uart_line_st *)user_data;
    uart_line_priv_st *pv = (uart_line_priv_st *)uline->priv;

    if ((NULL == uline) || (NULL == pv))
    {
        return;
    }

    switch (evt->type)
    {
        case UART_RX_RDY:
        {
            const uint8_t *buf = evt->data.rx.buf + evt->data.rx.offset;
            const size_t   len = evt->data.rx.len;

            for (size_t i = 0u; i < len; ++i)
            {
                uart_line_consume_char(uline, buf[i]);
            }
            break;
        }

        case UART_RX_BUF_REQUEST:
        {
            if (true == pv->a_free)
            {
                pv->a_free = false;
                (void)uart_rx_buf_rsp(dev, pv->rx_a, UART_LINE_DMA_CHUNK);
            }
            else if (true == pv->b_free)
            {
                pv->b_free = false;
                (void)uart_rx_buf_rsp(dev, pv->rx_b, UART_LINE_DMA_CHUNK);
            }
            else
            {
                /* No free buffer; RX may pause briefly */
            }
            break;
        }

        case UART_RX_BUF_RELEASED:
        {
            if (evt->data.rx_buf.buf == pv->rx_a)
            {
                pv->a_free = true;
            }
            else if (evt->data.rx_buf.buf == pv->rx_b)
            {
                pv->b_free = true;
            }
            break;
        }

        case UART_RX_STOPPED:
        {
            /* Flush any partial line before restarting */
            uart_line_flush(uline);
            /* Mark + recover; keep warnings minimal and deferred */
            uline->overflowed = true;
            (void)uart_line_rx_enable_async(dev, pv);
            break;
        }

        case UART_RX_DISABLED:
        {
            uart_line_flush(uline);
            uart_line_rx_enable_async(dev, pv);
            break;
        }

        default:
            break;
    }
}

/*----------------------------------------------------------------------------------------------
 * Public APIs
 *---------------------------------------------------------------------------------------------*/
int uart_line_init(const struct device *dev,
                   uart_line_st *uline,
                   struct k_msgq *queue)
{
    if ((NULL == uline) || (NULL == dev) || (NULL == queue))
    {
        return -EINVAL;
    }

    if (0 == device_is_ready(dev))
    {
        return -ENODEV;
    }

    uline->uart_dev   = dev;
    uline->msgq       = queue;
    uline->pos        = 0u;
    uline->last_char  = '\0';
    uline->overflowed = false;
    uline->dropped    = false;
    uline->priv       = NULL;

    uart_line_priv_st *pv = NULL;
    if (0 != k_mem_slab_alloc(&uart_line_priv_slab, (void **)&pv, K_NO_WAIT))
    {
        return -ENOMEM;
    }
    (void)memset(pv, 0, sizeof(*pv));
    uline->priv = pv;

    const int rc_cb = uart_callback_set(dev, uart_async_cb, uline);
    if (0 != rc_cb)
    {
        printk("uart_line: async not available (rc=%d), no DMA path\n", rc_cb);
        k_mem_slab_free(&uart_line_priv_slab, (void **)&pv);
        uline->priv = NULL;
        return rc_cb;
    }

    const int rc_en = uart_line_rx_enable_async(dev, pv);
    if (0 != rc_en)
    {
        k_mem_slab_free(&uart_line_priv_slab, (void **)&pv);
        uline->priv = NULL;
        return rc_en;
    }

    return 0;
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
    if ((NULL == uline) || (NULL == uline->msgq) || (NULL == dst) || (0u == dst_len))
    {
        return false;
    }

    /* Dequeue into a local buffer, then bounded-copy into dst.
     * This avoids writing past dst if the msgq element size is UART_LINE_MAX_LEN.
     */
    char tmp[UART_LINE_MAX_LEN];

    if (0 != k_msgq_get(uline->msgq, tmp, timeout))
    {
        return false; /* timeout/no line */
    }

    /* Bounded copy & ensure null-termination */
    size_t n = strnlen(tmp, UART_LINE_MAX_LEN);
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
    if ((NULL == uline) || (NULL == msg))
    {
        return;
    }

    for (size_t i = 0u; '\0' != msg[i]; ++i)
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

void uart_line_deinit(uart_line_st *uline)
{
    if (NULL == uline)
    {
        return;
    }
    if (NULL == uline->uart_dev)
    {
        return;
    }

    /* Stop RX and detach callback before freeing buffers */
    (void)uart_rx_disable(uline->uart_dev);
    (void)uart_callback_set(uline->uart_dev, NULL, NULL);

    /* Ensure not to lose a partial line */
    uart_line_flush(uline);

    if (NULL != uline->priv)
    {
        k_mem_slab_free(&uart_line_priv_slab, (void **)&uline->priv);
        uline->priv = NULL;
    }
}
