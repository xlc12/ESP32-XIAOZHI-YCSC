/*
 * 验证Java AES/ECB/NoPadding模式的C代码 - 增加特定测试用例
 */
#include "app_aes.h"
#include <stdio.h>
#include <string.h>
#include "mbedtls/aes.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

static const char *TAG = "AES_JAVA_VERIFY";

// Java使用的是AES/ECB/NoPadding模式
#define BLOCK_SIZE 16

// Java风格的0填充（不是PKCS7）
static int java_zero_pad(uint8_t *data, size_t *data_len) {
    size_t original_len = *data_len;
    size_t padded_len = ((original_len + BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;
    
    if (padded_len == original_len) {
        return 0; // 不需要填充
    }
    
    // 用0填充剩余部分
    memset(data + original_len, 0, padded_len - original_len);
    *data_len = padded_len;
    
    return 0;
}

// Java风格的去除填充（去除末尾的0）
static int java_zero_unpad(uint8_t *data, size_t *data_len) {
    if (*data_len == 0) {
        return -1;
    }
    
    // 找到最后一个非0字节
    size_t original_len = *data_len;
    while (original_len > 0 && data[original_len - 1] == 0) {
        original_len--;
    }
    
    *data_len = original_len;
    return 0;
}

// AES/ECB加密 - 匹配Java实现
int java_aes_ecb_encrypt(const uint8_t *key, const uint8_t *input, 
                        uint8_t *output, size_t *length) {
    int ret;
    mbedtls_aes_context aes;
    
    mbedtls_aes_init(&aes);
    
    // 设置加密密钥
    ret = mbedtls_aes_setkey_enc(&aes, key, 128);
    if (ret != 0) {
        ESP_LOGE(TAG, "AES setkey encrypt failed: %d", ret);
        mbedtls_aes_free(&aes);
        return ret;
    }
    
    // Java风格的0填充
    uint8_t padded_data[64];
    size_t padded_len = *length;
    memcpy(padded_data, input, *length);
    
    ret = java_zero_pad(padded_data, &padded_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "Java zero padding failed");
        mbedtls_aes_free(&aes);
        return ret;
    }
    
    // ECB加密 - 逐块加密
    for (size_t i = 0; i < padded_len; i += BLOCK_SIZE) {
        ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, 
                                   padded_data + i, output + i);
        if (ret != 0) {
            ESP_LOGE(TAG, "AES ECB encrypt failed at block %d: %d", i/BLOCK_SIZE, ret);
            mbedtls_aes_free(&aes);
            return ret;
        }
    }
    
    *length = padded_len; // 更新长度为填充后的长度
    mbedtls_aes_free(&aes);
    return ret;
}

// AES/ECB解密 - 匹配Java实现
int java_aes_ecb_decrypt(const uint8_t *key, const uint8_t *input, 
                        uint8_t *output, size_t *length) {
    int ret;
    mbedtls_aes_context aes;
    
    mbedtls_aes_init(&aes);
    
    // 设置解密密钥
    ret = mbedtls_aes_setkey_dec(&aes, key, 128);
    if (ret != 0) {
        ESP_LOGE(TAG, "AES setkey decrypt failed: %d", ret);
        mbedtls_aes_free(&aes);
        return ret;
    }
    
    // 输入长度必须是BLOCK_SIZE的倍数
    if (*length % BLOCK_SIZE != 0) {
        ESP_LOGE(TAG, "Input length must be multiple of %d", BLOCK_SIZE);
        mbedtls_aes_free(&aes);
        return -1;
    }
    
    // ECB解密 - 逐块解密
    for (size_t i = 0; i < *length; i += BLOCK_SIZE) {
        ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, 
                                   input + i, output + i);
        if (ret != 0) {
            ESP_LOGE(TAG, "AES ECB decrypt failed at block %d: %d", i/BLOCK_SIZE, ret);
            mbedtls_aes_free(&aes);
            return ret;
        }
    }
    
    // Java风格的去除填充（去除末尾的0）
    ret = java_zero_unpad(output, length);
    if (ret != 0) {
        ESP_LOGE(TAG, "Java zero unpadding failed");
    }
    
    mbedtls_aes_free(&aes);
    return ret;
}

// 打印十六进制数据
void print_hex(const char *label, const uint8_t *data, size_t len) {
    printf("%s: ", label);
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
    printf("\n");
}

// 打印字符串和十六进制
void print_string_and_hex(const char *label, const uint8_t *data, size_t len) {
    printf("%s: '", label);
    for (size_t i = 0; i < len; i++) {
        if (data[i] >= 32 && data[i] <= 126) {
            printf("%c", data[i]);
        } else {
            printf("\\x%02x", data[i]);
        }
    }
    printf("'\n");
}

// Base64编码函数
char* base64_encode(const uint8_t *input, size_t input_len) {
    size_t output_len;
    char *output = malloc((input_len + 2) / 3 * 4 + 1); // Base64输出大小
    
    if (output == NULL) {
        return NULL;
    }
    
    int ret = mbedtls_base64_encode((unsigned char *)output, (input_len + 2) / 3 * 4 + 1, 
                                   &output_len, input, input_len);
    if (ret != 0) {
        free(output);
        return NULL;
    }
    
    // 移除换行符（模拟Java代码中的replace("\n","")）
    char *dst = output;
    char *src = output;
    while (*src) {
        if (*src != '\n') {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';
    
    return output;
}

// Base64解码函数
int base64_decode(const char *input, uint8_t *output, size_t *output_len) {
    // 移除可能存在的换行符
    char clean_input[256];
    size_t clean_len = 0;
    for (size_t i = 0; input[i] != '\0' && clean_len < sizeof(clean_input) - 1; i++) {
        if (input[i] != '\n' && input[i] != ' ') {
            clean_input[clean_len++] = input[i];
        }
    }
    clean_input[clean_len] = '\0';
    
    int ret = mbedtls_base64_decode(output, *output_len, output_len, 
                                   (const unsigned char *)clean_input, clean_len);
    return ret;
}
/**

I (523) AES_JAVA_VERIFY: Starting Java AES/ECB/NoPadding Verification
Java Base64密钥: bX1vW9TqRrSGoKBUvz+CDg==
解码后的密钥: 6d7d6f5bd4ea46b486a0a054bf3f820e

=== Java AES/ECB/NoPadding 验证 ===

测试 1:
原始文本: 'Hello ESP32!' (长度: 12)
加密结果: ee73d43e179601d189b92257d6be01e3
Base64加密结果: 7nPUPheWAdGJuSJX1r4B4w==
解密结果: 'Hello ESP32!'
✓ 验证成功！

测试 2:
原始文本: 'Test123' (长度: 7)
加密结果: 224e62989cd8ff4da41234e1634b7924
Base64加密结果: Ik5imJzY/02kEjThY0t5JA==
解密结果: 'Test123'
✓ 验证成功！

测试 3:
原始文本: 'A' (长度: 1)
加密结果: e4d02b0d977c825cc5dbc23cc977c0fe
Base64加密结果: 5NArDZd8glzF28I8yXfA/g==
解密结果: 'A'
✓ 验证成功！

测试 4:
原始文本: 'Exactly16Byte!!' (长度: 15)
加密结果: 2c370a4b8d1e347d6f3b3ace7faa25d6
Base64加密结果: LDcKS40eNH1vOzrOf6ol1g==
解密结果: 'Exactly16Byte!!'
✓ 验证成功！

测试 5:
原始文本: 'More than 16 bytes test' (长度: 23)
加密结果: ce2ca503ea52632ab3290d60b40fb47ef66805db64a16aefc22e8ddf248a89a6
Base64加密结果: ziylA+pSYyqzKQ1gtA+0fvZoBdtkoWrvwi6N3ySKiaY=
解密结果: 'More than 16 bytes test'
✓ 验证成功！

=== 与Java加密结果对比测试 ===
已知文本: 'Hello Java AES!'
C语言加密结果: 60c3f55e7d96ce7329b29cc3f23d4888
C语言Base64结果: YMP1Xn2WznMpspzD8j1IiA==
请在Java端运行以下代码进行对比:
String text = "Hello Java AES!";
String encrypted = AesUtils.encryptAesByPk(text);
System.out.println("Java加密结果: " + encrypted);

=== 特定测试用例验证 ===
测试文本: 'wifi_84f7037f19c1_1982827294154412034'
期望Base64结果: Rx44oMcH7fjBdzDsFOJ1OoofJCHSb89EDWMnH90PvdjHw5E5zASc/bJ/yN3L2Plr
加密结果: 471e38a0c707edf8c17730ec14e2753a8a1f2421d26fcf440d63271fdd0fbdd8c7c39139cc049cfdb27fc8ddcbd8f96b
实际Base64结果: Rx44oMcH7fjBdzDsFOJ1OoofJCHSb89EDWMnH90PvdjHw5E5zASc/bJ/yN3L2Plr
✓ 特定测试用例验证成功！
解密结果验证: 'wifi_84f7037f19c1_1982827294154412034'
✓ 加解密循环验证成功！

 */
// 验证Java AES实现
#define PK_FIRST        0//"bX1vW9TqRrSGoKBUvz+CDg=="// Java代码中的PK
#define PK_20251105     1//"5u5PgufMY/tuZ5wl1uflbA=="// 2025-11-05//lin new pk1105_bA__

//#define JAVA_PK PK_FIRST
#define JAVA_PK PK_20251105

#if JAVA_PK == PK_FIRST
    #define   C_STR_PK "bX1vW9TqRrSGoKBUvz+CDg=="// Java代码中的PK
#elif JAVA_PK == PK_20251105
    #define   C_STR_PK "5u5PgufMY/tuZ5wl1uflbA=="// 2025-11-05//lin new pk1105_bA__
#else
    #error "Undefined JAVA_PK value"

#endif


void verify_java_aes(void) {
    ESP_LOGI(TAG, "Starting Java AES/ECB/NoPadding Verification");
    
    // 使用Java代码中的默认密钥
    //;const char *base64_key = "bX1vW9TqRrSGoKBUvz+CDg=="; // Java代码中的PK
    //const char *base64_key =    "5u5PgufMY/tuZ5wl1uflbA==";// 2025-11-05//lin new pk1105_bA__
    const char *base64_key =C_STR_PK;
    
    uint8_t key[16];
    size_t key_len = sizeof(key);
    
    // 解码Base64密钥
    if (base64_decode(base64_key, key, &key_len) != 0 || key_len != 16) {
        ESP_LOGE(TAG, "Failed to decode base64 key");
        return;
    }
    
    printf("Java Base64密钥: %s\n", base64_key);
    print_hex("解码后的密钥", key, key_len);
    
    // 测试数据
    const char *test_texts[] = {
        "Hello ESP32!",
        "Test123",
        "A",  // 单字节
        "Exactly16Byte!!", // 正好16字节
        "More than 16 bytes test", // 超过16字节
        NULL
    };
    
    printf("\n=== Java AES/ECB/NoPadding 验证 ===\n");
    
    for (int i = 0; test_texts[i] != NULL; i++) {
        const char *original_text = test_texts[i];
        size_t original_len = strlen(original_text);
        
        printf("\n测试 %d:\n", i + 1);
        printf("原始文本: '%s' (长度: %d)\n", original_text, original_len);
        
        // 分配缓冲区
        uint8_t encrypted[64];
        uint8_t decrypted[64];
        size_t encrypted_len = sizeof(encrypted);
        size_t decrypted_len;
        
        // 加密
        encrypted_len = original_len;
        int ret = java_aes_ecb_encrypt(key, (uint8_t *)original_text, encrypted, &encrypted_len);
        
        if (ret == 0) {
            print_hex("加密结果", encrypted, encrypted_len);
            
            // Base64编码加密结果
            char *base64_encrypted = base64_encode(encrypted, encrypted_len);
            if (base64_encrypted) {
                printf("Base64加密结果: %s\n", base64_encrypted);
                free(base64_encrypted);
            }
            
            // 解密
            decrypted_len = encrypted_len;
            ret = java_aes_ecb_decrypt(key, encrypted, decrypted, &decrypted_len);
            
            if (ret == 0) {
                print_string_and_hex("解密结果", decrypted, decrypted_len);
                
                // 验证
                if (decrypted_len == original_len && 
                    memcmp(original_text, decrypted, original_len) == 0) {
                    printf("✓ 验证成功！\n");
                } else {
                    printf("✗ 验证失败！\n");
                    printf("  期望长度: %d, 实际长度: %d\n", original_len, decrypted_len);
                }
            } else {
                printf("✗ 解密失败\n");
            }
        } else {
            printf("✗ 加密失败\n");
        }
    }
    
    // 特别测试：与Java加密结果对比
    printf("\n=== 与Java加密结果对比测试 ===\n");
    
    // 选择一个已知文本，在Java端加密后得到的结果
    const char *known_text = "Hello Java AES!";
    size_t known_len = strlen(known_text);
    
    printf("已知文本: '%s'\n", known_text);
    
    // 在C端加密
    uint8_t c_encrypted[64];
    size_t c_encrypted_len = known_len;
    java_aes_ecb_encrypt(key, (uint8_t *)known_text, c_encrypted, &c_encrypted_len);
    
    print_hex("C语言加密结果", c_encrypted, c_encrypted_len);
    
    // Base64编码
    char *c_base64 = base64_encode(c_encrypted, c_encrypted_len);
    if (c_base64) {
        printf("C语言Base64结果: %s\n", c_base64);
        free(c_base64);
    }
    
    // 这里您需要将相同的文本在Java端加密，然后比较结果
    printf("请在Java端运行以下代码进行对比:\n");
    printf("String text = \"%s\";\n", known_text);
    printf("String encrypted = AesUtils.encryptAesByPk(text);\n");
    printf("System.out.println(\"Java加密结果: \" + encrypted);\n");
    
    // 新增特定测试用例
    printf("\n=== 特定测试用例验证 ===\n");
    const char *specific_text = "wifi_84f7037f19c1_1982827294154412034";
#if JAVA_PK == PK_FIRST//old
    const char *expected_base64 = "Rx44oMcH7fjBdzDsFOJ1OoofJCHSb89EDWMnH90PvdjHw5E5zASc/bJ/yN3L2Plr";
    printf("PK_FIRST:%d", PK_FIRST);
#elif JAVA_PK == PK_20251105
    const char *expected_base64 = "TA2sfFcSaQT9xCFzYKjOtxGg4Z0Wc9SenduW1ZJVyYDFyuCGM6+EU8e+yQPElGCm";
    printf("PK_20251105:%d", PK_20251105);
#else 
    #error "Unknown JAVA_PK value"
#endif

    printf("测试文本: '%s'\n", specific_text);
    printf("期望Base64结果: %s\n", expected_base64);
    
    size_t specific_len = strlen(specific_text);
    uint8_t specific_encrypted[128];
    size_t specific_encrypted_len = specific_len;
    
    // 加密
    int ret = java_aes_ecb_encrypt(key, (uint8_t *)specific_text, specific_encrypted, &specific_encrypted_len);
    if (ret == 0) {
        print_hex("加密结果", specific_encrypted, specific_encrypted_len);
        
        // Base64编码
        char *actual_base64 = base64_encode(specific_encrypted, specific_encrypted_len);
        if (actual_base64) {
            printf("实际Base64结果: %s\n", actual_base64);
            
            // 比较结果
            if (strcmp(actual_base64, expected_base64) == 0) {
                printf("✓ 特定测试用例验证成功！\n");
            } else {
                printf("✗ 特定测试用例验证失败！\n");
                printf("  期望: %s\n", expected_base64);
                printf("  实际: %s\n", actual_base64);
                
                // 尝试解码期望的Base64并解密
                printf("\n尝试解码期望的Base64结果:\n");
                uint8_t expected_decoded[128];
                size_t expected_decoded_len = sizeof(expected_decoded);
                
                if (base64_decode(expected_base64, expected_decoded, &expected_decoded_len) == 0) {
                    print_hex("期望Base64解码结果", expected_decoded, expected_decoded_len);
                    
                    // 解密期望的结果
                    uint8_t decrypted_expected[128];
                    size_t decrypted_expected_len = expected_decoded_len;
                    ret = java_aes_ecb_decrypt(key, expected_decoded, decrypted_expected, &decrypted_expected_len);
                    
                    if (ret == 0) {
                        print_string_and_hex("期望结果解密后", decrypted_expected, decrypted_expected_len);
                    } else {
                        printf("解密期望结果失败\n");
                    }
                } else {
                    printf("解码期望Base64失败\n");
                }
            }
            
            free(actual_base64);
        }
        
        // 解密我们加密的结果进行验证
        uint8_t specific_decrypted[128];
        size_t specific_decrypted_len = specific_encrypted_len;
        ret = java_aes_ecb_decrypt(key, specific_encrypted, specific_decrypted, &specific_decrypted_len);
        
        if (ret == 0) {
            print_string_and_hex("解密结果验证", specific_decrypted, specific_decrypted_len);
            
            if (specific_decrypted_len == specific_len && 
                memcmp(specific_text, specific_decrypted, specific_len) == 0) {
                printf("✓ 加解密循环验证成功！\n");
            } else {
                printf("✗ 加解密循环验证失败！\n");
            }
        }
    } else {
        printf("✗ 特定测试用例加密失败\n");
    }//特定测试用例验证
    
    ESP_LOGI(TAG, "Java AES Verification Completed");
    
}



// 验证封装函数
void test_encrypted_functions(void) {
    ESP_LOGI(TAG, "Testing Encrypted Functions");
    
    // 使用Java代码中的默认密钥
    //const char *base64_key = "bX1vW9TqRrSGoKBUvz+CDg==";
    //const char *base64_key =    "5u5PgufMY/tuZ5wl1uflbA==";// 2025-11-05//lin new pk1105_bA__
    const char *base64_key =C_STR_PK;//"5u5PgufMY/tuZ5wl1uflbA=="
    
    printf("测试密钥: %s\n", base64_key);
#if 0    
    // 测试用例1：已知的测试
    const char *test1_plaintext = "wifi_84f7037f19c1_1982827294154412034";
    const char *expected_base64 = "Rx44oMcH7fjBdzDsFOJ1OoofJCHSb89EDWMnH90PvdjHw5E5zASc/bJ/yN3L2Plr";
    
    printf("\n=== 测试用例1 ===\n");
    printf("明文: %s\n", test1_plaintext);
    printf("期望Base64: %s\n", expected_base64);
    
    // 加密
    char *actual_base64 = java_aes_encrypt_to_base64(test1_plaintext, base64_key);
    if (actual_base64) {
        printf("实际Base64: %s\n", actual_base64);
        
        if (strcmp(actual_base64, expected_base64) == 0) {
            printf("✓ 加密结果匹配！\n");
        } else {
            printf("✗ 加密结果不匹配！\n");
        }
        
        // 解密验证
        char *decrypted = java_aes_decrypt_from_base64(actual_base64, base64_key);
        if (decrypted) {
            printf("解密验证: %s\n", decrypted);
            
            if (strcmp(decrypted, test1_plaintext) == 0) {
                printf("✓ 解密验证成功！\n");
            } else {
                printf("✗ 解密验证失败！\n");
            }
            
            free(decrypted);
        }
        
        free(actual_base64);
    }
#endif    
    // 测试用例2：您提供的新测试用例
    const char *test2_plaintext = "wifi_84f7037f19c1_1982827294154412034";
    const char *expected_base64_2 = "TA2sfFcSaQT9xCFzYKjOtxGg4Z0Wc9SenduW1ZJVyYDFyuCGM6+EU8e+yQPElGCm";
    
    printf("\n=== 测试用例2 ===\n");
    printf("明文: %s\n", test2_plaintext);
    printf("期望Base64: %s\n", expected_base64_2);
    
    // 加密
    char *actual_base64_2 = java_aes_encrypt_to_base64(test2_plaintext, base64_key);
    if (actual_base64_2) {
        printf("实际Base64: %s\n", actual_base64_2);
        
        if (strcmp(actual_base64_2, expected_base64_2) == 0) {
            printf("✓ 加密结果匹配！\n");
        } else {
            printf("✗ 加密结果不匹配！\n");
        }
        
        // 解密验证
        char *decrypted_2 = java_aes_decrypt_from_base64(actual_base64_2, base64_key);
        if (decrypted_2) {
            printf("解密验证: %s\n", decrypted_2);
            
            if (strcmp(decrypted_2, test2_plaintext) == 0) {
                printf("✓ 解密验证成功！\n");
            } else {
                printf("✗ 解密验证失败！\n");
            }
            
            free(decrypted_2);
        }
        
        free(actual_base64_2);
    }
}
// 封装函数：明文 + Base64密钥 -> Base64密文
char* java_aes_encrypt_to_base64(const char *plaintext, const char *base64_key) {
    if (plaintext == NULL || base64_key == NULL) {
        return NULL;
    }
    
    // 解码Base64密钥
    uint8_t key[16];
    size_t key_len = sizeof(key);
    
    if (base64_decode(base64_key, key, &key_len) != 0 || key_len != 16) {
        ESP_LOGE(TAG, "Failed to decode base64 key");
        return NULL;
    }
    
    // 准备加密
    size_t plaintext_len = strlen(plaintext);
    uint8_t encrypted[128]; // 足够大的缓冲区
    size_t encrypted_len = plaintext_len;
    
    // 加密
    int ret = java_aes_ecb_encrypt(key, (uint8_t *)plaintext, encrypted, &encrypted_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "AES encryption failed: %d", ret);
        return NULL;
    }
    
    // Base64编码
    char *base64_result = base64_encode(encrypted, encrypted_len);
    return base64_result;
}

// 封装函数：Base64密文 + Base64密钥 -> 明文
char* java_aes_decrypt_from_base64(const char *base64_ciphertext, const char *base64_key) {
    if (base64_ciphertext == NULL || base64_key == NULL) {
        return NULL;
    }
    
    // 解码Base64密钥
    uint8_t key[16];
    size_t key_len = sizeof(key);
    
    if (base64_decode(base64_key, key, &key_len) != 0 || key_len != 16) {
        ESP_LOGE(TAG, "Failed to decode base64 key");
        return NULL;
    }
    
    // 解码Base64密文
    uint8_t ciphertext[128];
    size_t ciphertext_len = sizeof(ciphertext);
    
    if (base64_decode(base64_ciphertext, ciphertext, &ciphertext_len) != 0) {
        ESP_LOGE(TAG, "Failed to decode base64 ciphertext");
        return NULL;
    }
    
    // 准备解密
    uint8_t decrypted[128];
    size_t decrypted_len = ciphertext_len;
    
    // 解密
    int ret = java_aes_ecb_decrypt(key, ciphertext, decrypted, &decrypted_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "AES decryption failed: %d", ret);
        return NULL;
    }
    
    // 复制结果到新字符串
    char *result = malloc(decrypted_len + 1);
    if (result == NULL) {
        return NULL;
    }
    
    memcpy(result, decrypted, decrypted_len);
    result[decrypted_len] = '\0';
    
    return result;
}
// 在app_main中调用
// void app_main(void) {
//     // 初始化...
//     esp_err_t ret = nvs_flash_init();
//     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         ret = nvs_flash_init();
//     }
//     ESP_ERROR_CHECK(ret);
    
//     // 验证Java AES实现
//     verify_java_aes();
// }
// 验证Java AES实现
#if 0
void new_verify_java_aes(void) {
    ESP_LOGI(TAG, "Starting Java AES/ECB/NoPadding Verification");
    
    // 使用Java代码中的默认密钥
    const char *base64_key = C_STR_PK;//"bX1vW9TqRrSGoKBUvz+CDg=="; // Java代码中的PK
    uint8_t key[16];
    size_t key_len = 0;
    
    // 解码Base64密钥
    if (base64_decode(base64_key, key, &key_len) != 0 || key_len != 16) {
        ESP_LOGE(TAG, "Failed to decode base64 key");
        return;
    }
    
    printf("Java Base64密钥: %s\n", base64_key);
    print_hex("解码后的密钥", key, key_len);
    
    // 测试数据
    const char *test_texts[] = {
        "Hello ESP32!",
        "Test123",
        "A",  // 单字节
        "Exactly16Byte!!", // 正好16字节
        "More than 16 bytes test", // 超过16字节
        NULL
    };
    
    printf("\n=== Java AES/ECB/NoPadding 验证 ===\n");
    
    for (int i = 0; test_texts[i] != NULL; i++) {
        const char *original_text = test_texts[i];
        size_t original_len = strlen(original_text);
        
        printf("\n测试 %d:\n", i + 1);
        printf("原始文本: '%s' (长度: %d)\n", original_text, original_len);
        
        // 分配缓冲区
        uint8_t encrypted[64];
        uint8_t decrypted[64];
        size_t encrypted_len = sizeof(encrypted);
        size_t decrypted_len;
        
        // 加密
        encrypted_len = original_len;
        int ret = java_aes_ecb_encrypt(key, (uint8_t *)original_text, encrypted, &encrypted_len);
        
        if (ret == 0) {
            print_hex("加密结果", encrypted, encrypted_len);
            
            // 解密
            decrypted_len = encrypted_len;
            ret = java_aes_ecb_decrypt(key, encrypted, decrypted, &decrypted_len);
            
            if (ret == 0) {
                print_string_and_hex("解密结果", decrypted, decrypted_len);
                
                // 验证
                if (decrypted_len == original_len && 
                    memcmp(original_text, decrypted, original_len) == 0) {
                    printf("✓ 验证成功！\n");
                } else {
                    printf("✗ 验证失败！\n");
                    printf("  期望长度: %d, 实际长度: %d\n", original_len, decrypted_len);
                }
            } else {
                printf("✗ 解密失败\n");
            }
        } else {
            printf("✗ 加密失败\n");
        }
    }
    
    // 特别测试：与Java加密结果对比
    printf("\n=== 与Java加密结果对比测试 ===\n");
    
    // 选择一个已知文本，在Java端加密后得到的结果
    const char *known_text = "Hello Java AES!";
    size_t known_len = strlen(known_text);
    
    printf("已知文本: '%s'\n", known_text);
    
    // 在C端加密
    uint8_t c_encrypted[64];
    size_t c_encrypted_len = known_len;
    java_aes_ecb_encrypt(key, (uint8_t *)known_text, c_encrypted, &c_encrypted_len);
    
    print_hex("C语言加密结果", c_encrypted, c_encrypted_len);
    
    // 这里您需要将相同的文本在Java端加密，然后比较结果
    printf("请在Java端运行以下代码进行对比:\n");
    printf("String text = \"%s\";\n", known_text);
    printf("String encrypted = AesUtils.encryptAesByPk(text);\n");
    printf("System.out.println(\"Java加密结果: \" + encrypted);\n");
    
    ESP_LOGI(TAG, "Java AES Verification Completed");
}
#endif