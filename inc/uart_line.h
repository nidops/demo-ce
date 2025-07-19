/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file uart_line.h
 * @brief Minimal UART line RX/TX API with deferred warnings.
 */

#ifndef UART_LINE_H_
#define UART_LINE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

/**
 * @brief Max length of a single received line (including null terminator)
 */
#define UART_LINE_MAX_LEN       (128u)

/**
 * @brief Max number of queued lines
 */
#define UART_LINE_QUEUE_LENGTH  (8u)

/**
 * @brief Per-instance UART line context (RX + TX + warnings)
 */
typedef struct
{
    const struct device *uart_dev;        /**< Bound UART device */
    struct k_msgq *msgq;                  /**< Queue for completed lines */
    char buf[UART_LINE_MAX_LEN];          /**< Assembly buffer */
    size_t pos;                           /**< Current buffer write index */
    char last_char;                       /**< Previous char for CRLF handling */
    bool overflowed;                      /**< Line overflow occurred */
    bool dropped;                         /**< Queue full occurred */
} uart_line_st;

/**
 * @brief Initialize interrupt-driven UART line handler.
 *
 * @param dev    UART device (from DEVICE_DT_GET)
 * @param uline  Pointer to UART line context
 * @param queue  Message queue to push completed lines
 *
 * @return 0 on success, negative errno on failure
 */
int uart_line_init(const struct device *dev,
                   uart_line_st *uline,
                   struct k_msgq *queue);

/**
 * @brief Retrieve one completed line from the queue.
 *
 * @param uline    UART line context
 * @param dst      Destination buffer
 * @param dst_len  Size of destination buffer
 * @param timeout  Zephyr timeout (K_NO_WAIT, K_FOREVER, etc.)
 *
 * @retval true  if a line was retrieved
 * @retval false on timeout or invalid params
 */
bool uart_line_rx_get(uart_line_st *uline,
                      char *dst,
                      size_t dst_len,
                      k_timeout_t timeout);

/**
 * @brief Check and clear deferred warning flags.
 *
 * @param uline    UART line context
 * @param warn_cb  Callback function taking (line, msg)
 */
void uart_line_rx_poll_warnings(uart_line_st *uline,
                                void (*warn_cb)(uart_line_st *uline, const char *msg));

/**
 * @brief Blocking UART TX helper.
 *
 * @param uline  UART line context
 * @param msg    Null-terminated string to send
 */
void uart_line_tx(uart_line_st *uline, const char *msg);

/**
 * @brief Set the default UART instance for uart_line_transmit()
 *
 * After calling this, uart_line_transmit() will redirect to uart_line_tx()
 * using the given default line context.
 */
void uart_line_set_default(uart_line_st *uline);

/**
 * @brief Demo-compatible TX helper
 *
 * Redirects to uart_line_tx() using the default UART line set by
 * uart_line_set_default(). Keeps legacy demo.c unchanged.
 *
 * @param msg Null-terminated string to send
 */
void uart_line_transmit(const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* UART_LINE_H_ */
