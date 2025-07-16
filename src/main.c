/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file main.c
 * @brief CEVO UART Line Receiver Demo (portable, interrupt-driven)
 *
 * Receives lines via UART interrupts, queues them using a Zephyr message queue,
 * and dispatches complete lines to CEVO. Compatible with most Zephyr boards
 * and QEMU (with echo enabled only in QEMU).
 */

#include <string.h>
#include <ctype.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#include "uart.h"
#include "ce_dispatch.h"

/**
 * @brief Alignment (in bytes) for UART message queue storage.
 *
 * Zephyr requires each queued element to be naturally aligned
 * for the target CPU. Most 32-bit platforms require 4-byte alignment.
 *
 * @note If running on 64-bit targets, this may need to be 8.
 */
#define UART_LINE_MSGQ_ALIGN   (4u)

#define UART_PROMPT            ">> "

/* Platform-agnostic UART selection */
#define UART_NODE DT_CHOSEN(zephyr_console)

/* RX line queue */
K_MSGQ_DEFINE(uart_msgq, UART_RX_BUF_SIZE, UART_LINE_QUEUE_LENGTH, UART_LINE_MSGQ_ALIGN);

/* UART device reference */
static const struct device *uart_dev = NULL;

/* Warning messages reused in ISR */
static const char WARN_QUEUE_FULL[] = "\r\n[WARN] RX queue full, line dropped\r\n";
static const char WARN_LINE_TOO_LONG[] = "\r\n[WARN] Line too long, discarded\r\n";

void uart_send_line(const char *line)
{
    if (NULL == line)
    {
        return;
    }

    for (size_t i = 0u; '\0' != line[i]; ++i)
    {
        (void)uart_poll_out(uart_dev, line[i]);
    }
}

/**
 * @brief UART ISR: accumulates chars, queues complete lines
 */
static void uart_isr(const struct device *dev, void *user_data)
{
    /* Static RX buffer persists across ISR calls */
    static char rx_buf[UART_RX_BUF_SIZE];
    static size_t rx_pos = 0u;
    static char last_char = '\0';
    uint8_t c;

    ARG_UNUSED(user_data);

    if ((NULL == dev) || (!uart_irq_update(dev)) || (!uart_irq_rx_ready(dev)))
    {
        return;
    }

    while (uart_fifo_read(dev, &c, 1u) == 1)
    {
#ifdef CONFIG_QEMU_TARGET
        /* Echo only in QEMU for demo */
        (void)uart_poll_out(dev, c);
#endif

        /* Detect end-of-line */
        if ((('\n' == c) || ('\r' == c)) && (rx_pos > 0u))
        {
            /* Skip CRLF / LFCR double sequences */
            if (((last_char == '\r') && (c == '\n')) ||
                ((last_char == '\n') && (c == '\r')))
            {
                last_char = (char)c;
                continue;
            }

            /* Null-terminate */
            rx_buf[rx_pos] = '\0';

            /* Push to queue or drop if full */
            if (0 != k_msgq_put(&uart_msgq, rx_buf, K_NO_WAIT))
            {
                uart_send_line(WARN_QUEUE_FULL);
            }
            rx_pos = 0u;
        }
        else if (rx_pos < (UART_RX_BUF_SIZE - 1u))
        {
            rx_buf[rx_pos++] = (char)c;
        }
        else
        {
            /* Overflow: discard current line */
            rx_pos = 0u;
            uart_send_line(WARN_LINE_TOO_LONG);
        }

        last_char = (char)c;
    }
}

/**
 * @brief Initialize interrupt-driven UART line receiver
 */
static int uart_line_rx_init(const struct device *dev)
{
    if ((NULL == dev) || (!device_is_ready(dev)))
    {
        return -ENODEV;
    }

    uart_dev = dev;

    int ret = uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
    if (0 != ret)
    {
        return ret;
    }

    uart_irq_rx_enable(uart_dev);
    return 0;
}

/**
 * @brief Process received line if available
 */
static void uart_line_rx_process(k_timeout_t timeout)
{
    char line_buf[UART_RX_BUF_SIZE];

    if (0 == k_msgq_get(&uart_msgq, line_buf, timeout))
    {
        if ('\0' != line_buf[0])
        {
            uart_send_line("\r\n");
            const bool ok = ce_dispatch_from_line(line_buf);
            uart_send_line(ok ? "\r\n" UART_PROMPT : "ERR\r\n" UART_PROMPT);
        }
        else
        {
            uart_send_line(UART_PROMPT);
        }

        uart_irq_rx_enable(uart_dev);
    }
}

/**
 * @brief Application entry point
 */
int main(void)
{
    const struct device *uart = DEVICE_DT_GET(UART_NODE);

    if (uart_line_rx_init(uart) != 0)
    {
        uart_send_line("[FATAL] UART init failed\r\n");
        return 0;
    }

    uart_send_line("\r\nCEVO Demo " CONFIG_BOARD " ready\r\n>> ");

    while (true)
    {
#ifdef CONFIG_QEMU_TARGET
        /* QEMU: must poll, K_FOREVER freezes emulation */
        uart_line_rx_process(K_NO_WAIT);
        k_msleep(1);
#else
        /* Real HW: safe to block, wakes on UART IRQ */
        uart_line_rx_process(K_FOREVER);
#endif
    }

    return 0;
}
