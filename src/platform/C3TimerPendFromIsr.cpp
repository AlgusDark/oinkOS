#if defined(ARDUINO_ARCH_ESP32)
#include <sdkconfig.h>
#endif

#if defined(ARDUINO_ARCH_ESP32) && CONFIG_IDF_TARGET_ESP32C3 && defined(ESP32_ARDUINO_NO_RGB_BUILTIN)

#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

#include <cstdlib>

// The event-group FromISR helpers reference this, which would link timers.c and its task. With
// ESP32_ARDUINO_NO_RGB_BUILTIN set, RMT was their only caller, so this must never run. Users of
// other timer APIs (xTimerCreate) fail the link with a duplicate symbol instead.
extern "C" BaseType_t xTimerPendFunctionCallFromISR(PendedFunction_t, void*, uint32_t, BaseType_t*) { abort(); }

#endif
