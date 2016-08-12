
#include <util/platform.h>
#include <obs.hpp>
#include <media-io/video-io.h>

#include "OBSHelpers.hpp"

#include "IPC.hpp"

#include <atomic>
#include <mutex>
#include <vector>

#define CONCAT2(x, y) x ## y
#define CONCAT(x, y) CONCAT2(x, y)
#define LOCK(x) lock_guard<decltype(x)> CONCAT(lockGuard, __LINE__){x};

using namespace std;

static const string info_header_fragment = "FramebufferInfo";

struct CrucibleFramebufferServer {
	IPCServer server;

	std::string name;

	atomic<bool> died = true;

	struct Metadata {
		uint32_t width;
		uint32_t height;
		uint32_t line_size;
	};

	Metadata incoming_data;
	bool have_metadata = false;

#pragma optimize("tsg", on)
	template <typename Fun>
	void Start(Fun fun)
	{
		static atomic<int> restarts = 0;
		died = false;

		name = "CrucibleFramebufferServer" + to_string(GetCurrentProcessId()) + "-" + to_string(restarts++);

		server.Start(name, [&, fun](uint8_t *data, size_t size)
		{
			if (!data) {
				died = true;
				blog(LOG_WARNING, "CrucibleFramebufferServer: died");
				return;
			}

			if (!have_metadata)	{
				if (size < info_header_fragment.size() + 2 || memcmp(info_header_fragment.data(), data, info_header_fragment.size()) != 0)
					return;

				auto info = OBSDataCreate({data + info_header_fragment.size(), data + size - 1 });

				incoming_data.width = static_cast<uint32_t>(obs_data_get_int(info, "width"));
				incoming_data.height = static_cast<uint32_t>(obs_data_get_int(info, "height"));
				incoming_data.line_size = static_cast<uint32_t>(obs_data_get_int(info, "line_size"));

				have_metadata = true;
				return;
			}

			if (size < incoming_data.line_size * incoming_data.height) {
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

struct FramebufferSource;

static FramebufferSource *cast(void *context)
{
	return reinterpret_cast<FramebufferSource*>(context);
}

struct FramebufferSource {
	CrucibleFramebufferServer server;
	obs_source_t *source;

	obs_source_frame frame = {};

	FramebufferSource() = default;
	FramebufferSource(obs_source_t *source)
		: source(source)
	{
		StartServer();

		auto proc = obs_source_get_proc_handler(source);
		proc_handler_add(proc, "void get_server_name(out string name)", [](void *context, calldata_t *data)
		{
			auto self = cast(context);
			if (self->server.died)
				self->StartServer();

			calldata_set_string(data, "name", self->server.name.c_str());
		}, this);
	}

protected:
	void StartServer()
	{
		frame.format = VIDEO_FORMAT_BGRA;
		frame.full_range = true;

		server.Start([&](uint8_t *data, size_t size, CrucibleFramebufferServer::Metadata metadata)
		{
			frame.width = metadata.width;
			frame.height = metadata.height;
			frame.linesize[0] = metadata.line_size;
			frame.data[0] = data;

			frame.timestamp = os_gettime_ns();

			obs_source_output_video(source, &frame);
		});
	}
};

void RegisterFramebufferSource()
{
	obs_source_info info = {};
	info.id = "FramebufferSource";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	info.get_name = [](auto) { return "Framebuffer Source"; };
	info.create = [](auto, obs_source_t *source) { return static_cast<void*>(new FramebufferSource{ source }); };
	info.destroy = [](void *context) { delete cast(context); };

	obs_register_source(&info);
}
