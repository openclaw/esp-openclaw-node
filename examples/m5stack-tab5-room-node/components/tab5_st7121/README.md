# Tab5 ST7121 extension

Minimal Apache-2.0 panel driver copied without source changes from
`platforms/tab5/components/esp_lcd_st7121` in M5Stack's official
`M5Tab5-UserDemo` at commit `68b19d37fbf9cefd5f256992f5dca34794c62ab4`.
The source and header retain Espressif's copyright and SPDX notices; the full
Apache License 2.0 text is the repository root `LICENSE`.
It is isolated here because `espressif/m5stack_tab5` 1.2.0~1 supports the
ILI9881C/GT911 and ST7123 revisions but does not yet expose ST7121 creation.
All other Tab5 services continue to use the maintained BSP.
