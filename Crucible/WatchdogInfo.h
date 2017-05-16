#pragma once

#pragma pack(push, 1)
struct WatchdogInfo {
	atomic<bool> message_thread_stuck = false;
	atomic<bool> graphics_thread_stuck = false;
};
#pragma pack(pop)
