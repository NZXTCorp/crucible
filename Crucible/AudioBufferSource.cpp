
#include <util/platform.h>
#include <obs.hpp>

#include "OBSHelpers.hpp"

#include "IPC.hpp"

#include <atomic>
#include <mutex>
#include <vector>

#define CONCAT2(x, y) x ## y
#define CONCAT(x, y) CONCAT2(x, y)
#define LOCK(x) lock_guard<decltype(x)> CONCAT(lockGuard, __LINE__){x};

using namespace std;

static const string info_header_fragment = "AudioBufferInfo";

struct CrucibleAudioBufferServer {
	std::string name;

	atomic<bool> died = true;

	struct Metadata {
		uint32_t sample_rate;
		enum speaker_layout speakers;
		enum audio_format format;
		uint32_t frames;
	};

	Metadata incoming_data;
	bool have_metadata = false;

	IPCServer server;

#pragma optimize("tsg", on)
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

			if (!have_metadata) {
				if (size < info_header_fragment.size() + 2 || memcmp(info_header_fragment.data(), data, info_header_fragment.size()) != 0)
					return;

				auto info = OBSDataCreate({ data + info_header_fragment.size(), data + size - 1 });

				incoming_data.sample_rate = static_cast<uint32_t>(obs_data_get_int(info, "sample_rate"));
				incoming_data.speakers = static_cast<speaker_layout>(obs_data_get_int(info, "speaker_layout"));
				incoming_data.format = static_cast<audio_format>(obs_data_get_int(info, "format"));
				incoming_data.frames = static_cast<uint32_t>(obs_data_get_int(info, "frames"));

				have_metadata = true;
				return;
			}

			if (size < incoming_data.frames * incoming_data.format) {
				have_metadata = false;
				return;
			}

			have_metadata = false;

			fun(data, size, incoming_data);
		}, -1);
	}

	void Stop()
	{
		server.server.reset();
		died = true;
	}
};

struct AudioBufferSource;

static AudioBufferSource *cast(void *context)
{
	return reinterpret_cast<AudioBufferSource*>(context);
}

struct AudioBufferSource {
	obs_source_t *source;

	obs_source_audio frame = {};

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
		server.Start([&](uint8_t *data, size_t size, CrucibleAudioBufferServer::Metadata metadata)
		{
			frame.samples_per_sec = metadata.sample_rate;
			frame.speakers = metadata.speakers;
			frame.format = metadata.format;
			frame.frames = metadata.frames;
			frame.data[0] = data;			

			frame.timestamp = os_gettime_ns();

			obs_source_output_audio(source, &frame);
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
