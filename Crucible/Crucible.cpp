// [crucible.cpp 2015-10-22 abright]
// libobs-based game capture (currently an experimental thing based on the libobs sample app)

#include <stdio.h>
#include <windows.h>

#include <util/base.h>
#include <util/dstr.hpp>
#include <util/profiler.hpp>
#include <obs.hpp>

#include "OBSHelpers.hpp"

#include <atomic>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
using namespace std;

#include <boost/logic/tribool.hpp>

#include "IPC.hpp"

// window class borrowed from forge, remove once we've got headless mode working
#include "TestWindow.h"

extern OBSEncoder CreateAudioEncoder(const char *name);

static IPCClient event_client, log_client;

atomic<bool> store_startup_log = false;
vector<string> startup_logs;
mutex startup_log_mutex;

#define CONCAT2(x, y) x ## y
#define CONCAT(x, y) CONCAT2(x, y)
#define LOCK(x) lock_guard<decltype(x)> CONCAT(lockGuard, __LINE__){x};

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

	if (log_level < LOG_WARNING)
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

	OBSData EventCreate(const char * name)
	{
		auto event = OBSDataCreate();

		obs_data_set_string(event, "event", name);
		obs_data_set_int(event, "timestamp", GetTickCount64());

		return event;
	}

	void SendRecordingStart(const char *filename)
	{
		auto event = EventCreate("started_recording");

		obs_data_set_string(event, "filename", filename);

		SendEvent(event);
	}

	void SendRecordingStop(const char *filename, int total_frames)
	{
		auto event = EventCreate("stopped_recording");

		obs_data_set_string(event, "filename", filename);
		obs_data_set_int(event, "total_frames", total_frames);

		SendEvent(event);
	}

	void SendQueryMicsResponse(obs_data_array_t *devices)
	{
		auto event = EventCreate("query_mics_response");

		obs_data_set_array(event, "devices", devices);

		SendEvent(event);
	}

	void SendBufferReady(const char *filename)
	{
		auto event = EventCreate("buffer_ready");

		obs_data_set_string(event, "filename", filename);

		SendEvent(event);
	}
}

namespace AnvilCommands {
	IPCClient anvil_client;
	recursive_mutex commandMutex;

	atomic<bool> recording = false;
	atomic<bool> using_mic = false;
	atomic<bool> using_ptt = false;
	atomic<bool> mic_muted = false;

	string forge_overlay_channel;

	void SendForgeInfo(const char *info=nullptr);

	void Connect(DWORD pid)
	{
		LOCK(commandMutex);

		anvil_client.Open("AnvilRenderer" + to_string(pid));

		SendForgeInfo();
	}

	void SendCommand(obs_data_t *cmd)
	{
		if (!cmd)
			return;

		auto data = obs_data_get_json(cmd);
		if (!data)
			return;

		LOCK(commandMutex);
		if (anvil_client.Write(data, strlen(data) + 1))
			return;

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

		const char *indicator = recording ? "capturing" : "idle";
		if (recording && using_mic)
			indicator = mic_muted ? (using_ptt ? "mic_idle" : "mic_muted") : "mic_active";

		obs_data_set_string(cmd, "indicator", indicator);

		SendCommand(cmd);
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

	void MicUpdated(boost::tribool muted, boost::tribool active=boost::indeterminate, boost::tribool ptt=boost::indeterminate)
	{
		bool changed = false;
		if (!boost::indeterminate(active))
			changed = active != using_mic.exchange(active);
		if (!boost::indeterminate(muted))
			changed = (muted != mic_muted.exchange(muted)) || changed;
		if (!boost::indeterminate(ptt))
			changed = (ptt != using_ptt.exchange(ptt)) || changed;

		if (!changed)
			return;

		SendIndicator();
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
}

template <typename T, typename U>
static void InitRef(T &ref, const char *msg, void (*release)(U*), U *val)
{
	if (!val)
		throw msg;

	ref = val;
	release(val);
}

static decltype(ProfileSnapshotCreate()) last_session;

struct CrucibleContext {
	obs_video_info ovi;
	uint32_t fps_den;
	OBSSource tunes, mic, gameCapture;
	OBSSourceSignal micMuted, pttActive;
	OBSSourceSignal stopCapture, startCapture;
	OBSEncoder h264, aac;
	string filename = "";
	string muxerSettings = "";
	OBSOutput output, buffer;
	OBSOutputSignal startRecording, stopRecording;
	OBSOutputSignal bufferSaved;

	uint32_t target_width = 1280;
	uint32_t target_height = 720;

	DWORD game_pid = -1;

	obs_hotkey_id ptt_hotkey_id = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id mute_hotkey_id = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id unmute_hotkey_id = OBS_INVALID_HOTKEY_ID;

	struct RestartThread {
		thread t;
		~RestartThread()
		{
			if (t.joinable())
				t.join();
		}
	} restartThread;

	bool ResetVideo()
	{
		return obs_reset_video(&ovi) == 0;
	}

	void InitLibobs(bool standalone)
	{
		ovi.adapter = 0;
		ovi.base_width = 1280;
		ovi.base_height = 720;
		ovi.fps_num = 30;
		ovi.fps_den = fps_den = 1;
		ovi.graphics_module = "libobs-d3d11.dll";
		ovi.output_format = VIDEO_FORMAT_NV12;
		ovi.output_width = 1280;
		ovi.output_height = 720;
		ovi.scale_type = OBS_SCALE_BICUBIC;
		ovi.range = VIDEO_RANGE_PARTIAL;
		ovi.gpu_conversion = true;
		ovi.colorspace = VIDEO_CS_601;
		if (ovi.output_width >= 1280 || ovi.output_height >= 720)
			ovi.colorspace = VIDEO_CS_709;

		if (!ResetVideo())
			throw "Couldn't initialize video";

		obs_audio_info ai;
		ai.samples_per_sec = 44100;
		ai.speakers = SPEAKERS_STEREO;
		ai.buffer_ms = 1000;
		if (!obs_reset_audio(&ai))
			throw "Couldn't initialize audio";

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

	void InitSources()
	{
		InitRef(mic, "Couldn't create audio input device source", obs_source_release,
			obs_source_create(OBS_SOURCE_TYPE_INPUT, "wasapi_input_capture", "wasapi mic", nullptr, nullptr));

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

		// create game capture video source
		InitRef(gameCapture, "Couldn't create game capture source", obs_source_release,
				obs_source_create(OBS_SOURCE_TYPE_INPUT, "game_capture", "game capture", nullptr, nullptr));

		obs_set_output_source(0, gameCapture);
		obs_set_output_source(1, tunes);
	}

	void InitEncoders()
	{
		auto vsettings = OBSDataCreate();
		obs_data_set_int(vsettings, "bitrate", 2500);
		obs_data_set_int(vsettings, "buffer_size", 0);
		obs_data_set_int(vsettings, "crf", 23);
		obs_data_set_bool(vsettings, "use_bufsize", true);
		obs_data_set_bool(vsettings, "cbr", false);
		obs_data_set_string(vsettings, "profile", "high");
		obs_data_set_string(vsettings, "preset", "veryfast");

		InitRef(h264, "Couldn't create video encoder", obs_encoder_release,
				obs_video_encoder_create("obs_x264", "x264 video", vsettings, nullptr));


		aac = CreateAudioEncoder("aac");
		if (!aac)
			throw "Couldn't create audio encoder";


		obs_encoder_set_video(h264, obs_get_video());
		obs_encoder_set_audio(aac, obs_get_audio());
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

		stopRecording
			.SetSignal("stop")
			.SetFunc([=](calldata*)
		{
			auto data = OBSTransferOwned(obs_output_get_settings(output));
			ForgeEvents::SendRecordingStop(obs_data_get_string(data, "path"),
				obs_output_get_total_frames(output));
			AnvilCommands::ShowIdle();
			StopVideo(); // leak here!!!

			auto snap = ProfileSnapshotCreate();
			auto diff = unique_ptr<profiler_snapshot_t>{profile_snapshot_diff(last_session.get(), snap.get())};

			profiler_print(diff.get());
			profiler_print_time_between_calls(diff.get());

			last_session = move(snap);
		});

		startRecording
			.SetSignal("start")
			.SetFunc([=](calldata*)
		{
			auto data = OBSTransferOwned(obs_output_get_settings(output));
			ForgeEvents::SendRecordingStart(obs_data_get_string(data, "path"));
			AnvilCommands::ShowRecording();
		});

		bufferSaved
			.SetSignal("buffer_output_finished")
			.SetFunc([=](calldata_t *data)
		{
			auto filename = calldata_string(data, "filename");
			ForgeEvents::SendBufferReady(filename);
		});

		stopCapture
			.SetOwner(gameCapture)
			.SetSignal("stop_capture");

		startCapture
			.SetOwner(gameCapture)
			.SetSignal("start_capture");
	}

	void CreateOutput()
	{
		auto osettings = OBSDataCreate();
		obs_data_set_string(osettings, "path", filename.c_str());
		obs_data_set_string(osettings, "muxer_settings", muxerSettings.c_str());

		InitRef(output, "Couldn't create output", obs_output_release,
				obs_output_create("ffmpeg_muxer", "ffmpeg recorder", osettings, nullptr));

		obs_output_set_video_encoder(output, h264);
		obs_output_set_audio_encoder(output, aac, 0);

		InitRef(buffer, "Couldn't create buffer output", obs_output_release,
				obs_output_create("ffmpeg_recordingbuffer", "ffmpeg recordingbuffer", nullptr, nullptr));

		obs_output_set_video_encoder(buffer, h264);
		obs_output_set_audio_encoder(buffer, aac, 0);

		stopRecording
			.Disconnect()
			.SetOwner(output)
			.Connect();

		startRecording
			.Disconnect()
			.SetOwner(output)
			.Connect();

		bufferSaved
			.Disconnect()
			.SetOwner(buffer)
			.Connect();

		auto weakOutput = OBSGetWeakRef(output);
		auto weakBuffer = OBSGetWeakRef(buffer);

		stopCapture
			.Disconnect()
			.SetFunc([=](calldata_t*)
		{
			auto ref = OBSGetStrongRef(weakOutput);
			if (ref)
				obs_output_stop(ref);

			ref = OBSGetStrongRef(weakBuffer);
			if (ref)
				obs_output_stop(ref);
		}).Connect();

		startCapture
			.Disconnect()
			.SetFunc([=](calldata_t *data)
		{
			AnvilCommands::Connect(game_pid);

			if (UpdateSize(static_cast<uint32_t>(calldata_int(data, "width")),
				       static_cast<uint32_t>(calldata_int(data, "height"))))
				return;

			auto ref = OBSGetStrongRef(weakOutput);
			if (ref)
				obs_output_start(ref);

			ref = OBSGetStrongRef(weakBuffer);
			if (ref)
				obs_output_start(ref);
		}).Connect();
	}

	recursive_mutex updateMutex;

	void SaveRecordingBuffer(obs_data_t *settings)
	{
		if (!settings)
			return;

		LOCK(updateMutex);
		auto proc = obs_output_get_proc_handler(buffer);
		calldata_t param{};
		calldata_init(&param);
		calldata_set_string(&param, "filename", obs_data_get_string(settings, "filename"));
		proc_handler_call(proc, "output_precise_buffer", &param);
	}

	void UpdateGameCapture(obs_data_t *settings)
	{
		if (!settings)
			return;

		LOCK(updateMutex);
		game_pid = static_cast<DWORD>(obs_data_get_int(settings, "process_id"));
		obs_source_update(gameCapture, settings);
	}

	void UpdateSettings(obs_data_t *settings)
	{
		if (!settings)
			return;

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
		ptt = enabled && ptt;

		obs_key_combination combo = {
			(obs_data_get_bool(ptt_key, "shift") ? INTERACT_SHIFT_KEY : 0) |
			(obs_data_get_bool(ptt_key, "meta")  ? INTERACT_COMMAND_KEY : 0) |
			(obs_data_get_bool(ptt_key, "ctrl")  ? INTERACT_CONTROL_KEY : 0) |
			(obs_data_get_bool(ptt_key, "alt")   ? INTERACT_ALT_KEY : 0),
			obs_key_from_virtual_key(static_cast<int>(obs_data_get_int(ptt_key, "keycode")))
		};

		DStr str;
		obs_key_combination_to_str(combo, str);
		blog(LOG_INFO, "mic hotkey uses '%s'", str->array);

		LOCK(updateMutex);
		obs_source_update(mic, source_settings);
		obs_source_set_muted(mic, false);
		AnvilCommands::MicUpdated(ptt, enabled, ptt);
		obs_hotkey_load_bindings(ptt_hotkey_id, &combo, ptt ? 1 : 0);
		obs_hotkey_load_bindings(mute_hotkey_id, &combo, continuous ? 1 : 0);
		obs_hotkey_load_bindings(unmute_hotkey_id, &combo, continuous ? 1 : 0);
		obs_set_output_source(2, enabled ? mic : nullptr);
	}

	void UpdateEncoder(obs_data_t *settings)
	{
		if (!settings)
			return;

		obs_encoder_update(h264, settings);
	}

	void UpdateFilename(const char *path)
	{
		if (!path)
			return;

		LOCK(updateMutex);
		filename = path;
	}

	void UpdateMuxerSettings(const char *settings)
	{
		if (!settings)
			return;

		LOCK(updateMutex);
		muxerSettings = settings;
	}

	bool UpdateSize(uint32_t width, uint32_t height)
	{
		LOCK(updateMutex);

		if (width == ovi.base_width && height == ovi.base_height)
			return false;

		if (width > target_width) {
			auto scale = width / static_cast<float>(target_width);
			auto new_height = height / scale;

			ovi.base_width = width;
			ovi.base_height = height;
			ovi.output_width = target_width;
			ovi.output_height = static_cast<uint32_t>(new_height);

		} else {
			ovi.base_width = width;
			ovi.base_height = height;
			ovi.output_width = width;
			ovi.output_height = height;
		}

		// TODO: this is probably not really safe, should introduce a command queue soon
		if (restartThread.t.joinable())
			restartThread.t.join();

		restartThread.t = thread{[=]()
		{
			StopVideo();
			StartVideo();

			obs_output_start(this->output);
		}};

		return true;
	}

	bool stopping = false;
	void StopVideo()
	{
		LOCK(updateMutex);
		if (stopping)
			return;

		ProfileScope(profile_store_name(obs_get_profiler_name_store(), "StopVideo()"));

		stopping = true;
		if (obs_output_active(output))
			obs_output_stop(output);

		output = nullptr;

		ovi.fps_den = 0;
		ResetVideo();
		stopping = false;
	}

	void StartVideo()
	{
		LOCK(updateMutex);
		auto name = profile_store_name(obs_get_profiler_name_store(),
			"StartVideo(%s)", filename.c_str());
		profile_register_root(name, 0);

		ProfileScope(name);

		ovi.fps_den = fps_den;
		ResetVideo();

		obs_encoder_set_video(h264, obs_get_video());
		obs_encoder_set_audio(aac, obs_get_audio());

		CreateOutput();
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
	cc.StopVideo();

	last_session = ProfileSnapshotCreate();

	cc.UpdateGameCapture(OBSDataGetObj(obj, "game_capture"));
	cc.UpdateEncoder(OBSDataGetObj(obj, "encoder"));
	cc.UpdateFilename(obs_data_get_string(obj, "filename"));
	cc.UpdateMuxerSettings(obs_data_get_string(obj, "muxer_settings"));
	blog(LOG_INFO, "Starting new capture");
	cc.StartVideo();
}

static void HandleQueryMicsCommand(CrucibleContext&, OBSData&)
{
	unique_ptr<obs_properties_t> props{obs_get_source_properties(OBS_SOURCE_TYPE_INPUT, "wasapi_input_capture")};

	auto devices = OBSDataArrayCreate();

	auto prop = obs_properties_get(props.get(), "device_id");

	for (size_t i = 0, c = obs_property_list_item_count(prop); i < c; i++) {
		auto device = OBSDataCreate();
		obs_data_set_string(device, "name", obs_property_list_item_name(prop, i));
		obs_data_set_string(device, "device", obs_property_list_item_string(prop, i));
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

static void HandleCommand(CrucibleContext &cc, const uint8_t *data, size_t size)
{
	static const map<string, void(*)(CrucibleContext&, OBSData&)> known_commands = {
		{"connect", HandleConnectCommand},
		{"capture_new_process", HandleCaptureCommand},
		{"query_mics", HandleQueryMicsCommand},
		{"update_settings", HandleUpdateSettingsCommand},
		{"save_recording_buffer", HandleSaveRecordingBuffer},
	};
	if (!data)
		return;

	auto obj = OBSDataCreate({data, data+size});

	blog(LOG_INFO, "got: %s", data);

	unique_ptr<obs_data_item_t> item{obs_data_item_byname(obj, "command")};
	if (!item) {
		blog(LOG_WARNING, "Missing command element on command channel");
		return;
	}

	const char *str = obs_data_item_get_string(item.get());
	if (!str) {
		blog(LOG_WARNING, "Invalid command element");
		return;
	}

	auto elem = known_commands.find(str);
	if (elem == cend(known_commands))
		return blog(LOG_WARNING, "Unknown command: %s", str);

	elem->second(cc, obj);

	// TODO: Handle changes to frame rate, target resolution, encoder type,
	//       ...
}

auto FreeProcessHandle = [](HANDLE h) { CloseHandle(h); };
using ProcessHandle = unique_ptr<void, decltype(FreeProcessHandle)>;

void TestVideoRecording(TestWindow &window, ProcessHandle &forge, HANDLE start_event)
{
	try
	{
		CrucibleContext crucibleContext;

		{
			ProfileScope("CrucibleContext Init");

			crucibleContext.InitLibobs(!forge);
			crucibleContext.InitSources();
			crucibleContext.InitEncoders();
			crucibleContext.InitSignals();
			crucibleContext.StopVideo();
		}

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

		auto path = GetModulePath(nullptr);
		DStr path64;
		dstr_printf(path64, "%sAnvilRendering64.dll", path->array);
		dstr_cat(path, "AnvilRendering.dll");

		// update source settings - tell game_capture to try and capture hl2: lost coast
		auto csettings = OBSDataCreate();
		obs_data_set_bool(csettings, "capture_any_fullscreen", false);
		obs_data_set_bool(csettings, "capture_cursor", true);
		obs_data_set_string(csettings, "overlay_dll", path);
		obs_data_set_string(csettings, "overlay_dll64", path64);
		obs_data_set_string(csettings, "window", "Half-Life 2#3A Lost Coast:Valve001:hl2.exe");
		crucibleContext.UpdateGameCapture(csettings);

		auto handleCommand = [&](const uint8_t *data, size_t size)
		{
			HandleCommand(crucibleContext, data, size);
		};

		IPCServer remote{"ForgeCrucible", handleCommand};

		last_session = ProfileSnapshotCreate();
		profiler_print(last_session.get()); // print startup stats

		if (start_event)
			SetEvent(start_event);

		MSG msg;

		if (forge) {
			DWORD reason = WAIT_TIMEOUT;
			HANDLE h = forge.get();
			while (WAIT_OBJECT_0 != reason)
			{
				switch (reason = MsgWaitForMultipleObjects(1, &h, false, INFINITE, QS_ALLINPUT)) {
				case WAIT_OBJECT_0:
					blog(LOG_INFO, "Forge exited, exiting");
					break;

				case WAIT_OBJECT_0 + 1:
					while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
					{
						TranslateMessage(&msg);
						DispatchMessage(&msg);
					}
					break;

				default:
					throw "Unexpected value from MsgWaitForMultipleObjects";
				}
			}

		} else {
			while (GetMessage(&msg, nullptr, 0, 0))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}

		crucibleContext.StopVideo();
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

	return ProcessHandle{OpenProcess(SYNCHRONIZE, false, pid), FreeProcessHandle};
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmd)
{
	base_set_log_handler(do_log, nullptr);

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

	if (forge)
		store_startup_log = true;

	try
	{
		if (!obs_startup("en-US", "module-config", profiler_names.get()))
			throw "Couldn't init OBS";

		TestWindow window(hInstance);

		TestWindow::RegisterWindowClass(hInstance);

		if (!window.Create(800, 480, "libobs test"))
			throw "Couldn't create test window";

		window.Show();

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
