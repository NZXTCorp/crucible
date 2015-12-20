#pragma once

#include <functional>

#define C_EXPORT extern "C" __declspec(dllexport)

extern void (*hlog)(const char *fmt, ...);

#define LOG_MSG(x, ...) hlog(x, __VA_ARGS__)
#define LOG_WARN LOG_MSG
#define LOG_CR

struct ProcessCompat {
	struct ProcStatsCompat {
		SIZE m_SizeWnd;					// the window/backbuffer size. (pixels)
	} m_Stats;
};

extern ProcessCompat g_Proc;

class IndicatorManager;
extern IndicatorManager indicatorManager;

enum IndicatorEvent;
void ShowCurrentIndicator(const std::function<void(IndicatorEvent, BYTE /*alpha*/)> &func);
