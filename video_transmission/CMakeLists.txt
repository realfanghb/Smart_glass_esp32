# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS
        "/Users/jackli/Documents/EspressifIDE/473Proj/esp-adf/components"
        "/Users/jackli/Documents/EspressifIDE/473Proj/esp-adf/components/esp-adf-libs"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_definitions(
  portTICK_RATE_MS=portTICK_PERIOD_MS
  xSemaphoreHandle=SemaphoreHandle_t
  xQueueHandle=QueueHandle_t
  xTaskHandle=TaskHandle_t
  xTimerHandle=TimerHandle_t
  portTickType=TickType_t
)

project(video_transmission)
