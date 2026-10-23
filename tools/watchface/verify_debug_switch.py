"""Verify both master-switch modes, real CMake wiring, and console/file isolation."""
from pathlib import Path
import subprocess
import json
import sys
import verify_standby_power as native
from render_lvgl import ROOT

BUILD=ROOT/'build/debug-switch-host'
CMAKE=Path(r'C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe')
HARNESS=r'''
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#define WATCH_DEBUG_ENABLED MODE
static unsigned level_calls,sink_calls,write_calls;
static int last_fd;
static const void *last_data;
static int sink(const char *,va_list);
typedef int (*log_sink_t)(const char *,va_list);
static log_sink_t captured;
static void esp_log_level_set(const char *tag,int level){assert(tag[0]=='*' && level==0);++level_calls;}
static void esp_log_set_vprintf(log_sink_t callback){captured=callback;++sink_calls;}
#define ESP_LOG_NONE 0
#include "components/watch_debug/watch_debug.c"
#if !WATCH_DEBUG_ENABLED
static int write_result=7;
ssize_t __real__write_r(struct _reent *r,int fd,const void *data,size_t size){
 ++write_calls;last_fd=fd;last_data=data;return write_result;}
static void check_console(const char *format,...){va_list args;va_start(args,format);
 assert(captured(format,args)==0);va_end(args);}
#endif
int main(void){
 watch_debug_init();
#if !WATCH_DEBUG_ENABLED
 assert(level_calls==1 && sink_calls==1 && captured);check_console("ignored %u",123U);
 const char payload[]="file content";
 assert(__wrap__write_r(NULL,1,payload,12)==12 && !write_calls);
 assert(__wrap__write_r(NULL,2,payload,0)==0 && !write_calls);
 assert(__wrap__write_r(NULL,3,payload,12)==7 && write_calls==1 && last_fd==3 && last_data==payload);
 write_result=-1;assert(__wrap__write_r(NULL,4,payload,12)==-1 && write_calls==2);
 assert(__wrap_printf("discard %u",42U)==0 && __wrap_puts("discard")==0);
 assert(__wrap_putchar('X')=='X' && __wrap_esp_rom_printf("discard")==0 && __wrap_ets_printf("discard")==0);
#else
 assert(!level_calls && !sink_calls && !write_calls); /* Debug mode installs no filter. */
#endif
 puts("DEBUG_CONSOLE=PASS");
}
'''

def main():
    BUILD.mkdir(parents=True,exist_ok=True)
    config=BUILD/'sdkconfig'
    config.write_text('CONFIG_UNRELATED_SENTINEL=42\nCONFIG_LOG_DEFAULT_LEVEL_INFO=y\n',encoding='utf-8')
    source=BUILD/'cmake-src';source.mkdir(exist_ok=True)
    (source/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
project(debug_switch NONE)
include("${CMAKE_CURRENT_LIST_DIR}/debug.cmake")
function(idf_component_register)
    set(COMPONENT_LIB "test_${CURRENT_COMPONENT}" PARENT_SCOPE)
    add_library("test_${CURRENT_COMPONENT}" INTERFACE)
    file(WRITE "${CMAKE_BINARY_DIR}/${CURRENT_COMPONENT}-registration.txt" "${ARGV}")
endfunction()
set(CURRENT_COMPONENT power)
include("'''+(ROOT/'components/watch_power/CMakeLists.txt').as_posix()+'''")
get_target_property(wrappers test_power INTERFACE_LINK_OPTIONS)
file(WRITE "${CMAKE_BINARY_DIR}/power-wrappers.txt" "${wrappers}")
set(CURRENT_COMPONENT console)
include("'''+(ROOT/'components/watch_debug/CMakeLists.txt').as_posix()+'''")
get_target_property(wrappers test_console INTERFACE_LINK_OPTIONS)
file(WRITE "${CMAKE_BINARY_DIR}/console-wrappers.txt" "${wrappers}")
''',encoding='utf-8')
    original=(ROOT/'debug.cmake').read_text(encoding='utf-8')
    assert 'set(WATCH_DEBUG OFF)' in original
    report=[]
    previous=None
    for mode in ['OFF','OFF','ON','ON','OFF']:
        before=config.read_text(encoding='utf-8')
        (source/'debug.cmake').write_text(original.replace('set(WATCH_DEBUG OFF)',f'set(WATCH_DEBUG {mode})'),encoding='utf-8')
        result=subprocess.run([str(CMAKE),'-S',str(source),'-B',str(BUILD/'cmake-build'),'-G','Ninja',
                               '-DCMAKE_MAKE_PROGRAM=C:/Espressif/tools/ninja/1.12.1/ninja.exe',
                               '-DSDKCONFIG='+str(config)],capture_output=True,text=True)
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
        text=config.read_text(encoding='utf-8')
        if previous==mode:assert text==before, 'Same-mode reconfigure must not rewrite config'
        previous=mode
        assert 'CONFIG_UNRELATED_SENTINEL=42' in text
        active='INFO' if mode=='ON' else 'NONE'
        for key in [f'LOG_DEFAULT_LEVEL_{active}',f'BOOTLOADER_LOG_LEVEL_{active}',f'LOG_BOOTLOADER_LEVEL_{active}']:
            assert f'CONFIG_{key}=y' in text
        for key in ['LV_USE_LOG','FREERTOS_GENERATE_RUN_TIME_STATS']:
            assert (f'CONFIG_{key}=y' if mode=='ON' else f'# CONFIG_{key} is not set') in text
        power=(BUILD/'cmake-build/power-registration.txt').read_text()
        wrappers=(BUILD/'cmake-build/power-wrappers.txt').read_text()
        console=(BUILD/'cmake-build/console-wrappers.txt').read_text()
        assert ('watch_power_gate.c' in power)==(mode=='ON')
        assert ('wrap=vApplicationSleep' in wrappers)==(mode=='ON')
        assert ('wrap=_write_r' in console)==(mode=='OFF')
    native.BUILD=BUILD
    for name,body in [('esp_log.h',''),('esp_attr.h','#define IRAM_ATTR\n'),
                      ('sys/types.h','typedef long long ssize_t;\n')]:
        p=BUILD/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body,encoding='utf-8')
    for mode in [0,1]:
        # Directly include the real implementation with no firmware/platform side effects.
        code=HARNESS.replace('MODE',str(mode))
        code=code.replace('#include "components/watch_debug/watch_debug.c"',
            '#include "'+(ROOT/'components/watch_debug/watch_debug.c').as_posix()+'"')
        # The production header is a tiny C ABI declaration; make it discoverable.
        (BUILD/'watch_debug.h').write_text((ROOT/'components/watch_debug/include/watch_debug.h').read_text(),encoding='utf-8')
        report.append(native.compile_run(f'debug_{mode}',code).strip()+f' (mode={mode})')
    report.append('DEBUG_SWITCH=PASS (5 OFF/ON CMake transitions, SDK/bootloader/LVGL/RTOS settings, unrelated config preserved, sleep hooks excluded when OFF, console filtering preserves file writes/errors)')
    if '--firmware' in sys.argv:
        elf=(ROOT/'build/esp32-s3-touch-amoled-2-06-brookesia.elf').read_bytes()
        for name in ['__wrap_vApplicationSleep','__wrap_esp_light_sleep_start',
                     '__wrap_esp_wifi_internal_is_tsf_active','watch_power_sleep_poll',
                     'watch_power_tasks_poll','watch_power_gate_snapshot','WatchGifPerf','FacePerf:']:
            assert name.encode() not in elf, f'Quiet ELF retains diagnostic: {name}'
        for name in ['__wrap__write_r','__wrap_printf','__wrap_esp_rom_printf','watch_power_get_screen_state']:
            assert name.encode() in elf, f'Missing quiet filter or screen policy: {name}'
        cfg=(ROOT/'build/config/sdkconfig.h').read_text(encoding='utf-8')
        assert '#define CONFIG_LOG_DEFAULT_LEVEL 0' in cfg
        assert '#define CONFIG_LOG_MAXIMUM_LEVEL 0' in cfg
        assert '#define CONFIG_BOOTLOADER_LOG_LEVEL 0' in cfg
        assert '#define CONFIG_LV_USE_LOG 1' not in cfg
        assert '#define CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS 1' not in cfg
        commands=json.loads((ROOT/'build/compile_commands.json').read_text(encoding='utf-8'))
        for command in commands:
            assert Path(command['file']).name not in ['watch_power_gate.c','watch_power_sleep.c','watch_power_tasks.c']
        report.append('DEBUG_FIRMWARE=PASS (final quiet ELF: no sleep/GIF/face diagnostics, filters linked, screen policy retained; SDK/bootloader log level 0, LVGL logs and RTOS runtime stats disabled)')
    output='\n'.join(report)+'\n';print(output)
    (ROOT/'output/watchface/debug-switch-verification.txt').write_text(output,encoding='utf-8')

if __name__=='__main__':main()
