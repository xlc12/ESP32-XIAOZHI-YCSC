bootloader_start.c:27
I (34) octal_psram: vendor id    : 0x0d (AP)
I (34) octal_psram: dev id       : 0x02 (generation 3)
I (34) octal_psram: density      : 0x03 (64 Mbit)
I (36) octal_psram: good-die     : 0x01 (Pass)
I (40) octal_psram: Latency      : 0x01 (Fixed)
I (44) octal_psram: VCC          : 0x01 (3V)
I (48) octal_psram: SRF          : 0x01 (Fast Refresh)
I (53) octal_psram: BurstType    : 0x01 (Hybrid Wrap)
I (58) octal_psram: BurstLen     : 0x01 (32 Byte)
I (62) octal_psram: Readlatency  : 0x02 (10 cycles@Fixed)
I (67) octal_psram: DriveStrength: 0x00 (1/1)
I (71) MSPI Timing: Enter psram timing tuning
I (76) esp_psram: Found 8MB PSRAM device
I (79) esp_psram: Speed: 80MHz
I (82) cpu_start: Multicore app
I (96) cpu_start: GPIO 44 and 43 are used as console UART I/O pins
I (97) cpu_start: Pro cpu start user code
I (97) cpu_start: cpu freq: 240000000 Hz
I (98) app_init: Application information:
I (102) app_init: Project name:     xiaozhi
I (106) app_init: App version:      1.9.4
I (110) app_init: Compile time:     Apr 24 2026 19:06:44
I (115) app_init: ELF file SHA256:  15b8af0ce...
I (119) app_init: ESP-IDF:          v5.5.3
I (123) efuse_init: Min chip rev:     v0.0
I (127) efuse_init: Max chip rev:     v0.99
I (131) efuse_init: Chip rev:         v0.2
I (134) heap_init: Initializing. RAM available for dynamic allocation:
I (141) heap_init: At 3FCAC150 len 0003D5C0 (245 KiB): RAM
I (146) heap_init: At 3FCE9710 len 00005724 (21 KiB): RAM
I (151) heap_init: At 3FCF0000 len 00008000 (32 KiB): DRAM
I (156) heap_init: At 600FE000 len 00001FD8 (7 KiB): RTCRAM
I (162) esp_psram: Adding pool of 8192K of PSRAM memory to heap allocator
I (169) spi_flash: detected chip: generic
I (172) spi_flash: flash io: qio
I (175) sleep_gpio: Configure to isolate all GPIO pins in sleep state
I (181) sleep_gpio: Enable automatic switching of GPIO sleep configuration
I (196) main_task: Started on CPU0
I (206) esp_psram: Reserving pool of 64K of internal memory for DMA/internal allocations
I (206) main_task: Calling app_main()
I (216) Board: UUID=d5ae031d-5552-4f55-8e93-afb4d6c1c49b SKU=ycsc-esp32s3-8311-wgxl-cat
I (216) button: IoT Button Version: 4.1.6
I (226) gc9a01: LCD panel create success, version: 2.0.1
I (666) Display: Power management not supported
I (696) LcdDisplay: Turning display on
I (696) LcdDisplay: Initialize LVGL library
I (696) LcdDisplay: Use 2MB of PSRAM for image cache
I (696) LcdDisplay: Initialize LVGL port
I (696) LcdDisplay: Adding LCD display
I (696) LVGL: Starting LVGL task
I (856) MCP: Add tool: self.motor
I (856) Backlight: Set brightness to 75
I (856) PowerSaveTimer: Power save timer enabled
I (4856) Application: STATE: starting
I (4876) OttoEmojiDisplay: 设置聊天消息 [system]: ycsc-esp32s3-8311-wgxl-cat/1.9.4
I (4876) Es8311AudioCodec: Duplex channels created
I (4886) ES8311: Work in Slave mode
I (4886) Es8311AudioCodec: Es8311AudioCodec initialized
I (4886) AudioCodec: Set input enable to true
I (4886) I2S_IF: channel mode 0 bits:16/16 channel:2 mask:1
I (4896) I2S_IF: STD Mode 1 bits:16/16 channel:2 sample_rate:16000 mask:1
I (4906) I2S_IF: channel mode 0 bits:16/16 channel:2 mask:1
I (4906) I2S_IF: STD Mode 0 bits:16/16 channel:2 sample_rate:16000 mask:1
I (4926) Adev_Codec: Open codec device OK
I (4926) AudioCodec: Set output enable to true
I (4926) AudioCodec: Audio codec started
I (4936) Application: STATE: configuring
W (4936) Application: Alert [] 配网模式: ac:a7:04:11:94:90
网络配置中
I (4966) OttoEmojiDisplay: 未知表情''，使用默认
I (4966) OttoEmojiDisplay: 设置聊天消息 [system]: ac:a7:04:11:94:90
网络配置中
I (4966) AudioService: OpusHead: version=1, channels=1, sample_rate=16000
I (6976) WifiBoard: WiFi initialized: false, mode: 1070270576
I (6976) pp: pp rom version: e7ae62f
I (6976) net80211: net80211 rom version: e7ae62f
I (6986) wifi:wifi driver task: 3fcd6f84, prio:23, stack:6144, core=0
I (6986) wifi:wifi firmware version: 4df78f2
I (6986) wifi:wifi certification version: v7.0
I (6986) wifi:config NVS flash: enabled
I (6996) wifi:config nano formatting: enabled
I (6996) wifi:Init data frame dynamic rx buffer num: 8
I (6996) wifi:Init dynamic rx mgmt buffer num: 5
I (7006) wifi:Init management short buffer num: 32
I (7006) wifi:Init dynamic tx buffer num: 32
I (7016) wifi:Init static tx FG buffer num: 2
I (7016) wifi:Init static rx buffer size: 1600
I (7026) wifi:Init static rx buffer num: 6
I (7026) wifi:Init dynamic rx buffer num: 8
I (7026) wifi_init: rx ba win: 6
I (7036) wifi_init: accept mbox: 6
I (7036) wifi_init: tcpip mbox: 32
I (7036) wifi_init: udp mbox: 6
I (7036) wifi_init: tcp mbox: 6
I (7046) wifi_init: tcp tx win: 5760
I (7046) wifi_init: tcp rx win: 5760
I (7046) wifi_init: tcp mss: 1440
I (7056) phy_init: phy_version 711,97bcf0a2,Aug 25 2025,19:04:10
W (7056) phy_init: failed to load RF calibration data (0x1102), falling back to full calibration
I (7106) phy_init: Saving new calibration data due to checksum failure or outdated calibration data, mode(2)
I (7116) wifi:mode : softAP (ac:a7:04:11:94:91)
I (7116) wifi:Total power save buffer number: 16
I (7116) wifi:Init max length of beacon: 752/752
I (7116) wifi:Init max length of beacon: 752/752
I (7126) esp_netif_lwip: DHCP server started on interface WIFI_AP_DEF with IP: 192.168.4.1
I (7126) BLE_INIT: BT controller compile version [1bb2f50]
I (7136) BLE_INIT: Put all controller code in flash
I (7136) BLE_INIT: Using main XTAL as clock source
I (7146) BLE_INIT: Bluetooth MAC: ac:a7:04:11:94:92
I (7156) BLUFI_EXAMPLE: BLE Host Task Started
I (7156) BLUFI_EXAMPLE: registered service 0x1800 with handle=1
I (7156) BLUFI_EXAMPLE: registering characteristic 0x2a00 with def_handle=2 val_handle=3

I (7166) BLUFI_EXAMPLE: registering characteristic 0x2a01 with def_handle=4 val_handle=5

I (7176) BLUFI_EXAMPLE: registered service 0x1801 with handle=6
I (7176) BLUFI_EXAMPLE: registering characteristic 0x2a05 with def_handle=7 val_handle=8

I (7186) BLUFI_EXAMPLE: registering characteristic 0x2b3a with def_handle=10 val_handle=11

I (7196) BLUFI_EXAMPLE: registering characteristic 0x2b29 with def_handle=12 val_handle=13

I (7206) BLUFI_EXAMPLE: registered service 0xffff with handle=14
I (7206) BLUFI_EXAMPLE: registering characteristic 0xff01 with def_handle=15 val_handle=16

I (7216) BLUFI_EXAMPLE: registering characteristic 0xff02 with def_handle=17 val_handle=18

I (7236) NimBLE: GAP procedure initiated: stop advertising.

I (7236) NimBLE: Failed to restore IRKs from store; status=8

I (7236) NimBLE: GAP procedure initiated: advertise;
I (7246) NimBLE: disc_mode=2
I (7246) NimBLE:  adv_channel_map=0 own_addr_type=0 adv_filter_policy=0 adv_itvl_min=0 adv_itvl_max=0
I (7256) NimBLE:

I (14946) SystemInfo: free sram: 99799 minimal sram: 98595
I (15936) AudioCodec: Set input enable to false
I (21936) AudioCodec: Set output enable to false
I (24946) SystemInfo: free sram: 99799 minimal sram: 98595
I (27836) BLUFI_EXAMPLE: connection established; status=0
I (27836) NimBLE: GAP procedure initiated: stop advertising.

I (28176) BLUFI_EXAMPLE: connection updated; status=0
I (28416) BLUFI_EXAMPLE: mtu update event; conn_handle=1 cid=4 mtu=255
I (28466) BLUFI_EXAMPLE: connection updated; status=0
I (28566) BLUFI_EXAMPLE: subscribe event; conn_handle=1 attr_handle=18 reason=1 prevn=0 curn=1 previ=0 curi=0

I (28766) wifi:mode : sta (ac:a7:04:11:94:90)
I (28766) wifi:enable tsf
I (33666) NimBLE: GATT procedure initiated: notify;
I (33666) NimBLE: att_handle=18

I (34416) WifiBoard: Received custom data: AT+OTA=https://xrobo.qiniuapi.com/v1/ota/
I (34416) WifiBoard: ota_url: https://xrobo.qiniuapi.com/v1/ota/
I (34946) SystemInfo: free sram: 101455 minimal sram: 98183
W (42656) wifi:Password length matches WPA2 standards, authmode threshold changes from OPEN to WPA2
I (42846) wifi:new:<1,0>, old:<1,1>, ap:<255,255>, sta:<1,0>, prof:1, snd_ch_cfg:0x0
I (42846) wifi:state: init -> auth (0xb0)
I (42866) wifi:state: auth -> assoc (0x0)
I (42886) wifi:state: assoc -> run (0x10)
I (42956) wifi:connected with 123456, aid = 4, channel 1, BW20, bssid = 0e:42:d9:ac:f1:37
I (42956) wifi:security: WPA2-PSK, phy: bgn, rssi: -31
I (42966) wifi:pm start, type: 1

I (42966) wifi:dp: 1, bi: 102400, li: 3, scale listen interval from 307200 us to 307200 us
I (42966) wifi:set rx beacon pti, rx_bcn_pti: 14, bcn_timeout: 25000, mt_pti: 14, mt_time: 10000
I (42976) wifi:dp: 2, bi: 102400, li: 4, scale listen interval from 307200 us to 409600 us
I (42986) wifi:AP's beacon interval = 102400 us, DTIM period = 2
I (44076) esp_netif_handlers: sta ip: 10.195.11.140, mask: 255.255.255.0, gw: 10.195.11.110
I (44076) WifiBoard: Received sta config, ssid: 123456, password: 12345678
I (44076) NimBLE: GATT procedure initiated: notify;
I (44076) NimBLE: att_handle=18

I (44256) Ota: Current version: 1.9.4
I (44326) wifi:<ba-add>idx:0 (ifx:0, 0e:42:d9:ac:f1:37), tid:0, ssn:4, winSize:64
I (44536) wifi:<ba-add>idx:1 (ifx:0, 0e:42:d9:ac:f1:37), tid:6, ssn:8, winSize:64
I (44676) esp-x509-crt-bundle: Certificate validated
I (44936) SystemInfo: free sram: 96747 minimal sram: 95259
I (45356) Ota: No mqtt section found !
I (45366) Ota: Current is the latest version
I (45366) WifiBoard: Activation code:
I (45366) NimBLE: GATT procedure initiated: notify;
I (45366) NimBLE: att_handle=18

I (45486) BLUFI_EXAMPLE: subscribe event; conn_handle=1 attr_handle=18 reason=1 prevn=1 curn=0 previ=0 curi=0

I (45566) wifi:state: run -> init (0x0)
I (45576) wifi:pm stop, total sleep time: lu us / lu us

I (45576) wifi:<ba-del>idx:0, tid:0
I (45576) wifi:<ba-del>idx:1, tid:6
I (45576) wifi:new:<1,0>, old:<1,0>, ap:<255,255>, sta:<1,0>, prof:1, snd_ch_cfg:0x0
I (45696) wifi:flush txq
I (45696) wifi:stop sw txq
I (45696) wifi:lmac stop hw txq
ESP-ROM:esp32s3-20210327