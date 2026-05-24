# Firmware

这个目录存放 ESP8266 屏幕端固件。

## 目录

- `esp_nas_monitor/`
  - `nas_notice_font.h`
  - `tools/generate_notice_font.py`
- `releases/`
  - `esp.bin`

## 说明

- `esp_nas_monitor/` 是当前 ESP8266 NAS 监控屏幕的源码目录
- `releases/esp.bin` 是已编译好的固件二进制
- 当前二进制来源于 `esp8266:esp8266:generic` 目标

## 固件二进制

- 文件：`firmware/releases/esp.bin`
- SHA-256：`6787c74559a7cddd1734ff7cb9920c785eb797422e5383fb0cabac5e799b0fc7`

## 备注

- 仓库当前没有收 Arduino 构建缓存目录
- 如果后续重新编译并更新固件，建议同时更新 `releases/` 下的二进制和这里的校验值
