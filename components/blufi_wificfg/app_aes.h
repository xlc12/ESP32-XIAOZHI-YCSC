/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void test_app_aes(void);
void verify_java_aes(void);
//void new_verify_java_aes(void);
char* java_aes_encrypt_to_base64(const char *plaintext, const char *base64_key);
char* java_aes_decrypt_from_base64(const char *base64_ciphertext, const char *base64_key);
void test_encrypted_functions(void);

#ifdef __cplusplus
}
#endif