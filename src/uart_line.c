/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file uart_line.c
 */

#include "uart_line.h"
#include <string.h>

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
 * If the queue is full, sets @ref uart_line_st::dropped.
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
 * On newline, calls @ref uart_line_push().
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
        /* collapse double newline (covers CRLF/LFCR after normalization) */
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

/*----------------------------------------------------------------------------------------------
 * IRQ/FIFO
 *---------------------------------------------------------------------------------------------*/
/**
 * @brief ISR for IRQ/FIFO path (non-async mode).
 *
 * Reads incoming bytes from the UART FIFO and forwards them to
 * @ref uart_line_consume_char(). Echoes to TX if CONFIG_QEMU_TARGET is set.
 *
 * Trade-offs:
 *  - ISR does more work (CRLF filtering, overflow detection, msgq_put).
 *  - Not ideal for high UART traffic or multiple UARTs.
 *
 * @param dev       UART device.
 * @param user_data Pointer to @ref uart_line_st context.
 */
static void uart_isr(const struct device *dev, void *user_data)
{
    uart_line_st *uline = (uart_line_st *)user_data;
    uint8_t c;

    if ((1 != uart_irq_update(dev)) || (1 != uart_irq_rx_ready(dev)))
    {
        return;
    }

    while (1 == uart_fifo_read(dev, &c, 1u))
    {
#ifdef CONFIG_QEMU_TARGET
        /* Echo only in QEMU */
        uart_poll_out(dev, c);
#endif
        uart_line_consume_char(uline, c);
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

    const int rc_cb = uart_irq_callback_user_data_set(dev, uart_isr, uline);
    if (0 != rc_cb)
    {
        return rc_cb;
    }

    uart_irq_rx_enable(dev);

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
