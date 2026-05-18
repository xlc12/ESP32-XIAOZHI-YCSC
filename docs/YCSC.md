# 小智编译注意事项

## 更换唤醒词

更换唤醒词后，需要执行 idf.py fullclean 清除build后再编译才会生效

因为2.2版本的代码不是直接更新sdkconfig里的配置，而是通过在编译的时候，cmake 会自动执行scripts/build_default_assets.py，最后生成 generated_assets.bin


## 驱动0.71屏幕，添加本地组件

使用本地组件，在添加本地组件的时候需要同步在cmakelists里添加，搜索 idf_component_register ，在 PRIV_REQUIRES 下添加组件名称。
