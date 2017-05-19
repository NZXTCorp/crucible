
#include <util/platform.h>
#include <obs.hpp>

#include "OBSHelpers.hpp"

#include "IPC.hpp"

#include <atomic>
#include <mutex>
#include <vector>
#include <map>

#define CONCAT2(x, y) x ## y
#define CONCAT(x, y) CONCAT2(x, y)
#define LOCK(x) lock_guard<decltype(x)> CONCAT(lockGuard, __LINE__){x};

using namespace std;

static const string info_header_fragment = "AudioBufferInfo";

struct CrucibleAudioBufferServer {
	std::string name;

	atomic<bool> died = true;

	IPCServer server;

	template <typename Fun>
	void Start(Fun fun)
	{
		static atomic<int> restarts = 0;
		died = false;

		server.Start(name, [&, fun](uint8_t *data, size_t size)
		{
			if (!data) {
				died = true;
				blog(LOG_WARNING, "CrucibleAudioBufferServer: died");
				return;
			}

			fun(data, size);
		}, -1);
	}

	void Stop()
	{
		server.server.reset();
		died = true;
	}
};

template <typename T, typename U>
static void read(uint8_t *&read_ptr, U &val)
{
	T tmp;
	memcpy(&tmp, read_ptr, sizeof(T));
	read_ptr += sizeof(T);
	val = static_cast<U>(tmp);
}

template <typename T>
static void read(uint8_t *&read_ptr, T &val)
{
	memcpy(&val, read_ptr, sizeof(T));
	read_ptr += sizeof(T);
}

struct AudioBufferSource;

static AudioBufferSource *cast(void *context)
{
	return reinterpret_cast<AudioBufferSource*>(context);
}

struct AudioBufferSource {
	obs_source_t *source;

	obs_source_audio frame = {};

	map<uint64_t, obs_source_audio_stream_t*> streams;

	CrucibleAudioBufferServer server;

	AudioBufferSource() = default;
	AudioBufferSource(obs_data_t *settings, obs_source_t *source)
		: source(source)
	{
		server.name = obs_data_get_string(settings, "pipe_name");
		if (!server.name.empty())
			StartServer();
	}

protected:
	void StartServer()
	{
		server.Start([&](uint8_t *data, size_t size)
		{
			auto read_ptr = data;
			uint64_t id;

			read(read_ptr, id);
			read(read_ptr, frame.samples_per_sec);
			read<uint32_t>(read_ptr, frame.speakers);
			read<uint32_t>(read_ptr, frame.format);
			read(read_ptr, frame.frames);
			frame.data[0] = read_ptr;			

			frame.timestamp = os_gettime_ns();

			auto it = streams.find(id);
			if (it == end(streams)) {
				auto stream = obs_source_add_audio_stream(source);
				blog(LOG_INFO, "[AudioBufferSource '%s']: adding new stream %llu (%p)", obs_source_get_name(source), id, stream);
				it = streams.emplace(id, stream).first;
			}

			obs_source_output_audio_stream(source, it->second, &frame);
		});
	}
};

void RegisterAudioBufferSource()
{
	obs_source_info info = {};
	info.id = "AudioBufferSource";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = [](auto) { return "AudioBuffer Source"; };
	info.create = [](obs_data_t *settings, obs_source_t *source) { return static_cast<void*>(new AudioBufferSource{ settings, source }); };
	info.destroy = [](void *context) { delete cast(context); };

	obs_register_source(&info);
}
