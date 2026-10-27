#ifndef SPEEX_CONFIG_H
#define SPEEX_CONFIG_H

/* ESP32 侧只使用 Speex 解码器。
 * CI1302 侧：宽带（WB）16 kHz，20 ms 一帧 = 320 个采样点（640 字节 PCM），
 * quality=5，码流约 42 B，加 1 字节长度头共 43 B 上行
 * （对应 CI1302 user_config.h 中 COMPRESS_NEED_PCM_LEN=320、
 *   COMPRESS_UPLOAD_QUEUE_TIEM_SIZE=43）。
 * 本组件采用定点解码以降低 CPU/内存开销；Speex 码流在浮点与定点实现间格式互通，
 * CI1302 侧为浮点编码器，可直接被此定点解码器解码。 */
#define FIXED_POINT 1
#define DISABLE_ENCODER 1
#define DISABLE_VBR 1

#ifndef EXPORT
#define EXPORT
#endif

#ifndef restrict
#define restrict __restrict
#endif

#endif /* SPEEX_CONFIG_H */
