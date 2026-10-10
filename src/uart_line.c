/* SPDX-License-Identifier: Apache-2.0 */
#include "uart_line.h"

#include <errno.h>
#include <string.h>
#include <zephyr/sys/util.h>

#define RX_IDLE_TIMEOUT_US 1000
#define WARN_LINE_TOO_LONG BIT(0)
#define WARN_QUEUE_FULL    BIT(1)
#define WARN_RX_STOPPED    BIT(2)

static uart_line_st *default_uart_line;

static void queue_line(uart_line_st *line)
{
	line->buf[line->pos] = '\0';
	if (k_msgq_put(line->msgq, line->buf, K_NO_WAIT) != 0) {
		atomic_or(&line->warnings, WARN_QUEUE_FULL);
	}
	line->pos = 0;
}

/* Called under line_lock. Idle DMA notifications never complete a command. */
static void consume_byte(uart_line_st *line, uint8_t byte)
{
	char c = byte == '\r' ? '\n' : (char)byte;

	if (c == '\n') {
		if (line->discarding) {
			line->discarding = false;
			line->pos = 0;
			queue_line(line); /* Empty line is one explicit command rejection. */
		} else if (line->last_char != '\n' && line->pos != 0) {
			queue_line(line);
		}
	} else if (!line->discarding) {
		if (line->pos < sizeof(line->buf) - 1) {
			line->buf[line->pos++] = c;
		} else {
			line->pos = 0;
			line->discarding = true;
			atomic_or(&line->warnings, WARN_LINE_TOO_LONG);
		}
	}
	line->last_char = c;
}

static int start_rx(uart_line_st *line)
{
	uint8_t *buffer;
	int rc = k_mem_slab_alloc(&line->rx_pool, (void **)&buffer, K_NO_WAIT);

	if (rc != 0) {
		return rc;
	}
	atomic_clear(&line->disabled);
	k_sem_reset(&line->disabled_sem);
	rc = uart_rx_enable(line->uart_dev, buffer, UART_LINE_DMA_CHUNK, RX_IDLE_TIMEOUT_US);
	if (rc != 0) {
		/* A rejected enable does not transfer ownership to the driver. */
		k_mem_slab_free(&line->rx_pool, buffer);
		atomic_set(&line->disabled, 1);
		k_sem_give(&line->disabled_sem);
	}
	return rc;
}

static void restart_rx(struct k_work *work)
{
	uart_line_st *line =
		CONTAINER_OF(k_work_delayable_from_work(work), uart_line_st, restart_work);

	k_mutex_lock(&line->lifecycle_lock, K_FOREVER);
	if (!atomic_get(&line->closing) && atomic_get(&line->disabled)) {
		/* Wait for the DISABLED callback's final context access, including
		 * on SMP. A timer delay alone cannot provide that ordering.
		 */
		k_sem_take(&line->disabled_sem, K_FOREVER);
		if (atomic_get(&line->closing)) {
			k_sem_give(&line->disabled_sem);
		} else {
			atomic_set(&line->rx_error, start_rx(line));
		}
	}
	k_mutex_unlock(&line->lifecycle_lock);
}

static void async_callback(const struct device *dev, struct uart_event *event, void *user_data)
{
	uart_line_st *line = user_data;

	switch (event->type) {
	case UART_RX_RDY: {
		if (atomic_get(&line->closing)) {
			break;
		}
		k_spinlock_key_t key = k_spin_lock(&line->line_lock);
		const uint8_t *bytes = event->data.rx.buf + event->data.rx.offset;

		for (size_t i = 0; i < event->data.rx.len; ++i) {
			consume_byte(line, bytes[i]);
		}
		k_spin_unlock(&line->line_lock, key);
		break;
	}
	case UART_RX_BUF_REQUEST: {
		uint8_t *buffer;

		if (atomic_get(&line->closing)) {
			break;
		}
		if (k_mem_slab_alloc(&line->rx_pool, (void **)&buffer, K_NO_WAIT) == 0) {
			if (uart_rx_buf_rsp(dev, buffer, UART_LINE_DMA_CHUNK) != 0) {
				k_mem_slab_free(&line->rx_pool, buffer);
				atomic_or(&line->warnings, WARN_RX_STOPPED);
			}
		} else {
			atomic_or(&line->warnings, WARN_RX_STOPPED);
		}
		break;
	}
	case UART_RX_BUF_RELEASED:
		/* RX_RDY bytes were consumed; only this event ends driver ownership. */
		k_mem_slab_free(&line->rx_pool, event->data.rx_buf.buf);
		break;
	case UART_RX_STOPPED: {
		k_spinlock_key_t key = k_spin_lock(&line->line_lock);

		line->pos = 0;
		line->discarding = true;
		k_spin_unlock(&line->line_lock, key);
		atomic_or(&line->warnings, WARN_RX_STOPPED);
		/* Do not restart here: the driver still owns its DMA buffers. */
		break;
	}
	case UART_RX_DISABLED:
		atomic_set(&line->disabled, 1);
		if (!atomic_get(&line->closing)) {
			k_spinlock_key_t key = k_spin_lock(&line->line_lock);

			/* A reception gap cannot turn an incomplete line into a command. */
			line->pos = 0;
			line->discarding = true;
			queue_line(line);
			k_spin_unlock(&line->line_lock, key);
			atomic_or(&line->warnings, WARN_RX_STOPPED);
			k_work_reschedule(&line->restart_work, K_MSEC(1));
		}
		/* Final access to context: teardown may wake and release it now. */
		k_sem_give(&line->disabled_sem);
		break;
	default:
		break;
	}
}

int uart_line_init(const struct device *dev, uart_line_st *line, struct k_msgq *queue)
{
	if (dev == NULL || line == NULL || queue == NULL || k_is_in_isr()) {
		return -EINVAL;
	}
	if (line->initialized) {
		return -EALREADY;
	}
	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	if (queue->msg_size != UART_LINE_MAX_LEN) {
		return -EINVAL;
	}
	memset(line, 0, sizeof(*line));
	line->uart_dev = dev;
	line->msgq = queue;
	k_sem_init(&line->disabled_sem, 0, 1);
	k_mutex_init(&line->lifecycle_lock);
	k_work_init_delayable(&line->restart_work, restart_rx);
	int rc = k_mem_slab_init(&line->rx_pool, line->dma_storage, UART_LINE_DMA_CHUNK,
				 UART_LINE_DMA_BUFFER_COUNT);

	if (rc != 0) {
		return rc;
	}
	rc = uart_callback_set(dev, async_callback, line);
	if (rc != 0) {
		return rc;
	}
	line->initialized = true;
	rc = start_rx(line);
	if (rc != 0) {
		if (uart_callback_set(dev, NULL, NULL) == 0) {
			line->initialized = false;
		}
		return rc;
	}
	return 0;
}

int uart_line_deinit(uart_line_st *line)
{
	if (line == NULL || k_is_in_isr()) {
		return -EINVAL;
	}
	if (!line->initialized) {
		return 0;
	}
	atomic_set(&line->closing, 1);
	struct k_work_sync sync;

	k_work_cancel_delayable_sync(&line->restart_work, &sync);
	k_mutex_lock(&line->lifecycle_lock, K_FOREVER);
	int rc = 0;

	if (!atomic_get(&line->disabled)) {
		rc = uart_rx_disable(line->uart_dev);
		if (rc == -EFAULT) {
			rc = 0;
		}
	}
	if (rc == 0) {
		rc = k_sem_take(&line->disabled_sem, K_SECONDS(1));
		if (rc == 0) {
			/* Keep completion signalled if callback detachment must be retried. */
			k_sem_give(&line->disabled_sem);
		} else {
			rc = -ETIMEDOUT;
		}
	}
	if (rc == 0 && k_mem_slab_num_free_get(&line->rx_pool) != UART_LINE_DMA_BUFFER_COUNT) {
		rc = -EBUSY;
	}
	if (rc == 0) {
		rc = uart_callback_set(line->uart_dev, NULL, NULL);
	}
	if (rc == 0) {
		line->initialized = false;
		line->pos = 0;
		if (default_uart_line == line) {
			default_uart_line = NULL;
		}
	}
	k_mutex_unlock(&line->lifecycle_lock);
	if (rc == 0) {
		/* A DISABLED callback racing the first cancellation may have queued
		 * recovery before noticing closing. It has now finished; drain again
		 * without holding the mutex needed by the recovery worker.
		 */
		k_work_cancel_delayable_sync(&line->restart_work, &sync);
	}
	/* On failure, keep context and callback alive for outstanding driver releases. */
	return rc;
}

int uart_line_rx_error(const uart_line_st *line)
{
	return atomic_get(&line->rx_error);
}

void uart_line_rx_poll_warnings(uart_line_st *line, void (*warn)(uart_line_st *, const char *))
{
	if (line == NULL || warn == NULL) {
		return;
	}
	atomic_val_t flags = atomic_set(&line->warnings, 0);

	if (flags & WARN_LINE_TOO_LONG) {
		warn(line, "\r\n[WARN] Line too long; discarded\r\n");
	}
	if (flags & WARN_QUEUE_FULL) {
		warn(line, "\r\n[WARN] RX queue full, line dropped\r\n");
	}
	if (flags & WARN_RX_STOPPED) {
		warn(line, "\r\n[WARN] RX interrupted; incomplete line discarded\r\n");
	}
}

bool uart_line_rx_get(uart_line_st *line, char *dst, size_t dst_len, k_timeout_t timeout)
{
	char buffer[UART_LINE_MAX_LEN];

	if (line == NULL || dst == NULL || dst_len == 0 || !line->initialized) {
		return false;
	}
	if (k_msgq_get(line->msgq, buffer, timeout) != 0) {
		return false;
	}
	const char *terminator = memchr(buffer, '\0', sizeof(buffer));

	if (terminator == NULL || (size_t)(terminator - buffer) >= dst_len) {
		/* Reject malformed or truncated commands before exposing them. */
		dst[0] = '\0';
		return false;
	}
	size_t length = (size_t)(terminator - buffer);

	memcpy(dst, buffer, length + 1);
	return true;
}

void uart_line_tx(uart_line_st *line, const char *msg)
{
	if (line == NULL || msg == NULL || !line->initialized) {
		return;
	}
	for (; *msg != '\0'; ++msg) {
		uart_poll_out(line->uart_dev, *msg);
	}
}

void uart_line_set_default(uart_line_st *line)
{
	default_uart_line = line;
}

void uart_line_transmit(const char *msg)
{
	uart_line_tx(default_uart_line, msg);
}
