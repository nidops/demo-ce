/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file uart.h
 * @brief Simple UART TX + line RX interface (defined in main.c)
 */

#ifndef UART_H_
#define UART_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Max length of a single received line (including null terminator)
 */
#define UART_RX_BUF_SIZE        (128u)

/**
 * @brief Max number of queued lines
 */
#define UART_LINE_QUEUE_LENGTH  (8u)

/**
 * @brief Blocking UART TX function to send a line
 *
 * @param line Null-terminated string to send
 */
void uart_send_line(const char *line);

#ifdef __cplusplus
}
#endif

#endif /* UART_H_ */
