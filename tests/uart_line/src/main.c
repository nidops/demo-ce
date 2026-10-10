/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/ztest.h>

#include "uart_line.h"

struct fake_uart {
	uart_callback_t callback;
	void *user_data;
	uint8_t *active;
	uint8_t *next;
	size_t offset;
	int enable_calls;
	int enable_error;
	int enable_callback_error;
	int response_error;
	int callback_error;
	int disable_error;
	bool delayed_disable;
	bool suppress_disable;
	bool detached_while_owned;
	struct k_work_delayable stop_work;
};

static struct fake_uart uart;
static uart_line_st line;
static struct k_msgq queue;
static char messages[UART_LINE_QUEUE_LENGTH][UART_LINE_MAX_LEN];
static char warnings[512];
static const struct device *device;

static void emit(struct uart_event event)
{
	zassert_not_null(uart.callback, "callback detached before driver finished");
	uart.callback(device, &event, uart.user_data);
}

static void finish_disable(void)
{
	if (uart.active != NULL) {
		uint8_t *buffer = uart.active;

		uart.active = NULL;
		emit((struct uart_event){.type = UART_RX_BUF_RELEASED, .data.rx_buf.buf = buffer});
	}
	if (uart.next != NULL) {
		uint8_t *buffer = uart.next;

		uart.next = NULL;
		emit((struct uart_event){.type = UART_RX_BUF_RELEASED, .data.rx_buf.buf = buffer});
	}
	emit((struct uart_event){.type = UART_RX_DISABLED});
}

static void delayed_stop(struct k_work *work)
{
	ARG_UNUSED(work);
	finish_disable();
}

static int callback_set(const struct device *dev, uart_callback_t callback, void *user_data)
{
	ARG_UNUSED(dev);
	if (uart.callback_error != 0) {
		return uart.callback_error;
	}
	if (callback == NULL && (uart.active != NULL || uart.next != NULL)) {
		uart.detached_while_owned = true;
	}
	uart.callback = callback;
	uart.user_data = user_data;
	return 0;
}

static int rx_enable(const struct device *dev, uint8_t *buffer, size_t size, int32_t timeout)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(timeout);
	zassert_equal(size, UART_LINE_DMA_CHUNK);
	zassert_equal((uintptr_t)buffer % sizeof(void *), 0, "DMA buffer alignment");
	uart.enable_calls++;
	if (uart.enable_error != 0) {
		uart.callback_error = uart.enable_callback_error;
		return uart.enable_error;
	}
	zassert_is_null(uart.active, "RX restarted before old buffers released");
	zassert_is_null(uart.next);
	uart.active = buffer;
	uart.offset = 0;
	emit((struct uart_event){.type = UART_RX_BUF_REQUEST});
	return 0;
}

static int rx_response(const struct device *dev, uint8_t *buffer, size_t size)
{
	ARG_UNUSED(dev);
	zassert_equal(size, UART_LINE_DMA_CHUNK);
	if (uart.response_error != 0) {
		return uart.response_error;
	}
	zassert_is_null(uart.next);
	uart.next = buffer;
	return 0;
}

static int rx_disable(const struct device *dev)
{
	ARG_UNUSED(dev);
	if (uart.disable_error != 0) {
		return uart.disable_error;
	}
	if (uart.suppress_disable) {
		return 0;
	}
	if (uart.delayed_disable) {
		k_work_reschedule(&uart.stop_work, K_MSEC(10));
	} else {
		finish_disable();
	}
	return 0;
}

static void poll_out(const struct device *dev, unsigned char byte)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(byte);
}

static DEVICE_API(uart, fake_api) = {
	.callback_set = callback_set,
	.rx_enable = rx_enable,
	.rx_buf_rsp = rx_response,
	.rx_disable = rx_disable,
	.poll_out = poll_out,
};

DEVICE_DEFINE(uart_line_fake, "uart-line-fake", NULL, NULL, NULL, NULL, POST_KERNEL,
	      CONFIG_SERIAL_INIT_PRIORITY, &fake_api);

static void feed(const char *bytes)
{
	size_t remaining = strlen(bytes);

	while (remaining != 0) {
		zassert_not_null(uart.active);
		size_t length = MIN(remaining, UART_LINE_DMA_CHUNK - uart.offset);

		memcpy(uart.active + uart.offset, bytes, length);
		emit((struct uart_event){
			.type = UART_RX_RDY,
			.data.rx = {.buf = uart.active, .offset = uart.offset, .len = length}});
		uart.offset += length;
		bytes += length;
		remaining -= length;
		if (uart.offset == UART_LINE_DMA_CHUNK) {
			uint8_t *released = uart.active;

			uart.active = uart.next;
			uart.next = NULL;
			uart.offset = 0;
			emit((struct uart_event){.type = UART_RX_BUF_RELEASED,
						 .data.rx_buf.buf = released});
			if (uart.active != NULL) {
				emit((struct uart_event){.type = UART_RX_BUF_REQUEST});
			} else {
				emit((struct uart_event){.type = UART_RX_DISABLED});
			}
		}
	}
}

static void expect_line(const char *expected)
{
	char received[UART_LINE_MAX_LEN];

	zassert_true(uart_line_rx_get(&line, received, sizeof(received), K_NO_WAIT));
	zassert_str_equal(received, expected);
}

static void capture_warning(uart_line_st *context, const char *message)
{
	ARG_UNUSED(context);
	strcat(warnings, message);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(&uart, 0, sizeof(uart));
	memset(&line, 0, sizeof(line));
	memset(warnings, 0, sizeof(warnings));
	k_msgq_init(&queue, (char *)messages, UART_LINE_MAX_LEN, UART_LINE_QUEUE_LENGTH);
	k_work_init_delayable(&uart.stop_work, delayed_stop);
	device = DEVICE_GET(uart_line_fake);
}

static void after(void *fixture)
{
	ARG_UNUSED(fixture);
	uart.disable_error = 0;
	uart.callback_error = 0;
	uart.suppress_disable = false;
	zassert_ok(uart_line_deinit(&line));
	zassert_false(uart.detached_while_owned);
	zassert_is_null(uart.active);
	zassert_is_null(uart.next);
}

ZTEST(uart_line, test_partial_idle_ranges_and_crlf)
{
	zassert_ok(uart_line_init(device, &line, &queue));
	feed("add ");
	char received[UART_LINE_MAX_LEN];

	zassert_false(uart_line_rx_get(&line, received, sizeof(received), K_NO_WAIT));
	feed("10 20\r");
	expect_line("add 10 20");
	feed("\n\r\nupper hello\n");
	expect_line("upper hello");
	zassert_false(uart_line_rx_get(&line, received, sizeof(received), K_NO_WAIT));
}

ZTEST(uart_line, test_multiple_dma_rollovers_recycle_exactly_two_buffers)
{
	zassert_ok(uart_line_init(device, &line, &queue));
	for (int i = 0; i < 100; ++i) {
		feed("add 10 20\r\n");
		expect_line("add 10 20");
		zassert_equal(k_mem_slab_num_free_get(&line.rx_pool), 0);
	}
	zassert_equal(uart.enable_calls, 1, "normal turnover must not restart RX");
}

ZTEST(uart_line, test_overflow_discards_valid_suffix_across_dma_buffers)
{
	zassert_ok(uart_line_init(device, &line, &queue));
	char oversized[UART_LINE_DMA_CHUNK + 1];

	memset(oversized, 'x', sizeof(oversized) - 1);
	oversized[sizeof(oversized) - 1] = '\0';
	feed(oversized);
	feed("add 77 88\r\nadd 2 3\r\n");
	expect_line("");
	expect_line("add 2 3");
	uart_line_rx_poll_warnings(&line, capture_warning);
	zassert_not_null(strstr(warnings, "Line too long"));
}

ZTEST(uart_line, test_stop_waits_for_releases_and_discards_incomplete_line)
{
	zassert_ok(uart_line_init(device, &line, &queue));
	feed("add 10 ");
	emit((struct uart_event){.type = UART_RX_STOPPED});
	zassert_equal(uart.enable_calls, 1);
	zassert_equal(k_mem_slab_num_free_get(&line.rx_pool), 0);
	finish_disable();
	k_sleep(K_MSEC(20));
	zassert_equal(uart.enable_calls, 2);
	expect_line("");
	feed("20\r\nupper safe\r\n");
	expect_line("");
	expect_line("upper safe");
}

ZTEST(uart_line, test_restart_failure_is_visible_and_returns_unaccepted_buffer)
{
	zassert_ok(uart_line_init(device, &line, &queue));
	uart.enable_error = -EIO;
	finish_disable();
	k_sleep(K_MSEC(20));
	zassert_equal(uart_line_rx_error(&line), -EIO);
	zassert_equal(k_mem_slab_num_free_get(&line.rx_pool), UART_LINE_DMA_BUFFER_COUNT);
}

ZTEST(uart_line, test_failed_buffer_response_returns_buffer_to_slab)
{
	uart.response_error = -EIO;
	zassert_ok(uart_line_init(device, &line, &queue));
	zassert_equal(k_mem_slab_num_free_get(&line.rx_pool), 1);
	uart.response_error = 0;
	emit((struct uart_event){.type = UART_RX_BUF_REQUEST});
	zassert_equal(k_mem_slab_num_free_get(&line.rx_pool), 0);
}

ZTEST(uart_line, test_disable_waits_for_asynchronous_release)
{
	zassert_ok(uart_line_init(device, &line, &queue));
	uart.delayed_disable = true;
	zassert_ok(uart_line_deinit(&line));
	zassert_is_null(uart.callback);
	zassert_equal(k_mem_slab_num_free_get(&line.rx_pool), UART_LINE_DMA_BUFFER_COUNT);
	k_sleep(K_MSEC(20));
	zassert_equal(uart.enable_calls, 1, "teardown must not restart RX");
}

ZTEST(uart_line, test_failed_disable_keeps_driver_owned_buffers_and_callback)
{
	zassert_ok(uart_line_init(device, &line, &queue));
	uart.disable_error = -EIO;
	zassert_equal(uart_line_deinit(&line), -EIO);
	zassert_not_null(uart.callback);
	zassert_equal(k_mem_slab_num_free_get(&line.rx_pool), 0);
	uart.disable_error = 0;
	zassert_ok(uart_line_deinit(&line));
}

ZTEST(uart_line, test_repeated_init_deinit_returns_all_resources)
{
	for (int i = 0; i < 25; ++i) {
		zassert_ok(uart_line_init(device, &line, &queue));
		zassert_equal(uart_line_init(device, &line, &queue), -EALREADY);
		zassert_ok(uart_line_deinit(&line));
		zassert_ok(uart_line_deinit(&line));
		zassert_equal(k_mem_slab_num_free_get(&line.rx_pool), UART_LINE_DMA_BUFFER_COUNT);
	}
}

ZTEST(uart_line, test_disable_timeout_retains_callback_and_allows_retry)
{
	zassert_ok(uart_line_init(device, &line, &queue));
	uart.suppress_disable = true;
	zassert_equal(uart_line_deinit(&line), -ETIMEDOUT);
	zassert_not_null(uart.callback);
	zassert_equal(k_mem_slab_num_free_get(&line.rx_pool), 0);
	finish_disable();
	zassert_ok(uart_line_deinit(&line));
	zassert_equal(uart.enable_calls, 1);
}

ZTEST(uart_line, test_callback_detachment_failure_can_be_retried)
{
	zassert_ok(uart_line_init(device, &line, &queue));
	uart.callback_error = -EIO;
	zassert_equal(uart_line_deinit(&line), -EIO);
	zassert_true(line.initialized);
	zassert_not_null(uart.callback);
	zassert_equal(k_mem_slab_num_free_get(&line.rx_pool), UART_LINE_DMA_BUFFER_COUNT);
	uart.callback_error = 0;
	zassert_ok(uart_line_deinit(&line));
}

ZTEST(uart_line, test_failed_init_retains_context_when_callback_cleanup_fails)
{
	uart.enable_error = -EIO;
	uart.enable_callback_error = -EIO;
	zassert_equal(uart_line_init(device, &line, &queue), -EIO);
	zassert_true(line.initialized);
	zassert_not_null(uart.callback);
	zassert_equal(k_mem_slab_num_free_get(&line.rx_pool), UART_LINE_DMA_BUFFER_COUNT);
	zassert_equal(uart_line_deinit(&line), -EIO);
	uart.callback_error = 0;
	zassert_ok(uart_line_deinit(&line));
}

ZTEST(uart_line, test_init_failures_do_not_leak)
{
	uart.callback_error = -ENOSYS;
	zassert_equal(uart_line_init(device, &line, &queue), -ENOSYS);
	zassert_equal(uart.enable_calls, 0);
	uart.callback_error = 0;
	uart.enable_error = -EIO;
	zassert_equal(uart_line_init(device, &line, &queue), -EIO);
	zassert_equal(k_mem_slab_num_free_get(&line.rx_pool), UART_LINE_DMA_BUFFER_COUNT);
	zassert_is_null(uart.callback);
}

ZTEST(uart_line, test_queue_overflow_drops_whole_lines_and_recovers)
{
	zassert_ok(uart_line_init(device, &line, &queue));
	for (int i = 0; i < UART_LINE_QUEUE_LENGTH + 1; ++i) {
		feed("add 1 2\n");
	}
	for (int i = 0; i < UART_LINE_QUEUE_LENGTH; ++i) {
		expect_line("add 1 2");
	}
	uart_line_rx_poll_warnings(&line, capture_warning);
	zassert_not_null(strstr(warnings, "queue full"));
	feed("add 2 3\n");
	expect_line("add 2 3");
}

ZTEST(uart_line, test_small_destination_never_returns_truncated_command)
{
	zassert_ok(uart_line_init(device, &line, &queue));
	feed("add 100 200\n");
	char small[5];

	zassert_false(uart_line_rx_get(&line, small, sizeof(small), K_NO_WAIT));
	zassert_equal(small[0], '\0');
}

ZTEST(uart_line, test_unterminated_queue_message_is_rejected)
{
	zassert_ok(uart_line_init(device, &line, &queue));
	char malformed[UART_LINE_MAX_LEN];
	char received[UART_LINE_MAX_LEN];

	memset(malformed, 'x', sizeof(malformed));
	zassert_ok(k_msgq_put(&queue, malformed, K_NO_WAIT));
	zassert_false(uart_line_rx_get(&line, received, sizeof(received), K_NO_WAIT));
	zassert_equal(received[0], '\0');
	feed("add 2 3\n");
	expect_line("add 2 3");
}

ZTEST_SUITE(uart_line, NULL, NULL, before, after, NULL);
