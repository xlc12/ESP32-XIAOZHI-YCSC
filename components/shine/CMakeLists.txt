idf_component_register(SRCS "bitstream.c"
                            "huffman.c"
                            "l3bitstream.c"
                            "l3loop.c"
                            "l3mdct.c"
                            "l3subband.c"
                            "layer3.c"
                            "reservoir.c"
                            "tables.c"
                    INCLUDE_DIRS ".")

# shine 为第三方定点/浮点 MP3 编码器，屏蔽其编译告警
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
