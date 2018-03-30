// [crucible.cpp 2015-10-22 abright]
// libobs-based game capture (currently an experimental thing based on the libobs sample app)

#define NOMINMAX
#include <ShlObj.h>
#include <stdio.h>
#include <windows.h>

#include <util/base.h>
#include <util/dstr.hpp>
#include <util/platform.h>
#include <util/profiler.hpp>
#include <obs.hpp>

#include "OBSHelpers.hpp"

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using namespace std;

#include <boost/date_time/c_local_time_adjustor.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/format.hpp>
#include <boost/logic/tribool.hpp>
#include <boost/optional.hpp>

#include "../AnvilRendering/AnvilRendering.h"

#include "RemoteDisplay.h"

#include "IPC.hpp"
#include "ProtectedObject.hpp"
#include "scopeguard.hpp"
#include "ThreadTools.hpp"

#include "ScreenshotProvider.h"

#include "WatchdogInfo.h"

// window class borrowed from forge, remove once we've got headless mode working
#include "TestWindow.h"

//#define TEST_WINDOW

#ifdef USE_BUGSPLAT
#include <BugSplat.h>
#include <csignal>
#pragma comment(lib, "bugsplat.lib")
MiniDmpSender *dmpSender;
#endif

extern "C" {
	_declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

extern OBSEncoder CreateAudioEncoder(const char *name, uint32_t mixer_idx = 0);
extern void RegisterAudioBufferSource();
extern void RegisterFramebufferSource();
extern void RegisterNVENCEncoder();
#ifdef WEBRTC_WIN
extern void RegisterWebRTCOutput();
extern bool WebRTCNVENCAvailable();
#endif

static IPCClient event_client, log_client;

atomic<bool> store_startup_log = false;
vector<string> startup_logs;
mutex startup_log_mutex;

HANDLE exit_event = nullptr;

static void AddWaitHandleCallback(HANDLE h, function<void()> cb);
static void RemoveWaitHandle(HANDLE h);

static const vector<pair<string, string>> allowed_hardware_encoder_names = {
	{ "crucible_nvenc", "Nvidia NVENC" },
	{ "ffmpeg_nvenc", "Nvidia NVENC (via FFmpeg)" },
	{ "obs_qsv11", "Intel Quick Sync Video" },
	{ "amd_amf_h264", "AMD AMF Video Encoder" },
};

#define CONCAT2(x, y) x ## y
#define CONCAT(x, y) CONCAT2(x, y)
#define LOCK(x) lock_guard<decltype(x)> CONCAT(lockGuard, __LINE__){x};

template <typename Fun>
static void QueueOperation(Fun &&f);

struct OutputResolution {
	uint32_t width;
	uint32_t height;

	uint32_t pixels() const { return width * height; }

	OutputResolution MinByPixels(const boost::optional<OutputResolution> &other)
	{
		return (!other || pixels() <= other->pixels()) ? *this : *other;
	}

	bool operator!=(const OutputResolution &other)
	{
		return width != other.width || height != other.height;
	}
};

#ifdef USE_BUGSPLAT
// try to grab the current username stored in registry for bugsplat reporting
boost::optional<wstring> GetCurrentUsername() 
{
	HKEY hKey = NULL;
	DWORD dwType = REG_SZ;
	wchar_t name[256] = {};
	DWORD dwSize = sizeof(name);

	if (RegOpenKeyExW(HKEY_CURRENT_USER, BUGSPLAT_USER_REG_PATH, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
		return boost::none;

	DEFER{ RegCloseKey(hKey); };

	if (RegGetValueW(hKey, NULL, BUGSPLAT_USER_KEY_NAME, RRF_RT_REG_SZ, &dwType, name, &dwSize) == ERROR_SUCCESS)
		return wstring(name, dwSize / sizeof(wchar_t));

	return boost::none;
}
#endif

// logging lifted straight out of the test app
void do_log(int log_level, const char *msg, va_list args, void *param)
{
	char bla[4096];
	size_t n = vsnprintf(bla, 4095, msg, args);

	OutputDebugStringA(bla);
	OutputDebugStringA("\n");

	if (log_client)
		log_client.Write(bla, n + 1);

	//cout << bla << endl;

	if (store_startup_log) {
		LOCK(startup_log_mutex);
		startup_logs.push_back(bla);
	}

	if (log_level < LOG_WARNING && IsDebuggerPresent())
		__debugbreak();

	UNUSED_PARAMETER(param);
}

void RenderWindow(void *data, uint32_t cx, uint32_t cy)
{
	obs_render_main_view();

	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
}

void OBSStartRecording(void *data, calldata_t *params)
{
	blog(LOG_INFO, "Recording started");
}

void OBSStopRecording(void *data, calldata_t *params)
{
	int code = (int)calldata_int(params, "code");
	blog(LOG_INFO, "Recording stopped, code %d", code);
}

/*template <typename T>
static DStr GetModulePath(T *sym)*/
static DStr GetModulePath(const char *name)
{
	DStr res;

	HMODULE module;
	if (!GetModuleHandleEx(
			//GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			/*(LPCTSTR)sym*/ name, &module)) {
		blog(LOG_ERROR, "module handle ex: %d", GetLastError());
		return res;
	}

	char filename[MAX_PATH];
	if (!GetModuleFileNameA(module, filename, MAX_PATH))
		return res;

	filename[MAX_PATH - 1] = 0;

	char drive[_MAX_DRIVE] = "";
	char dir[_MAX_DIR] = "";
	if (_splitpath_s(filename, drive, _MAX_DRIVE, dir, _MAX_DIR,
			NULL, 0, NULL, 0))
		return res;

	dstr_printf(res, "%s%s", drive, dir);
	return res;
}

#ifdef _WIN64
#define BIT_STRING "64bit"
#else
#define BIT_STRING "32bit"
#endif

static string recording_filename_prefix = "recording_";

static string TimeZoneOffset()
{
	using boost::format;

	auto utc = boost::posix_time::second_clock::universal_time();
	auto now = boost::date_time::c_local_adjustor<boost::posix_time::ptime>::utc_to_local(utc);

	auto diff = now - utc;

	if (diff.hours() == 0 && diff.minutes() == 0)
		return "Z";

	return (format("%+03d%02d") % diff.hours() % diff.minutes()).str();
}

static OBSData GenerateExtraData(const string &type)
{
	auto res = OBSDataCreate();
	obs_data_set_string(res, "type", type.c_str());
	obs_data_set_string(res, "iso8601_time", (to_iso_extended_string(boost::posix_time::second_clock::local_time()) + TimeZoneOffset()).c_str());
	return res;
}

struct OutputStatus {
	bool started = false;
	bool stopped = false;
	bool stopping_for_restart = false;
	bool restarting = false;
};

string to_string(OutputStatus os)
{
	auto to_s = [](bool b)
	{
		return b ? "true"s : "false"s;
	};

	return "OutputStatus { started = " + to_s(os.started) + ", stopped = " + to_s(os.stopped) + ", stopping_for_restart = " + to_s(os.stopping_for_restart) + ", restarting = " + to_s(os.restarting) +" }";
}

OutputStatus GetOutputStatus(obs_data_t *data)
{
	auto val = OBSDataGetObj(data, "CrucibleOutputStatus");

	OutputStatus res;
	res.started = obs_data_get_bool(val, "started");
	res.stopped = obs_data_get_bool(val, "stopped");
	res.stopping_for_restart = obs_data_get_bool(val, "stopping_for_restart");
	res.restarting = obs_data_get_bool(val, "restarting");

	return res;
}

void SetOutputStatus(obs_data_t *data, OutputStatus os)
{
	auto val = OBSDataCreate();

	obs_data_set_bool(val, "started", os.started);
	obs_data_set_bool(val, "stopped", os.stopped);
	obs_data_set_bool(val, "stopping_for_restart", os.stopping_for_restart);
	obs_data_set_bool(val, "restarting", os.restarting);

	obs_data_set_obj(data, "CrucibleOutputStatus", val);
}

struct Bookmark {
	int id = 0;
	video_tracked_frame_id tracked_id = 0;
	int64_t pts = 0;
	uint32_t fps_den = 1;
	double time = 0.;

	OBSData extra_data;
};

namespace ForgeEvents {
	mutex eventMutex;
	vector<OBSData> queuedEvents;

	void SendEvent(obs_data_t *event)
	{
		if (!event)
			return;

		auto data = obs_data_get_json(event);
		if (!data)
			return;

		LOCK(eventMutex);
		if (event_client.Write(data, strlen(data) + 1))
			return;

		queuedEvents.push_back(event);
		blog(LOG_INFO, "event_client.Write failed, queueing event");
	}

	void SendQueuedEvents()
	{
		LOCK(eventMutex);
		auto i = begin(queuedEvents);
		auto end_ = end(queuedEvents);
		for (; i != end_; i++) {
			auto data = obs_data_get_json(*i);
			if (!event_client.Write(data, strlen(data) + 1))
				break;
		}

		queuedEvents.erase(begin(queuedEvents), i);
	}

	static void SendFileCompleteEvent(obs_data_t *event, const char *filename, int total_frames, double duration, const vector<double> &bookmarks,
		uint32_t width, uint32_t height, DWORD *pid=nullptr)
	{
		obs_data_set_string(event, "filename", filename);
		obs_data_set_int(event, "total_frames", total_frames);
		obs_data_set_double(event, "duration", duration);
		obs_data_set_int(event, "width", width);
		obs_data_set_int(event, "height", height);
		if (pid)
			obs_data_set_int(event, "process_id", *pid);

		auto array = OBSDataArrayCreate();
		obs_data_set_array(event, "bookmarks", array);

		for (auto bookmark : bookmarks) {
			auto tmp = OBSDataCreate();
			obs_data_set_double(tmp, "val", bookmark);
			obs_data_array_push_back(array, tmp);
		}

		SendEvent(event);
	}

	OBSData EventCreate(const char * name)
	{
		auto event = OBSDataCreate();

		obs_data_set_string(event, "event", name);
		obs_data_set_int(event, "timestamp", GetTickCount64());

		return event;
	}

	void SendRecordingStart(const char *filename, bool split_recording, uint32_t recording_bitrate, uint32_t width, uint32_t height)
	{
		auto event = EventCreate("started_recording");

		obs_data_set_string(event, "filename", filename);
		if (split_recording)
			obs_data_set_bool(event, "split_recording", true);
		if (recording_bitrate)
			obs_data_set_int(event, "bitrate", recording_bitrate);
		obs_data_set_int(event, "width", width);
		obs_data_set_int(event, "height", height);

		SendEvent(event);
	}

	static void SendRecordingStopEvent(obs_data_t *event, const char *filename, int total_frames, double duration, const vector<double> &bookmarks,
		uint32_t width, uint32_t height, DWORD *pid, const vector<Bookmark> &full_bookmarks)
	{
		auto arr = OBSDataArrayCreate();
		for (auto &bookmark : full_bookmarks) {
			auto mark = OBSDataCreate();

			obs_data_set_int(mark, "bookmark_id", bookmark.id);
			obs_data_set_double(mark, "timestamp", bookmark.time);
			obs_data_set_obj(mark, "extra_data", bookmark.extra_data);

			obs_data_array_push_back(arr, mark);
		}

		obs_data_set_array(event, "full_bookmarks", arr);

		SendFileCompleteEvent(event, filename, total_frames, duration, bookmarks, width, height, pid);
	}

	void SendRecordingStop(const char *filename, int total_frames, double duration, const vector<double> &bookmarks,
		uint32_t width, uint32_t height, DWORD *pid, const vector<Bookmark> &full_bookmarks, bool split_recording)
	{
		auto event = EventCreate("stopped_recording");

		if (split_recording)
			obs_data_set_bool(event, "split_recording", split_recording);

		SendRecordingStopEvent(event, filename, total_frames, duration, bookmarks, width, height, pid, full_bookmarks);
	}

	void SendGameSessionEnded(const char *filename, int total_frames, double duration, const vector<double> &bookmarks,
		uint32_t width, uint32_t height, DWORD pid, const vector<Bookmark> &full_bookmarks, boost::optional<int> game_start_id, boost::optional<int> game_end_id,
		bool split_recording)
	{
		auto event = EventCreate("game_session_ended");

		const Bookmark *start = nullptr;
		const Bookmark *end_ = nullptr;
		for (auto &mark : full_bookmarks) {
			if (game_start_id && mark.id == *game_start_id)
				start = &mark;
			else if (game_end_id && mark.id == *game_end_id)
				end_ = &mark;
		}

		auto game_start = start ? start->time : 0.;
		auto game_end = end_ ? end_->time : duration;

		obs_data_set_double(event, "game_start", game_start);
		obs_data_set_double(event, "game_end", game_end);
		obs_data_set_double(event, "game_duration", game_end - game_start);
		obs_data_set_bool(event, "split_recording", split_recording);

		SendRecordingStopEvent(event, filename, total_frames, duration, bookmarks, width, height, &pid, full_bookmarks);
	}

	void SendQueryMicsResponse(obs_data_array_t *devices)
	{
		auto event = EventCreate("query_mics_response");

		obs_data_set_array(event, "devices", devices);

		SendEvent(event);
	}

	void SendBufferReady(const char *filename, int total_frames, double duration, const vector<double> &bookmarks,
		uint32_t width, uint32_t height, boost::optional<Bookmark> bookmark_info)
	{
		auto event = EventCreate("buffer_ready");

		if (bookmark_info) {
			obs_data_set_double(event, "created_at_offset", bookmark_info->time);
			obs_data_set_int(event, "bookmark_id", bookmark_info->id);
			obs_data_set_obj(event, "bookmark_extra_data", bookmark_info->extra_data);
		}

		SendFileCompleteEvent(event, filename, total_frames, duration, bookmarks, width, height);
	}

	void SendBufferFailure(const char *filename)
	{
		auto event = EventCreate("buffer_save_failed");

		obs_data_set_string(event, "filename", filename);

		SendEvent(event);
	}

	void SendInjectFailed(long *injector_exit_code)
	{
		auto event = EventCreate("inject_failed");
		
		if (injector_exit_code)
			obs_data_set_int(event, "injector_exit_code", *injector_exit_code);

		SendEvent(event);
	}

	void SendInjectRequest(bool process_is_64bit, bool anti_cheat, DWORD process_thread_id, const string &hook_dir)
	{
		auto event = EventCreate("inject_request");

		obs_data_set_bool(event, "64bit", process_is_64bit);
		obs_data_set_bool(event, "anti_cheat", anti_cheat);
		obs_data_set_int(event, "id", process_thread_id);
		obs_data_set_string(event, "hook_dir", hook_dir.c_str());

		SendEvent(event);
	}

	void SendMonitorProcess(DWORD process_id)
	{
		auto event = EventCreate("monitor_process");

		obs_data_set_int(event, "process_id", process_id);

		SendEvent(event);
	}

	void SendCleanupComplete(const string *profiler_data, DWORD pid)
	{
		auto event = EventCreate("cleanup_complete");

		if (profiler_data)
			obs_data_set_string(event, "profiler_data", profiler_data->c_str());

		obs_data_set_int(event, "process_id", pid);

		SendEvent(event);
	}

	void SendBrowserSizeHint(uint32_t width, uint32_t height)
	{
		auto event = EventCreate("browser_size_hint");

		obs_data_set_int(event, "width", width);
		obs_data_set_int(event, "height", height);

		SendEvent(event);
	}

	void SendStreamingStartExecuted(bool success)
	{
		auto event = EventCreate("streaming_start_executed");
		obs_data_set_bool(event, "success", success);

		SendEvent(event);

	};

	void SendStreamingStopExecuted(bool success)
	{
		auto event = EventCreate("streaming_stop_executed");
		obs_data_set_bool(event, "success", success);

		SendEvent(event);

	};

	void SendStreamingStart()
	{
		auto event = EventCreate("started_streaming");

		SendEvent(event);
	}

	void SendStreamingStop(long long code)
	{
#define EXPAND2(x) x
#define EXPAND(x) x, EXPAND2(#x)
		static const map<long long, const char*> known_code = {
			{EXPAND(OBS_OUTPUT_SUCCESS)},
			{EXPAND(OBS_OUTPUT_BAD_PATH)},
			{EXPAND(OBS_OUTPUT_CONNECT_FAILED)},
			{EXPAND(OBS_OUTPUT_INVALID_STREAM)},
			{EXPAND(OBS_OUTPUT_ERROR)},
			{EXPAND(OBS_OUTPUT_DISCONNECTED)},
			{EXPAND(OBS_OUTPUT_UNSUPPORTED)},
			{EXPAND(OBS_OUTPUT_NO_SPACE)},
		};
#undef EXPAND
#undef EXPAND2

		auto event = EventCreate("stopped_streaming");

		obs_data_set_int(event, "code", code);

		auto it = known_code.find(code);
		if (it != end(known_code))
			obs_data_set_string(event, "name", it->second);

		SendEvent(event);
	}

	void SendSavedGameScreenshot(const char *requested_filename, const char *actual_filename)
	{
		auto event = EventCreate("saved_game_screenshot");

		obs_data_set_string(event, "requested_filename", requested_filename);
		obs_data_set_string(event, "actual_filename", actual_filename);

		SendEvent(event);
	}

	void SendQueryWebcamsResponse(const OBSDataArray &arr)
	{
		auto event = EventCreate("query_webcams_response");

		obs_data_set_array(event, "devices", arr);

		SendEvent(event);
	}

	void SendFramebufferConnectionInfo(const char *id, const char *name, const LUID *luid)
	{
		auto event = EventCreate("framebuffer_connection_info");

		obs_data_set_string(event, "id", id);
		obs_data_set_string(event, "name", name);

		if (luid) {
			obs_data_set_int(event, "luid_low", luid->LowPart);
			obs_data_set_int(event, "luid_high", luid->HighPart);
		}

		SendEvent(event);
	}

	void SendQueryDesktopAudioDevicesResponse(const OBSDataArray &arr)
	{
		auto event = EventCreate("query_desktop_audio_devices_response");

		obs_data_set_array(event, "devices", arr);

		SendEvent(event);
	}

	void SendQueryWindowsResponse(const OBSDataArray &arr)
	{
		auto event = EventCreate("query_windows_response");

		obs_data_set_array(event, "windows", arr);

		SendEvent(event);
	}

	void SendSelectSceneResult(const string &scene, const string &current_scene, bool success)
	{
		auto event = EventCreate("select_scene_result");

		obs_data_set_string(event, "scene", scene.c_str());
		obs_data_set_string(event, "current_scene", current_scene.c_str());
		obs_data_set_bool(event, "success", success);

		SendEvent(event);
	}

	void SendCanvasSize(uint32_t width, uint32_t height)
	{
		auto event = EventCreate("canvas_size");

		obs_data_set_int(event, "width", width);
		obs_data_set_int(event, "height", height);

		SendEvent(event);
	}

	void SendSceneInfo(obs_data_t *scenes)
	{
		auto event = EventCreate("scene_info");

		obs_data_set_obj(event, "scenes", scenes);

		SendEvent(event);
	}

	void SendAudioSourceLevel(const char *source, float level, float magnitude, float peak, bool muted)
	{
		auto event = EventCreate("audio_source_level");

		obs_data_set_string(event, "source", source);
		obs_data_set_double(event, "level", level);
		obs_data_set_double(event, "magnitude", magnitude);
		obs_data_set_double(event, "peak", peak);
		obs_data_set_bool(event, "muted", muted);

		SendEvent(event);
	}

	void SendGameSessionStarted()
	{
		SendEvent(EventCreate("game_session_started"));
	}

	void SendGameCaptureStarted()
	{
		SendEvent(EventCreate("game_capture_started"));
	}

	void SendGameCaptureStopped(chrono::steady_clock::duration duration)
	{
		auto event = EventCreate("game_capture_stopped");

		obs_data_set_double(event, "duration", chrono::duration_cast<chrono::milliseconds>(duration).count() / 1000.);

		SendEvent(event);
	}

	void SendScreenshotSaved(boost::optional<std::string> error, uint32_t cx, uint32_t cy, const std::string filename)
	{
		auto event = EventCreate("screenshot_saved");

		obs_data_set_bool(event, "success", !error);
		obs_data_set_int(event, "width", cx);
		obs_data_set_int(event, "height", cy);
		obs_data_set_string(event, "filename", filename.c_str());

		if (error)
			obs_data_set_string(event, "error", error->c_str());

		SendEvent(event);
	}

	void SendBookmarkFinalized(const Bookmark &bookmark, uint32_t width, uint32_t height)
	{
		auto event = EventCreate("bookmark_finalized");

		obs_data_set_double(event, "created_at_offset", bookmark.time);
		obs_data_set_int(event, "bookmark_id", bookmark.id);
		obs_data_set_obj(event, "bookmark_extra_data", bookmark.extra_data);

		obs_data_set_int(event, "width", width);
		obs_data_set_int(event, "height", height);

		SendEvent(event);
	}

	void SendQueryHardwareEncodersResponse(obs_data_array *arr)
	{
		auto event = EventCreate("query_hardware_encoders_response");

		obs_data_set_array(event, "encoders", arr);

		SendEvent(event);
	}

	void SendProcessInaccessible(DWORD pid)
	{
		auto event = EventCreate("process_inaccessible");

		obs_data_set_int(event, "process_id", pid);

		SendEvent(event);
	}

	void SendWatchdogInfoName(const string &name)
	{
		auto event = EventCreate("watchdog_info_name");

		obs_data_set_string(event, "name", name.c_str());

		SendEvent(event);
	}

	void SendAudioBufferMuted(bool muted)
	{
		auto event = EventCreate("audio_buffer_muted");

		obs_data_set_bool(event, "muted", muted);

		SendEvent(event);
	}

	void SendPTTStatus(bool active)
	{
		auto cmd = EventCreate("push_to_talk_status");
		obs_data_set_bool(cmd, "active", active);
		SendEvent(cmd);
	}

	void SendBookmarkRequest()
	{
		SendEvent(EventCreate("bookmark_request"));
	}

	void SendCreateWebRTCOutputResult(OBSData &original, boost::optional<string> err)
	{
		auto event = original;

		obs_data_set_string(event, "event", "create_webrtc_output_result");
		if (err)
			obs_data_set_string(event, "error", err->c_str());

		SendEvent(event);
	}

	void SendRemoteOfferResult(OBSData &original, boost::optional<string> err)
	{
		auto event = original;

		obs_data_set_string(event, "event", "remote_webrtc_offer_result");
		if (err)
			obs_data_set_string(event, "error", err->c_str());

		SendEvent(event);
	}

	void SendGetWebRTCStatsResult(OBSData &original, boost::optional<string> err)
	{
		auto event = original;

		obs_data_set_string(event, "event", "get_webrtc_stats_result");
		if (err)
			obs_data_set_string(event, "error", err->c_str());

		SendEvent(event);
	}

	void SendStopWebRTCOutputResult(OBSData &original, boost::optional<string> err)
	{
		auto event = original;

		obs_data_set_string(event, "event", "stop_webrtc_output_result");
		if (err)
			obs_data_set_string(event, "error", err->c_str());

		SendEvent(event);
	}

	void SendWebRTCSessionDescription(const string &type, const string &sdp)
	{
		auto event = EventCreate("webrtc_session_description");

		obs_data_set_string(event, "type", type.c_str());
		obs_data_set_string(event, "sdp", sdp.c_str());

		SendEvent(event);
	}

	void SendWebRTCIceCandidate(const string &sdp_mid, int sdp_mline_index, const string &sdp)
	{
		auto event = EventCreate("webrtc_ice_candidate");

		obs_data_set_string(event, "sdp_mid", sdp_mid.c_str());
		obs_data_set_int(event, "sdp_mline_index", sdp_mline_index);
		obs_data_set_string(event, "sdp", sdp.c_str());

		SendEvent(event);
	}

	void SendWebRTCResolutionScaled(OutputResolution target, OutputResolution actual)
	{
		auto event = EventCreate("webrtc_resolution_scaled");

		obs_data_set_bool(event, "scaled", target.MinByPixels(actual) != target);
		obs_data_set_int(event, "target_width", target.width);
		obs_data_set_int(event, "target_height", target.height);
		obs_data_set_int(event, "actual_width", actual.width);
		obs_data_set_int(event, "actual_height", actual.height);

		SendEvent(event);
	}
}

namespace AnvilCommands {
	IPCClient anvil_client;
	std::string current_connection;
	recursive_mutex commandMutex;

	DWORD pid;

	atomic<bool> show_welcome = false;
	atomic<bool> recording = false;
	atomic<bool> clipping  = false;
	atomic<bool> using_mic = false;
	atomic<bool> using_ptt = false;
	atomic<bool> mic_muted = false;
	atomic<bool> mic_acquired = false;
	atomic<bool> display_enabled_hotkey = false;
	atomic<bool> streaming = false;
	atomic<bool> screenshotting = false;
	atomic<bool> showingtutorial = false;
	atomic<bool> forward_buffer_in_progress = false;

	atomic<bool> disable_native_indicators = false;

	atomic<bool> show_notifications = false;

	const uint64_t enabled_timeout_seconds = 10;
	atomic<uint64_t> enabled_timeout = 0;

	const uint64_t bookmark_timeout_seconds = 3;
	atomic<uint64_t> bookmark_timeout = 0;

	const uint64_t cache_limit_timeout_seconds = 10;
	atomic<uint64_t> cache_limit_timeout = 0;

	const uint64_t clip_finished_timeout_seconds = 5;
	atomic<uint64_t> clip_finished_timeout = 0;

	const uint64_t screenshot_timeout_seconds = 5;
	atomic<uint64_t> screenshot_timeout = 0;

	const uint64_t stream_timeout_seconds = 3;
	atomic<uint64_t> stream_timeout = 0;

	atomic<uint64_t> forward_buffer_timeout = 0;

	vector<JoiningThread> indicator_updaters;

	string forge_overlay_channel;
	OBSData bookmark_key;
	OBSData highlight_key;
	OBSData quick_clip_key;
	OBSData quick_clip_forward_key;
	OBSData stream_key;
	OBSData start_stop_stream_key;
	OBSData ptt_key;
	OBSData screenshot_key;
	OBSData cancel_key;
	OBSData select_key;
	OBSData accept_key;
	OBSData decline_key;
	OBSData cursor;

	void SendForgeInfo(const char *info=nullptr);
	void SendSettings(obs_data_t *bookmark_key_=nullptr, 
		obs_data_t *highlight_key_ = nullptr, 
		obs_data_t *stream_key_ = nullptr,
		obs_data_t *start_stop_stream_key_ = nullptr,
		obs_data_t *ptt_key_ = nullptr,
		obs_data_t *screenshot_key_ = nullptr,
		obs_data_t *quick_clip_key_ = nullptr,
		obs_data_t *quick_clip_foward_key_ = nullptr,
		obs_data_t *cancel_key_ = nullptr,
		obs_data_t *select_key_ = nullptr,
		obs_data_t *accept_key_ = nullptr,
		obs_data_t *sdecline_key_ = nullptr);
	void SendIndicator();
	void SendCursor(obs_data_t *cmd=nullptr);

	void CreateIndicatorUpdater(uint64_t timeout_seconds, atomic<uint64_t> &timeout_var)
	{
		uint64_t timeout = timeout_var = os_gettime_ns() + timeout_seconds * 1000 * 1000 * 1000;

		auto ev = shared_ptr<void>{ CreateEvent(nullptr, true, false, nullptr), HandleDeleter() };
		auto make_joinable = [ev] { SetEvent(ev.get()); };

		LOCK(commandMutex);
		indicator_updaters.emplace_back();
		auto &jt = indicator_updaters.back();

		jt.make_joinable = make_joinable;
		jt.Run([ev, timeout_seconds, timeout]
		{
			auto res = WaitForSingleObject(ev.get(), static_cast<DWORD>(timeout_seconds * 1000));
			if (res == WAIT_OBJECT_0)
				return;

			while (timeout > os_gettime_ns()) {
				res = WaitForSingleObject(ev.get(), 1000);
				if (res == WAIT_OBJECT_0)
					return;
			}

			QueueOperation(SendIndicator);
		});

		SendIndicator();

		indicator_updaters.erase(remove_if(begin(indicator_updaters), end(indicator_updaters), [](JoiningThread &jt)
		{
			return jt.TryJoin();
		}), end(indicator_updaters));
	}

	bool Connect(DWORD pid_, bool write_failed=false)
	{
		LOCK(commandMutex);

		pid = pid_;

		auto connection_name = ANVIL_PIPE_NAME + to_string(pid_);

		if (!write_failed && current_connection == connection_name && anvil_client) {
			SendIndicator();
			return true;
		}

		if (!anvil_client.Open(connection_name))
			return false;

		current_connection = connection_name;

		SendForgeInfo();
		SendSettings();
		SendCursor();

		if (!show_welcome) {
			SendIndicator();
			return true;
		}

		CreateIndicatorUpdater(enabled_timeout_seconds, enabled_timeout);
		show_welcome = false;

		return true;
	}

	void SendCommand(obs_data_t *cmd)
	{
		if (!cmd)
			return;

		auto data = obs_data_get_json(cmd);
		if (!data)
			return;

		LOCK(commandMutex);
		for (;;) {
			if (anvil_client.Write(data, strlen(data) + 1))
				return;

			if (!Connect(pid, true))
				return;
		}

		blog(LOG_INFO, "anvil_client.Write failed");
	}

	OBSData CommandCreate(const char *cmd)
	{
		auto obj = OBSDataCreate();
		obs_data_set_string(obj, "command", cmd);
		return obj;
	}

	void SendIndicator()
	{
		auto cmd = CommandCreate("indicator");

		obs_data_set_bool(cmd, "show_notifications", show_notifications);

		if (disable_native_indicators)
			goto skip_additional_indicators;

		const char *indicator = recording ? "capturing" : "idle";
		if (recording && using_mic) {
			if (!mic_acquired)
				indicator = "mic_disconnected";
			else
				indicator = mic_muted ? (using_ptt ? "mic_idle" : "mic_muted") : "mic_active";
		}

		if (streaming) {
			indicator = "streaming";

			if (using_mic)
				if (!mic_acquired)
					indicator = "stream_mic_disconnected";
				else
					indicator = mic_muted ? (using_ptt ? "stream_mic_idle" : "stream_mic_muted") : "stream_mic_active";
		}

		if (!disable_native_indicators) {
			if (enabled_timeout >= os_gettime_ns())
				indicator = "enabled";

			if (clipping)
				indicator = "clip_processing";

			if (screenshotting)
				indicator = "screenshot_processing";

			if (forward_buffer_in_progress)
				indicator = "forward_buffer_in_progress";

			if (cache_limit_timeout >= os_gettime_ns())
				indicator = "cache_limit";

			if (clip_finished_timeout >= os_gettime_ns())
				indicator = "clip_processed";

			if (screenshot_timeout >= os_gettime_ns())
				indicator = "screenshot";

			if (bookmark_timeout >= os_gettime_ns())
				indicator = "bookmark";

			if (stream_timeout >= os_gettime_ns())
				indicator = streaming ? "stream_started" : "stream_stopped";

			if (showingtutorial)
				indicator = "first_time_tutorial";
		}

		obs_data_set_string(cmd, "indicator", indicator);

	skip_additional_indicators:;
		SendCommand(cmd);
	}

	void DisableNativeIndicators(bool disable)
	{
		auto cmd = CommandCreate("disable_native_indicators");
		obs_data_set_bool(cmd, "disable_indicators", disable);

		SendCommand(cmd);
	}

	void ResetShowWelcome(bool show=true)
	{
		show_welcome = show;
	}

	void ShowRecording()
	{
		if (recording.exchange(true))
			return;

		SendIndicator();
	}

	void ShowIdle()
	{
		if (!recording.exchange(false))
			return;

		SendIndicator();
	}

	void ShowClipping()
	{
		if (clipping.exchange(true))
			return;

		SendIndicator();
	}

	void ClipFinished(bool success)
	{
		clipping = false;

		if (success)
			CreateIndicatorUpdater(clip_finished_timeout_seconds, clip_finished_timeout);
		else
			SendIndicator();
	}

	void ShowCacheLimitExceeded()
	{
		CreateIndicatorUpdater(cache_limit_timeout_seconds, cache_limit_timeout);
	}

	void ShowBookmark()
	{
		CreateIndicatorUpdater(bookmark_timeout_seconds, bookmark_timeout);
	}

	void ShowScreenshotting()
	{
		if (screenshotting.exchange(true))
			return;

		SendIndicator();
	}

	void ShowFirstTimeTutorial()
	{
		if (showingtutorial.exchange(true))
			return;

		SendIndicator();
	}

	void HideFirstTimeTutorial()
	{
		showingtutorial = false;

		SendIndicator();
	}

	void ShowScreenshot(bool success)
	{
		screenshotting = false;

		if (success)
			CreateIndicatorUpdater(screenshot_timeout_seconds, screenshot_timeout);
		else
			SendIndicator();
	}

	void HotkeyMatches(bool matches)
	{
		bool changed = display_enabled_hotkey != matches;
		display_enabled_hotkey = matches;

		if (changed)
			SendIndicator();
	}

	void MicUpdated(boost::tribool muted, boost::tribool active=boost::indeterminate, boost::tribool ptt=boost::indeterminate, boost::tribool acquired=boost::indeterminate)
	{
		bool changed = false;
		if (!boost::indeterminate(active))
			changed = active != using_mic.exchange(active);
		if (!boost::indeterminate(muted))
			changed = (muted != mic_muted.exchange(muted)) || changed;
		if (!boost::indeterminate(ptt))
			changed = (ptt != using_ptt.exchange(ptt)) || changed;
		if (!boost::indeterminate(acquired))
			changed = (acquired != mic_acquired.exchange(acquired)) || changed;

		if (!changed)
			return;

		if (changed && !boost::indeterminate(muted) && using_ptt) {
			ForgeEvents::SendPTTStatus(!muted);
		}

		SendIndicator();
	}

	ProtectedObject<string> last_message;
	void UpdateForwardBufferIndicator(string message)
	{
		auto lm = last_message.Lock();
		if (*lm == message)
			return;

		*lm = message;
		auto cmd = CommandCreate("update_forward_buffer_indicator");

		if (!message.empty())
			obs_data_set_string(cmd, "text", message.c_str());

		LOCK(commandMutex);

		SendCommand(cmd);
	}

	void ForwardBufferInProgress(bool in_progress, double timeout_ = 0.);
	shared_ptr<void> forward_buffer_update_timer;
	void AddForwardBufferTimer()
	{
		AddWaitHandleCallback(forward_buffer_update_timer.get(), []
		{
			auto time = os_gettime_ns();
			if (forward_buffer_timeout < time) {
				ShowClipping();
				ForwardBufferInProgress(false);
				return;
			}

			auto remaining = forward_buffer_timeout - time;
			UpdateForwardBufferIndicator(to_string(static_cast<int>(ceil(remaining / 1000000000.))));
			AddForwardBufferTimer();
		});
	}

	void ForwardBufferInProgress(bool in_progress, double timeout_)
	{
		if (forward_buffer_update_timer) {
			RemoveWaitHandle(forward_buffer_update_timer.get());
			CancelWaitableTimer(forward_buffer_update_timer.get());
		}

		if (forward_buffer_in_progress.exchange(in_progress) != in_progress)
			SendIndicator();

		if (!in_progress)
			return;

		forward_buffer_timeout = os_gettime_ns() + timeout_ * 1000000000ULL;

		forward_buffer_update_timer = shared_ptr<void>(CreateWaitableTimer(nullptr, false, nullptr), HandleDeleter{});
		LARGE_INTEGER timeout = { 0 };
		timeout.QuadPart = -1000000LL; // 100 ms
		SetWaitableTimer(forward_buffer_update_timer.get(), &timeout, 100, nullptr, nullptr, false);

		AddForwardBufferTimer();
	}

	void SendForgeInfo(const char *info)
	{
		LOCK(commandMutex);

		if (info && info[0])
			forge_overlay_channel = info;

		auto cmd = CommandCreate("forge_info");

		obs_data_set_string(cmd, "anvil_event", forge_overlay_channel.c_str());

		SendCommand(cmd);
	}

	void SendSettings(obs_data_t *bookmark_key_, obs_data_t *highlight_key_, obs_data_t *stream_key_, obs_data_t *start_stop_stream_key_, obs_data_t *ptt_key_,
		obs_data_t *screenshot_key_, obs_data_t *quick_clip_key_, obs_data_t *quick_clip_forward_key_,
		obs_data_t *cancel_key_, obs_data_t *select_key_, obs_data_t *accept_key_, obs_data_t *decline_key_)
	{
		auto cmd = CommandCreate("update_settings");

		LOCK(commandMutex);

		if (bookmark_key_)
			bookmark_key = bookmark_key_;
		if (highlight_key_)
			highlight_key = highlight_key_;
		if (quick_clip_key_)
			quick_clip_key = quick_clip_key_;
		if (quick_clip_forward_key_)
			quick_clip_forward_key = quick_clip_forward_key_;

		if (stream_key_)
			stream_key = stream_key_;
		if (start_stop_stream_key_)
			start_stop_stream_key = start_stop_stream_key_;

		if (ptt_key_)
			ptt_key = ptt_key_;
		if (screenshot_key_)
			screenshot_key = screenshot_key_;

		if (cancel_key_)
			cancel_key = cancel_key_;
		if (select_key_)
			select_key = select_key_;
		if (accept_key_)
			accept_key = accept_key_;
		if (decline_key_)
			decline_key = decline_key_;

		if (bookmark_key)
			obs_data_set_obj(cmd, "bookmark_key", bookmark_key);
		if (highlight_key)
			obs_data_set_obj(cmd, "highlight_key", highlight_key);
		if (stream_key)
			obs_data_set_obj(cmd, "stream_key", stream_key);
		if (start_stop_stream_key)
			obs_data_set_obj(cmd, "start_stop_stream_key", start_stop_stream_key);
		if (start_stop_stream_key)
			obs_data_set_obj(cmd, "ptt_key", ptt_key);
		if (start_stop_stream_key)
			obs_data_set_obj(cmd, "screenshot_key", screenshot_key);
		if (quick_clip_key)
			obs_data_set_obj(cmd, "quick_clip_key", quick_clip_key);
		if (quick_clip_forward_key)
			obs_data_set_obj(cmd, "quick_clip_forward_key", quick_clip_forward_key);
		if (cancel_key)
			obs_data_set_obj(cmd, "cancel_key", cancel_key);
		if (select_key)
			obs_data_set_obj(cmd, "select_key", select_key);
		if (accept_key)
			obs_data_set_obj(cmd, "accept_key", accept_key);
		if (decline_key)
			obs_data_set_obj(cmd, "decline_key", decline_key);

		SendCommand(cmd);

		AnvilCommands::DisableNativeIndicators(disable_native_indicators);
	}

	void SendCursor(obs_data_t *cmd)
	{
		LOCK(commandMutex);

		if (cmd)
			cursor = cmd;

		if (cursor)
			SendCommand(cursor);
	}

	void DismissOverlay(OBSData &data)
	{
		auto cmd = CommandCreate("dismiss_overlay");

		if (obs_data_has_user_value(data, "name"))
			obs_data_set_string(cmd, "name", obs_data_get_string(data, "name"));

		LOCK(commandMutex);

		SendCommand(cmd);
	}

	void DismissQuickSelect()
	{
		auto cmd = CommandCreate("dismiss_quick_select");

		LOCK(commandMutex);

		SendCommand(cmd);
	}

	void BeginQuickSelectTimeout(uint32_t timeout_ms)
	{
		auto cmd = CommandCreate("begin_quick_select_timeout");

		obs_data_set_int(cmd, "timeout_ms", timeout_ms);

		LOCK(commandMutex);

		SendCommand(cmd);
	}

	void StreamStatus(bool streaming_)
	{
		auto cmd = CommandCreate("stream_status");

		streaming.exchange(streaming_);
		obs_data_set_bool(cmd, "streaming", streaming_);

		{
			LOCK(commandMutex);
			SendCommand(cmd);
		}

		CreateIndicatorUpdater(stream_timeout_seconds, stream_timeout);
	}

	void ShowNotifications(bool show)
	{
		show_notifications = show;
		SendIndicator();
	}

	void SendSharedTextureIncompatible(obs_data_t *data)
	{
		auto cmd = CommandCreate("shared_texture_incompatible");

		obs_data_set_string(cmd, "name", obs_data_get_string(data, "name"));
		obs_data_set_int(cmd, "shared_handle", obs_data_get_int(data, "shared_handle"));
		obs_data_set_int(cmd, "luid_low", obs_data_get_int(data, "luid_low"));
		obs_data_set_int(cmd, "luid_high", obs_data_get_int(data, "luid_high"));

		SendCommand(cmd);
	}
}

static const struct {
	uint32_t alignment;
	string name;
} alignment_names[] = {
#define ALIGNMENT_(x) { OBS_ALIGN_ ## x, #x }
	ALIGNMENT_(CENTER),
	ALIGNMENT_(LEFT),
	ALIGNMENT_(RIGHT),
	ALIGNMENT_(TOP),
	ALIGNMENT_(BOTTOM),
#undef ALIGNMENT_
};

static uint32_t OBSDataGetAlignment(obs_data_t *data, const char *name)
{
	auto alignment = OBSDataGetObj(data, name);
	auto item = obs_data_first(alignment);
	DEFER {
		obs_data_item_release(&item);
	};

	uint32_t result = 0;
	for (; item; obs_data_item_next(&item)) {
		string name = obs_data_item_get_name(item);
		std::transform(begin(name), end(name), begin(name), toupper);
		for (auto &item : alignment_names)
			if (name == item.name)
				result |= item.alignment;
	}

	return result;
}

static OBSData OBSDataFromAlignment(uint32_t alignment)
{
	auto result = OBSDataCreate();
	if (!alignment) {
		obs_data_set_int(result, "center", OBS_ALIGN_CENTER);
		return result;
	}

	for (auto &item : alignment_names)
		if (alignment & item.alignment) {
			auto name = item.name;
			std::transform(begin(name), end(name), begin(name), tolower);
			obs_data_set_int(result, name.c_str(), item.alignment);
		}

	return result;
}

static const struct {
	obs_bounds_type type;
	string name;
} bounds_names[] = {
#define BOUNDS_(x) { OBS_BOUNDS_ ## x, #x }
	BOUNDS_(NONE),
	BOUNDS_(STRETCH),
	BOUNDS_(SCALE_INNER),
	BOUNDS_(SCALE_OUTER),
	BOUNDS_(SCALE_TO_WIDTH),
	BOUNDS_(SCALE_TO_HEIGHT),
	BOUNDS_(MAX_ONLY),
#undef BOUNDS_
};

static obs_bounds_type OBSDataGetBoundsType(obs_data_t *data, const char *name)
{
	string bounds = obs_data_get_string(data, name);
	std::transform(begin(bounds), end(bounds), begin(bounds), toupper);
	for (auto &item : bounds_names)
		if (bounds == item.name)
			return item.type;

	return OBS_BOUNDS_NONE;
}

static void OBSDataSetBoundsType(obs_data_t *data, const char *name_, obs_bounds_type type)
{
	auto result = OBSDataCreate();
	for (auto &bounds : bounds_names)
		if (bounds.type == type) {
			auto name = bounds.name;
			std::transform(begin(name), end(name), begin(name), tolower);
			obs_data_set_string(data, name_, name.c_str());
			return;
		}

	obs_data_set_string(data, name_, "none");
}

static uint32_t AlignX264Height(uint32_t height)
{
	// We're currently using NV12, so height has to be a multiple of 2, see:
	// http://git.videolan.org/?p=x264.git;a=blob;f=encoder/encoder.c;hb=a01e33913655f983df7a4d64b0a4178abb1eb618#l502
	return (height + 1) & ~static_cast<uint32_t>(1);
}

static uint32_t gcd(uint32_t a, uint32_t b)
{
	if (!a) return b;
	if (!b) return a;

	uint32_t c = a;
	a = max(a, b);
	b = min(c, b);

	uint32_t remainder;
	do
	{
		remainder = a % b;
		a = b;
		b = remainder;
	} while (remainder);

	return a;
}

static boost::optional<OutputResolution> ScaleResolutionInteger(const OutputResolution &target, const OutputResolution &source, OutputResolution max_dimensions)
{
	auto aspect_segments = gcd(source.width, source.height);
	auto aspect_width = source.width / aspect_segments;
	auto aspect_height = source.height / aspect_segments;

	auto pixel_ratio = min(target.pixels() / static_cast<double>(source.pixels()), 1.0);
	auto target_aspect_segments = static_cast<uint32_t>(floor(sqrt(pixel_ratio * aspect_segments * aspect_segments)));

	if (target_aspect_segments + 1 * aspect_width > max_dimensions.width)
		target_aspect_segments = max_dimensions.width / aspect_width - 1;
	if (target_aspect_segments + 1 * aspect_height > max_dimensions.height)
		target_aspect_segments = max_dimensions.height / aspect_height - 1;

	for (auto i : { 0, 1, -1 }) {
		auto target_segments = max(static_cast<uint32_t>(1), min(aspect_segments, target_aspect_segments + i));
		OutputResolution res{ aspect_width * target_segments, aspect_height * target_segments };

		if (res.width > source.width || res.height > source.height)
			continue;

		auto ratio = static_cast<float>(res.pixels()) / target.pixels();
		if (ratio < 0.9 || ratio > 1.1)
			continue;
		
		if (res.width % 4 == 0 && res.height % 2 == 0) //libobs enforces multiple of 4 width and multiple of 2 height
			return res;
	}

	return boost::none;
}

OutputResolution ScaleResolution(const OutputResolution &target, const OutputResolution &source, OutputResolution max_dimensions = { numeric_limits<uint32_t>::max(), numeric_limits<uint32_t>::max() })
{
	{
		auto res = ScaleResolutionInteger(target, source, max_dimensions);
		if (res)
			return *res;
	}

	auto pixel_ratio_sqrt = sqrt(min(target.pixels() / static_cast<double>(source.pixels()), 1.0));
	if (pixel_ratio_sqrt * source.width > max_dimensions.width)
		pixel_ratio_sqrt = max_dimensions.width / static_cast<double>(source.width);
	if (pixel_ratio_sqrt * source.height > max_dimensions.height)
		pixel_ratio_sqrt = max_dimensions.height / static_cast<double>(source.height);
	OutputResolution res{
		static_cast<uint32_t>(source.width * pixel_ratio_sqrt),
		static_cast<uint32_t>(source.height * pixel_ratio_sqrt)
	};

	//libobs enforces multiple of 4 width and multiple of 2 height
	res.width &= ~3;
	res.height &= ~1;

	return res;
}

template <typename T, typename U>
static void InitRef(T &ref, const char *msg, void (*release)(U*), U *val)
{
	if (!val)
		throw msg;

	ref = val;
	release(val);
}

static void ApplyTransforms(obs_sceneitem_t *item, obs_data_t *properties)
{
	if (!item || !properties)
		return;

	vec2 vec;
	obs_data_get_vec2(properties, "pos", &vec);
	obs_sceneitem_set_pos(item, &vec);

	obs_sceneitem_set_rot(item, static_cast<float>(obs_data_get_double(properties, "rot")));

	obs_data_get_vec2(properties, "scale", &vec);
	obs_sceneitem_set_scale(item, &vec);

	obs_sceneitem_set_alignment(item, OBSDataGetAlignment(properties, "alignment"));


	obs_sceneitem_set_bounds_type(item, OBSDataGetBoundsType(properties, "bounds_type"));

	obs_sceneitem_set_bounds_alignment(item, OBSDataGetAlignment(properties, "bounds_alignment"));

	obs_data_get_vec2(properties, "bounds", &vec);
	obs_sceneitem_set_bounds(item, &vec);

	obs_sceneitem_set_visible(item, !obs_data_get_bool(properties, "invisible"));
}

static OBSData ExtractTransforms(obs_sceneitem_t *item, obs_data_t *prior)
{
	if (!item)
		return prior;

	auto properties = OBSDataCreate();

	vec2 vec;
	obs_sceneitem_get_pos(item, &vec);
	obs_data_set_vec2(properties, "pos", &vec);

	obs_data_set_double(properties, "rot", obs_sceneitem_get_rot(item));

	obs_sceneitem_get_scale(item, &vec);
	obs_data_set_vec2(properties, "scale", &vec);

	obs_data_set_obj(properties, "alignment", OBSDataFromAlignment(obs_sceneitem_get_alignment(item)));


	OBSDataSetBoundsType(properties, "bounds_type", obs_sceneitem_get_bounds_type(item));

	obs_data_set_obj(properties, "bounds_alignment", OBSDataFromAlignment(obs_sceneitem_get_bounds_alignment(item)));

	obs_sceneitem_get_bounds(item, &vec);
	obs_data_set_vec2(properties, "bounds", &vec);

	obs_data_set_bool(properties, "invisible", !obs_sceneitem_visible(item));

	return properties;
}

struct CrucibleContext {
	mutex bookmarkMutex;
	vector<Bookmark> estimatedBookmarks;
	vector<Bookmark> bookmarks;
	vector<Bookmark> estimatedBufferBookmarks;
	vector<Bookmark> bufferBookmarks;
	int next_bookmark_id = 0;

	uint64_t recordingStartTime = 0;
	
	struct {
		OBSScene scene;
		OBSSceneItem game, webcam, theme;
		OBSData game_data, webcam_data, theme_data;

		void MakePresentable()
		{
			ApplyTransforms(game, game_data);
			ApplyTransforms(webcam, webcam_data);
			ApplyTransforms(theme, theme_data);

			obs_sceneitem_set_order(theme, OBS_ORDER_MOVE_TOP);
			obs_sceneitem_set_order(game, OBS_ORDER_MOVE_BOTTOM);
		}
	} game_and_webcam;

	struct {
		OBSScene scene;
		OBSSceneItem window, webcam, theme;
		OBSData window_data, webcam_data, theme_data;
		
		void MakePresentable()
		{
			ApplyTransforms(window, window_data);
			ApplyTransforms(webcam, webcam_data);
			ApplyTransforms(theme, theme_data);

			obs_sceneitem_set_order(theme, OBS_ORDER_MOVE_TOP);
			obs_sceneitem_set_order(window, OBS_ORDER_MOVE_BOTTOM);
		}
	} window_and_webcam;

	struct {
		OBSScene scene;
		OBSSceneItem wallpaper, webcam, theme;
		OBSData wallpaper_data, webcam_data, theme_data;

		void MakePresentable()
		{
			ApplyTransforms(wallpaper, wallpaper_data);
			ApplyTransforms(webcam, webcam_data);
			ApplyTransforms(theme, theme_data);

			obs_sceneitem_set_order(theme, OBS_ORDER_MOVE_TOP);
			obs_sceneitem_set_order(wallpaper, OBS_ORDER_MOVE_BOTTOM);
		}
	} wallpaper_and_webcam;

	struct {
		OBSScene scene;
		OBSSceneItem webcam, theme;
		OBSData webcam_data, theme_data;

		void MakePresentable()
		{
			ApplyTransforms(webcam, webcam_data);
			ApplyTransforms(theme, theme_data);

			obs_sceneitem_set_order(theme, OBS_ORDER_MOVE_TOP);
		}
	} webcam_and_theme;

	unordered_set<string> disallowed_hardware_encoders;

	OBSData buffer_settings;

	obs_video_info ovi;
	uint32_t fps_den;
	std::string webcam_device;
	OBSSource tunes, mic, gameCapture, webcam, theme, window, wallpaper, audioBuffer, gameWindow;
	OBSSourceSignal micMuted, pttActive, micAcquired;
	OBSSourceSignal stopCapture, startCapture, injectFailed, injectRequest, monitorProcess, screenshotSaved, processInaccessible;
	OBSEncoder h264, aac, stream_h264, recordingStream_h264, recordingStream_aac;
	string filename = "";
	string profiler_filename = "";
	string muxerSettings = "";
	OBSOutput output, buffer, stream, recordingStream, webrtc;
	OBSOutputSignal startRecording, stopRecording;
	OBSOutputSignal sentTrackedFrame, bufferSentTrackedFrame;
	OBSOutputSignal bufferSaved, bufferSaveFailed;
	OBSOutputSignal recordingStreamStart, recordingStreamStop;
	OBSOutputSignal webrtcSessionDescription, webrtcICECandidate, webrtcResolution, webrtcTexture;
	bool record_audio_buffer_only = false;
	bool check_audio_streams = false;
	shared_ptr<void> check_audio_streams_timer;

	unique_ptr<obs_volmeter_t> tunesMeter;
	OBSSignal tunesLevelsUpdated;

	unique_ptr<obs_volmeter_t> micMeter;
	OBSSignal micLevelsUpdated;

	OutputResolution recording_resolution_limit{ numeric_limits<uint32_t>::max(), numeric_limits<uint32_t>::max() }; // maximum per dimension limits
	OutputResolution target = OutputResolution{ 1280, 720 }; //thanks VS2013
	uint32_t target_bitrate = 3500;
	uint32_t target_fps = 30;

	OutputResolution game_res = OutputResolution{ 0, 0 };
	bool sli_compatibility = false;

	boost::optional<DWORD> game_pid;
	shared_ptr<void> game_process;
	bool recording_game = false;
	boost::optional<int> game_start_bookmark_id, game_end_bookmark_id;

	boost::optional<chrono::steady_clock::time_point> game_capture_start_time;

	obs_hotkey_id ptt_hotkey_id = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id mute_hotkey_id = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id unmute_hotkey_id = OBS_INVALID_HOTKEY_ID;

	obs_hotkey_id custom_ptt_hotkey_id = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id bookmark_hotkey_id = OBS_INVALID_HOTKEY_ID;

	struct obs_service_info forge_streaming_service;
	OBSService stream_service;
	bool streaming = false;
	bool recording_stream = false;
	bool stream_active = false;
	OutputResolution target_stream = OutputResolution{ 1280, 720 };
	uint32_t target_stream_bitrate = 3000;
	OBSOutputSignal startStreaming, stopStreaming;
	obs_source_t *streaming_source = nullptr;

	vector<pair<string, long long>> requested_screenshots;

	decltype(ProfileSnapshotCreate()) game_session_snapshot;

	bool ResetVideo()
	{
		uint32_t old_fps = ovi.fps_num;
		ovi.fps_num = target_fps;
		if (obs_reset_video(&ovi)) {
			ovi.fps_num = old_fps;
			return false;
		}

		ForgeEvents::SendCanvasSize(ovi.base_width, ovi.base_height);
		return true;
	}

	void InitLibobs(bool standalone)
	{
		ovi.adapter = 0;
		ovi.base_width = 1280;
		ovi.base_height = 720;
		ovi.fps_num = 30;
		ovi.fps_den = fps_den = 1;
		ovi.graphics_module = "libobs-d3d11.dll";

		if (!ResetVideo())
			throw "Couldn't initialize video";

		obs_audio_info ai;
		ai.samples_per_sec = 48000;
		ai.speakers = SPEAKERS_STEREO;
		ai.max_buffer_ms = 1000;
		if (!obs_reset_audio(&ai))
			throw "Couldn't initialize audio";

		RegisterAudioBufferSource();
		RegisterFramebufferSource();
		RegisterNVENCEncoder();
#ifdef WEBRTC_WIN
		RegisterWebRTCOutput();
		
		blog(LOG_INFO, "WebRTC NVENC %savailable", WebRTCNVENCAvailable() ? "" : "not ");
#endif

		if (standalone)
		{
			DStr obs_path = GetModulePath(/*&obs_startup*/ "obs.dll");
			DStr bin_path, data_path;
			dstr_printf(bin_path, "%s../../obs-plugins/" BIT_STRING, obs_path->array);
			dstr_printf(data_path, "%s../../data/obs-plugins/%%module%%", obs_path->array);
			obs_add_module_path(bin_path, data_path);
		}

		obs_load_all_modules();
	}

	void UpdateSourceAudioSettings()
	{
		if (record_audio_buffer_only && output && obs_output_active(output) && !check_audio_streams && !check_audio_streams_timer) {
			check_audio_streams_timer.reset(CreateWaitableTimer(nullptr, true, nullptr), HandleDeleter{});
			LARGE_INTEGER timeout = { 0 };
			timeout.QuadPart = -50000000LL; // 5 seconds
			SetWaitableTimer(check_audio_streams_timer.get(), &timeout, 0, nullptr, nullptr, false);

			AddWaitHandleCallback(check_audio_streams_timer.get(), [=, timer = check_audio_streams_timer]
			{
				check_audio_streams = true;
				UpdateSourceAudioSettings();
			});
		}

		auto buffer_only = [&]
		{
			if (!record_audio_buffer_only)
				return false;

			if (!check_audio_streams || !audioBuffer)
				return true;

			auto proc = obs_source_get_proc_handler(audioBuffer);

			uint8_t buf[100];
			calldata_t data;
			calldata_init_fixed(&data, buf, sizeof(buf));

			proc_handler_call(proc, "num_audio_streams", &data);

			return calldata_int(&data, "num") != 0;
		}();

		obs_source_set_audio_mixers(tunes, buffer_only ? 0 : (1 << 0));
		obs_source_set_audio_mixers(audioBuffer, (1 << 1) | (buffer_only ? (1 << 0) : 0));

		bool recordingStream_active = recordingStream && obs_output_active(recordingStream);
		bool webrtc_active = webrtc && obs_output_active(webrtc);
		auto audio_buffer_muted = obs_source_muted(tunes) || (!buffer_only && !recordingStream_active && !webrtc_active);
		obs_source_set_muted(audioBuffer, audio_buffer_muted);
		ForgeEvents::SendAudioBufferMuted(audio_buffer_muted);
	}

	void InitSources()
	{
		auto settings = OBSDataCreate();
		obs_data_set_string(settings, "device_id", "<disabled>");

		InitRef(mic, "Couldn't create audio input device source", obs_source_release,
			obs_source_create(OBS_SOURCE_TYPE_INPUT, "wasapi_input_capture", "wasapi mic", settings, nullptr));

		obs_source_set_audio_mixers(mic, (1 << 0));

		auto weak_mic = OBSGetWeakRef(mic);
		OBSEnumHotkeys([&](obs_hotkey_id id, obs_hotkey_t *key)
		{
			if (obs_hotkey_get_registerer_type(key) != OBS_HOTKEY_REGISTERER_SOURCE)
				return;

			if (obs_hotkey_get_registerer(key) != weak_mic)
				return;

			auto name = obs_hotkey_get_name(key);
			if (name == string("libobs.mute"))
				mute_hotkey_id = id;
			else if (name == string("libobs.unmute"))
				unmute_hotkey_id = id;
			else if (name == string("libobs.push-to-talk"))
				ptt_hotkey_id = id;
		});

		// create audio source
		InitRef(tunes, "Couldn't create audio input source", obs_source_release,
				obs_source_create(OBS_SOURCE_TYPE_INPUT, "wasapi_output_capture", "wasapi loopback", nullptr, nullptr));

		obs_set_output_source(1, tunes);
		obs_source_set_audio_mixers(tunes, (1 << 0));
		
		tunesMeter = OBSVolMeterCreate(OBS_FADER_LOG);
		obs_volmeter_set_update_interval(tunesMeter.get(), 100);

		micMeter = OBSVolMeterCreate(OBS_FADER_LOG);
		obs_volmeter_set_update_interval(micMeter.get(), 100);

		InitRef(game_and_webcam.scene, "Couldn't create game_and_webcam scene", obs_scene_release,
				obs_scene_create("game_and_webcam"));

		InitRef(window_and_webcam.scene, "Couldn't create window_and_webcam scene", obs_scene_release,
				obs_scene_create("window_and_webcam"));

		InitRef(wallpaper_and_webcam.scene, "Couldn't create wallpaper_and_webcam scene", obs_scene_release,
			obs_scene_create("wallpaper_and_webcam"));

		InitRef(webcam_and_theme.scene, "Couldn't create webcam_and_theme scene", obs_scene_release,
			obs_scene_create("webcam_and_theme"));

		InitRef(theme, "Couldn't create theme source", obs_source_release,
				obs_source_create(OBS_SOURCE_TYPE_INPUT, "FramebufferSource", "theme overlay", nullptr, nullptr));

		InitRef(wallpaper, "Couldn't create wallpaper source", obs_source_release,
				obs_source_create(OBS_SOURCE_TYPE_INPUT, "FramebufferSource", "wallpaper", nullptr, nullptr));

		game_and_webcam.theme = obs_scene_add(game_and_webcam.scene, theme);
		window_and_webcam.theme = obs_scene_add(window_and_webcam.scene, theme);
		wallpaper_and_webcam.theme = obs_scene_add(wallpaper_and_webcam.scene, theme);
		webcam_and_theme.theme = obs_scene_add(webcam_and_theme.scene, theme);

		wallpaper_and_webcam.wallpaper = obs_scene_add(wallpaper_and_webcam.scene, wallpaper);

		obs_enter_graphics();
		auto graphics_adapter_luid = reinterpret_cast<const LUID*>(gs_get_device_luid());
		obs_leave_graphics();

		auto connect_framebuffer = [&](obs_source_t *source, const char *connection)
		{
			auto proc = obs_source_get_proc_handler(source);
			calldata_t data = {};
			proc_handler_call(proc, "get_server_name", &data);

			if (auto name = calldata_string(&data, "name")) {
				ForgeEvents::SendFramebufferConnectionInfo(connection, name, graphics_adapter_luid);
			} else {
				blog(LOG_WARNING, "CrucibleContext::InitSources: failed to get framebuffer name for %s", connection);
			}
		};

		connect_framebuffer(theme, "theme");
		connect_framebuffer(wallpaper, "wallpaper");


		auto init_display = [&](const char *display, auto &container)
		{
			Display::SetSource(display, obs_scene_get_source(container.scene));
		};

		init_display("game", game_and_webcam);
		init_display("window", window_and_webcam);
		init_display("wallpaper", wallpaper_and_webcam);
		init_display("webcam", webcam_and_theme);
	}

	OBSData CreateH264EncoderSettings(const string &id, uint32_t bitrate, bool stream_compatible = false, OutputResolution *max_encoder_resolution = nullptr)
	{
		auto vsettings = OBSDataCreate();

		if (max_encoder_resolution)
			*max_encoder_resolution = { numeric_limits<uint32_t>::max(), numeric_limits<uint32_t>::max() };

		if (id == "obs_x264"s) {
			if (!stream_compatible) {
				obs_data_set_int(vsettings, "bitrate", 0);
				obs_data_set_int(vsettings, "buffer_size", 2 * bitrate);
				obs_data_set_int(vsettings, "crf", 23);
				obs_data_set_bool(vsettings, "use_bufsize", true);
				obs_data_set_bool(vsettings, "cbr", false);
				obs_data_set_string(vsettings, "profile", "high");
				obs_data_set_string(vsettings, "preset", "veryfast");
				obs_data_set_double(vsettings, "keyint_sec", 1);

				ostringstream os;
				os << "vbv-maxrate=" << bitrate;
				obs_data_set_string(vsettings, "x264opts", os.str().c_str());

			} else {
				obs_data_set_int(vsettings, "bitrate", bitrate);
				obs_data_set_int(vsettings, "crf", 23);
				obs_data_set_bool(vsettings, "cbr", true);
				obs_data_set_string(vsettings, "profile", "high");
				obs_data_set_string(vsettings, "preset", "veryfast");
				obs_data_set_double(vsettings, "keyint_sec", 1);
			}

		} else if (id == "crucible_nvenc"s) {
			if (!stream_compatible) {
				obs_data_set_int(vsettings, "bitrate", 2 * bitrate);
				obs_data_set_string(vsettings, "profile", "high");
				obs_data_set_double(vsettings, "keyint_sec", 1);

			} else {
				obs_data_set_int(vsettings, "bitrate", 2 * bitrate);
				obs_data_set_string(vsettings, "rate_control", "cbr");
				obs_data_set_string(vsettings, "profile", "high");
				obs_data_set_double(vsettings, "keyint_sec", 1);
				obs_data_set_bool(vsettings, "dynamic_bitrate", true);
			}

		} else if (id == "ffmpeg_nvenc"s) {
			if (!stream_compatible) {
				obs_data_set_int(vsettings, "bitrate", 2 * bitrate);
				obs_data_set_bool(vsettings, "cbr", false);
				obs_data_set_string(vsettings, "profile", "high");
				obs_data_set_double(vsettings, "keyint_sec", 1);

			} else {
				obs_data_set_int(vsettings, "bitrate", 2 * bitrate);
				obs_data_set_bool(vsettings, "cbr", true);
				obs_data_set_string(vsettings, "profile", "high");
				obs_data_set_double(vsettings, "keyint_sec", 1);
			}

		} else if (id == "obs_qsv11"s) {
			if (!stream_compatible) {
				obs_data_set_int(vsettings, "bitrate", 2 * bitrate);
				obs_data_set_int(vsettings, "max_bitrate", 2 * bitrate);
				obs_data_set_bool(vsettings, "cbr", false);
				obs_data_set_string(vsettings, "profile", "high");
				obs_data_set_double(vsettings, "keyint_sec", 1);

				auto props = obs_get_encoder_properties(id.c_str());
				DEFER{ obs_properties_destroy(props); };

				{
					auto best = "VBR"s;
					auto rcs = obs_properties_get(props, "rate_control");
					auto size = obs_property_list_item_count(rcs);
					for (size_t i = 0; i < size; i++) {
						if (obs_property_list_item_disabled(rcs, i))
							continue;

						auto rc = obs_property_list_item_string(rcs, i);
						if (rc == "AVBR"s) {
							best = rc;

						} else if (rc == "LA"s) {
							best = rc;
							break;
						}
					}

					if (max_encoder_resolution) {
						auto width = obs_properties_get(props, "max_width");
						if (width)
							max_encoder_resolution->width = obs_property_int_max(width);

						auto height = obs_properties_get(props, "max_height");
						if (height)
							max_encoder_resolution->height = obs_property_int_max(height);

						blog(LOG_INFO, "QSV max resolution: %ux%u", max_encoder_resolution->width, max_encoder_resolution->height);
					}

					obs_data_set_string(vsettings, "rate_control", best.c_str());
				}

			} else {
				obs_data_set_int(vsettings, "bitrate", 2 * bitrate);
				obs_data_set_bool(vsettings, "cbr", true);
				obs_data_set_string(vsettings, "profile", "high");
				obs_data_set_double(vsettings, "keyint_sec", 1);

				if (max_encoder_resolution) {
					auto props = obs_get_encoder_properties(id.c_str());
					DEFER{ obs_properties_destroy(props); };

					auto width = obs_properties_get(props, "max_width");
					if (width)
						max_encoder_resolution->width = obs_property_int_max(width);

					auto height = obs_properties_get(props, "max_height");
					if (height)
						max_encoder_resolution->height = obs_property_int_max(height);

					blog(LOG_INFO, "QSV max resolution: %ux%u", max_encoder_resolution->width, max_encoder_resolution->height);
				}
			}

		} else if (id == "amd_amf_h264"s) {
			if (!stream_compatible) {
				obs_data_set_int(vsettings, "bitrate", 2 * bitrate);
				obs_data_set_string(vsettings, "profile", "high");
				obs_data_set_double(vsettings, "keyint_sec", 1);

				obs_data_set_int(vsettings, "AMF.H264.Profile", 100); // VCEProfile_High
				obs_data_set_int(vsettings, "AMF.H264.RateControlMethod", 1); // VCERateControlMethod_ConstantBitrate
				obs_data_set_int(vsettings, "AMF.H264.Bitrate.Target", 2 * bitrate);
				obs_data_set_int(vsettings, "AMF.H264.EnforceHRDCompatibility", 0);
				obs_data_set_double(vsettings, "AMF.H264.KeyframeInterval", 2.0); // 1.0 seems to produce a keyframe interval of .5 seconds
				obs_data_set_int(vsettings, "AMF.H264.FillerData", 0);

			} else {
				obs_data_set_int(vsettings, "bitrate", 2 * bitrate);
				obs_data_set_string(vsettings, "profile", "high");
				obs_data_set_double(vsettings, "keyint_sec", 0.5);

				obs_data_set_int(vsettings, "AMF.H264.Profile", 100); // VCEProfile_High
				obs_data_set_int(vsettings, "AMF.H264.RateControlMethod", 1); // VCERateControlMethod_ConstantBitrate
				obs_data_set_int(vsettings, "AMF.H264.Bitrate.Target", 2 * bitrate);
				obs_data_set_int(vsettings, "AMF.H264.EnforceHRDCompatibility", 0);
				obs_data_set_double(vsettings, "AMF.H264.KeyframeInterval", 2.0); // 1.0 seems to produce a keyframe interval of .5 seconds
				obs_data_set_int(vsettings, "AMF.H264.FillerData", 1);
			}
		}

		return vsettings;
	}

	void CreateH264Encoder(OBSEncoder *enc = nullptr, uint32_t *bitrate = nullptr, bool stream_compatible = false, const char *last_encoder_id = nullptr)
	{
		if (!enc)
			enc = &h264;
		if (!bitrate)
			bitrate = &target_bitrate;

		auto vsettings = OBSDataCreate();

		auto encoder_available = [&](const decltype(allowed_hardware_encoder_names[0]) &ahe)
		{
			size_t i = 0;
			const char *id;
			while (obs_enum_encoder_types(i++, &id))
				if (id && ahe.first == id)
					return true;

			return false;
		};

		auto create_encoder = [&](const decltype(allowed_hardware_encoder_names[0]) &info)
		{
			try {
				InitRef(*enc, ("Couldn't create " + info.second + " video encoder").c_str(), obs_encoder_release,
					obs_video_encoder_create(info.first.c_str(), (info.first + " video").c_str(), vsettings, nullptr));
				return true;
			} catch (const char*) {
				return false;
			}
		};

		DEFER
		{
			obs_encoder_set_video(*enc, obs_get_video());
		};

		bool last_encoder_found = false;
		for (const auto &ahe : allowed_hardware_encoder_names) {
			if (disallowed_hardware_encoders.find(ahe.first) != end(disallowed_hardware_encoders))
				continue;

			if (last_encoder_id && !last_encoder_found) {
				last_encoder_found = ahe.first == last_encoder_id;
				continue;
			}

			if (!encoder_available(ahe))
				continue;

			if (stream_compatible && !obs_can_encoder_update(ahe.first.c_str()))
				continue;
			
			vsettings = CreateH264EncoderSettings(ahe.first, *bitrate, stream_compatible, !stream_compatible ? &recording_resolution_limit : nullptr);

			if (create_encoder(ahe))
				return;
		}

		InitRef(*enc, "Couldn't create video encoder", obs_encoder_release,
			obs_video_encoder_create("obs_x264", "x264 video", CreateH264EncoderSettings("obs_x264", *bitrate, stream_compatible, !stream_compatible ? &recording_resolution_limit : nullptr), nullptr));
	}

	bool StartRecordingOutputs(obs_output_t *output, obs_output_t *buffer)
	{
		auto set_scale_info = [&]
		{
			auto scaled = ScaleResolution(target, game_res, recording_resolution_limit);
			video_scale_info vsi{};
			vsi.format = VIDEO_FORMAT_NV12;
			vsi.gpu_conversion = true;
			vsi.range = VIDEO_RANGE_PARTIAL;
			vsi.scale_type = OBS_SCALE_BICUBIC;
			vsi.width = scaled.width;
			vsi.height = scaled.height;
			vsi.colorspace = (vsi.width >= 1280 || vsi.height >= 720) ? VIDEO_CS_709 : VIDEO_CS_601;
			obs_encoder_set_video_conversion(h264, &vsi);
		};

		set_scale_info();
		while (!obs_output_active(output) && !obs_output_start(output)) {
			auto encoder = obs_output_get_video_encoder(output);
			auto id = obs_encoder_get_id(encoder);
			if (id && id == "obs_x264"s)
				return false;

			if (!id)
				id = "obs_x264"; // force software encoding

			CreateH264Encoder(nullptr, nullptr, false, id);
			set_scale_info();
			obs_output_set_video_encoder(output, h264);
		}

		if (buffer && !obs_output_active(buffer)) {
			obs_output_set_video_encoder(buffer, h264);
			obs_output_start(buffer);
		}

		return true;
	}

	void InitEncoders()
	{
		CreateH264Encoder();

		CreateH264Encoder(&recordingStream_h264, nullptr, true);

		auto ssettings = OBSDataCreate();
		obs_data_set_int(ssettings, "bitrate", target_stream_bitrate);
		obs_data_set_bool(ssettings, "cbr", true);
		obs_data_set_string(ssettings, "profile", "high");
		obs_data_set_string(ssettings, "preset", "veryfast");
		obs_data_set_int(ssettings, "keyint_sec", 2);

		InitRef(stream_h264, "Couldn't create stream video encoder", obs_encoder_release,
			obs_video_encoder_create("obs_x264", "stream video", ssettings, nullptr));

		aac = CreateAudioEncoder("aac");
		if (!aac)
			throw "Couldn't create audio encoder";

		recordingStream_aac = CreateAudioEncoder("recordingStream_aac", 1);
		if (!recordingStream_aac)
			throw "Coudln't create recording stream audio encoder";


		obs_encoder_set_video(stream_h264, obs_get_video());

		obs_encoder_set_audio(aac, obs_get_audio());
		obs_encoder_set_audio(recordingStream_aac, obs_get_audio());
	}

	void InitSignals()
	{
		micMuted
			.SetOwner(mic)
			.SetSignal("mute")
			.SetFunc([=](calldata_t *data)
		{
			AnvilCommands::MicUpdated(calldata_bool(data, "muted"));
		})
			.Connect();

		pttActive
			.SetOwner(mic)
			.SetSignal("push_to_talk_active")
			.SetFunc([=](calldata_t *data)
		{
			AnvilCommands::MicUpdated(!calldata_bool(data, "active"));
		})
			.Connect();

		micAcquired
			.SetOwner(mic)
			.SetSignal("acquired")
			.SetFunc([=](calldata_t *data)
		{
			bool active = calldata_bool(data, "active");
			AnvilCommands::MicUpdated(boost::indeterminate, boost::indeterminate, boost::indeterminate, active);
		})
			.Connect();

		stopRecording
			.SetSignal("stop")
			.SetFunc([=](calldata *data)
		{
			auto output_ = reinterpret_cast<obs_output_t*>(calldata_ptr(data, "output"));
			OBSOutput output = OBSGetRef(output_);
			if (!output) {
				blog(LOG_WARNING, "Could not GetRef for output '%s' (%p) during stopRecording", obs_output_get_name(output_), output_);
				return;
			}

			QueueOperation([=]
			{
				recording_stream = false;

				auto data = OBSTransferOwned(obs_output_get_settings(output));
				auto status = GetOutputStatus(data);
				if (!status.started || status.stopped) {
					blog(LOG_WARNING, "output '%s'(%p) signals stop after it was already stopped (%s)", obs_output_get_name(output), output, to_string(status).c_str());
					return;
				}

				status.stopped = true;
				SetOutputStatus(data, status);

				auto restarting_recording = status.stopping_for_restart;

				bool stop;
				string profiler_path;
				{
					profiler_path = profiler_filename;
					profiler_filename.clear();
					decltype(bookmarks) full_bookmarks;
					{
						LOCK(bookmarkMutex);
						full_bookmarks = bookmarks;
					}

					GameSessionEnded(output, restarting_recording);

					ForgeEvents::SendRecordingStop(obs_data_get_string(data, "path"),
						obs_output_get_total_frames(output),
						obs_output_get_output_duration(output),
						BookmarkTimes(bookmarks), ovi.base_width, ovi.base_height, recording_game && game_pid ? game_pid.get_ptr() : nullptr, full_bookmarks,
						restarting_recording);
					AnvilCommands::ShowIdle();
				}
				//StopVideo(); // leak here!!!

				ClearBookmarks();

				auto snap = ProfileSnapshotCreate();
				auto diff = unique_ptr<profiler_snapshot_t>{ profile_snapshot_diff(game_session_snapshot.get(), snap.get()) };

				profiler_print(diff.get());
				profiler_print_time_between_calls(diff.get());

				if (!profiler_path.empty()) {
					if (!profiler_snapshot_dump_csv_gz(diff.get(), profiler_path.c_str())) {
						blog(LOG_INFO, "Failed to dump profiler data to '%s'", profiler_path.c_str());
						profiler_path = "";
					}
					else {
						blog(LOG_INFO, "Profiler data dumped to '%s'", profiler_path.c_str());
					}
				}

				if (!restarting_recording && (!webrtc || !obs_output_active(webrtc)))
					ForgeEvents::SendCleanupComplete(profiler_path.empty() ? nullptr : &profiler_path, game_pid ? *game_pid : 0);
			});
		});

		startRecording
			.SetSignal("start")
			.SetFunc([=](calldata *data)
		{
			OBSOutput output = reinterpret_cast<obs_output_t*>(calldata_ptr(data, "output"));
			QueueOperation([=]
			{
				UpdateSourceAudioSettings();

				auto settings = OBSTransferOwned(obs_output_get_settings(output));

				auto status = GetOutputStatus(settings);
				if (status.started) {
					blog(LOG_WARNING, "output '%s'(%p) signals start after it was already started (%s)", obs_output_get_name(output), output, to_string(status).c_str());
					return;
				}

				status.started = true;
				SetOutputStatus(settings, status);

				auto restarting_recording = status.restarting;

				recordingStartTime = os_gettime_ns();
				{
					auto encoder = obs_output_get_video_encoder(output);
					auto encoder_settings = obs_encoder_get_settings(encoder);
					ForgeEvents::SendRecordingStart(obs_data_get_string(settings, "path"), restarting_recording, obs_data_get_int(encoder_settings, "bitrate"),
						ovi.base_width, ovi.base_height);
				}
				AnvilCommands::ShowRecording();

				if (!game_start_bookmark_id && recording_game) {
					// this can happen in recording only mode; requesting the bookmark earlier could cause us to not be notified of the tracked frame
					auto data = OBSDataCreate();
					obs_data_set_bool(data, "suppress_indicator", true);
					obs_data_set_obj(data, "extra_data", GenerateExtraData("game_begin"));
					game_start_bookmark_id = CreateBookmark(data);
					ForgeEvents::SendGameSessionStarted();
				}
			});
		});

		sentTrackedFrame
			.SetSignal("sent_tracked_frame")
			.SetFunc([=](calldata *data)
		{
			auto id_ = calldata_int(data, "id");
			auto pts = calldata_int(data, "pts");
			auto timebase = static_cast<uint32_t>(calldata_int(data, "timebase_den"));
			OBSOutput output = reinterpret_cast<obs_output_t*>(calldata_ptr(data, "output"));
			QueueOperation([=]
			{
				auto bookmark = FinalizeBookmark(estimatedBookmarks, bookmarks, id_,
					pts, timebase);

				if (!bookmark || !game_end_bookmark_id)
					return;

				if (bookmark->id != *game_end_bookmark_id)
					return;

				auto status = GetOutputStatus(OBSTransferOwned(obs_output_get_settings(output)));
				GameSessionEnded(output, status.stopping_for_restart);
			});
		});

		bufferSaved
			.SetSignal("buffer_output_finished")
			.SetFunc([=](calldata_t *data)
		{
			string filename = calldata_string(data, "filename");
			video_tracked_frame_id tracked_id = calldata_int(data, "tracked_frame_id");
			auto frames = static_cast<uint32_t>(calldata_int(data, "frames"));
			auto duration = calldata_float(data, "duration");
			auto start_pts = calldata_int(data, "start_pts");
			
			boost::optional<uint32_t> buffer_id;
			if (auto ptr = static_cast<uint32_t*>(calldata_ptr(data, "buffer_id")))
				buffer_id = *ptr;

			QueueOperation([=]
			{
				if (buffer_id && forward_buffer_id && *buffer_id == *forward_buffer_id)
					forward_buffer_id = boost::none;

				ForgeEvents::SendBufferReady(filename.c_str(), frames,
					duration, BookmarkTimes(bufferBookmarks, start_pts),
					ovi.base_width, ovi.base_height, FindBookmark(bookmarks, tracked_id));
			});
		});

		bufferSaveFailed
			.SetSignal("buffer_output_failed")
			.SetFunc([=](calldata_t *data)
		{
			string filename = calldata_string(data, "filename");
			QueueOperation([=]
			{
				ForgeEvents::SendBufferFailure(filename.c_str());
			});
		});

		bufferSentTrackedFrame
			.SetSignal("sent_tracked_frame")
			.SetFunc([=](calldata *data)
		{
			auto id_ = calldata_int(data, "id");
			auto pts = calldata_int(data, "pts");
			auto timebase_den = static_cast<uint32_t>(calldata_int(data, "timebase_den"));
			QueueOperation([=]
			{
				FinalizeBookmark(estimatedBufferBookmarks, bufferBookmarks, id_, pts, timebase_den);
			});
		});

		stopCapture
			.SetOwner(gameCapture)
			.SetSignal("stop_capture");

		startCapture
			.SetOwner(gameCapture)
			.SetSignal("start_capture");

		injectFailed
			.SetOwner(gameCapture)
			.SetSignal("inject_failed");

		injectRequest
			.SetOwner(gameCapture)
			.SetSignal("inject_request")
			.SetFunc([](calldata_t *data)
		{
			auto is_64bit = calldata_bool(data, "process_is_64bit");
			auto anti_cheat = calldata_bool(data, "anti_cheat");
			auto pid = static_cast<DWORD>(calldata_int(data, "process_thread_id"));
			string hook_dir = calldata_string(data, "hook_dir");
			QueueOperation([=]
			{
				ForgeEvents::SendInjectRequest(is_64bit, anti_cheat, pid, hook_dir);
			});
		});

		monitorProcess
			.SetSignal("monitor_process")
			.SetFunc([](calldata_t *data)
		{
			auto pid = static_cast<DWORD>(calldata_int(data, "process_id"));
			QueueOperation([=]
			{
				ForgeEvents::SendMonitorProcess(pid);
			});
		});

		screenshotSaved
			.SetSignal("screenshot_saved")
			.SetFunc([=](calldata_t *data)
		{
			string filename = calldata_string(data, "filename");
			auto id = calldata_int(data, "screenshot_id");
			QueueOperation([=]
			{
				auto rs = find_if(begin(requested_screenshots), end(requested_screenshots), [&](const pair<string, long long> &p) { return p.second == id; });
				if (rs == end(requested_screenshots))
					return;

				ForgeEvents::SendSavedGameScreenshot(rs->first.c_str(), filename.c_str());
				requested_screenshots.erase(rs);
			});
		});

		processInaccessible
			.SetOwner(gameCapture)
			.SetSignal("process_inaccessible");

		recordingStreamStart
			.SetSignal("start")
			.SetFunc([=](calldata*)
		{
			QueueOperation([=]
			{
				UpdateSourceAudioSettings();
				AnvilCommands::StreamStatus(true);
				ForgeEvents::SendStreamingStart();
			});
		});

		recordingStreamStop
			.SetSignal("stop")
			.SetFunc([=](calldata *data)
		{
			auto weakStream = OBSGetWeakRef(reinterpret_cast<obs_output_t*>(calldata_ptr(data, "output")));
			auto code = calldata_int(data, "code");
			QueueOperation([=]
			{
				UpdateSourceAudioSettings();
				AnvilCommands::StreamStatus(false);
				ForgeEvents::SendStreamingStop(code);
			});
		});

		startStreaming
			.SetSignal("start")
			.SetFunc([=](calldata*)
		{
			QueueOperation([=]
			{
				stream_active = true;
				AnvilCommands::StreamStatus(stream_active);
				ForgeEvents::SendStreamingStart();
			});
		});

		stopStreaming
			.SetSignal("stop")
			.SetFunc([=](calldata *data)
		{
			auto weakStream = OBSGetWeakRef(reinterpret_cast<obs_output_t*>(calldata_ptr(data, "output")));
			auto code = calldata_int(data, "code");
			QueueOperation([=]
			{
				if (OBSGetStrongRef(weakStream) == stream) // check if stop wasn't requested
					streaming = false;

				stream_active = false;
				AnvilCommands::StreamStatus(stream_active);
				ForgeEvents::SendStreamingStop(code);
			});
		});

	}

	void InitStreamService()
	{
		memset(&forge_streaming_service, 0, sizeof(forge_streaming_service));

		forge_streaming_service.id = "forge_rtmp";
		forge_streaming_service.get_name = [](void *data)->const char *{ return "forge streaming service"; };
		forge_streaming_service.create = [](obs_data_t *settings, obs_service_t *service)->void *{ return obs_data_create(); };
		forge_streaming_service.destroy = [](void *data)->void{ obs_data_release((obs_data_t *)data); };
		forge_streaming_service.update = [](void *data, obs_data_t *settings)->void{ obs_data_apply((obs_data_t *)data, settings); };
		//forge_streaming_service.get_properties = [](void *data)->obs_properties_t *{ return obs_properties_create(); };
		forge_streaming_service.get_url = [](void *data)->const char *{ return obs_data_get_string((obs_data_t *)data, "server"); };
		forge_streaming_service.get_key = [](void *data)->const char *{ return obs_data_get_string((obs_data_t *)data, "key"); };

		obs_register_service(&forge_streaming_service);
		
		InitRef(stream_service, "Couldn't create streaming service", obs_service_release,
			obs_service_create("forge_rtmp", "forge streaming", nullptr, nullptr));
	}

	void UpdateStreamSettings()
	{
		uint32_t width, height;
		if (game_res.width && game_res.height) {
			auto scaled = ScaleResolution(target_stream, game_res);

			width = scaled.width;
			height = scaled.height;

			blog(LOG_INFO, "setting stream output size to %ux%u", scaled.width, scaled.height);
		} else {
			width = target_stream.width;
			height = AlignX264Height(target_stream.height);
		}

		video_scale_info vsi{};
		vsi.format = VIDEO_FORMAT_NV12;
		vsi.gpu_conversion = true;
		vsi.range = VIDEO_RANGE_PARTIAL;
		vsi.scale_type = OBS_SCALE_BICUBIC;
		vsi.width = width;
		vsi.height = height;
		vsi.colorspace = (vsi.width >= 1280 || vsi.height >= 720) ? VIDEO_CS_709 : VIDEO_CS_601;

		obs_encoder_set_video_conversion(stream_h264, &vsi);

		auto ssettings = OBSDataCreate();
		obs_data_set_int(ssettings, "bitrate", target_stream_bitrate);
		obs_data_set_bool(ssettings, "cbr", true);
		obs_data_set_string(ssettings, "profile", "high");
		obs_data_set_string(ssettings, "preset", "veryfast");
		obs_data_set_int(ssettings, "keyint_sec", 2);

		obs_encoder_update(stream_h264, ssettings);

		// todo: set some useful default for these?
		obs_output_set_delay(stream, 0, 0);
		obs_output_set_reconnect_settings(stream, 0, 0);
	}

	void GameSessionEnded(obs_output_t *output, bool split_recording)
	{
		if (!recording_game && !game_end_bookmark_id)
			return;

		decltype(bookmarks) full_bookmarks;
		{
			LOCK(bookmarkMutex);
			full_bookmarks = bookmarks;
		}
		auto data = OBSTransferOwned(obs_output_get_settings(output));
		ForgeEvents::SendGameSessionEnded(obs_data_get_string(data, "path"),
			obs_output_get_total_frames(output),
			obs_output_get_output_duration(output),
			BookmarkTimes(bookmarks), ovi.base_width, ovi.base_height, game_pid ? *game_pid : 0, full_bookmarks,
			game_start_bookmark_id, game_end_bookmark_id, split_recording);

		game_start_bookmark_id = boost::none;
		game_end_bookmark_id = boost::none;
	}

	void ResetCaptureSignals()
	{
		auto capture_source = gameCapture ? gameCapture : gameWindow;
		auto weakGameCapture = OBSGetWeakRef(gameCapture ? gameCapture : gameWindow);
		auto weakOutput = OBSGetWeakRef(output);
		auto weakBuffer = OBSGetWeakRef(buffer);

		blog(LOG_INFO, "ResetCaptureSignals: weakGameCapture = %p", static_cast<obs_weak_source_t*>(weakGameCapture));

		RemoveWaitHandle(game_process.get());
		game_process.reset();

		if (game_pid)
			game_process = { OpenProcess(SYNCHRONIZE, false, *game_pid), HandleDeleter{} };

		auto end_capture = [=, proc=game_process]
		{
			recording_game = false;

			{
				auto ref = OBSGetStrongRef(weakGameCapture);
				if (!ref) {
					blog(LOG_INFO, "end_capture: called with expired game capture (weakGameCapture = %p)", static_cast<obs_weak_source_t*>(weakGameCapture));
					return;
				}

				blog(LOG_INFO, "end_capture: ending capture (weakGameCapture = %p)", static_cast<obs_weak_source_t*>(weakGameCapture));

				auto settings = OBSTransferOwned(obs_source_get_settings(ref));
				obs_data_set_int(settings, "process_id", 0);
				obs_data_set_int(settings, "thread_id", 0);
				obs_source_update(ref, settings);

				if (OBSGetOutputSource(0) == ref)
					obs_set_output_source(0, nullptr);

				if (ref == gameCapture || ref == gameWindow)
					DeleteGameCapture();
			}

			if (output) {
				if (!game_end_bookmark_id) {
					auto data = OBSDataCreate();
					obs_data_set_bool(data, "suppress_indicator", true);
					obs_data_set_obj(data, "extra_data", GenerateExtraData("game_exit"));
					game_end_bookmark_id = CreateBookmark(data);
				} else {
					blog(LOG_WARNING, "stopCapture: called with game_end_bookmark_id already set");
				}
			}

			if (game_capture_start_time) {
				AnvilCommands::ShowNotifications(false);
				ForgeEvents::SendGameCaptureStopped(chrono::steady_clock::now() - *game_capture_start_time);
				game_capture_start_time.reset();
			}

			bool should_send_cleanup_complete = false;
			{
				auto ref = OBSGetStrongRef(weakOutput);
				should_send_cleanup_complete = !ref || (!game_end_bookmark_id && !obs_output_active(ref));
			}

			auto send_cleanup_complete = [&]
			{
				if (should_send_cleanup_complete) {
					if (!profiler_filename.empty()) {
						auto snap = ProfileSnapshotCreate();
						auto diff = unique_ptr<profiler_snapshot_t>{ profile_snapshot_diff(game_session_snapshot.get(), snap.get()) };

						profiler_print(diff.get());
						profiler_print_time_between_calls(diff.get());
						if (!profiler_snapshot_dump_csv_gz(diff.get(), profiler_filename.c_str())) {
							blog(LOG_INFO, "Failed to dump profiler data to '%s'", profiler_filename.c_str());
							profiler_filename.clear();
						} else
							blog(LOG_INFO, "Profiler data dumped to '%s'", profiler_filename.c_str());
					}

					ForgeEvents::SendCleanupComplete(profiler_filename.empty() ? nullptr : &profiler_filename, game_pid ? *game_pid : 0);
					profiler_filename.clear();
				}
			};
			DEFER{ send_cleanup_complete(); };

			if (streaming) {
				blog(LOG_INFO, "end_capture: Not stopping outputs because stream is active");
				return;
			}

			auto stop_ = [&](obs_weak_output_t *weak, OBSOutput &out)
			{
				if (auto ref = OBSGetStrongRef(weak)) {
					blog(LOG_INFO, "end_capture: Stopping output '%s'", obs_output_get_name(ref));
					obs_output_stop_with_timeout(ref, 2500);
					if (ref == out)
						out = nullptr;
				} else {
					auto name = out ? obs_output_get_name(out) : nullptr;
					if (name)
						blog(LOG_INFO, "end_capture: weak ref for output '%s' expired (weak = %p, output = %p)",
							name, weak, static_cast<obs_output_t*>(out));
					else
						blog(LOG_INFO, "end_capture: weak ref and output expired (weak = %p, output = %p)",
							weak, static_cast<obs_output_t*>(out));
				}
			};

			stop_(weakOutput, output);
			stop_(weakBuffer, buffer);
			if (recordingStream)
				stop_(OBSGetWeakRef(recordingStream), recordingStream);
			if (webrtc)
				stop_(OBSGetWeakRef(webrtc), webrtc);

			shared_ptr<void> force_stop_timer{ CreateWaitableTimer(nullptr, true, nullptr), HandleDeleter{} };
			LARGE_INTEGER timeout = { 0 };
			timeout.QuadPart = -35000000LL; // 3.5 seconds
			SetWaitableTimer(force_stop_timer.get(), &timeout, 0, nullptr, nullptr, false);

			AddWaitHandleCallback(force_stop_timer.get(), [=, timer=force_stop_timer]
			{
				auto force_stop = [](obs_weak_output *out)
				{
					if (auto ref = OBSGetStrongRef(out)) {
						blog(LOG_INFO, "end_capture: Force stopping output '%s' (%p)", obs_output_get_name(ref), ref);
						obs_output_force_stop(ref);
					} else {
						blog(LOG_INFO, "end_capture: weak ref expired when trying to force stop (weak = %p)", out);
					}
				};
				force_stop(weakOutput);
				force_stop(weakBuffer);
				if (recordingStream)
					force_stop(OBSGetWeakRef(recordingStream));
				if (webrtc)
					force_stop(OBSGetWeakRef(webrtc));
			});
		};

		if (game_process)
			AddWaitHandleCallback(game_process.get(), end_capture);

		stopCapture
			.Disconnect()
			.SetOwner(capture_source)
			.SetFunc([=](calldata_t*)
		{
			QueueOperation([=]
			{
				if (game_process)
					RemoveWaitHandle(game_process.get());
				end_capture();
			});
		}).Connect();

		startCapture
			.Disconnect()
			.SetOwner(capture_source)
			.SetFunc([=](calldata_t *data)
		{
			auto width = static_cast<uint32_t>(calldata_int(data, "width"));
			auto height = static_cast<uint32_t>(calldata_int(data, "height"));
			QueueOperation([=]
			{
				if (game_pid)
					AnvilCommands::Connect(*game_pid);
				else
					blog(LOG_WARNING, "startCapture: not sending AnvilCommands::Connect because game_pid is missing");

				DEFER
				{
					if (game_start_bookmark_id)
						return;

					auto data = OBSDataCreate();
					obs_data_set_bool(data, "suppress_indicator", true);
					obs_data_set_obj(data, "extra_data", GenerateExtraData("game_begin"));
					game_start_bookmark_id = CreateBookmark(data);
					if (game_start_bookmark_id)
						ForgeEvents::SendGameSessionStarted();
				};

				if (!game_capture_start_time) {
					game_capture_start_time = chrono::steady_clock::now();
					AnvilCommands::ShowNotifications(true);
					ForgeEvents::SendGameCaptureStarted();
				}

				if (UpdateSize(width, height))
					return;

				if (streaming)
					return;

				auto ref = OBSGetStrongRef(weakOutput);
				if (ref)
					StartRecordingOutputs(ref, OBSGetStrongRef(weakBuffer));
			});
		}).Connect();
	}

	void CreateOutput(bool restarting = false)
	{
		if (!filename.empty()) {
			auto osettings = OBSDataCreate();
			obs_data_set_string(osettings, "path", filename.c_str());
			obs_data_set_string(osettings, "muxer_settings", muxerSettings.c_str());

			{
				OutputStatus status;
				status.restarting = restarting;
				SetOutputStatus(osettings, status);
			}

			InitRef(output, "Couldn't create output", obs_output_release,
				obs_output_create("ffmpeg_muxer", "ffmpeg recorder", osettings, nullptr));

			obs_output_set_video_encoder(output, streaming ? stream_h264 : h264);
			obs_output_set_audio_encoder(output, aac, 0);


			obs_data_set_string(buffer_settings, "muxer_settings", muxerSettings.c_str());

			InitRef(buffer, "Couldn't create buffer output", obs_output_release,
				obs_output_create("ffmpeg_recordingbuffer", "ffmpeg recordingbuffer", buffer_settings, nullptr));

			obs_output_set_video_encoder(buffer, streaming ? stream_h264 : h264);
			obs_output_set_audio_encoder(buffer, aac, 0);

		} else {
			output = nullptr;
			buffer = nullptr;
		}
		
		
		if (output) {
			stopRecording
				.Disconnect()
				.SetOwner(output)
				.Connect();

			startRecording
				.Disconnect()
				.SetOwner(output)
				.Connect();

			sentTrackedFrame
				.Disconnect()
				.SetOwner(output)
				.Connect();
		}

		if (buffer) {
			bufferSaved
				.Disconnect()
				.SetOwner(buffer)
				.Connect();

			bufferSaveFailed
				.Disconnect()
				.SetOwner(buffer)
				.Connect();

			bufferSentTrackedFrame
				.Disconnect()
				.SetOwner(buffer)
				.Connect();
		}

		ResetCaptureSignals();
	}

	void CreateStreamOutput()
	{
		InitRef(stream, "Couldn't create stream output", obs_output_release,
			obs_output_create("rtmp_output", "rtmp streaming", nullptr, nullptr));

		obs_output_set_video_encoder(stream, stream_h264);
		obs_output_set_audio_encoder(stream, aac, 0);
		obs_output_set_service(stream, stream_service);

		stopStreaming
			.Disconnect()
			.SetOwner(stream)
			.Connect();

		startStreaming
			.Disconnect()
			.SetOwner(stream)
			.Connect();
	}

	void ClearBookmarks()
	{
		LOCK(bookmarkMutex);
		estimatedBookmarks.clear();
		bookmarks.clear();

		estimatedBufferBookmarks.clear();
		bufferBookmarks.clear();

		next_bookmark_id = 0;
	}

	vector<double> BookmarkTimes(vector<Bookmark> &bookmarks, int64_t start_pts = 0)
	{
		vector<double> res;
		{
			LOCK(bookmarkMutex);

			res.reserve(bookmarks.size());
			for (auto &bookmark : bookmarks) {
				if (bookmark.pts < start_pts)
					continue;

				res.push_back((bookmark.pts - start_pts) / static_cast<double>(bookmark.fps_den));
			}
		}

		return res;
	}

	boost::optional<Bookmark> FindBookmark(const vector<Bookmark> &bookmarks, video_tracked_frame_id id)
	{
		LOCK(bookmarkMutex);

		for (auto &bookmark : bookmarks) {
			if (bookmark.tracked_id == id)
				return bookmark;
		}

		return boost::none;
	}

	boost::optional<Bookmark> FinalizeBookmark(vector<Bookmark> &estimates, vector<Bookmark> &bookmarks, video_tracked_frame_id tracked_id, int64_t pts, uint32_t fps_den)
	{
		LOCK(bookmarkMutex);

		auto it = find_if(begin(estimates), end(estimates), [&](const Bookmark &bookmark)
		{
			return bookmark.tracked_id == tracked_id;
		});
		if (it == end(estimates))
			return boost::none;

		auto new_time = pts / static_cast<double>(fps_den);

		blog(LOG_INFO, "Updated bookmark from %g s to %g s (tracked frame %lld)", it->time, new_time, tracked_id);

		it->fps_den = fps_den;
		it->pts = pts;
		it->time = new_time;

		bookmarks.push_back(*it);
		estimates.erase(it);

		ForgeEvents::SendBookmarkFinalized(bookmarks.back(), ovi.base_width, ovi.base_height);

		return bookmarks.back();
	}

	boost::optional<int> CreateBookmark(OBSData &obj)
	{
		if (!output || !obs_output_active(output))
			return boost::none;

		LOCK(bookmarkMutex);
		estimatedBookmarks.emplace_back();
		auto &bookmark = estimatedBookmarks.back();

		estimatedBufferBookmarks.emplace_back();
		auto &bufferBookmark = estimatedBufferBookmarks.back();

		bookmark.id = bufferBookmark.id = ++next_bookmark_id;

		bookmark.time = bufferBookmark.time = (os_gettime_ns() - recordingStartTime) / 1000000000.;

		bookmark.extra_data = bufferBookmark.extra_data = OBSDataGetObj(obj, "extra_data");

		video_tracked_frame_id tracked_id = 0;

		bool interruptible = obs_data_get_bool(obj, "interruptible");
		if ((interruptible && !StartForwardBuffer(obj, &tracked_id)) ||
			(!interruptible && !SaveRecordingBuffer(obj, &tracked_id)))
			tracked_id = obs_track_next_frame();

		bookmark.tracked_id = bufferBookmark.tracked_id = tracked_id;

		blog(LOG_INFO, "Created bookmark at offset %g s (estimated, tracking frame %lld)", bookmark.time, tracked_id);

		{
			const char *filename = obs_data_get_string(obj, "screenshot");
			if (filename && *filename)
				SaveGameScreenshot(filename);
		}

		if (!obs_data_get_bool(obj, "suppress_indicator"))
			AnvilCommands::ShowBookmark();

		return bookmark.id;
	}

	boost::optional<uint32_t> forward_buffer_id;
	bool StartForwardBuffer(obs_data_t *settings, video_tracked_frame_id *tracked_id=nullptr)
	{
		if (forward_buffer_id) {
			blog(LOG_INFO, "Tried to save forward buffer while forward buffer is already in progress");
			return false;
		}

		calldata_t calldata;
		calldata_init(&calldata);
		DEFER{ calldata_free(&calldata); };

		auto proc = obs_output_get_proc_handler(buffer);
		auto duration = obs_data_get_double(settings, "max_duration");

		calldata_set_string(&calldata, "filename", obs_data_get_string(settings, "filename"));
		calldata_set_float(&calldata, "maximum_recording_duration", duration);

		proc_handler_call(proc, "output_interruptible_future_buffer", &calldata);

		forward_buffer_id = static_cast<uint32_t>(calldata_int(&calldata, "buffer_id"));
		blog(LOG_INFO, "started forward buffer id %d", *forward_buffer_id);

		if (tracked_id)
			*tracked_id = calldata_int(&calldata, "tracked_frame_id");

		AnvilCommands::ForwardBufferInProgress(true, duration);

		return true;
	}

	void StopForwardBuffer()
	{
		if (!forward_buffer_id) {
			blog(LOG_WARNING, "Tried to stop forward buffer without an active forward buffer");
			return;
		}

		calldata_t calldata;
		calldata_init(&calldata);
		DEFER{ calldata_free(&calldata); };

		auto proc = obs_output_get_proc_handler(buffer);

		blog(LOG_INFO, "Stopping buffer id %d early", *forward_buffer_id);
		calldata_set_int(&calldata, "buffer_id", *forward_buffer_id);
		proc_handler_call(proc, "interrupt_buffer", &calldata);

		AnvilCommands::ShowClipping();
		AnvilCommands::ForwardBufferInProgress(false);
	}

	bool SaveRecordingBuffer(obs_data_t *settings, video_tracked_frame_id *tracked_id=nullptr)
	{
		if (!settings)
			return false;

		const char *filename = obs_data_get_string(settings, "filename");
		if (!filename || !*filename)
			return false;

		if (!buffer || !obs_output_active(buffer))
			return false;

		calldata_t param{};
		calldata_init(&param);
		calldata_set_string(&param, "filename", filename);
		calldata_set_float(&param, "extra_recording_duration", obs_data_get_double(settings, "extra_recording_duration"));
		calldata_set_float(&param, "save_duration", obs_data_get_double(settings, "save_duration"));

		bool continue_recording = obs_data_has_user_value(settings, "extra_recording_duration");

		{
			auto proc = obs_output_get_proc_handler(buffer);
			proc_handler_call(proc, continue_recording ? "output_precise_buffer_and_keep_recording" : "output_precise_buffer", &param);
		}

		if (tracked_id)
			*tracked_id = calldata_int(&param, "tracked_frame_id");

		calldata_free(&param);
		return true;
	}

	void ForwardInjectorResult(obs_data_t *res)
	{
		if (!res)
			return;

		DWORD code = obs_data_has_user_value(res, "code") ? static_cast<DWORD>(obs_data_get_int(res, "code")) : -1;

		calldata_t param{};
		calldata_init(&param);
		calldata_set_int(&param, "code", code);

		{
			auto proc = obs_source_get_proc_handler(gameCapture);
			proc_handler_call(proc, "injector_result", &param);
		}

		calldata_free(&param);
	}

	void ForwardMonitoredProcessExit(obs_data_t *res)
	{
		if (!res)
			return;

		auto pid = static_cast<DWORD>(obs_data_get_int(res, "process_id"));
		auto code = static_cast<DWORD>(obs_data_has_user_value(res, "code") ? obs_data_get_int(res, "process_id") : -1);

		calldata_t param{};
		calldata_init(&param);
		calldata_set_int(&param, "process_id", pid);
		calldata_set_int(&param, "code", code);

		{
			auto proc = obs_source_get_proc_handler(gameCapture);
			proc_handler_call(proc, "monitored_process_exit", &param);
		}

		calldata_free(&param);
	}

	void DeleteGameCapture()
	{
		auto source = OBSGetOutputSource(0);
		if (source == gameCapture || source == gameWindow)
			obs_set_output_source(0, nullptr);

		obs_set_output_source(3, nullptr);

		gameCapture = nullptr;
		gameWindow = nullptr;
		audioBuffer = nullptr;

		check_audio_streams_timer.reset();

		obs_sceneitem_remove(game_and_webcam.game);
		game_and_webcam.game = nullptr;
	}

	void CreateAudioBufferSource(obs_data_t *settings)
	{
		InitRef(audioBuffer, "Couldn't create audio buffer source", obs_source_release,
			obs_source_create(OBS_SOURCE_TYPE_INPUT, "AudioBufferSource", "audio buffer", settings, nullptr));

		obs_source_set_volume(audioBuffer, obs_source_get_volume(tunes));

		obs_set_output_source(3, audioBuffer);

		check_audio_streams = false;
		check_audio_streams_timer.reset();
		UpdateSourceAudioSettings();
	}

	void CreateGameWindow(obs_data_t *settings)
	{
		if (!settings)
			return;

		game_pid = static_cast<DWORD>(obs_data_get_int(settings, "process_id"));
		if (!game_pid || !*game_pid) {
			blog(LOG_WARNING, "CrucibleContext::CreateGameWindow: invalid game pid supplied");
			return;
		}

		gameCapture = nullptr;
		gameWindow = nullptr;

		InitRef(gameWindow, "Couldn't create game window source", obs_source_release,
			obs_source_create(OBS_SOURCE_TYPE_INPUT, "window_capture", "game window", settings, nullptr));

		CreateH264Encoder();

		CreateH264Encoder(&recordingStream_h264, nullptr, true);

		recording_game = true;

		injectFailed.Disconnect();
		injectRequest.Disconnect();
		monitorProcess.Disconnect();
		screenshotSaved.Disconnect();
		processInaccessible.Disconnect();

		ResetCaptureSignals();

		if (game_and_webcam.game)
			obs_sceneitem_remove(game_and_webcam.game);
		game_and_webcam.game = obs_scene_add(game_and_webcam.scene, gameWindow);
		game_and_webcam.MakePresentable();

		if (!OBSGetOutputSource(0) || !streaming)
			obs_set_output_source(0, gameWindow);
	}

	void CreateGameCapture(obs_data_t *settings)
	{
		if (!settings)
			return;

		game_pid = static_cast<DWORD>(obs_data_get_int(settings, "process_id"));

		gameCapture = nullptr;
		gameWindow = nullptr;

		auto path = GetModulePath(nullptr);
		DStr path64;
		dstr_printf(path64, "%sAnvilRendering64.dll", path->array);
		dstr_cat(path, "AnvilRendering.dll");

		obs_data_set_string(settings, "overlay_dll", path);
		obs_data_set_string(settings, "overlay_dll64", path64);
		obs_data_set_bool(settings, "sli_compatibility", sli_compatibility);
		//obs_data_set_bool(settings, "allow_ipc_injector", true);

		InitRef(gameCapture, "Couldn't create game capture source", obs_source_release,
			obs_source_create(OBS_SOURCE_TYPE_INPUT, "game_capture", "game capture", settings, nullptr));

		CreateH264Encoder();
		
		CreateH264Encoder(&recordingStream_h264, nullptr, true);

		recording_game = true;

		injectFailed
			.Disconnect()
			.SetOwner(gameCapture)
			.SetFunc([=](calldata_t *data)
		{
			ForgeEvents::SendInjectFailed(static_cast<long*>(calldata_ptr(data, "injector_exit_code")));
		}).Connect();

		injectRequest
			.Disconnect()
			.SetOwner(gameCapture)
			.Connect();

		monitorProcess
			.Disconnect()
			.SetOwner(gameCapture)
			.Connect();

		screenshotSaved
			.Disconnect()
			.SetOwner(gameCapture)
			.Connect();

		processInaccessible
			.Disconnect()
			.SetOwner(gameCapture)
			.SetFunc([=](calldata_t *data)
		{
			ForgeEvents::SendProcessInaccessible(static_cast<DWORD>(calldata_int(data, "process_id")));
		}).Connect();

		ResetCaptureSignals();

		if (game_and_webcam.game)
			obs_sceneitem_remove(game_and_webcam.game);
		game_and_webcam.game = obs_scene_add(game_and_webcam.scene, gameCapture);
		game_and_webcam.MakePresentable();

		if (!OBSGetOutputSource(0) || !streaming)
			obs_set_output_source(0, gameCapture);
	}

	void ResetWindowCapture(obs_data_t *settings)
	{
		if (!settings) {
			obs_sceneitem_remove(window_and_webcam.window);
			window_and_webcam.window = nullptr;
			window = nullptr;

			return;
		}

		string name = obs_data_get_string(settings, "window");

		bool use_display = name.find("Google Chrome:Chrome") != name.npos && name.find(":chrome.exe") != name.npos;

		if (window && (string("window_capture") == obs_source_get_id(window)) != use_display) {
			obs_source_update(window, settings);
			return;
		}

		if (window) {
			obs_sceneitem_remove(window_and_webcam.window);
			window_and_webcam.window = nullptr;
			window = nullptr;
		}

		try {
			InitRef(window, "Couldn't create window capture source", obs_source_release,
				obs_source_create(OBS_SOURCE_TYPE_INPUT, use_display ? "display_window_capture" : "window_capture", "window capture", settings, nullptr));
		} catch (const char*) {
			if (use_display)
				InitRef(window, "Couldn't create window capture source", obs_source_release,
					obs_source_create(OBS_SOURCE_TYPE_INPUT, "window_capture", "window capture", settings, nullptr));
			else
				throw;
		}

		if (!window)
			return;

		window_and_webcam.window = obs_scene_add(window_and_webcam.scene, window);
		window_and_webcam.MakePresentable();
	}

	void SetOutputScene(string scene_name)
	{
		obs_source_t *source = nullptr;

		auto source_from_scene = [&](auto &container)
		{
			container.MakePresentable();
			return obs_scene_get_source(container.scene);
		};

		auto current_scene_name = [&]()
		{
			auto cur = OBSGetOutputSource(0);
			if (!cur || cur == gameCapture || cur == gameWindow)
				return "game_only";
			if (cur == source_from_scene(game_and_webcam))
				return "game";
			if (cur == source_from_scene(window_and_webcam))
				return "window";
			if (cur == source_from_scene(wallpaper_and_webcam))
				return "wallpaper";
			if (cur == source_from_scene(webcam_and_theme))
				return "webcam";

			return "unknown";
		};

		if (scene_name == "game_only") {
			source = gameCapture ? gameCapture : gameWindow;
		} else if (scene_name == "game") {
			source = source_from_scene(game_and_webcam);
		} else if (scene_name == "window") {
			source = source_from_scene(window_and_webcam);
		} else if (scene_name == "wallpaper") {
			source = source_from_scene(wallpaper_and_webcam);
		} else if (scene_name == "webcam") {
			source = source_from_scene(webcam_and_theme);
		} else {
			ForgeEvents::SendSelectSceneResult(scene_name, current_scene_name(), false);
			return;
		}

		Display::SetSource("preview", source);
		streaming_source = source;
		if (streaming || recording_stream)
			obs_set_output_source(0, source);
		ForgeEvents::SendSelectSceneResult(scene_name, scene_name, true);
	}

	void UpdateScenes(obs_data_t *settings)
	{
		bool send_info = false;

		if (auto game_settings = OBSDataGetObj(settings, "game")) {
			game_and_webcam.game_data = OBSDataGetObj(game_settings, "game");
			game_and_webcam.webcam_data = OBSDataGetObj(game_settings, "webcam");
			game_and_webcam.theme_data = OBSDataGetObj(game_settings, "theme");

			game_and_webcam.MakePresentable();
			send_info = true;
		}

		if (auto window_settings = OBSDataGetObj(settings, "window")) {
			window_and_webcam.window_data = OBSDataGetObj(window_settings, "window");
			window_and_webcam.webcam_data = OBSDataGetObj(window_settings, "webcam");
			window_and_webcam.theme_data = OBSDataGetObj(window_settings, "theme");

			window_and_webcam.MakePresentable();
			send_info = true;
		}

		if (auto wallpaper_settings = OBSDataGetObj(settings, "wallpaper")) {
			wallpaper_and_webcam.wallpaper_data = OBSDataGetObj(wallpaper_settings, "wallpaper");
			wallpaper_and_webcam.webcam_data = OBSDataGetObj(wallpaper_settings, "webcam");
			wallpaper_and_webcam.theme_data = OBSDataGetObj(wallpaper_settings, "theme");

			wallpaper_and_webcam.MakePresentable();
			send_info = true;
		}

		if (auto webcam_settings = OBSDataGetObj(settings, "webcam")) {
			webcam_and_theme.webcam_data = OBSDataGetObj(webcam_settings, "webcam");
			webcam_and_theme.theme_data = OBSDataGetObj(webcam_settings, "theme");

			webcam_and_theme.MakePresentable();
			send_info = true;
		}

		if (send_info)
			SendSceneInfo();
	}

	void SendSceneInfo()
	{
		auto add_item = [](obs_data_t *data, const char *name, obs_sceneitem_t *item, obs_data_t *prior)
		{
			auto transforms = ExtractTransforms(item, prior);
			if (auto source = obs_sceneitem_get_source(item)) {
				obs_data_set_int(transforms, "width", obs_source_get_width(source));
				obs_data_set_int(transforms, "height", obs_source_get_height(source));
			}

			obs_data_set_obj(data, name, transforms);
		};

		auto scenes = OBSDataCreate();

		auto game = OBSDataCreate();
		add_item(game, "game", game_and_webcam.game, game_and_webcam.game_data);
		add_item(game, "webcam", game_and_webcam.webcam, game_and_webcam.webcam_data);
		add_item(game, "theme", game_and_webcam.theme, game_and_webcam.theme_data);
		obs_data_set_obj(scenes, "game", game);

		auto window = OBSDataCreate();
		add_item(window, "window", window_and_webcam.window, window_and_webcam.window_data);
		add_item(window, "webcam", window_and_webcam.webcam, window_and_webcam.webcam_data);
		add_item(window, "theme", window_and_webcam.theme, window_and_webcam.theme_data);
		obs_data_set_obj(scenes, "window", window);

		auto wallpaper = OBSDataCreate();
		add_item(wallpaper, "wallpaper", wallpaper_and_webcam.wallpaper, wallpaper_and_webcam.wallpaper_data);
		add_item(wallpaper, "webcam", wallpaper_and_webcam.webcam, wallpaper_and_webcam.webcam_data);
		add_item(wallpaper, "theme", wallpaper_and_webcam.theme, wallpaper_and_webcam.theme_data);
		obs_data_set_obj(scenes, "wallpaper", wallpaper);

		auto webcam = OBSDataCreate();
		add_item(webcam, "webcam", webcam_and_theme.webcam, webcam_and_theme.webcam_data);
		add_item(webcam, "theme", webcam_and_theme.theme, webcam_and_theme.theme_data);
		obs_data_set_obj(scenes, "webcam", webcam);

		ForgeEvents::SendSceneInfo(scenes);
	}

	void SetSourceVolume(const string source_name, double volume, bool mute)
	{
		const map<string, OBSSource &> sources = 
		{
			{"desktop", tunes},
			{"microphone", mic}
		};

		auto elem = sources.find(source_name);
		if (elem == sources.end())
		{
			blog(LOG_INFO, "SetSourceVolume: source '%s' not found", source_name.c_str());
			return;
		}

		obs_source_set_volume(elem->second, volume);
		obs_source_set_muted(elem->second, mute);
		if (source_name == "desktop" && audioBuffer) {
			obs_source_set_volume(audioBuffer, volume);
			UpdateSourceAudioSettings();
		}
	}

	void EnableSourceLevelMeters(bool enabled)
	{	
		tunesLevelsUpdated.Disconnect();
		micLevelsUpdated.Disconnect();

		obs_volmeter_detach_source(tunesMeter.get());
		obs_volmeter_detach_source(micMeter.get());

		if (enabled)
		{
			auto handler = [](void *param, calldata_t *calldata)
			{
				const char *source = (const char *)param;

				float level = calldata_float(calldata, "level");
				float mag = calldata_float(calldata, "magnitude");
				float peak = calldata_float(calldata, "peak");
				bool  muted = calldata_bool(calldata, "muted");
				//blog(LOG_INFO, "levels_updated for %s: level %.3f, magnitude %.3f, peak %.3f, muted: %s", source, mag, level, peak, muted ? "true" : "false");
				ForgeEvents::SendAudioSourceLevel(source, level, mag, peak, muted);
			};

			tunesLevelsUpdated.Connect(obs_volmeter_get_signal_handler(tunesMeter.get()), "levels_updated", handler, "desktop");
			micLevelsUpdated.Connect(obs_volmeter_get_signal_handler(micMeter.get()), "levels_updated", handler, "microphone");

			obs_volmeter_attach_source(tunesMeter.get(), tunes);
			obs_volmeter_attach_source(micMeter.get(), mic);
		}
	}

	boost::optional<string> CreateWebRTC(obs_data_t *settings, OBSData &obj)
	{
		auto fail = [&](const char *err)
		{
			blog(LOG_WARNING, "CrucibleContext::CreateWebRTC: %s", err);
			return err;
		};

#ifdef WEBRTC_WIN
		bool poke_firewall = obs_data_get_bool(obj, "poke_firewall");
		if (!(gameCapture || gameWindow) && !poke_firewall)
			return fail("Tried to create webrtc output while no game capture is active");

		if (webrtc && obs_output_active(webrtc)) {
			blog(LOG_WARNING, "CreateWebRTC: webrtc output already active, stopping");
			obs_output_force_stop(webrtc);
		}

		webrtc = nullptr;

		OutputResolution webrtc_target_res = { 1280, 720 };
		auto scaled = ScaleResolution(webrtc_target_res.MinByPixels(GetWebRTCMaxResolution()), { ovi.base_width, ovi.base_height });
		auto webrtc_target = webrtc_target_res.MinByPixels(OutputResolution{ ovi.base_width, ovi.base_height });

		InitRef(webrtc, "Couldn't create webrtc output", obs_output_release,
			obs_output_create("webrtc_output", "webrtc", settings, nullptr));

		video_scale_info vsi{};
		vsi.width = scaled.width;
		vsi.height = scaled.height;
		vsi.colorspace = (vsi.width >= 1280 || vsi.height >= 720) ? VIDEO_CS_709 : VIDEO_CS_601;
		vsi.range = VIDEO_RANGE_PARTIAL;
		vsi.gpu_conversion = true;
		vsi.scale_type = OBS_SCALE_BICUBIC;
		vsi.texture_output = WebRTCNVENCAvailable();
		vsi.format = vsi.texture_output ? VIDEO_FORMAT_NV12 : VIDEO_FORMAT_I420;
		vsi.range = VIDEO_RANGE_PARTIAL;

		obs_output_set_video_conversion(webrtc, &vsi);

		blog(LOG_INFO, "webrtcResolution(%s): Updating scaled resolution: %dx%d", obs_output_get_name(webrtc), scaled.width, scaled.height);

		ForgeEvents::SendWebRTCResolutionScaled(webrtc_target, scaled);

		webrtcSessionDescription.Disconnect()
			.SetSignal("session_description")
			.SetOwner(webrtc)
			.SetFunc([](calldata_t *data)
		{
			auto type = calldata_string(data, "type");
			auto sdp = calldata_string(data, "sdp");
			if (!type || !sdp) {
				blog(LOG_WARNING, "webrtcSessionDescription: type(%p)/sdp(%p) missing");
				return;
			}

			string type_ = type;
			string sdp_ = sdp;
			QueueOperation([=, type = move(type_), sdp = move(sdp_)]
			{
				ForgeEvents::SendWebRTCSessionDescription(type, sdp);
			});
		})
			.Connect();

		webrtcICECandidate.Disconnect()
			.SetSignal("ice_candidate")
			.SetOwner(webrtc)
			.SetFunc([](calldata_t *data)
		{
			auto sdp_mid = calldata_string(data, "sdp_mid");
			auto sdp = calldata_string(data, "sdp");
			auto sdp_mline_index = calldata_int(data, "sdp_mline_index");
			if (!sdp_mid || !sdp) {
				blog(LOG_WARNING, "webrtcICECandidate: sdp_mid(%p)/sdp(%p) missing", sdp_mid, sdp);
				return;
			}

			string sdp_mid_ = sdp_mid;
			string sdp_ = sdp;
			QueueOperation([=, sdp_mid = move(sdp_mid_), sdp = move(sdp_)]
			{
				ForgeEvents::SendWebRTCIceCandidate(sdp_mid, sdp_mline_index, sdp);
			});
		})
			.Connect();

		webrtcResolution.Disconnect()
			.SetSignal("max_resolution")
			.SetOwner(webrtc)
			.SetFunc([&](calldata_t *data)
		{
			OBSOutput out = reinterpret_cast<obs_output_t*>(calldata_ptr(data, "output"));
			QueueOperation([=]
			{
				OutputResolution webrtc_target_res = { 1280, 720 };
				auto scaled = ScaleResolution(webrtc_target_res.MinByPixels(GetWebRTCMaxResolution()), { ovi.base_width, ovi.base_height });

				video_scale_info vsi{};
				obs_output_get_video_conversion(out, &vsi);
				vsi.width = scaled.width;
				vsi.height = scaled.height;
				vsi.colorspace = (vsi.width >= 1280 || vsi.height >= 720) ? VIDEO_CS_709 : VIDEO_CS_601;
				vsi.gpu_conversion = true;
				vsi.scale_type = OBS_SCALE_BICUBIC;
				vsi.range = VIDEO_RANGE_PARTIAL;

				obs_output_set_video_conversion(out, &vsi);

				blog(LOG_INFO, "webrtcResolution(%s): Updating scaled resolution: %dx%d", obs_output_get_name(out), scaled.width, scaled.height);

				ForgeEvents::SendWebRTCResolutionScaled(webrtc_target_res.MinByPixels(OutputResolution{ ovi.base_width, ovi.base_height }), scaled);
			});
		})
			.Connect();

		webrtcTexture.Disconnect()
			.SetSignal("request_texture")
			.SetOwner(webrtc)
			.SetFunc([&](calldata_t *data)
		{
			OBSOutput out = reinterpret_cast<obs_output_t*>(calldata_ptr(data, "output"));
			auto request = calldata_bool(data, "request");
			QueueOperation([=]
			{
				video_scale_info vsi{};
				obs_output_get_video_conversion(out, &vsi);

				if (request == vsi.texture_output)
					return;

				vsi.texture_output = request;
				vsi.format = vsi.texture_output ? VIDEO_FORMAT_NV12 : VIDEO_FORMAT_I420;

				obs_output_set_video_conversion(out, &vsi);
			});
		})
			.Connect();

		obs_output_set_mixer(webrtc, 1);

		if (!obs_output_start(webrtc))
			return fail("Failed to start webrtc output");

		if (obs_data_get_bool(obj, "create_offer")) {
			auto proc = obs_output_get_proc_handler(webrtc);

			calldata_t data{};
			DEFER{ calldata_free(&data); };

			auto offer = OBSDataCreate();
			calldata_set_ptr(&data, "description", offer);
			calldata_set_bool(&data, "set_local_description", obs_data_get_bool(obj, "set_local_description"));

			proc_handler_call(proc, "create_offer", &data);

			if (auto err = calldata_string(&data, "error"))
				return err;

			obs_data_set_obj(obj, "description", offer);
		}

		UpdateSourceAudioSettings();

		return boost::none;
#else
		return fail("WebRTC not enabled");
#endif
	}

	boost::optional<string> HandleRemoteWebRTCOffer(OBSData &obj)
	{
		auto fail = [&](const char *err)
		{
			blog(LOG_WARNING, "CrucibleContext::HandleRemoteWebRTCOffer: %s", err);
			return err;
		};

#ifdef WEBRTC_WIN
		if (!webrtc)
			return fail("Received remote offer while webrtc wasn't initialized");

		calldata_t data{};
		DEFER{ calldata_free(&data); };

		auto answer = OBSDataCreate();
		calldata_set_ptr(&data, "description", answer);

		calldata_set_string(&data, "type", obs_data_get_string(obj, "type"));
		calldata_set_string(&data, "sdp", obs_data_get_string(obj, "sdp"));

		auto handler = obs_output_get_proc_handler(webrtc);
		proc_handler_call(handler, "handle_remote_offer", &data);

		if (auto err = calldata_string(&data, "error"))
			return err;

		obs_data_set_obj(obj, "description", answer);

		return boost::none;
#else
		return fail("WebRTC not enabled");
#endif
	}

	boost::optional<string> HandleGetWebRTCStats(OBSData &obj)
	{
		auto fail = [&](const char *err)
		{
			blog(LOG_WARNING, "CrucibleContext::HandleGetWebRTCStats: %s", err);
			return err;
		};

#ifdef WEBRTC_WIN
		if (!webrtc)
			return fail("Received stats request while webrtc wasn't initialized");

		calldata_t data{};
		DEFER{ calldata_free(&data); };

		auto stats_data = OBSDataCreate();
		calldata_set_ptr(&data, "data", stats_data);

		auto handler = obs_output_get_proc_handler(webrtc);
		proc_handler_call(handler, "get_stats", &data);

		if (auto err = calldata_string(&data, "error"))
			return err;

		obs_data_set_obj(obj, "stats", stats_data);

		return boost::none;
#else
		return fail("WebRTC not enabled");
#endif

	}

	boost::optional<string> StopWebRTCOutput()
	{
		auto fail = [&](const char *err)
		{
			blog(LOG_WARNING, "CrucibleContext::CreateWebRTC: %s", err);
			return err;
		};

#ifdef WEBRTC_WIN
		if (!webrtc)
			return fail("Received stop request while webrtc wasn't initialized");

		obs_output_force_stop(webrtc);
		webrtc = nullptr;

		return boost::none;
#else
		return fail("WebRTC not enabled");
#endif
	}

	void AddRemoteICECandidate(OBSData &obj)
	{
#ifdef WEBRTC_WIN
		if (!webrtc) {
			blog(LOG_WARNING, "Received ice candidate while webrtc wasn't initialized");
			return;
		}

		calldata_t data{};
		DEFER{ calldata_free(&data); };

		calldata_set_string(&data, "sdp_mid", obs_data_get_string(obj, "sdp_mid"));
		calldata_set_int(&data, "sdp_mline_index", obs_data_get_int(obj, "sdp_mline_index"));
		calldata_set_string(&data, "sdp", obs_data_get_string(obj, "sdp"));

		auto handler = obs_output_get_proc_handler(webrtc);
		proc_handler_call(handler, "add_remote_ice_candidate", &data);
#else
		blog(LOG_WARNING, "WebRTC not enabled");
#endif
	}

	boost::optional<OutputResolution> GetWebRTCMaxResolution()
	{
		if (webrtc) {
			calldata_t data{};
			DEFER{ calldata_free(&data); };

			proc_handler_call(obs_output_get_proc_handler(webrtc), "get_max_resolution", &data);

			auto width = static_cast<uint32_t>(calldata_int(&data, "width"));
			auto height = static_cast<uint32_t>(calldata_int(&data, "height"));
			if (width && height)
				return OutputResolution{ width, height };
		}
		return boost::none;
	}

	void StartRecordingStream(const char *server, const char *key, const char *version)
	{
		bool started = false;

		DEFER{ ForgeEvents::SendStreamingStartExecuted(started); };

		if (recordingStream) {
			blog(LOG_WARNING, "Tried to start recording stream while recording stream was initialized");
			return;
		}

		InitRef(recordingStream, "Couldn't create recording stream", obs_output_release,
			obs_output_create("rtmp_output", "recording stream", nullptr, nullptr));

		obs_output_set_video_encoder(recordingStream, recordingStream_h264);
		obs_output_set_audio_encoder(recordingStream, recordingStream_aac, 0);
		obs_output_set_service(recordingStream, stream_service);

		obs_output_set_reconnect_settings(recordingStream, 5, 1);

		recordingStreamStart
			.Disconnect()
			.SetOwner(recordingStream)
			.Connect();

		recordingStreamStop
			.Disconnect()
			.SetOwner(recordingStream)
			.Connect();

		if (!recordingStream) {
			blog(LOG_WARNING, "Tried to start recording stream while recording stream wasn't initialized");
			return;
		}

		if (obs_output_active(recordingStream)) {
			blog(LOG_WARNING, "Tried to start recording stream while it's already active");
			return;
		}

		auto settings = OBSDataCreate();
		obs_data_set_string(settings, "server", server);
		obs_data_set_string(settings, "key", key);
		obs_service_update(stream_service, settings);

		DStr encoder_name;
		dstr_printf(encoder_name, "Crucible (%s)", version);

		auto ssettings = OBSDataCreate();
		obs_data_set_string(ssettings, "encoder_name", encoder_name->array);
		obs_data_set_bool(ssettings, "new_socket_loop_enabled", true);
		obs_data_set_bool(ssettings, "low_latency_mode_enabled", true);
		obs_data_set_bool(ssettings, "autotune_enabled", true);
		obs_output_update(recordingStream, ssettings);

		if (recordingStream) {
			while (!obs_output_active(recordingStream) && !(started = obs_output_start(recordingStream))) {
				auto encoder = obs_output_get_video_encoder(recordingStream);
				auto id = obs_encoder_get_id(encoder);
				if (id && id == "obs_x264"s)
					break;

				if (!id)
					id = "obs_x264"; // force software encoding

				CreateH264Encoder(&recordingStream_h264, nullptr, true, id);
				obs_output_set_video_encoder(recordingStream, recordingStream_h264);
			}
		}
	}

	void StopRecordingStream()
	{
		ForgeEvents::SendStreamingStopExecuted(obs_output_active(recordingStream));

		obs_output_force_stop(recordingStream);
		recordingStream = nullptr;
	}

	void StartStreaming(const char *server, const char *key, const char *version)
	{
		auto settings = OBSDataCreate();
		obs_data_set_string(settings, "server", server);
		obs_data_set_string(settings, "key", key);
		obs_service_update(stream_service, settings);

		DStr encoder_name;
		dstr_printf(encoder_name, "Crucible (%s)", version);

		auto ssettings = OBSDataCreate();
		obs_data_set_string(ssettings, "encoder_name", encoder_name->array);
		obs_output_update(stream, ssettings);

		UpdateStreamSettings();
		if (obs_output_start(stream)) {
			if (streaming_source)
				obs_set_output_source(0, streaming_source);
			streaming = true;
			recording_stream = true;

			auto restarting_recording = obs_output_active(output);

			StopVideo(true, restarting_recording);
			StartVideoCapture(restarting_recording);

			StartRecordingOutputs(output, buffer);
		}
	
		ForgeEvents::SendStreamingStartExecuted(!obs_output_active(stream));
	}

	void StopStreaming()
	{
		ForgeEvents::SendStreamingStopExecuted(obs_output_active(stream));

		obs_output_stop(stream);
		stream = nullptr;

		streaming = false;
		StopVideo();
	}

	void UpdateSettings(obs_data_t *settings)
	{
		if (!settings)
			return;

		DStr str;
		bool try_custom_ptt = false;

		AnvilCommands::disable_native_indicators = obs_data_get_bool(settings, "disable_native_indicators");

		auto bookmark_key = OBSDataGetObj(settings, "bookmark_key");
		obs_key_combination bookmark_combo = {
			(obs_data_get_bool(bookmark_key, "shift") ? INTERACT_SHIFT_KEY : 0u) |
			(obs_data_get_bool(bookmark_key, "meta") ? INTERACT_COMMAND_KEY : 0u) |
			(obs_data_get_bool(bookmark_key, "ctrl") ? INTERACT_CONTROL_KEY : 0u) |
			(obs_data_get_bool(bookmark_key, "alt") ? INTERACT_ALT_KEY : 0u),
			obs_key_from_virtual_key(static_cast<int>(obs_data_get_int(bookmark_key, "keycode")))
		};

		AnvilCommands::HotkeyMatches(bookmark_combo.key == OBS_KEY_F5 && !bookmark_combo.modifiers);

#ifdef ANVIL_HOTKEYS
		AnvilCommands::SendSettings(bookmark_key,
			OBSDataGetObj(settings, "highlight_key"),
			OBSDataGetObj(settings, "stream_key"),
			OBSDataGetObj(settings, "start_stop_stream_key"),
			OBSDataGetObj(settings, "ptt_key"),
			OBSDataGetObj(settings, "screenshot_key"),
			OBSDataGetObj(settings, "quick_clip_key"),
			OBSDataGetObj(settings, "quick_clip_forward_key"),
			OBSDataGetObj(settings, "cancel_key"),
			OBSDataGetObj(settings, "select_key"),
			OBSDataGetObj(settings, "accept_key"),
			OBSDataGetObj(settings, "decline_key"));
#endif

		obs_key_combination_to_str(bookmark_combo, str);
		blog(LOG_INFO, "bookmark hotkey uses '%s'", str->array);
		obs_hotkey_load_bindings(bookmark_hotkey_id, &bookmark_combo, 1);

		auto ptt_key = OBSDataGetObj(settings, "ptt_key");
		auto microphone = OBSDataGetObj(settings, "microphone");
		if (!microphone) {
			blog(LOG_WARNING, "no microphone data in settings");
			return;
		}

		auto enabled = obs_data_get_bool(microphone, "enabled");
		auto ptt = obs_data_get_bool(microphone, "ptt_mode");
		auto source_settings = OBSDataGetObj(microphone, "source_settings");
		
		auto continuous = enabled && !ptt;

		if (!enabled) {
			try_custom_ptt = true;
		}
		else {
			obs_hotkey_load_bindings(custom_ptt_hotkey_id, nullptr, 0);
			custom_ptt_hotkey_id = OBS_INVALID_HOTKEY_ID;
		}

		ptt = enabled && ptt;

		obs_key_combination combo = {
			(obs_data_get_bool(ptt_key, "shift") ? INTERACT_SHIFT_KEY : 0u) |
			(obs_data_get_bool(ptt_key, "meta")  ? INTERACT_COMMAND_KEY : 0u) |
			(obs_data_get_bool(ptt_key, "ctrl")  ? INTERACT_CONTROL_KEY : 0u) |
			(obs_data_get_bool(ptt_key, "alt")   ? INTERACT_ALT_KEY : 0u),
			obs_key_from_virtual_key(static_cast<int>(obs_data_get_int(ptt_key, "keycode")))
		};

		obs_key_combination_to_str(combo, str);
		blog(LOG_INFO, "mic hotkey uses '%s'", str->array);

		auto desktop_audio_settings = OBSDataGetObj(settings, "desktop_audio");

		record_audio_buffer_only = obs_data_get_bool(settings, "record_audio_buffer_only");

		obs_source_update(tunes, desktop_audio_settings);
		obs_source_update(mic, source_settings);
		obs_source_enable_push_to_talk(mic, ptt);
		AnvilCommands::MicUpdated(ptt, enabled, ptt);
		obs_hotkey_load_bindings(ptt_hotkey_id, &combo, ptt ? 1 : 0);
		if(try_custom_ptt)
			obs_hotkey_load_bindings(custom_ptt_hotkey_id, &combo, 1);
		obs_hotkey_load_bindings(mute_hotkey_id, &combo, continuous ? 1 : 0);
		obs_hotkey_load_bindings(unmute_hotkey_id, &combo, continuous ? 1 : 0);
		obs_set_output_source(2, enabled ? mic : nullptr);

		UpdateSourceAudioSettings();

		[&]
		{
			auto webcam_ = OBSDataGetObj(settings, "webcam");

			auto remove_from_scene = [&](auto &container)
			{
				obs_sceneitem_remove(container.webcam);
				container.webcam = nullptr;
			};

			DEFER{
				game_and_webcam.MakePresentable();
				window_and_webcam.MakePresentable();
				wallpaper_and_webcam.MakePresentable();
				webcam_and_theme.MakePresentable();
			};

			if (!obs_data_has_user_value(webcam_, "device")) {
				remove_from_scene(game_and_webcam);
				remove_from_scene(window_and_webcam);
				remove_from_scene(wallpaper_and_webcam);
				remove_from_scene(webcam_and_theme);

				webcam = nullptr;

				return;
			}

			{
				auto dev = obs_data_get_string(webcam_, "device");

				auto webcam_settings = OBSDataCreate();
				obs_data_set_string(webcam_settings, "video_device_id", dev);

				if (webcam && webcam_device == dev)
					obs_source_update(webcam, webcam_settings);
				else {
					remove_from_scene(game_and_webcam);
					remove_from_scene(window_and_webcam);
					remove_from_scene(wallpaper_and_webcam);
					remove_from_scene(webcam_and_theme);

					InitRef(webcam, "Couldn't create webcam source", obs_source_release,
						obs_source_create(OBS_SOURCE_TYPE_INPUT, "dshow_input", "webcam", webcam_settings, nullptr));
				}

				webcam_device = dev;

				obs_source_set_muted(webcam, true); // webcams can have mics attached to them that dshow_input will pick up sometimes
			}

			auto add_to_scene = [&](auto &container)
			{
				if (container.webcam)
					return;

				container.webcam = obs_scene_add(container.scene, webcam);
				container.MakePresentable();
			};

			add_to_scene(game_and_webcam);
			add_to_scene(window_and_webcam);
			add_to_scene(wallpaper_and_webcam);
			add_to_scene(webcam_and_theme);
		}();

		[&]
		{
			auto new_sli_compatibility = obs_data_get_bool(settings, "sli_compatibility");
			if (new_sli_compatibility == sli_compatibility)
				return;

			sli_compatibility = new_sli_compatibility;

			if (!gameCapture)
				return;

			auto gc_settings = OBSTransferOwned(obs_source_get_settings(gameCapture));
			obs_data_set_bool(gc_settings, "sli_compatibility", sli_compatibility);
			obs_source_update(gameCapture, gc_settings);
		}();

		[&]
		{
			auto new_fps = obs_data_get_int(settings, "frame_rate");
			if (new_fps < 30 || new_fps == target_fps)
				return;

			target_fps = new_fps;

			ResetVideo();
		}();
	}

	void UpdateEncoder(obs_data_t *settings)
	{
		if (!settings)
			return;

		obs_encoder_update(h264, settings);
	}

	void UpdateFilenames(string path, string profiler_path)
	{
		filename = path;
		profiler_filename = profiler_path;
	}

	void UpdateMuxerSettings(const char *settings)
	{
		if (!settings)
			return;

		muxerSettings = settings;
	}

	bool UpdateSize(uint32_t width, uint32_t height)
	{
		bool output_dimensions_changed;
		OutputResolution new_game_res, scaled;

		{
			if (width == 0 || height == 0)
				return false;

			game_res = { width, height };

			ForgeEvents::SendBrowserSizeHint(width, height);

			if (streaming)
				return false;

			if (width == ovi.base_width && height == ovi.base_height)
				return false;
		}

		{
			{
				ovi.base_width = width;
				ovi.base_height = height;
			}

			ResetVideo();

			ResizeRecording();

			OutputResolution webrtc_target_res = { 1280, 720 };
			if (webrtc && obs_output_active(webrtc)) {
				auto scaled = ScaleResolution(webrtc_target_res.MinByPixels(GetWebRTCMaxResolution()), { ovi.base_width, ovi.base_height });
				video_scale_info vsi{};
				obs_output_get_video_conversion(webrtc, &vsi);
				vsi.width = scaled.width;
				vsi.height = scaled.height;
				vsi.colorspace = (vsi.width >= 1280 || vsi.height >= 720) ? VIDEO_CS_709 : VIDEO_CS_601;
				vsi.gpu_conversion = true;
				vsi.scale_type = OBS_SCALE_BICUBIC;
				vsi.range = VIDEO_RANGE_PARTIAL;

				obs_output_set_video_conversion(webrtc, &vsi);

				blog(LOG_INFO, "webrtcResolution(%s): Updating scaled resolution: %dx%d", obs_output_get_name(webrtc), scaled.width, scaled.height);

				ForgeEvents::SendWebRTCResolutionScaled(webrtc_target_res.MinByPixels(OutputResolution{ ovi.base_width, ovi.base_height }), scaled);
			}
		}

		return true;
	}

	void UpdateVideoSettings(obs_data_t *settings)
	{
		if (!settings)
			return;

		OutputResolution new_res{
			static_cast<uint32_t>(obs_data_get_int(settings, "width")),
			static_cast<uint32_t>(obs_data_get_int(settings, "height"))
		};
		uint32_t max_rate = static_cast<uint32_t>(obs_data_get_int(settings, "max_rate"));

		OutputResolution new_stream_res{
			static_cast<uint32_t>(obs_data_get_int(settings, "stream_width")),
			static_cast<uint32_t>(obs_data_get_int(settings, "stream_height"))
		};
		uint32_t stream_rate = static_cast<uint32_t>(obs_data_get_int(settings, "stream_rate"));

		if (max_rate) {
			target_bitrate = max_rate;

			if (h264) {
				auto vsettings = CreateH264EncoderSettings(obs_encoder_get_id(h264), max_rate);

				obs_encoder_update(h264, vsettings);
			}
		}

		if (stream_rate) {
			target_stream_bitrate = stream_rate;

			target_stream = new_stream_res;
			
			if (stream_h264) {
				auto vsettings = OBSDataCreate();
				obs_data_set_int(vsettings, "bitrate", stream_rate);
				obs_data_set_bool(vsettings, "cbr", true);
				obs_data_set_string(vsettings, "profile", "high");
				obs_data_set_string(vsettings, "preset", "veryfast");
				obs_data_set_int(vsettings, "keyint_sec", 2);

				uint32_t width, height;
				if (game_res.width && game_res.height) {
					auto scaled = ScaleResolution(target_stream, game_res);

					width = scaled.width;
					height = scaled.height;
				} else {
					width = target_stream.width;
					height = AlignX264Height(target_stream.height);
				}

				video_scale_info vsi{};
				vsi.format = VIDEO_FORMAT_NV12;
				vsi.gpu_conversion = true;
				vsi.range = VIDEO_RANGE_PARTIAL;
				vsi.scale_type = OBS_SCALE_BICUBIC;
				vsi.width = width;
				vsi.height = height;
				vsi.colorspace = (vsi.width >= 1280 || vsi.height >= 720) ? VIDEO_CS_709 : VIDEO_CS_601;

				obs_encoder_update(stream_h264, vsettings);
			}
		}

		if (new_res.width && new_res.height) {
			target = new_res;

			if (obs_output_active(output))
				UpdateSize(game_res.width, game_res.height);
		}

		if (obs_data_has_user_value(settings, "muxer_settings"))
			UpdateMuxerSettings(obs_data_get_string(settings, "muxer_settings"));
	}

	void SaveGameScreenshot(const char *filename)
	{
		calldata_t data;
		calldata_init(&data);

		calldata_set_string(&data, "filename", filename);

		{
			auto proc = obs_source_get_proc_handler(gameCapture);
			proc_handler_call(proc, "save_screenshot", &data);

			auto id = calldata_int(&data, "screenshot_id");
			requested_screenshots.emplace_back(filename, id);
		}

		calldata_free(&data);
	}

	void SaveScreenshot(const string &source_name, int width, int height, const char *filename)
	{
		OBSSource source;
		if (source_name == "output") {
			source = OBSGetOutputSource(0);
		} else /*if (source_name == "stream_scene")*/ {
			source = streaming_source;
		}

		if (!source) {
			ForgeEvents::SendScreenshotSaved("source '"s + source_name + "' isn't active"s, width, height, filename);
			return;
		}

		Screenshot::Request(source, width, height, filename, ForgeEvents::SendScreenshotSaved);
	}

	bool RecordingActive()
	{
		return obs_output_active(output);
	}

	void UpdateRecordingBufferSettings(OBSData data)
	{
		buffer_settings = data;
	}

	void UpdateDisallowedHardwareEncoders(OBSData &data)
	{
		decltype(disallowed_hardware_encoders) dhe;

		auto arr = OBSDataGetArray(data, "encoders");
		auto size = obs_data_array_count(arr);
		for (size_t i = 0; i < size; i++)
			dhe.emplace(obs_data_get_string(OBSDataArrayItem(arr, i), "id"));

		if (disallowed_hardware_encoders == dhe)
			return;

		swap(disallowed_hardware_encoders, dhe);
		if (obs_encoder_active(h264))
			return;

		CreateH264Encoder();
	}

	void ResizeRecording()
	{
		auto stop_ = [&](obs_output_t *out)
		{
			if (!obs_output_active(out))
				return false;

			auto settings = OBSTransferOwned(obs_output_get_settings(out));
			auto status = GetOutputStatus(settings);
			status.stopping_for_restart = true;
			SetOutputStatus(settings, status);

			obs_output_force_stop(out);
			return true;
		};

		auto output_was_active = stop_(output);
		auto buffer_was_active = stop_(buffer);
		bool recordingStream_was_active = recordingStream && stop_(recordingStream);

		output = nullptr;
		buffer = nullptr;
		recordingStream = nullptr;

		forward_buffer_id = boost::none;

		if (!recording_filename_prefix.empty()) { // Make a new filename for the split recording
			auto cur = boost::posix_time::second_clock::local_time();
			filename = recording_filename_prefix + to_iso_string(cur) + TimeZoneOffset() + ".mp4";
		}

		CreateOutput(true);

		if (output)
			StartRecordingOutputs(output, buffer);
		if (recordingStream_was_active)
			obs_output_start(recordingStream);
	}
	
	bool stopping = false;
	void StopVideo(bool force=false, bool restart=false, bool stop_recording_only=false)
	{
		if (!force && (stopping || streaming || recording_game))
			return;

		stopping = true;

		ProfileScope(profile_store_name(obs_get_profiler_name_store(), "StopVideo()"));

		auto stop_ = [&](obs_output_t *out)
		{
			if (!obs_output_active(out))
				return;

			if (restart) {
				auto settings = OBSTransferOwned(obs_output_get_settings(out));
				auto status = GetOutputStatus(settings);
				status.stopping_for_restart = true;
				SetOutputStatus(settings, status);
			}
			(force ? obs_output_force_stop : obs_output_stop)(out);
		};

		stop_(output);
		if (!stop_recording_only) {
			stop_(buffer);
			if (recordingStream)
				stop_(recordingStream);
			if (webrtc)
				stop_(webrtc);
		}

		output = nullptr;
		if (!stop_recording_only) {
			buffer = nullptr;
			recordingStream = nullptr;
		}

		forward_buffer_id = boost::none;

		if (restart) {
			game_res.width = 0;
			game_res.height = 0;
		}

		stopping = false;
	}

	void StartVideo(bool restarting = false)
	{
		auto iso_time = []
		{
			auto cur = boost::posix_time::second_clock::local_time();
			return to_iso_string(cur) + TimeZoneOffset();
		};

		auto name = profile_store_name(obs_get_profiler_name_store(),
			"StartVideo(%s)", (filename.empty() ? iso_time() : filename).c_str());
		profile_register_root(name, 0);

		ProfileScope(name);

		ovi.fps_den = fps_den;
		ResetVideo();

		game_and_webcam.MakePresentable();
		window_and_webcam.MakePresentable();

		obs_encoder_set_video(h264, obs_get_video());
		obs_encoder_set_video(stream_h264, obs_get_video());
		obs_encoder_set_video(recordingStream_h264, obs_get_video());

		obs_encoder_set_audio(aac, obs_get_audio());
		
		CreateOutput(restarting);
	}

	void StartVideoCapture(bool restarting = false)
	{
		if (obs_output_active(output))
			return;

		StartVideo(restarting);
	}
};

static void HandleConnectCommand(CrucibleContext &cc, OBSData &obj)
{
	const char *str = nullptr;

	if ((str = obs_data_get_string(obj, "log"))) {
		if (log_client.Open(str)) {
			vector<string> buffered_logs;
			if (store_startup_log) {
				LOCK(startup_log_mutex);
				swap(buffered_logs, startup_logs);
				store_startup_log = false;
			}

			blog(LOG_INFO, "Connected log to '%s'", str);

			if (buffered_logs.size()) {
				blog(LOG_INFO, "Replaying startup log to '%s':", str);
				for (auto &log : buffered_logs)
					log_client.Write(log);
				blog(LOG_INFO, "Done replaying startup log to '%s'", str);
			}
		}
	}

	if ((str = obs_data_get_string(obj, "event"))) {
		if (event_client.Open(str)) {
			blog(LOG_INFO, "Connected event to '%s'", str);

			ForgeEvents::SendQueuedEvents();
		}
	}

	if ((str = obs_data_get_string(obj, "anvil_event"))) {
		AnvilCommands::SendForgeInfo(str);
	}
}

static void HandleCaptureCommand(CrucibleContext &cc, OBSData &obj)
{
#ifdef USE_BUGSPLAT
	if (auto user = GetCurrentUsername())
		dmpSender->setDefaultUserName(user->c_str());
#endif

	bool recording_active = cc.RecordingActive();

	if (!recording_active) {
		cc.StopVideo();

		cc.game_session_snapshot = ProfileSnapshotCreate();

		cc.UpdateEncoder(OBSDataGetObj(obj, "encoder"));
		cc.UpdateStreamSettings();

		cc.game_capture_start_time.reset();
		cc.UpdateFilenames(obs_data_get_bool(obj, "recording_disabled") ? "" : obs_data_get_string(obj, "filename"),
			obs_data_get_string(obj, "profiler_data"));
	}

	cc.CreateAudioBufferSource(OBSDataGetObj(obj, "audio_buffer"));
	if (!obs_data_get_bool(obj, "use_window_capture"))
		cc.CreateGameCapture(OBSDataGetObj(obj, "game_capture"));
	else
		cc.CreateGameWindow(OBSDataGetObj(obj, "game_capture"));

	blog(LOG_INFO, "Starting new capture");

	AnvilCommands::ResetShowWelcome(!obs_data_get_bool(obj, "hide_welcome"));

	recording_filename_prefix = string(obs_data_get_string(obj, "filename_prefix"));

	if (!recording_active)
		cc.StartVideoCapture();
}

static void HandleQueryMicsCommand(CrucibleContext&, OBSData&)
{
	unique_ptr<obs_properties_t> props{obs_get_source_properties(OBS_SOURCE_TYPE_INPUT, "wasapi_input_capture")};

	auto devices = OBSDataArrayCreate();
	
	{
		auto data = OBSDataCreate();
		obs_data_set_string(data, "name", "Default");
		obs_data_set_string(data, "device", "default");
		obs_data_array_push_back(devices, data);
	}

	auto prop = obs_properties_get(props.get(), "device_id");

	for (size_t i = 0, c = obs_property_list_item_count(prop); i < c; i++) {
		auto name = obs_property_list_item_name(prop, i);
		if (!name || name == "Default"s)
			continue;

		auto dev = obs_property_list_item_string(prop, i);
		if (!dev || dev == "default"s)
			continue;

		auto device = OBSDataCreate();
		obs_data_set_string(device, "name", name);
		obs_data_set_string(device, "device", dev);
		obs_data_array_push_back(devices, device);
	}

	ForgeEvents::SendQueryMicsResponse(devices);
}

static void HandleUpdateSettingsCommand(CrucibleContext &cc, OBSData &obj)
{
	cc.UpdateSettings(OBSDataGetObj(obj, "settings"));
}

static void HandleSaveRecordingBuffer(CrucibleContext &cc, OBSData &obj)
{
	cc.SaveRecordingBuffer(obj);
}

static void HandleCreateBookmark(CrucibleContext &cc, OBSData &obj)
{
	cc.CreateBookmark(obj);
}

static void HandleStopRecording(CrucibleContext &cc, OBSData &obj)
{
	if (obs_data_get_bool(obj, "cache_limit_exceeded"))
		AnvilCommands::ShowCacheLimitExceeded();

	cc.StopVideo(true, false, true);
}

static void HandleInjectorResult(CrucibleContext &cc, OBSData &data)
{
	cc.ForwardInjectorResult(data);
}

static void HandleMonitoredProcessExit(CrucibleContext &cc, OBSData &data)
{
	cc.ForwardMonitoredProcessExit(data);
}

static void HandleUpdateVideoSettingsCommand(CrucibleContext &cc, OBSData &obj)
{
	cc.UpdateVideoSettings(OBSDataGetObj(obj, "settings"));
}

static void HandleSetCursor(CrucibleContext&, OBSData &obj)
{
	AnvilCommands::SendCursor(obj);
}

static void HandleClipFinished(CrucibleContext &cc, OBSData &obj)
{
	AnvilCommands::ClipFinished(obs_data_get_bool(obj, "success"));
}

static void HandleForgeWillClose(CrucibleContext &cc, OBSData&)
{
	cc.StopVideo(true);
	SetEvent(exit_event);
}

static void HandleCreateWebRTCOutput(CrucibleContext &cc, OBSData &obj)
{
	auto err = cc.CreateWebRTC(OBSDataGetObj(obj, "webrtc"), obj);
	ForgeEvents::SendCreateWebRTCOutputResult(obj, err);
}

static void HandleRemoteWebRTCOffer(CrucibleContext &cc, OBSData &obj)
{
	auto err = cc.HandleRemoteWebRTCOffer(obj);
	ForgeEvents::SendRemoteOfferResult(obj, err);
}

static void HandleGetWebRTCStats(CrucibleContext &cc, OBSData &obj)
{
	auto err = cc.HandleGetWebRTCStats(obj);
	ForgeEvents::SendGetWebRTCStatsResult(obj, err);
}

static void HandleStopWebRTCOutput(CrucibleContext &cc, OBSData &obj)
{
	auto err = cc.StopWebRTCOutput();
	ForgeEvents::SendStopWebRTCOutputResult(obj, err);
}

static void HandleAddRemoteICECandidate(CrucibleContext &cc, OBSData &obj)
{
	cc.AddRemoteICECandidate(obj);
}

static void HandleStartRecordingStream(CrucibleContext &cc, OBSData &obj)
{
	cc.StartRecordingStream(obs_data_get_string(obj, "server"), obs_data_get_string(obj, "key"), obs_data_get_string(obj, "version"));
}

static void HandleStopRecordingStream(CrucibleContext &cc, OBSData&)
{
	cc.StopRecordingStream();
}

static void HandleStartStreaming(CrucibleContext &cc, OBSData& obj)
{
	if (cc.streaming) {
		blog(LOG_INFO, "Streaming already started.");
		ForgeEvents::SendStreamingStartExecuted(false);
		return;
	}

	cc.CreateStreamOutput();
	cc.UpdateFilenames(obs_data_get_string(obj, "filename"), obs_data_get_string(obj, "profiler_data"));
	cc.StartStreaming(obs_data_get_string(obj, "server"), obs_data_get_string(obj, "key"), obs_data_get_string(obj, "version"));
}

static void HandleStopStreaming(CrucibleContext &cc, OBSData&)
{
	cc.StopStreaming();
}

static void HandleGameScreenshot(CrucibleContext &cc, OBSData &obj)
{
	cc.SaveGameScreenshot(obs_data_get_string(obj, "filename"));
}

static void HandleQueryWebcams(CrucibleContext&, OBSData&)
{
	auto props = obs_get_source_properties(OBS_SOURCE_TYPE_INPUT, "dshow_input");

	DEFER {
		obs_properties_destroy(props);
	};

	auto result = OBSDataArrayCreate();

	DEFER {
		ForgeEvents::SendQueryWebcamsResponse(result);
	};

	auto prop = obs_properties_get(props, "video_device_id");
	if (prop) {
		auto count = obs_property_list_item_count(prop);
		for (decltype(count) i = 0; i < count; i++) {
			if (obs_property_list_item_disabled(prop, i))
				continue;

			auto data = OBSDataCreate();
			obs_data_set_string(data, "name", obs_property_list_item_name(prop, i));
			obs_data_set_string(data, "device", obs_property_list_item_string(prop, i));

			obs_data_array_push_back(result, data);
		}
	}
}

static void HandleQueryDesktopAudioDevices(CrucibleContext&, OBSData&)
{
	auto props = obs_get_source_properties(OBS_SOURCE_TYPE_INPUT, "wasapi_output_capture");

	DEFER {
		obs_properties_destroy(props);
	};

	auto result = OBSDataArrayCreate();

	DEFER {
		ForgeEvents::SendQueryDesktopAudioDevicesResponse(result);
	};

	{
		auto data = OBSDataCreate();
		obs_data_set_string(data, "name", "Default");
		obs_data_set_string(data, "device", "default");
		obs_data_array_push_back(result, data);
	}

	auto prop = obs_properties_get(props, "device_id");
	if (prop) {
		auto count = obs_property_list_item_count(prop);
		for (decltype(count) i = 0; i < count; i++) {
			if (obs_property_list_item_disabled(prop, i))
				continue;

			auto name = obs_property_list_item_name(prop, i);
			if (!name || name == "Default"s)
				continue;

			auto device = obs_property_list_item_string(prop, i);
			if (!device || device == "default"s)
				continue;

			auto data = OBSDataCreate();
			obs_data_set_string(data, "name", name);
			obs_data_set_string(data, "device", device);

			obs_data_array_push_back(result, data);
		}
	}
}

static void HandleQueryWindows(CrucibleContext&, OBSData&)
{
	auto props = obs_get_source_properties(OBS_SOURCE_TYPE_INPUT, "window_capture");

	DEFER{
		obs_properties_destroy(props);
	};

	auto result = OBSDataArrayCreate();

	DEFER{
		ForgeEvents::SendQueryWindowsResponse(result);
	};

	auto prop = obs_properties_get(props, "window");
	if (prop) {
		auto count = obs_property_list_item_count(prop);
		for (decltype(count) i = 0; i < count; i++) {
			if (obs_property_list_item_disabled(prop, i))
				continue;

			auto data = OBSDataCreate();
			obs_data_set_string(data, "name", obs_property_list_item_name(prop, i));
			obs_data_set_string(data, "window", obs_property_list_item_string(prop, i));

			obs_data_array_push_back(result, data);
		}
	}
}

static void HandleCaptureWindow(CrucibleContext &cc, OBSData &data)
{
	cc.ResetWindowCapture(OBSDataGetObj(data, "window"));
}

static void HandleSelectScene(CrucibleContext &cc, OBSData &data)
{
	cc.SetOutputScene(obs_data_get_string(data, "scene"));
}

static void HandleConnectDisplay(CrucibleContext&, OBSData &obj)
{
	auto name = obs_data_get_string(obj, "name");
	auto channel = obs_data_get_string(obj, "server");

	auto cx = obs_data_get_int(obj, "width");
	auto cy = obs_data_get_int(obj, "height");

	Display::Connect(name, channel);
	if (cx && cy)
		Display::Resize(name, cx, cy);

	Display::SetEnabled(name, true);
}

static void HandleResizeDisplay(CrucibleContext&, OBSData &obj)
{
	auto name = obs_data_get_string(obj, "name");
	auto cx = obs_data_get_int(obj, "width");
	auto cy = obs_data_get_int(obj, "height");

	Display::Resize(name, cx, cy);
}

static void HandleQueryCanvasSize(CrucibleContext &cc, OBSData&)
{
	ForgeEvents::SendCanvasSize(cc.ovi.base_width, cc.ovi.base_height);
}

static void HandleQuerySceneInfo(CrucibleContext &cc, OBSData&)
{
	cc.SendSceneInfo();
}

static void HandleUpdateScenes(CrucibleContext &cc, OBSData &data)
{
	cc.UpdateScenes(data);
}

static void HandleSetSourceVolume(CrucibleContext &cc, OBSData &data)
{
	cc.SetSourceVolume(obs_data_get_string(data, "source"), obs_data_get_double(data, "volume"), obs_data_get_bool(data, "mute"));
}

static void HandleEnableSourceLevelMeters(CrucibleContext &cc, OBSData &data)
{
	cc.EnableSourceLevelMeters(obs_data_get_bool(data, "enabled"));
}

static void HandleSaveScreenshot(CrucibleContext &cc, OBSData &data)
{
	cc.SaveScreenshot(obs_data_get_string(data, "source"), obs_data_get_int(data, "width"), obs_data_get_int(data, "height"), obs_data_get_string(data, "filename"));
}

static void HandleUpdateRecordingBufferSettings(CrucibleContext &cc, OBSData &data)
{
	cc.UpdateRecordingBufferSettings(OBSDataGetObj(data, "settings"));
}

static void HandleQueryHardwareEncoders(CrucibleContext&, OBSData&)
{
	auto arr = OBSDataArrayCreate();
	DEFER{ ForgeEvents::SendQueryHardwareEncodersResponse(arr); };

	const char *id;
	for (size_t i = 0; obs_enum_encoder_types(i, &id) && id; i++) {
		auto it = find_if(begin(allowed_hardware_encoder_names), end(allowed_hardware_encoder_names), [&](const auto &val) { return val.first == id; });
		if (it == end(allowed_hardware_encoder_names))
			continue;

		auto item = OBSDataCreate();
		obs_data_set_string(item, "id", id);
		obs_data_set_string(item, "name", it->second.c_str());
		obs_data_array_push_back(arr, item);
	}
}

static void HandleUpdateDisallowedHardwareEncoders(CrucibleContext &cc, OBSData &data)
{
	cc.UpdateDisallowedHardwareEncoders(data);
}

static void ShowScreenshotSaved(CrucibleContext &cc, OBSData &data)
{
	AnvilCommands::ShowScreenshot(obs_data_get_bool(data, "success"));
}

static void ShowScreenshotUploading(CrucibleContext &cc, OBSData &data)
{
	AnvilCommands::ShowScreenshotting();
}

static void ShowFirstTimeTutorial(CrucibleContext &cc, OBSData &data)
{
	if (obs_data_get_bool(data, "show"))
		AnvilCommands::ShowFirstTimeTutorial();
	else
		AnvilCommands::HideFirstTimeTutorial();
}

static void StartForwardBuffer(CrucibleContext &cc, OBSData &data)
{
	cc.StartForwardBuffer(data);
}

static void StopForwardBuffer(CrucibleContext &cc, OBSData&)
{
	cc.StopForwardBuffer();
}

static void HandleDismissQuickSelect(CrucibleContext &cc, OBSData&)
{
	AnvilCommands::DismissQuickSelect();
}

static void HandleBeginQuickSelectTimeout(CrucibleContext &cc, OBSData &data)
{
	AnvilCommands::BeginQuickSelectTimeout(obs_data_get_int(data, "timeout_ms"));
}

static void HandleSharedTextureIncompatible(CrucibleContext&, OBSData &data)
{
	AnvilCommands::SendSharedTextureIncompatible(data);
}

namespace {
	struct SkippedMessageLog {
		using clock = chrono::steady_clock;
		using LogType = ProtectedObject<map<string, SkippedMessageLog>>;

		bool first_skip = true;
		size_t times_skipped = 0;
		clock::time_point last_seen;

		static bool SkipMessage(LogType &log, const string &message);
		static void StartThread(LogType &log, JoiningThread &thread, string subsystem_name);
	};
	
	bool SkippedMessageLog::SkipMessage(LogType &log_, const string &message)
	{
		auto log = log_.Lock();
		auto &ml = (*log)[message];

		ml.times_skipped += 1;
		ml.last_seen = clock::now();

		auto first_skip = ml.first_skip;
		ml.first_skip = false;
		return !first_skip;
	}

	static SkippedMessageLog::clock::time_point PrintLogSummary(SkippedMessageLog::LogType &log_, const string &subsystem_name, SkippedMessageLog::clock::time_point last_time)
	{
		ostringstream ss;
		SkippedMessageLog::clock::time_point cur;

		{
			auto log = log_.Lock();
			cur = SkippedMessageLog::clock::now();
			for (auto &elem : *log) {
				if (!elem.second.times_skipped)
					continue;

				auto dur = chrono::duration_cast<chrono::milliseconds>(cur - elem.second.last_seen).count() / 1000.;
				ss << '\t' << elem.first << " skipped " << elem.second.times_skipped << " times (last " << dur << " seconds ago)\n";

				elem.second.times_skipped = 0;
			}
		}

		auto str = ss.str();
		if (!str.empty()) {
			auto dur = chrono::duration_cast<chrono::milliseconds>(cur - last_time).count() / 1000.;
			blog(LOG_INFO, "SkippedMessageLog(%s) summary for the last %g seconds:\n%s", subsystem_name.c_str(), dur, str.c_str());
		}

		return cur;
	}

	void SkippedMessageLog::StartThread(LogType &log, JoiningThread &thread, string subsystem_name)
	{
		thread.Join();

		shared_ptr<void> stop_event{ CreateEvent(nullptr, true, false, nullptr), HandleDeleter{} };
		thread.make_joinable = [=] { SetEvent(stop_event.get()); };
		thread.Run([=, &log]
		{
			auto current_time = clock::now();
			for (; WaitForSingleObject(stop_event.get(), 5 * 60 * 1000) == WAIT_TIMEOUT;)
				current_time = PrintLogSummary(log, subsystem_name, current_time);
		});
	}
}

static SkippedMessageLog::LogType skipped_message_log;
static JoiningThread skipped_message_log_announcer;

static void HandleCommand(CrucibleContext &cc, const uint8_t *data, size_t size)
{
	static const map<string, pair<bool, void(*)(CrucibleContext&, OBSData&)>> known_commands = {
		{ "connect", {false, HandleConnectCommand} },
		{ "capture_new_process", {false, HandleCaptureCommand} },
		{ "query_mics", {false, HandleQueryMicsCommand} },
		{ "update_settings", {false, HandleUpdateSettingsCommand} },
		{ "save_recording_buffer", {false, HandleSaveRecordingBuffer} },
		{ "create_bookmark", {false, HandleCreateBookmark} },
		{ "stop_recording", {false, HandleStopRecording} },
		{ "injector_result", {false, HandleInjectorResult} },
		{ "monitored_process_exit", {false, HandleMonitoredProcessExit} },
		{ "update_video_settings", {false, HandleUpdateVideoSettingsCommand} },
		{ "set_cursor", {false, HandleSetCursor} },
		{ "dismiss_overlay", {false, [](CrucibleContext&, OBSData &data) { AnvilCommands::DismissOverlay(data); }} },
		{ "clip_accepted", {false, [](CrucibleContext&, OBSData&) { AnvilCommands::ShowClipping(); }} },
		{ "clip_finished", {false, HandleClipFinished} },
		{ "forge_will_close", {false, HandleForgeWillClose} },
		{ "create_webrtc_output", {false, HandleCreateWebRTCOutput} },
		{ "remote_webrtc_offer", {false, HandleRemoteWebRTCOffer} },
		{ "get_webrtc_stats", {true, HandleGetWebRTCStats} },
		{ "stop_webrtc_output", {false, HandleStopWebRTCOutput} },
		{ "add_remote_ice_candidate", {false, HandleAddRemoteICECandidate} },
		{ "start_recording_stream", {false, HandleStartRecordingStream} },
		{ "stop_recording_stream", {false, HandleStopRecordingStream} },
		{ "start_streaming", {false, HandleStartStreaming} },
		{ "stop_streaming", {false, HandleStopStreaming} },
		{ "save_game_screenshot", {false, HandleGameScreenshot} },
		{ "query_webcams", {false, HandleQueryWebcams} },
		{ "query_desktop_audio_devices", {false, HandleQueryDesktopAudioDevices} },
		{ "query_windows", {false, HandleQueryWindows} },
		{ "capture_window", {false, HandleCaptureWindow} },
		{ "select_scene", {false, HandleSelectScene} },
		{ "connect_display", {false, HandleConnectDisplay} },
		{ "resize_display", {false, HandleResizeDisplay} },
		{ "query_canvas_size", {false, HandleQueryCanvasSize} },
		{ "query_scene_info", {false, HandleQuerySceneInfo} },
		{ "update_scenes", {false, HandleUpdateScenes} },
		{ "set_source_volume", {false, HandleSetSourceVolume} },
		{ "enable_source_level_meters", {false, HandleEnableSourceLevelMeters} },
		{ "save_screenshot", {false, HandleSaveScreenshot} },
		{ "update_recording_buffer_settings", {false, HandleUpdateRecordingBufferSettings} },
		{ "query_hardware_encoders", {false, HandleQueryHardwareEncoders} },
		{ "update_disallowed_hardware_encoders", {false, HandleUpdateDisallowedHardwareEncoders} },
		{ "screenshot_uploading", {false, ShowScreenshotUploading} },
		{ "show_first_time_tutorial", {false, ShowFirstTimeTutorial} },
		{ "screenshot_saved", {false, ShowScreenshotSaved} },
		{ "start_forward_buffer", {false, StartForwardBuffer} },
		{ "stop_forward_buffer", {false, StopForwardBuffer} },
		{ "dismiss_quick_select", {false, HandleDismissQuickSelect} },
		{ "begin_quick_select_timeout", {false, HandleBeginQuickSelectTimeout} },
		{ "shared_texture_incompatible", {false, HandleSharedTextureIncompatible} },
	};
	if (!data)
		return;

	auto obj = OBSDataCreate({data, data+size});

	unique_ptr<obs_data_item_t> item{obs_data_item_byname(obj, "command")};
	if (!item) {
		blog(LOG_WARNING, "Missing command element on command channel in message: %s", data);
		return;
	}

	const char *str = obs_data_item_get_string(item.get());
	if (!str) {
		blog(LOG_WARNING, "Invalid command element in message: %s", data);
		return;
	}

	auto elem = known_commands.find(str);
	if (elem == cend(known_commands))
		return blog(LOG_WARNING, "Unknown command: %s in message: %s", str, data);

	if (!elem->second.first || !SkippedMessageLog::SkipMessage(skipped_message_log, str))
		blog(LOG_INFO, "got: %s", data);

	QueueOperation([=, &cc]() mutable
	{
		elem->second.second(cc, obj);
	});

	// TODO: Handle changes to frame rate, target resolution, encoder type,
	//       ...
}

static DWORD main_thread_id = 0;

template <typename Fun>
static void QueueOperation(Fun &&f)
{
	auto ptr = make_unique<function<void()>>(f);
	PostThreadMessageA(main_thread_id, WM_APP, reinterpret_cast<WPARAM>(ptr.release()), 0);
}

static bool HandleQueuedOperation(MSG &msg)
{
	if (msg.hwnd)
		return false;

	if (msg.message != WM_APP)
		return false;

	unique_ptr<function<void()>> ptr;
	ptr.reset(reinterpret_cast<function<void()>*>(msg.wParam));
	(*ptr)();

	return true;
}


struct FreeHandle
{
	void operator()(HANDLE h)
	{
		CloseHandle(h);
	}
};
using ProcessHandle = unique_ptr<void, FreeHandle>;


static auto WaitForObjects(const std::initializer_list<HANDLE> &handles, DWORD timeout)
{
	return WaitForMultipleObjects(handles.size(), begin(handles), false, timeout);
}

JoiningThread message_watchdog;
static string StartWatchdog()
{
	WatchdogInfo *watchdog_info = nullptr;

	auto filemap_name = "CrucibleWatchdog" + to_string(GetCurrentProcessId());
	auto filemap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(WatchdogInfo), filemap_name.c_str());
	if (!filemap) {
		blog(LOG_WARNING, "Failed to create filemap '%s': %#x", filemap_name.c_str(), GetLastError());
		filemap_name.clear();
	} else {
		watchdog_info = reinterpret_cast<WatchdogInfo*>(MapViewOfFile(filemap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(WatchdogInfo)));
		if (!watchdog_info) {
			blog(LOG_WARNING, "Failed to MapViewOfFile(%s): %#x", filemap_name.c_str(), GetLastError());
			filemap_name.clear();
		}
	}

	shared_ptr<void> ev{ CreateEvent(nullptr, true, false, nullptr), HandleDeleter{} };
	message_watchdog.make_joinable = [ev] { SetEvent(ev.get()); };

	message_watchdog.t = thread([=]
	{
		using clock = chrono::steady_clock;

		unique_ptr<void, HandleDeleter> message_handled_event{ CreateEvent(nullptr, false, false, nullptr) };
		clock::duration wait_time = 5s;
		while (WaitForObjects({ ev.get() }, chrono::duration_cast<chrono::milliseconds>(wait_time).count()) != WAIT_OBJECT_0) {
			auto next_queue_at = clock::now() + 15s;
			QueueOperation([&]
			{
				SetEvent(message_handled_event.get());
			});

			const auto video_thread_timeout = static_cast<uint64_t>(chrono::duration_cast<chrono::nanoseconds>(20s).count());
			bool video_thread_caused_break = false;

			auto res = WaitForObjects({ ev.get(), message_handled_event.get() }, 10 * 1000);
			switch (res) {
			case WAIT_OBJECT_0:
				return;

			case WAIT_OBJECT_0 + 1:
				uint64_t video_thread_time;
				if (obs_get_video_thread_time(&video_thread_time)) {
					auto current_time = os_gettime_ns();
					if (video_thread_time < current_time && (current_time - video_thread_time) > video_thread_timeout) {
						video_thread_caused_break = true;
						break;
					}
				}
				wait_time = max(chrono::duration_cast<clock::duration>(5s), next_queue_at - clock::now());
				continue;
			}

			if (watchdog_info) {
				if (video_thread_caused_break)
					watchdog_info->graphics_thread_stuck = true;
				else
					watchdog_info->message_thread_stuck = true;
			}

			if (IsDebuggerPresent()) {
				__debugbreak();

				if (watchdog_info) {
					if (video_thread_caused_break)
						watchdog_info->graphics_thread_stuck = false;
					else
						watchdog_info->message_thread_stuck = false;
				}
			} else {
#ifdef USE_BUGSPLAT
				wostringstream sstr;
				sstr << boolalpha << "watchdog triggered dump; graphics_thread_stuck: " << video_thread_caused_break;
				dmpSender->setDefaultUserDescription(sstr.str().c_str());
				dmpSender->createReport(); // createReportAndExit seems to not create a report
				_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // turn off other error reporting for abort
#endif
				abort(); // Message wasn't handled
			}
		}
	});

	return filemap_name;
}

static vector<HANDLE> wait_handles;
static unordered_map<HANDLE, function<void()>> handle_callbacks;
static void AddWaitHandleCallback(HANDLE h, function<void()> cb)
{
	wait_handles.push_back(h);
	handle_callbacks.emplace(h, cb);
}

static void RemoveWaitHandle(HANDLE h)
{
	{
		auto it = find(begin(wait_handles), end(wait_handles), h);
		if (it != end(wait_handles))
			wait_handles.erase(it);
	}
	{
		auto it = handle_callbacks.find(h);
		if (it != end(handle_callbacks))
			handle_callbacks.erase(h);
	}
}

void TestVideoRecording(TestWindow &window, ProcessHandle &forge, HANDLE start_event)
{
	try
	{
		SkippedMessageLog::StartThread(skipped_message_log, skipped_message_log_announcer, "command");

		Display::SetEnabled("preview", false);

		CrucibleContext crucibleContext;

		{
			ProfileScope("CrucibleContext Init");

			crucibleContext.InitLibobs(!forge);
			crucibleContext.InitStreamService();
			crucibleContext.InitSources();
			crucibleContext.InitEncoders();
			crucibleContext.InitSignals();
			crucibleContext.StopVideo();
		}

#ifdef TEST_WINDOW
		// TODO: remove once we're done debugging
		gs_init_data dinfo = {};
		dinfo.cx = 800;
		dinfo.cy = 480;
		dinfo.format = GS_RGBA;
		dinfo.zsformat = GS_ZS_NONE;
		dinfo.window.hwnd = window.GetHandle();

		OBSDisplay display(obs_display_create(&dinfo));
		if (!display)
			throw "Couldn't create display";

		obs_display_add_draw_callback(display, RenderWindow, nullptr);
#endif

		auto path = GetModulePath(nullptr);
		DStr path64;
		dstr_printf(path64, "%sAnvilRendering64.dll", path->array);
		dstr_cat(path, "AnvilRendering.dll");

		// update source settings - tell game_capture to try and capture hl2: lost coast
		/*auto csettings = OBSDataCreate();
		obs_data_set_bool(csettings, "capture_any_fullscreen", false);
		obs_data_set_bool(csettings, "capture_cursor", true);
		obs_data_set_string(csettings, "overlay_dll", path);
		obs_data_set_string(csettings, "overlay_dll64", path64);
		obs_data_set_string(csettings, "window", "Half-Life 2#3A Lost Coast:Valve001:hl2.exe");
		crucibleContext.UpdateGameCapture(csettings);*/

		crucibleContext.bookmark_hotkey_id = obs_hotkey_register_frontend("bookmark hotkey", "bookmark hotkey",
			[](void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
		{
			if (!pressed)
				return;

			QueueOperation([=]
			{
				if (static_cast<CrucibleContext*>(data)->gameWindow)
					ForgeEvents::SendBookmarkRequest();
			});
		}, &crucibleContext);

		crucibleContext.custom_ptt_hotkey_id = obs_hotkey_register_frontend("custom PTT key", "custom PTT key",
			[](void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
		{
			ForgeEvents::SendPTTStatus(pressed);
		}, &crucibleContext);

		auto handleCommand = [&](const uint8_t *data, size_t size)
		{
			if (!data) {
				blog(LOG_WARNING, "Command connection died, shutting down");
				SetEvent(exit_event);
				return;
			}
			HandleCommand(crucibleContext, data, size);
		};

		exit_event = CreateEvent(nullptr, true, false, nullptr);

		IPCServer remote{"ForgeCrucible", handleCommand};

		{
			auto snap = ProfileSnapshotCreate();
			profiler_print(snap.get()); // print startup stats
		}

		if (start_event)
			SetEvent(start_event);

		{
			auto map_name = StartWatchdog();
			if (!map_name.empty())
				ForgeEvents::SendWatchdogInfoName(map_name);
		}

		wait_handles.emplace_back(exit_event);
		if (forge)
			wait_handles.emplace_back(forge.get());

		MSG msg;
		DWORD reason = WAIT_TIMEOUT;
		while (true)
		{
			reason = MsgWaitForMultipleObjects(wait_handles.size(), wait_handles.data(), false, INFINITE, QS_ALLINPUT);
			if (reason >= WAIT_OBJECT_0 && (reason - WAIT_OBJECT_0) < wait_handles.size()) {
				auto &handle = wait_handles[reason - WAIT_OBJECT_0];

				if (handle == exit_event) {
					blog(LOG_INFO, "Exit requested");
					break;
				}

				if (forge && handle == forge.get()) {
					blog(LOG_INFO, "Forge exited, exiting");
					break;
				}

				auto it = handle_callbacks.find(handle);
				if (it != end(handle_callbacks)) {
					auto cb = move(it->second);
					handle_callbacks.erase(it);
					wait_handles.erase(begin(wait_handles) + (reason - WAIT_OBJECT_0));
					cb();
					continue;
				} else {
					blog(LOG_WARNING, "Signaled object doesn't have an associated callback");
				}
			}
			
			if (reason == (WAIT_OBJECT_0 + wait_handles.size())) {
				while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
				{
					if (HandleQueuedOperation(msg))
						continue;
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}
				continue;
			}

			throw "Unexpected value from MsgWaitForMultipleObjects";
		}

		message_watchdog.Join();
		crucibleContext.StopVideo();
		Display::StopAll();
	}
	catch (const char *err)
	{
		blog(LOG_ERROR, "Error: %s", err);
	}

}

static ProcessHandle HandleCLIArgs(HANDLE &start_event)
{
	auto argvFree = [](wchar_t *argv[]) { LocalFree(argv); };
	int argc = 0;
	auto argv = unique_ptr<wchar_t*[], decltype(argvFree)>{CommandLineToArgvW(GetCommandLineW(), &argc), argvFree};

	if (!argv || argc <= 1)
		throw make_pair("Started without arguments, exiting", -1);

	if (wstring(L"-standalone") == argv[1]) {
		blog(LOG_INFO, "Running standalone");
		return {};
	}

	if (argc <= 2)
		throw make_pair("Not enough arguments for non-standalone", -4);

	DWORD pid;
	wistringstream ss(argv[1]);
	if (!(ss >> pid))
		throw make_pair("Couldn't read PID from argv", -2);

	ss = wistringstream(argv[2]);
	if (!(ss >> start_event))
		throw make_pair("Couldn't read event id from argv", -3);

	return ProcessHandle{ OpenProcess(SYNCHRONIZE, false, pid) };
}

#ifndef CONFIG_DIRECTORY_NAME
#define CONFIG_DIRECTORY_NAME "Forge"
#endif

static DStr GetConfigDirectory(const char *subdir)
{
	wchar_t *fpath;

	SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &fpath);
	DStr path;
	dstr_from_wcs(path, fpath);

	CoTaskMemFree(fpath);

	dstr_catf(path, "/" CONFIG_DIRECTORY_NAME "/%s", subdir);

	return path;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmd)
{
#ifdef USE_BUGSPLAT
	dmpSender = new MiniDmpSender(BUGSPLAT_DATABASE, L"Crucible", BUGSPLAT_APP_VERSION, L"", MDSF_USEGUARDMEMORY | MDSF_LOGFILE | MDSF_NONINTERACTIVE | MDSF_PREVENTHIJACKING);

	_set_purecall_handler([]
	{
		dmpSender->createReportAndExit();
	});

	_set_invalid_parameter_handler([](const wchar_t *exp, const wchar_t *function, const wchar_t *file, unsigned line, uintptr_t reserved)
	{
		dmpSender->createReportAndExit();
	});

	signal(SIGABRT, [](int)
	{
		dmpSender->createReportAndExit();
	});

	std::set_terminate([]
	{
		dmpSender->createReportAndExit();
	});

	if (auto user = GetCurrentUsername())
		dmpSender->setDefaultUserName(user->c_str());

	dmpSender->setCallback([](UINT nCode, LPVOID lVal1, LPVOID lVal2) -> bool {
		if (nCode == MDSCB_EXCEPTIONCODE)
		{
			if (auto name = GetCurrentUsername())
				dmpSender->setDefaultUserName(name->c_str());
		}
		return false;
	});
#endif

	main_thread_id = GetCurrentThreadId();

	base_set_log_handler(do_log, nullptr);
	base_set_crash_handler([](const char*, va_list, void*)
	{
		abort();
	}, nullptr);

	unique_ptr<profiler_name_store_t> profiler_names{profiler_name_store_create()};
	profiler_start();

	ProcessHandle forge;
	HANDLE start_event = nullptr;
	try
	{
		forge = HandleCLIArgs(start_event);
	}
	catch (std::pair<const char*, int> &err)
	{
		blog(LOG_ERROR, "ERROR: %s (%#x)", err.first, GetLastError());
		return err.second;
	}

	store_startup_log = true;

	blog(LOG_INFO, "Crucible PID %lu", GetCurrentProcessId());

	try
	{
		if (!obs_startup("en-US", GetConfigDirectory("obs-module-config"), profiler_names.get()))
			throw "Couldn't init OBS";

		TestWindow window(hInstance);

#ifdef TEST_WINDOW
		TestWindow::RegisterWindowClass(hInstance);

		if (!window.Create(800, 480, "libobs test"))
			throw "Couldn't create test window";

		window.Show();
#endif

		TestVideoRecording(window, forge, start_event);
	}
	catch (const char *err)
	{
		blog(LOG_ERROR, "Error: %s", err);
	}

	obs_shutdown();

	{
		auto snap = ProfileSnapshotCreate();
		profiler_print(snap.get());
	}

	profiler_stop();
	profiler_names.reset();

	blog(LOG_INFO, "Number of memory leaks: %ld", bnum_allocs());

	UNUSED_PARAMETER(hPrevInstance);
	UNUSED_PARAMETER(lpCmdLine);
	UNUSED_PARAMETER(nCmd);

	return 0;
}
