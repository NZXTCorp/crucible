
#define NOMINMAX
#include <stdio.h>
#include <windows.h>

#include <util/base.h>
#include <util/dstr.hpp>
#include <util/platform.h>
#include <util/profiler.hpp>
#include <obs.hpp>

#include "OBSHelpers.hpp"

#include "IPC.hpp"
#include "ProtectedObject.hpp"
#include "ThreadTools.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>

#include <boost/optional.hpp>

#include "scopeguard.hpp"

#include "RemoteDisplay.h"

static const std::string info_header_fragment = "FramebufferInfo";

#define LOCK(x) std::lock_guard<decltype(x)> CONCAT(lockGuard, __LINE__){x};

struct RemoteDisplay {
	const char *remote_display_name = "";
	OBSDisplay display;
	OBSView view;
	OBSSource source;

	bool Connect(const std::string &name)
	{
		LOCK(send_mutex);
		if (!framebuffer_client.Open(name))
			return false;

		StartSendThread();
		return true;
	}

	void Display(obs_source_t *source_)
	{
		LOCK(draw_mutex);
		if (!source_) {
			view = nullptr;
			display = nullptr;
			return;
		}

		CreateDisplay();

		if (!view)
			view = obs_view_create();

		obs_view_set_source(view, 0, source = source_);
	}

	void Enable(bool enabled)
	{
		LOCK(draw_mutex);
		CreateDisplay();

		obs_display_set_enabled(display, enabled);
	}

	~RemoteDisplay()
	{
		obs_enter_graphics();
		for (auto &tr : texrender)
			gs_texrender_destroy(tr);

		for (auto &sf : stagesurf)
			gs_stagesurface_destroy(sf);
		obs_leave_graphics();
	}

	void Resize(uint32_t cx, uint32_t cy)
	{
		LOCK(draw_mutex);
		draw_cx = cx;
		draw_cy = cy;
	}

protected:
	std::vector<gs_texrender_t*> texrender;
	std::vector<gs_stagesurf_t*> stagesurf;

	struct MappingInfo {
		uint32_t width;
		uint32_t height;
		uint32_t line_size;
		uint8_t *data = nullptr;
	};

	using Mapped_t = std::pair<gs_stagesurf_t*, MappingInfo>;

	std::deque<gs_texrender_t*> idle_texrender;
	std::deque<gs_stagesurf_t*> idle_stagesurface;
	std::deque<gs_texrender_t*> rendering_texrender;
	std::deque<std::pair<gs_texrender_t*, gs_stagesurf_t*>> staging_texrender;
	ProtectedObject<std::deque<Mapped_t>> copyable_stagesurface;
	ProtectedObject<std::deque<gs_stagesurf_t*>> copied_stagesurface;

	std::condition_variable cv;
	std::mutex mutex;
	bool send = false;
	bool exit = false;

	std::mutex send_mutex;
	IPCClient framebuffer_client;

	JoiningThread send_thread;

	uint32_t draw_cx = 0;
	uint32_t draw_cy = 0;

	void StartSendThread()
	{
		if (send_thread.t.get_id() != std::thread::id{})
			return;

		send_thread.make_joinable = [&]
		{
			exit = true;
			cv.notify_one();
		};

		send_thread.t = std::thread([&]
		{
			OBSData data = OBSDataCreate();
			bool last_send_failed = false;
			for (;;) {
				{
					std::unique_lock<std::mutex> lk(mutex);
					cv.wait(lk, [&] { return send || exit; });
					if (exit)
						return;

					send = false;
				}

				Mapped_t mapped;
				{
					auto cs = copyable_stagesurface.Lock();
					if (cs->empty())
						continue;
					mapped = cs->front();
					cs->pop_front();
				}

				DEFER
				{
					auto cs = copied_stagesurface.Lock();
					cs->push_back(mapped.first);
				};

				auto &info = mapped.second;

				obs_data_set_int(data, "line_size", info.line_size);
				obs_data_set_int(data, "width", info.width);
				obs_data_set_int(data, "height", info.height);

				bool success = false;
				do {
					LOCK(send_mutex);
					auto json = obs_data_get_json(data);
					success = !!json;
					if (!json) {
						blog(LOG_WARNING, "RemoteDisplay[%s]: failed to materialize info json");
						break;
					}

					success = framebuffer_client.Write(info_header_fragment + json);
					if (!success && !last_send_failed) {
						blog(LOG_WARNING, "RemoteDisplay[%s]: failed to send info");
						break;
					}

					success = framebuffer_client.Write(info.data, info.line_size * info.height);
					if (success && last_send_failed)
						blog(LOG_INFO, "RemoteDisplay[%s]: resumed sending (size: %u, payload: %u)", remote_display_name, info.line_size * info.height, info.width * info.height * 4);
					else if (!success && last_send_failed)
						blog(LOG_WARNING, "RemoteDisplay[%s]: failed to send (size: %u, payload: %u)", remote_display_name, info.line_size * info.height, info.width * info.height * 4);
				} while (false);

				last_send_failed = !success;
			}
		});
	}

	std::recursive_mutex draw_mutex;
	void Draw()
	{
		std::vector<gs_texrender_t*> almost_idle_texrender;
		std::vector<gs_stagesurf_t*> almost_idle_stagesurface;

		DEFER
		{
			idle_texrender.insert(end(idle_texrender), begin(almost_idle_texrender), end(almost_idle_texrender));
			idle_stagesurface.insert(end(idle_stagesurface), begin(almost_idle_stagesurface), end(almost_idle_stagesurface));
		};

		{
			auto cs = copied_stagesurface.Lock();
			if (!cs->empty()) {
				idle_stagesurface.push_back(cs->front());
				cs->pop_front();
			}
		}

		do {
			if (!staging_texrender.empty()) {
				auto staging = staging_texrender.front();

				Mapped_t mapped;
				if (!gs_stagesurface_map(staging.second, &mapped.second.data, &mapped.second.line_size))
					break;

				mapped.second.width = gs_stagesurface_get_width(staging.second);
				mapped.second.height = gs_stagesurface_get_height(staging.second);
				mapped.first = staging.second;

				almost_idle_texrender.push_back(staging.first);

				staging_texrender.pop_front();

				auto css = copyable_stagesurface.Lock();
				for (auto &cs : *css) {
					gs_stagesurface_unmap(cs.first);
					almost_idle_stagesurface.push_back(cs.first);
				}
				css->clear();

				css->push_back(mapped);

				{
					LOCK(mutex);
					send = true;
				}
				cv.notify_one();
			}
		} while (false);

		do {
			if (!rendering_texrender.empty()) {
				auto tr = rendering_texrender.front();
				auto tex = gs_texrender_get_texture(tr);

				auto cx = gs_texture_get_width(tex);
				auto cy = gs_texture_get_height(tex);

				while (!idle_stagesurface.empty()) {
					auto sf = idle_stagesurface.front();
					if (cx == gs_stagesurface_get_width(sf) && cy == gs_stagesurface_get_height(sf))
						break;

					gs_stagesurface_destroy(sf);

					stagesurf.erase(std::remove(begin(stagesurf), end(stagesurf), sf), end(stagesurf));
					idle_stagesurface.pop_front();
				}

				if (idle_stagesurface.empty()) {
					auto sf = gs_stagesurface_create(cx, cy, GS_RGBA);
					if (!sf)
						break;

					stagesurf.push_back(sf);
					idle_stagesurface.push_back(sf);
				}

				auto sf = idle_stagesurface.front();

				gs_stage_texture(sf, tex);

				staging_texrender.emplace_back(tr, sf);

				rendering_texrender.pop_front();
				idle_stagesurface.pop_front();
			}
		} while (false);

		uint32_t cx = 0;
		uint32_t cy = 0;
		{
			LOCK(draw_mutex);
			if (!view || !source)
				return;

			cx = obs_source_get_width(source);
			cy = obs_source_get_height(source);

			if (!draw_cx || !draw_cy)
			{
				draw_cx = cx;
				draw_cy = cy;
			}
		}

		if (!cx || !cy)
			return;

		do {
			if (idle_texrender.empty()) {
				auto tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
				if (!tr)
					break;

				texrender.push_back(tr);
				idle_texrender.push_back(tr);
			}

			auto tr = idle_texrender.front();

			gs_texrender_reset(tr);
			if (gs_texrender_begin(tr, draw_cx, draw_cy)) {
				gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
				gs_set_viewport(0, 0, draw_cx, draw_cy);

				obs_view_render(view);

				gs_texrender_end(tr);

				rendering_texrender.push_back(tr);
				idle_texrender.pop_front();
			}
		} while (false);
	}

	void CreateDisplay()
	{
		if (!display) {
			auto render_cb = [](void *param, uint32_t width, uint32_t height)
			{
				auto &rd = *reinterpret_cast<RemoteDisplay*>(param);
				rd.Draw();
			};

			display = obs_display_create(nullptr);

			obs_display_add_draw_callback(display, render_cb, this);
		}
	}
};


static std::map<std::string, RemoteDisplay> displays;

namespace Display {
	void SetSource(const char *name, obs_source_t *source)
	{
		auto &display = displays[name];
		display.remote_display_name = name;
		display.Display(source);
	}

	bool Connect(const char *name, const char *server)
	{
		auto &display = displays[name];
		display.remote_display_name = name;
		return display.Connect(server);
	}

	void SetEnabled(const char *name, bool enable)
	{
		auto &display = displays[name];
		display.remote_display_name = name;
		display.Enable(enable);
	}

	void Resize(const char *name, uint32_t cx, uint32_t cy)
	{
		auto &display = displays[name];
		display.remote_display_name = name;
		display.Resize(cx, cy);
	}

	std::vector<std::string> List()
	{
		std::vector<std::string> result;
		result.reserve(displays.size());
		for (auto &display : displays)
			result.push_back(display.first);

		return result;
	}

	void Stop(const char *name)
	{
		auto it = displays.find(name);
		if (it == end(displays))
			return;

		displays.erase(it);
	}

	void StopAll()
	{
		displays.clear();
	}
}
