/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file main.c
 * @brief CEVO UART Line Receiver Demo (portable, interrupt-driven)
 *
 * Receives lines via UART interrupts, queues them using a Zephyr message queue,
 * and dispatches complete lines to CEVO. Compatible with most Zephyr boards
 * and QEMU (with echo enabled only in QEMU).
 */

#include "uart_line.h"
#include "ce_dispatch.h"

/* Each receiver instance owns its own queue */
K_MSGQ_DEFINE(line_msgq,
              UART_LINE_MAX_LEN,
              UART_LINE_QUEUE_LENGTH,
              UART_LINE_MSGQ_ALIGN);

/* Default UART line context (TX + RX + warnings) */
static uart_line_st g_uart_line;

int main(void)
{
    const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

    if ((NULL == dev) || (false == device_is_ready(dev)))
    {
        /* Device not ready! */
        for(;;)
        {
            printk("FATAL: console UART not ready\r\n");
            k_msleep(1000);
        }
    }

    if (0 != uart_line_init(dev, &g_uart_line, &line_msgq))
    {
        for(;;)
        {
            printk("FATAL: UART init failed\r\n");
            k_msleep(1000);
        }
    }

    uart_line_set_default(&g_uart_line);

    uart_line_tx(&g_uart_line, "\r\nCEVO Demo " CONFIG_BOARD " ready\r\n>> ");

    char line_buf[UART_LINE_MAX_LEN];

    for(;;)
    {
        /* Print deferred warnings once per loop */
        uart_line_rx_poll_warnings(&g_uart_line, uart_line_tx);

#ifdef CONFIG_QEMU_TARGET
        /* QEMU: must poll. "K_FOREVER" freezes emulation */
        const k_timeout_t rx_timeout = K_NO_WAIT;
#else
        /* Real HW: safe to block, wakes on UART IRQ */
        const k_timeout_t rx_timeout = K_FOREVER;
#endif

        if (uart_line_rx_get(&g_uart_line, line_buf, sizeof(line_buf), rx_timeout))
        {
            uart_line_tx(&g_uart_line, "\r\n");
            const bool ok = ce_dispatch_from_line(line_buf);
            uart_line_tx(&g_uart_line, ok ? "\r\n>> " : "\r\nERR\r\n>> ");
        }

#ifdef CONFIG_QEMU_TARGET
        /* Yield a tick so emulation doesn't spin */
        k_msleep(1);
#endif
    }
}
