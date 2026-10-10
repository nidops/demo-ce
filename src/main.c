/* SPDX-License-Identifier: Apache-2.0 */
#include "ce_dispatch.h"
#include "uart_line.h"

K_MSGQ_DEFINE(line_msgq, UART_LINE_MAX_LEN, UART_LINE_QUEUE_LENGTH, UART_LINE_MSGQ_ALIGN);
static uart_line_st console;

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	int rc = uart_line_init(dev, &console, &line_msgq);

	if (rc != 0) {
		printk("FATAL: UART DMA init failed (%d)\r\n", rc);
		return rc;
	}
	uart_line_set_default(&console);
	uart_line_tx(&console, "\r\nCEVO Demo " CONFIG_BOARD " ready\r\n>> ");

	for (;;) {
		char line[UART_LINE_MAX_LEN];
		bool received = uart_line_rx_get(&console, line, sizeof(line), K_MSEC(100));

		uart_line_rx_poll_warnings(&console, uart_line_tx);
		rc = uart_line_rx_error(&console);
		if (rc != 0) {
			printk("FATAL: UART DMA restart failed (%d)\r\n", rc);
			uart_line_deinit(&console);
			return rc;
		}
		if (received) {
			uart_line_tx(&console, "\r\n");
			bool ok = ce_dispatch_from_line(line);

			uart_line_tx(&console, ok ? "\r\n>> " : "\r\nERR\r\n>> ");
		}
	}
}
