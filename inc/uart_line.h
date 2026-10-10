/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UART_LINE_H_
#define UART_LINE_H_

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UART_LINE_MAX_LEN          128U
#define UART_LINE_QUEUE_LENGTH     8U
#define UART_LINE_MSGQ_ALIGN       1U
#define UART_LINE_DMA_CHUNK        128U
#define UART_LINE_DMA_BUFFER_COUNT 2U

/* Zero-initialize before first init. Keep alive until deinit succeeds.
 * Each instance owns an aligned, fixed-size Zephyr memory slab for RX DMA.
 * Reception and lifecycle fields are private to uart_line.c.
 */
typedef struct {
	const struct device *uart_dev;
	struct k_msgq *msgq;
	struct k_mem_slab rx_pool;
	uint8_t dma_storage[UART_LINE_DMA_BUFFER_COUNT][UART_LINE_DMA_CHUNK]
		__aligned(sizeof(void *));
	struct k_spinlock line_lock;
	struct k_mutex lifecycle_lock;
	struct k_work_delayable restart_work;
	struct k_sem disabled_sem;
	atomic_t closing;
	atomic_t disabled;
	atomic_t warnings;
	atomic_t rx_error;
	char buf[UART_LINE_MAX_LEN];
	size_t pos;
	char last_char;
	bool discarding;
	bool initialized;
} uart_line_st;

/* Thread context only; callers serialize init/deinit for each instance.
 * Queue elements must be UART_LINE_MAX_LEN bytes. After a failed init, call
 * deinit before releasing the context: callback cleanup can also fail.
 */
int uart_line_init(const struct device *dev, uart_line_st *line, struct k_msgq *queue);

/* Stop RX, wait for all driver buffer releases, cancel recovery, detach callback.
 * Returns 0 when context/storage may be reused. On failure retain the context and
 * retry after the driver finishes; no driver-owned memory is freed or recycled.
 */
int uart_line_deinit(uart_line_st *line);

/* 0 while RX is healthy; negative driver error when a deferred restart failed. */
int uart_line_rx_error(const uart_line_st *line);
bool uart_line_rx_get(uart_line_st *line, char *dst, size_t dst_len, k_timeout_t timeout);
void uart_line_rx_poll_warnings(uart_line_st *line, void (*warn)(uart_line_st *, const char *));
void uart_line_tx(uart_line_st *line, const char *msg);
void uart_line_set_default(uart_line_st *line);
void uart_line_transmit(const char *msg);

#ifdef __cplusplus
}
#endif
#endif
