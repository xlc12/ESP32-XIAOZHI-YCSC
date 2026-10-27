# Speex 解码器组件（从官方 speex-1.2.0 移植，仅使用定点解码路径）
# 与 CI1302 上行 Speex 宽带帧（16kHz / 20ms / 320 点）保持一致。
file(GLOB SPEEX_SRCS "libspeex/*.c")

# 只保留定点 CELP 解码所需源码：FFT 仅服务于 vorbis_psy（已移除），
# 排除可避免浮点 FFT 源码在定点配置下的编译差异。
list(REMOVE_ITEM SPEEX_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/libspeex/smallft.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/libspeex/kiss_fft.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/libspeex/kiss_fftr.c"
)

idf_component_register(
    SRCS ${SPEEX_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "libspeex"
)

# HAVE_CONFIG_H 让 libspeex/*.c 引入 libspeex/config.h（FIXED_POINT / DISABLE_ENCODER）
target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_CONFIG_H)

# DISABLE_ENCODER 后，源码里仅供编码器使用的码表会变成未引用常量。
# 本组件是移植的第三方库，不为其逐个加条件编译，直接关掉该告警
# （ESP-IDF 默认 -Werror=all，否则会中断构建）。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable)
