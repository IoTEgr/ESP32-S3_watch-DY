#ifndef XIAOZHI_WAKE_CONFIG_H
#define XIAOZHI_WAKE_CONFIG_H

#include "sdkconfig.h"

/* Follow the model selected for srmodels.bin. The UI uses the user's spelling
 * 小嘉; the bundled 小珈 model has the same pronunciation. */
#if defined(CONFIG_SR_WN_WN9_XIAO3JIA1XIAO3JIA1_TTS3)
#define XIAOZHI_WAKE_WORD_TEXT "小嘉小嘉"
#elif defined(CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS)
#define XIAOZHI_WAKE_WORD_TEXT "你好小智"
#endif

#ifdef XIAOZHI_WAKE_WORD_TEXT
#define XIAOZHI_WAKE_HINT_TEXT "轻点小球，或说「" XIAOZHI_WAKE_WORD_TEXT "」"
#else
#define XIAOZHI_WAKE_HINT_TEXT "轻点小球，或说唤醒词"
#endif

#endif
