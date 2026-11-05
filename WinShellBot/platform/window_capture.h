#ifndef SB_WINDOWS_WINDOW_CAPTURE_H
#define SB_WINDOWS_WINDOW_CAPTURE_H
#include "sb.h"
/* 每次窗口捕获使用独立进程，应用绘图或显卡调用不能阻塞桌面线程。 */
int sb_win_capture_worker(void);
char *sb_win_capture_request(yyjson_val *request,bool (*guard)(void*),void *context,uint64_t deadline,sb_error *error);
#endif
