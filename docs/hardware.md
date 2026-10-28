# 已测试硬件

ESP32-S3 N16R8，16MB Flash，8MB OCT PSRAM；360×360 ST77916 QSPI 圆屏。

| 部件 | 引脚 |
| --- | --- |
| LCD CLK / CS | 9 / 10 |
| LCD D0/D1/D2/D3 | 11 / 12 / 13 / 14 |
| LCD RESET / BL | 47 / 15（高电平开） |
| CST816S SDA / SCL / RESET / IRQ | 7 / 8 / 40 / 41 |
| PDM 麦克风 CLK / DIN | 45 / 46 |
| I2S 功放 DOUT / BCLK / LRCK / ENABLE | 17 / 18 / 16 / 48 |
| BOOT | 0 |

显示使用 taiji-pi-s3 PDM 修订初始化表；触摸逻辑坐标为 `(raw_y,359-raw_x)`。
不同批次同名硬件可能有不同接线或屏幕初始化，不能只凭 N16R8 判断兼容。

背光GPIO15使用5kHz、10位LEDC PWM；正常占空比1023，夜间512。显示QSPI为40MHz，与原taiji-pi-s3驱动配置一致。双DMA缓冲按内部内存余量选择90/60/20行，并保留网络/语音的内存空间。
