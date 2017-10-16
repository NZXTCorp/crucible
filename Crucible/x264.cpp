#include <obs.hpp>
#include <x264.h>

#include <webrtc/common_video/h264/h264_bitstream_parser.h>
#include <webrtc/media/base/codec.h>
#include <webrtc/modules/include/module_common_types.h>
#include <webrtc/modules/video_coding/include/video_codec_interface.h>
#include <webrtc/modules/video_coding/include/video_error_codes.h>
#include <webrtc/video_encoder.h>

#include <deque>
#include <memory>
#include <vector>

#include <boost/optional.hpp>

#include <util/dstr.hpp>
#include <util/profiler.hpp>

#include "OBSHelpers.hpp"


#define do_log(level, output, format, ...) \
	blog(level, "[WebRTC(x264): '%s'] " format, \
			obs_output_get_name(output), ##__VA_ARGS__)

#define error(format, ...) do_log(LOG_ERROR,   output, format, ##__VA_ARGS__)
#define warn(format, ...)  do_log(LOG_WARNING, output, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    output, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   output, format, ##__VA_ARGS__)

using namespace std;

namespace std {
	template <>
	struct default_delete<x264_t> {
		void operator()(x264_t *context)
		{
			x264_encoder_close(context);
		}
	};
}

static const char *get_x264_colorspace_name(enum video_colorspace cs)
{
	switch (cs) {
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_601:
		return "undef";
	case VIDEO_CS_709:;
	}

	return "bt709";
}

static int get_x264_cs_val(enum video_colorspace cs,
	const char *const names[])
{
	const char *name = get_x264_colorspace_name(cs);
	int idx = 0;
	do {
		if (strcmp(names[idx], name) == 0)
			return idx;
	} while (!!names[++idx]);

	return 0;
}

static bool convert_profile(x264_param_t &param, webrtc::H264::Profile profile)
{
	using namespace webrtc::H264;

	const char *name = nullptr;
	bool constrained = false;

	switch (profile) {
	case kProfileConstrainedBaseline:
		constrained = true;
	case kProfileBaseline:
		name = "baseline";
		break;

	case kProfileMain:
		name = "main";
		break;

	case kProfileConstrainedHigh:
		constrained = true;
	case kProfileHigh:
		name = "high";
		break;
	}

	if (!name)
		return false;

	if (x264_param_apply_profile(&param, name)) {
		return false;
	}

	return true;
}

static webrtc::FrameType convert_frame_type(int type, int keyframe) {
	if (keyframe)
		return webrtc::kVideoFrameKey;

	switch (type) {
	case X264_TYPE_KEYFRAME:
		return webrtc::kVideoFrameKey;
	case X264_TYPE_IDR:
	case X264_TYPE_I:
	case X264_TYPE_P:
	case X264_TYPE_BREF:
	case X264_TYPE_B:
		return webrtc::kVideoFrameDelta;
	}
	RTC_NOTREACHED() << "Unexpected/invalid frame type: " << type;
	return webrtc::kEmptyFrame;
}


static void x264_log(void *context, int i_level, const char *psz, va_list args)
{
	auto output = static_cast<obs_output_t*>(context);

	DStr formatted;
	dstr_vprintf(formatted, psz, args);

	info("[x264] %s", formatted->array);
}


static void RTPFragmentize(webrtc::EncodedImage &encoded_image,
	vector<uint8_t> &encoded_image_buffer,
	int nal_count,
	x264_nal_t *nals,
	webrtc::RTPFragmentationHeader* frag_header);

namespace {
	struct x264Encoder : webrtc::VideoEncoder {
		obs_output_t *output = nullptr;

		unique_ptr<x264_t> context;
		x264_param_t param{};

		webrtc::H264PacketizationMode packetization_mode = webrtc::H264PacketizationMode::SingleNalUnit;
		webrtc::H264BitstreamParser bitstream_parser;

		webrtc::EncodedImageCallback *callback = nullptr;

		boost::optional<int> keyframe_interval;

		const char *profiler_name = nullptr;

		x264Encoder(obs_output_t *output, const cricket::VideoCodec &codec, boost::optional<int> keyframe_interval)
			: output(output), keyframe_interval(keyframe_interval)
		{
			info("Created encoder");

			string packetization_mode_str;
			if (codec.GetParam(cricket::kH264FmtpPacketizationMode, &packetization_mode_str) && packetization_mode_str == "1")
				packetization_mode = webrtc::H264PacketizationMode::NonInterleaved;
		}

		~x264Encoder()
		{
			if (!profiler_name)
				return;

			auto snap = ProfileSnapshotCreate();
			profiler_snapshot_filter_roots(snap.get(), [](void *data, const char *name, bool *remove)
			{
				auto profiler_name = static_cast<const char*>(data);

				*remove = strcmp(profiler_name, name) != 0;

				return true;
			}, const_cast<char*>(profiler_name));
			profiler_print(snap.get());
		}

		// Inherited via VideoEncoder
		int32_t InitEncode(const webrtc::VideoCodec *codec_settings, int32_t number_of_cores, size_t max_payload_size) override
		{
			Release();

			auto h264_settings = codec_settings->H264();

			profiler_name = profile_store_name(obs_get_profiler_name_store(), "webrtc_x264(%p)", this);

			info("InitEncode:\n"
				"\twidth: %d\n"
				"\theight: %d\n"
				"\tbitrate: %d\n"
				"\tmax_payload_size: %d\n"
				"\tname_store: %p (%p)",
				codec_settings->width,
				codec_settings->height,
				codec_settings->startBitrate,
				max_payload_size,
				profiler_name, this);

			if (x264_param_default_preset(&param, "veryfast", "zerolatency"))
				return WEBRTC_VIDEO_CODEC_ERROR;

			param.pf_log = x264_log;
			param.p_log_private = output;
			param.i_log_level = X264_LOG_WARNING;

			param.i_level_idc = 31;

			param.i_width = codec_settings->width;
			param.i_height = codec_settings->height;

			param.rc.f_rf_constant = 0.f;
			param.rc.i_rc_method = X264_RC_ABR;
			param.rc.b_filler = false; // OBS normally uses this, but webrtc's bitstream parser can't handle filler nals (nal unit 12), and it doesn't seem to be necessary currently
			param.i_csp = X264_CSP_I420;

			param.rc.i_vbv_buffer_size = codec_settings->startBitrate;
			param.rc.i_vbv_max_bitrate = codec_settings->startBitrate;
			param.rc.i_bitrate = codec_settings->startBitrate;

#if 1
			param.b_vfr_input = false;
#else
			param.b_vfr_input = true;

			param.i_timebase_num = 1;
			param.i_timebase_den = 90'000;
#endif

			param.b_annexb = true;

			//param.i_slice_max_size = max_payload_size;

			if (packetization_mode == webrtc::H264PacketizationMode::SingleNalUnit)
				param.i_slice_count = 1;

			auto video = obs_output_video(output);
			if (!video)
				return WEBRTC_VIDEO_CODEC_ERROR;

			auto info = video_output_get_info(video);
			if (!info)
				return WEBRTC_VIDEO_CODEC_ERROR;

			param.i_fps_num = info->fps_num;
			param.i_fps_den = info->fps_den;

			if (!keyframe_interval)
				param.i_keyint_max = info->fps_num * 2 / info->fps_den;
			else {
				param.i_keyint_max = *keyframe_interval;
				info("custom keyint_max: %d", *keyframe_interval);
			}
			param.i_keyint_min = min(param.i_keyint_max, param.i_keyint_min);

			param.vui.i_transfer = get_x264_cs_val(info->colorspace, x264_transfer_names);
			param.vui.i_colmatrix = get_x264_cs_val(info->colorspace, x264_colmatrix_names);
			param.vui.i_colorprim = get_x264_cs_val(info->colorspace, x264_colorprim_names);
			param.vui.b_fullrange = false;

			//param.rc.i_qp_max = codec_settings->qpMax;
			param.rc.i_lookahead = 10;
			param.i_sync_lookahead = 10;

			if (!convert_profile(param, h264_settings.profile))
				return WEBRTC_VIDEO_CODEC_ERROR;

			param.rc.i_lookahead = 2;
			param.i_sync_lookahead = 2;

			context.reset(x264_encoder_open(&param));
			if (!context)
				return WEBRTC_VIDEO_CODEC_ERROR;

			return WEBRTC_VIDEO_CODEC_OK;
		}

		int32_t Release() override
		{
			context.reset();
			return WEBRTC_VIDEO_CODEC_OK;
		}

		int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *callback_) override
		{
			if (callback)
				return WEBRTC_VIDEO_CODEC_ERROR;

			callback = callback_;
			return WEBRTC_VIDEO_CODEC_OK;
		}

		vector<uint8_t> buffer;
		deque<webrtc::EncodedImage> encoded_images;
		int32_t Encode(const webrtc::VideoFrame &frame, const webrtc::CodecSpecificInfo *codec_specific_info, const vector<webrtc::FrameType> *frame_types) override
		{
			ProfileScope(profiler_name);

			bool keyframe = false;
			if (frame_types) {
				if (frame_types->front() == webrtc::kEmptyFrame)
					return WEBRTC_VIDEO_CODEC_OK;
				keyframe = frame_types->front() == webrtc::kVideoFrameKey;
			}

			x264_picture_t pic, pic_out;

			x264_picture_init(&pic);
			pic.i_pts = frame.timestamp();
			pic.img.i_csp = X264_CSP_I420;
			pic.img.i_plane = 3;

			pic.b_keyframe = keyframe;

			auto framebuffer = frame.video_frame_buffer();
			pic.img.i_stride[0] = framebuffer->StrideY();
			pic.img.plane[0] = const_cast<uint8_t*>(framebuffer->DataY());
			pic.img.i_stride[1] = framebuffer->StrideU();
			pic.img.plane[1] = const_cast<uint8_t*>(framebuffer->DataU());
			pic.img.i_stride[2] = framebuffer->StrideV();
			pic.img.plane[2] = const_cast<uint8_t*>(framebuffer->DataV());

			x264_nal_t *nals = nullptr;
			int nal_count = 0;
			{
				ProfileScope("x264_encoder_encode");
				auto size = x264_encoder_encode(context.get(), &nals, &nal_count, &pic, &pic_out);
				if (size < 0)
					return WEBRTC_VIDEO_CODEC_ERROR;
			}

			{
				encoded_images.emplace_back();
				auto &encoded_image = encoded_images.back();
				encoded_image.capture_time_ms_ = frame.render_time_ms();
				encoded_image.ntp_time_ms_ = frame.ntp_time_ms();
				encoded_image.rotation_ = frame.rotation();
				encoded_image._encodedWidth = frame.width();
				encoded_image._encodedHeight = frame.height();
				encoded_image._timeStamp = frame.timestamp();
			}

			if (nal_count) {
				auto encoded_image = encoded_images.front();
				encoded_images.pop_front();

				webrtc::RTPFragmentationHeader frag_header;
				{
					ProfileScope("RTPFragmentize");
					RTPFragmentize(encoded_image, buffer, nal_count, nals, &frag_header);
				}

				encoded_image.qp_ = static_cast<int>(pic_out.prop.f_crf_avg);

				encoded_image._frameType = convert_frame_type(pic_out.i_type, pic_out.b_keyframe);
				encoded_image._completeFrame = true;

				webrtc::CodecSpecificInfo codec_specific;
				codec_specific.codecType = webrtc::kVideoCodecH264;
				codec_specific.codecSpecific.H264.packetization_mode = packetization_mode;

				ProfileScope("OnEncodedImage");
				callback->OnEncodedImage(encoded_image, &codec_specific, &frag_header);
			}

			return WEBRTC_VIDEO_CODEC_OK;
		}

		int32_t SetChannelParameters(uint32_t /*packet_loss*/, int64_t /*rtt*/) override
		{
			return WEBRTC_VIDEO_CODEC_OK;
		}


		int32_t SetRates(uint32_t bitrate, uint32_t framerate) override
		{
			if (bitrate != param.rc.i_bitrate) {
				info("Updating bitrate: %d -> %d", param.rc.i_bitrate, bitrate);

				param.rc.i_vbv_buffer_size = bitrate;
				param.rc.i_vbv_max_bitrate = bitrate;
				param.rc.i_bitrate = bitrate;

				x264_encoder_reconfig(context.get(), &param);
			}

			return WEBRTC_VIDEO_CODEC_OK;
		}

		ScalingSettings GetScalingSettings() const override
		{
			return ScalingSettings(true);
		}
	};
}

// Helper method used by H264EncoderImpl::Encode.
// Copies the encoded bytes from |info| to |encoded_image| and updates the
// fragmentation information of |frag_header|. The |encoded_image->_buffer| may
// be deleted and reallocated if a bigger buffer is required.
//
// After OpenH264 encoding, the encoded bytes are stored in |info| spread out
// over a number of layers and "NAL units". Each NAL unit is a fragment starting
// with the four-byte start code {0,0,0,1}. All of this data (including the
// start codes) is copied to the |encoded_image->_buffer| and the |frag_header|
// is updated to point to each fragment, with offsets and lengths set as to
// exclude the start codes.
static void RTPFragmentize(webrtc::EncodedImage &encoded_image,
	vector<uint8_t> &encoded_image_buffer,
	int nal_count,
	x264_nal_t *nals,
	webrtc::RTPFragmentationHeader* frag_header)
{
	// Calculate minimum buffer size required to hold encoded data.
	size_t required_size = 0;
	size_t fragments_count = 0;
	for (int nal = 0; nal < nal_count; ++nal, ++fragments_count)
		required_size += nals[nal].i_payload;

	encoded_image_buffer.clear();
	if (encoded_image_buffer.size() < required_size)
		encoded_image_buffer.reserve(required_size);

	encoded_image._buffer = encoded_image_buffer.data();

	// Iterate layers and NAL units, note each NAL unit as a fragment and copy
	// the data to |encoded_image->_buffer|.
	frag_header->VerifyAndAllocateFragmentationHeader(fragments_count);
	size_t frag = 0;
	encoded_image._length = 0;
	size_t layer_len = 0;
	for (int nal = 0; nal < nal_count; ++nal, ++frag) {
		auto start_code_length = nals[nal].b_long_startcode ? 4 : 3;
		frag_header->fragmentationOffset[frag] =
			encoded_image._length + layer_len + start_code_length;
		frag_header->fragmentationLength[frag] =
			nals[nal].i_payload - start_code_length;
		layer_len += nals[nal].i_payload;
		encoded_image_buffer.insert(end(encoded_image_buffer), nals[nal].p_payload, nals[nal].p_payload + nals[nal].i_payload);
	}
	encoded_image._size = encoded_image_buffer.size();
	encoded_image._length = layer_len;
}


unique_ptr<webrtc::VideoEncoder> CreateWebRTCX264Encoder(obs_output_t *out, const cricket::VideoCodec &codec, boost::optional<int> keyframe_interval)
{
	return make_unique<x264Encoder>(out, codec, keyframe_interval);
}
