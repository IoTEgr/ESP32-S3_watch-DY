#include "watch_debug.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>

#if !WATCH_DEBUG_ENABLED
static int discard_log(const char *format, va_list args)
{
    (void)format;(void)args;
    return 0;
}

/* Filter console descriptors only. SPIFFS/SD/NVS file writes must retain
 * normal return values and error handling. Also covers precompiled libraries
 * using stdio instead of ESP_LOG. ROM's own boot text is outside app control. */
struct _reent;
ssize_t __real__write_r(struct _reent *, int, const void *, size_t);
ssize_t __wrap__write_r(struct _reent *r, int fd, const void *data, size_t size)
{
    if(fd==1 || fd==2)return (ssize_t)size;
    return __real__write_r(r,fd,data,size);
}
int __wrap_printf(const char *format, ...){(void)format;return 0;}
int __wrap_vprintf(const char *format, va_list args){return discard_log(format,args);}
int __wrap_puts(const char *text){(void)text;return 0;}
int __wrap_putchar(int ch){return (unsigned char)ch;}
int IRAM_ATTR __wrap_esp_rom_printf(const char *format, ...){(void)format;return 0;}
int IRAM_ATTR __wrap_ets_printf(const char *format, ...){(void)format;return 0;}
#endif

void watch_debug_init(void)
{
#if !WATCH_DEBUG_ENABLED
    esp_log_level_set("*",ESP_LOG_NONE);
    esp_log_set_vprintf(discard_log);
#endif
}
