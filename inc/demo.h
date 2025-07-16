/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file demo.h
 * @brief Demo functions declaration
 */

#ifndef DEMO_H_
#define DEMO_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Add two 32-bit unsigned integers and print the result.
 *
 * @param a First operand
 * @param b Second operand
 * @return true on success, false on error
 */
bool demo_calc_add(uint32_t a, uint32_t b);

/**
 * @brief Divide two 32-bit unsigned integers and print the quotient.
 *
 * @param a Numerator
 * @param b Divisor (must be non-zero)
 * @return true on success, false on division by zero
 */
bool demo_calc_div(uint32_t a, uint32_t b);

/**
 * @brief Convert a string to uppercase and print the result.
 *
 * @param input Null-terminated input string
 * @return true on success, false on invalid input
 */
bool demo_str_upper(const char *input);

/**
 * @brief Reverse a string and print the result.
 *
 * @param input Null-terminated input string
 * @return true on success, false on invalid input
 */
bool demo_str_reverse(const char *input);

/**
 * @brief Sort a buffer of bytes and print them in ascending order.
 *
 * @param data Pointer to input buffer
 * @param len  Length of the buffer in bytes
 * @return true on success, false on invalid input
 */
bool demo_dump_sorted_bytes(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* DEMO_H_ */
