
#define NOMINMAX
#include <stdio.h>
#include <windows.h>

#include <obs.hpp>

#include "OBSHelpers.hpp"

#include "ProtectedObject.hpp"
#include "ThreadTools.hpp"

#include <algorithm>
#include <deque>

#include "scopeguard.hpp"

#include "ScreenshotProvider.h"

using namespace std;

#define LOCK(x) std::lock_guard<decltype(x)> CONCAT(lockGuard, __LINE__){x};

struct ScreenshotProvider
{
	struct ScreenshotRequest
	{
		OBSSource source;
		uint32_t cx, cy;
		string filename;
		request_completion_t callback;

		ScreenshotRequest(OBSSource source, uint32_t cx_, uint32_t cy_, const string& filename_, request_completion_t callback_)
			: source(source)
		{
			filename = filename_;
			cx = cx_;
			cy = cy_;
			callback = callback_;
		}

		void UpdateSize(uint32_t cx_, uint32_t cy_)
		{
			cx = cx_;
			cy = cy_;
		}

		void Complete(bool success)
		{
			if (callback)
				callback(success, cx, cy, filename);
		}
	};

	OBSDisplay display;
	OBSView view;
	
	deque<ScreenshotRequest> pending_request;
	
	bool requested = false;
	bool copied = false;
	bool staged = false;
	bool saving = false;
	bool saved = false;
	bool success = false;
	mutex save_mutex;

	gs_texrender_t *tr = nullptr;
	gs_stagesurf_t *stage = nullptr;

	~ScreenshotProvider()
	{
		pending_request.clear();

		// todo: kill the save thread

		obs_enter_graphics();
			
		if (tr)
			gs_texrender_destroy(tr);
		
		if (stage)
			gs_stagesurface_destroy(stage);

		obs_leave_graphics();
	}

	void Create()
	{
		if (!display)
		{
			display = obs_display_create(nullptr);

			auto draw_cb = [](void *param, uint32_t cx, uint32_t cy)
			{
				auto& provider = *reinterpret_cast<ScreenshotProvider *>(param);
				provider.Draw();
			};

			obs_display_add_draw_callback(display, draw_cb, this);
		}
	}

	void Enable(bool enabled)
	{
		LOCK(draw_mutex);

		obs_display_set_enabled(display, enabled);
		if (!enabled)
			return;

		Create();

		if (!view)
			view = obs_view_create();
	}

	void Request(OBSSource source, uint32_t cx, uint32_t cy, const string &filename, request_completion_t callback)
	{
		pending_request.push_back(ScreenshotRequest(source, cx, cy, filename, callback));
		requested = true;
		Enable(true);
	}

	std::recursive_mutex draw_mutex;
	void Draw()
	{
		if (saved) {
			auto request = pending_request.front();
			request.Complete(success);
			
			pending_request.pop_front();

			gs_stagesurface_destroy(stage);
			stage = nullptr;
			requested = !pending_request.empty();
			copied = false;
			staged = false;
			saving = false;
			saved = false;
			success = false;

			Enable(requested);
		}
		
		if (staged && !saving) {
			auto work = CreateThreadpoolWork([](PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work)
			{
				auto self = reinterpret_cast<ScreenshotProvider*>(context);
				auto request = self->pending_request.front();

				obs_enter_graphics();
				if (!(self->success = gs_stagesurface_save_to_file(self->stage, request.filename.c_str())))
					blog(LOG_WARNING, "screenshot: gs_stagesurface_save_to_file failed for \"%s\"", request.filename.c_str());
				obs_leave_graphics();
				
				self->saved = true;
			}, this, nullptr);

			SubmitThreadpoolWork(work);
			CloseThreadpoolWork(work);

			saving = true;
		}
	
		if (copied && !staged) {
			auto tex = gs_texrender_get_texture(tr);
			if (!stage) {
				auto& request = pending_request.front();
				stage = gs_stagesurface_create(request.cx, request.cy, GS_RGBA);
			}

			gs_stage_texture(stage, tex);
			staged = true;
		}

		if (requested && !copied) {
			auto& request = pending_request.front();

			uint32_t draw_cx = request.cx;
			uint32_t draw_cy = request.cy;

			uint32_t cx = 0;
			uint32_t cy = 0;
			{
				LOCK(draw_mutex);
				if (!view) 
					return;

				cx = obs_source_get_width(request.source);
				cy = obs_source_get_height(request.source);
				obs_view_set_source(view, 0, request.source);

				if (!draw_cx || !draw_cy) {
					draw_cx = cx;
					draw_cy = cy;
					request.UpdateSize(cx, cy);
				}
			}

			DEFER
			{
				if (request.source)
					obs_view_set_source(view, 0, nullptr);
			};

			if (!cx || !cy) 
				return;

			do {
				if (!tr) {
					tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
					if (!tr)
						break;
				}

				auto display_aspect = draw_cx / static_cast<double>(draw_cy);
				auto source_aspect = cx / static_cast<double>(cy);

				int x, y, width, height;
				double scale;
				if (display_aspect > source_aspect) {
					scale = draw_cy / static_cast<double>(cy);
					width = static_cast<int>(draw_cy * source_aspect);
					height = draw_cy;
				}
				else {
					scale = draw_cx / static_cast<double>(cx);
					width = draw_cx;
					height = static_cast<int>(draw_cx / source_aspect);
				}
				x = draw_cx / 2 - width / 2;
				y = draw_cy / 2 - height / 2;
				width = static_cast<int>(scale * cx);
				height = static_cast<int>(scale * cy);

				gs_texrender_reset(tr);
				if (gs_texrender_begin(tr, draw_cx, draw_cy)) {
					gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
					gs_set_viewport(x, y, width, height);

					gs_effect_t    *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
					gs_eparam_t    *color = gs_effect_get_param_by_name(solid, "color");
					gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

					vec4 colorVal;
					vec4_set(&colorVal, 0.0f, 0.0f, 0.0f, 1.0f);
					gs_effect_set_vec4(color, &colorVal);

					gs_technique_begin(tech);
					gs_technique_begin_pass(tech, 0);
					gs_matrix_push();
					gs_matrix_identity();
					gs_matrix_scale3f(float(cx), float(cy), 1.0f);

					gs_render_start(false);
					gs_vertex2f(0.0f, 0.0f);
					gs_vertex2f(0.0f, 1.0f);
					gs_vertex2f(1.0f, 1.0f);
					gs_vertex2f(1.0f, 0.0f);
					gs_vertex2f(0.0f, 0.0f);
					gs_render_stop(GS_TRISTRIP);

					gs_matrix_pop();
					gs_technique_end_pass(tech);
					gs_technique_end(tech);

					gs_load_vertexbuffer(nullptr);

					obs_view_render(view);

					gs_texrender_end(tr);

					copied = true;
				}
			} while (false);
		}
	}
};

static ScreenshotProvider provider;

namespace Screenshot {
	void Request(OBSSource source, uint32_t cx, uint32_t cy, const string &filename, request_completion_t callback)
	{
		provider.Request(source, cx, cy, filename, callback);
	}
};