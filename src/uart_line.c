/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file uart_line.c
 */

#include "uart_line.h"

static uart_line_st *default_uart_line = NULL;

/**
 * @brief Internal ISR: reads bytes, detects line termination, pushes to queue.
 *
 * This ISR directly parses incoming bytes into a single line buffer.
 *
 * Why process lines here instead of deferring?
 *  - Only keeps ONE active line buffer (uline->buf).
 *  - No extra ring buffer or worker thread, keeping RAM and complexity low.
 *  - On newline, it immediately pushes the completed line to msgq.
 *
 * Trade-offs:
 *  - ISR does more work (CRLF filtering, overflow detection, msgq_put).
 *  - Not ideal for high UART traffic or multiple UARTs.
 *
 * This should be acceptable for our simple, single-UART demo with low data rates.
 * For production/high-speed use, ISR shall be decoupled from any heavy execution.
 */
static void uart_isr(const struct device *dev, void *user_data)
{
    uart_line_st *uline = (uart_line_st *)user_data;
    uint8_t c;

    if ((!uart_irq_update(dev)) || (!uart_irq_rx_ready(dev)))
    {
        return;
    }

    while (1 == uart_fifo_read(dev, &c, 1u))
    {
#ifdef CONFIG_QEMU_TARGET
        /* Echo only in QEMU */
        (void)uart_poll_out(dev, c);
#endif

        if (('\n' == c) || ('\r' == c))
        {
            if (uline->pos > 0u)
            {
                /* Skip CRLF/LFCR pairs */
                if ((('\r' == uline->last_char) && ('\n' == c)) ||
                    (('\n' == uline->last_char) && ('\r' == c)))
                {
                    uline->last_char = (char)c;
                    continue;
                }

                /* Null-terminate & push */
                uline->buf[uline->pos] = '\0';

                if (0 != k_msgq_put(uline->msgq, uline->buf, K_NO_WAIT))
                {
                    uline->dropped = true;
                }

                uline->pos = 0u;
            }
        }
        else
        {
            if (uline->pos < (UART_LINE_MAX_LEN - 1u))
            {
                uline->buf[uline->pos++] = (char)c;
            }
            else
            {
                /* Overflow: discard current line */
                uline->pos = 0u;
                uline->overflowed = true;
            }
        }

        uline->last_char = (char)c;
    }
}

int uart_line_init(const struct device *dev,
                   uart_line_st *uline,
                   struct k_msgq *queue)
{
    if ((NULL == uline) || (NULL == dev) || (NULL == queue))
    {
        return -EINVAL;
    }

    if (!device_is_ready(dev))
    {
        return -ENODEV;
    }

    uline->uart_dev   = dev;
    uline->msgq       = queue;
    uline->pos        = 0u;
    uline->last_char  = '\0';
    uline->overflowed = false;
    uline->dropped    = false;

    int ret = uart_irq_callback_user_data_set(dev, uart_isr, uline);
    if (0 != ret)
    {
        return ret;
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
        warn_cb(uline, "\r\n[WARN] Line too long, discarded\r\n");
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
    char tmp_buf[UART_LINE_MAX_LEN];

    if ((NULL == uline) || (NULL == uline->msgq) || (NULL == dst) || (0u == dst_len))
    {
        return false;
    }

    if (0 != k_msgq_get(uline->msgq, tmp_buf, timeout))
    {
        return false; /* timeout/no line */
    }

    /* Copy with truncation */
    (void)strncpy(dst, tmp_buf, dst_len);
    dst[dst_len - 1u] = '\0';

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
        (void)uart_poll_out(uline->uart_dev, msg[i]);
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
