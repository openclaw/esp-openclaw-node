# M5Stack Tab5 BSP build bridge

This contains no BSP source. It builds the exact inspected Espressif BSP commit
`f0ef9497efce684997ce391edd19733483e250a5` (component version 1.2.0~1) while
letting the application resolve `esp_video ^2.1`, as required by the pinned
P4-capable `esp_capture`. The registry manifest's `esp_video ~2.0` constraint
otherwise makes the two official components unsatisfiable in one IDF project.
