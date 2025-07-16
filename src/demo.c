/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file demo.c
 * @brief Demo functions definition
 */

#include <string.h>
#include <ctype.h>
#include <zephyr/kernel.h>
#include "demo.h"
#include "uart.h"

#define DEMO_BUF_MAX   (128u)
#define DEMO_LINE_MAX  (64u)

bool demo_calc_add(uint32_t a, uint32_t b)
{
    char msg[DEMO_LINE_MAX];
    uint32_t sum = a + b;

    (void)snprintk(msg, sizeof(msg), "Sum: %u", sum);
    uart_send_line(msg);

    return true;
}

bool demo_calc_div(uint32_t a, uint32_t b)
{
    if (0U == b)
    {
        uart_send_line("Error: Division by zero");
        return false;
    }

    uint32_t result = a / b;

    char msg[DEMO_LINE_MAX];
    (void)snprintk(msg, sizeof(msg), "Quotient: %u", result);
    uart_send_line(msg);

    return true;
}

bool demo_str_upper(const char *input)
{
    if ((NULL == input) || ('\0' == input[0]))
    {
        uart_send_line("No input");
        return false;
    }

    char buf[DEMO_BUF_MAX];
    size_t i = 0u;

    /* Convert up to DEMO_BUF_MAX-1 chars */
    for (; (i < (sizeof(buf) - 1u)) && ('\0' != input[i]); ++i)
    {
        buf[i] = (char)toupper((int)input[i]);
    }
    buf[i] = '\0';

    uart_send_line(buf);
    return true;
}

bool demo_dump_sorted_bytes(const uint8_t *data, uint16_t len)
{
    if ((NULL == data) || (0u == len))
    {
        uart_send_line("No data");
        return false;
    }

    if (len > DEMO_BUF_MAX)
    {
        uart_send_line("[WARN] Input too long, truncating to 128 bytes\r\n");
        len = DEMO_BUF_MAX;
    }

    /* Make a local copy to avoid modifying caller buffer */
    uint8_t bytes[DEMO_BUF_MAX];
    (void)memcpy(bytes, data, len);

    /* Bubble sort (O(n²)) is fine for demo-sized buffers! */
    for (size_t i = 0u; i < (len - 1u); ++i)
    {
        for (size_t j = 0u; j < (len - 1u - i); ++j)
        {
            if (bytes[j] > bytes[j + 1u])
            {
                uint8_t tmp = bytes[j];
                bytes[j] = bytes[j + 1u];
                bytes[j + 1u] = tmp;
            }
        }
    }

    /* Print sorted hex dump, wrapping every 16 bytes */
    char hex_buf[8]; /* enough for "FF " + \0 */

    for (size_t i = 0u; i < len; ++i)
    {
        (void)snprintk(hex_buf, sizeof(hex_buf), "%02X ", bytes[i]);
        uart_send_line(hex_buf);

        /* Wrap every 16 bytes for readability */
        if (0u == ((i + 1u) & 0x0Fu))
        {
            uart_send_line("\r\n");
        }
    }

    return true;
}

bool demo_str_reverse(const char *input)
{
    if ((NULL == input) || ('\0' == input[0]))
    {
        /* Empty input! */
        return false;
    }

    size_t len = strlen(input);
    if (len >= DEMO_BUF_MAX)
    {
        len = DEMO_BUF_MAX - 1u; /* Safe truncate */
    }

    char buf[DEMO_BUF_MAX];
    for (size_t i = 0u; i < len; ++i)
    {
        buf[i] = input[len - 1u - i];
    }
    buf[len] = '\0';

    uart_send_line(buf);

    return true;
}
