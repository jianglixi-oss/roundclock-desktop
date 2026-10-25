# Sources

LCD initialization table: 78/xiaozhi-esp32, commit 03a7c65a9121a1e657e8d7c7869b83612aff454d,
main/boards/taiji-pi-s3/taiji_pi_s3.cc. MIT license in LICENSE-xiaozhi.
https://github.com/78/xiaozhi-esp32/tree/03a7c65a9121a1e657e8d7c7869b83612aff454d/main/boards/taiji-pi-s3

LCD driver 2.0.2: espressif/esp-iot-solution, commit 4962410c80a51cd5822da1d8d2041eb4285265f1,
components/display/lcd/esp_lcd_st77916. Apache-2.0; original source headers retained.
https://github.com/espressif/esp-iot-solution/tree/4962410c80a51cd5822da1d8d2041eb4285265f1/components/display/lcd/esp_lcd_st77916

The local CMake adapter compiles the unmodified SPI/QSPI source files and provides
the component version macros without downloading cmake_utilities.

WebSocket client 1.8.0: espressif/esp-protocols, commit
5ac9f897f8aeed77f0fde8575d41a7f824c02034. Original component source and license retained.
https://github.com/espressif/esp-protocols/tree/5ac9f897f8aeed77f0fde8575d41a7f824c02034/components/esp_websocket_client
