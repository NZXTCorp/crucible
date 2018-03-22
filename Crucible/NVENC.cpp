#include <obs.hpp>
#include <obs-avc.h>
#include <media-io/video-scaler.h>

#include <webrtc/common_video/h264/h264_bitstream_parser.h>
#include <webrtc/media/base/codec.h>
#include <webrtc/modules/include/module_common_types.h>
#include <webrtc/modules/video_coding/include/video_codec_interface.h>
#include <webrtc/modules/video_coding/include/video_error_codes.h>
#include <webrtc/api/video_codecs/video_encoder.h>

#include "NVENC/dynlink_cuda.h"
#include "NVENC/nvEncodeAPI.h"

#include "scopeguard.hpp"

#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "ThreadTools.hpp"

#include <boost/optional.hpp>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>

using namespace std;


#define do_log(level, output, format, ...) \
	blog(level, "[WebRTC(NVENC): '%s'] " format, \
			obs_output_get_name(output), ##__VA_ARGS__)

#define error(format, ...) do_log(LOG_ERROR,   output, format, ##__VA_ARGS__)
#define warn(format, ...)  do_log(LOG_WARNING, output, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    output, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   output, format, ##__VA_ARGS__)

#if defined(_WIN32) || defined(__CYGWIN__)
# define CUDA_LIBNAME "nvcuda.dll"
# if ARCH_X86_64
#  define NVENC_LIBNAME "nvEncodeAPI64.dll"
# else
#  define NVENC_LIBNAME "nvEncodeAPI.dll"
# endif
#endif

static uint32_t MakeVersion(uint32_t major, uint32_t minor)
{
	return major << 4 | minor;
}

template <>
struct default_delete<video_scaler> {
	void operator()(video_scaler *scaler)
	{
		video_scaler_destroy(scaler);
	}
};

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
	vector<uint8_t> &encoded_image_buffer, const NV_ENC_LOCK_BITSTREAM &lock,
	webrtc::RTPFragmentationHeader* frag_header)
{
	auto bitstream_ptr = reinterpret_cast<uint8_t*>(lock.bitstreamBufferPtr);
	auto bitstream_end = bitstream_ptr + lock.bitstreamSizeInBytes;

	// Calculate minimum buffer size required to hold encoded data.
	size_t required_size = 0;

	struct NALInfo {
		const uint8_t *data = nullptr;
		size_t start_code_size = 0;
		size_t length = 0;
	};

	vector<NALInfo> nals;

	auto *nal_start = obs_avc_find_startcode(bitstream_ptr, bitstream_end);
	while (true) {
		auto start_code = nal_start;
		while (nal_start < bitstream_end && !*(nal_start++));

		if (nal_start == bitstream_end)
			break;

		auto nal_end = obs_avc_find_startcode(nal_start, bitstream_end);
		auto len = nal_end - start_code;
		required_size += len;

		nals.emplace_back();
		auto &info = nals.back();
		info.data = start_code;
		info.start_code_size = nal_start - start_code;
		info.length = len;

		nal_start = nal_end;
	}

	encoded_image_buffer.clear();
	if (encoded_image_buffer.size() < required_size)
		encoded_image_buffer.reserve(required_size);


	// Iterate layers and NAL units, note each NAL unit as a fragment and copy
	// the data to |encoded_image->_buffer|.
	frag_header->VerifyAndAllocateFragmentationHeader(nals.size());
	size_t frag = 0;
	encoded_image._length = 0;
	size_t layer_len = 0;
	for (size_t nal_i = 0; nal_i < nals.size(); ++nal_i, ++frag) {
		auto &nal = nals[nal_i];
		frag_header->fragmentationOffset[frag] = encoded_image._length + layer_len + nal.start_code_size;
		frag_header->fragmentationLength[frag] = nal.length - nal.start_code_size;
		layer_len += nal.length;
		encoded_image_buffer.insert(end(encoded_image_buffer), nal.data, nal.data + nal.length);
	}
	encoded_image._size = encoded_image_buffer.size();
	encoded_image._length = layer_len;
	encoded_image._buffer = encoded_image_buffer.data();
}

namespace {
	struct NVENCEncoder;

#define STRINGIFY_VAL(x) case x: return #x;

	struct NVENCStatus {
		NVENCSTATUS sts = NV_ENC_SUCCESS;
		NVENCStatus() = default;
		NVENCStatus(NVENCSTATUS sts) : sts(sts) {}

		explicit operator bool() { return sts != NV_ENC_SUCCESS; }

		const char *Name()
		{
			switch (sts) {
				STRINGIFY_VAL(NV_ENC_SUCCESS);
				STRINGIFY_VAL(NV_ENC_ERR_NO_ENCODE_DEVICE);
				STRINGIFY_VAL(NV_ENC_ERR_UNSUPPORTED_DEVICE);
				STRINGIFY_VAL(NV_ENC_ERR_INVALID_ENCODERDEVICE);
				STRINGIFY_VAL(NV_ENC_ERR_INVALID_DEVICE);
				STRINGIFY_VAL(NV_ENC_ERR_DEVICE_NOT_EXIST);
				STRINGIFY_VAL(NV_ENC_ERR_INVALID_PTR);
				STRINGIFY_VAL(NV_ENC_ERR_INVALID_EVENT);
				STRINGIFY_VAL(NV_ENC_ERR_INVALID_PARAM);
				STRINGIFY_VAL(NV_ENC_ERR_INVALID_CALL);
				STRINGIFY_VAL(NV_ENC_ERR_OUT_OF_MEMORY);
				STRINGIFY_VAL(NV_ENC_ERR_ENCODER_NOT_INITIALIZED);
				STRINGIFY_VAL(NV_ENC_ERR_UNSUPPORTED_PARAM);
				STRINGIFY_VAL(NV_ENC_ERR_LOCK_BUSY);
				STRINGIFY_VAL(NV_ENC_ERR_NOT_ENOUGH_BUFFER);
				STRINGIFY_VAL(NV_ENC_ERR_INVALID_VERSION);
				STRINGIFY_VAL(NV_ENC_ERR_MAP_FAILED);
				STRINGIFY_VAL(NV_ENC_ERR_NEED_MORE_INPUT);
				STRINGIFY_VAL(NV_ENC_ERR_ENCODER_BUSY);
				STRINGIFY_VAL(NV_ENC_ERR_EVENT_NOT_REGISTERD);
				STRINGIFY_VAL(NV_ENC_ERR_GENERIC);
				STRINGIFY_VAL(NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY);
				STRINGIFY_VAL(NV_ENC_ERR_UNIMPLEMENTED);
				STRINGIFY_VAL(NV_ENC_ERR_RESOURCE_REGISTER_FAILED);
				STRINGIFY_VAL(NV_ENC_ERR_RESOURCE_NOT_REGISTERED);
				STRINGIFY_VAL(NV_ENC_ERR_RESOURCE_NOT_MAPPED);
			}

			return "Unknown NVENC_STATUS";
		}

		void Error(NVENCEncoder *enc, const char *func);
		void Warn(NVENCEncoder *enc, const char *func);
	};

	struct NVENCCaps {
		NV_ENC_CAPS cap;
		NVENCCaps(NV_ENC_CAPS cap) : cap(cap) {}

		const char *Name()
		{
			switch (cap) {
				STRINGIFY_VAL(NV_ENC_CAPS_NUM_MAX_BFRAMES);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_FIELD_ENCODING);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_MONOCHROME);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_FMO);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_QPELMV);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_BDIRECT_MODE);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_CABAC);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_ADAPTIVE_TRANSFORM);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_RESERVED);
				STRINGIFY_VAL(NV_ENC_CAPS_NUM_MAX_TEMPORAL_LAYERS);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_HIERARCHICAL_PFRAMES);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_HIERARCHICAL_BFRAMES);
				STRINGIFY_VAL(NV_ENC_CAPS_LEVEL_MAX);
				STRINGIFY_VAL(NV_ENC_CAPS_LEVEL_MIN);
				STRINGIFY_VAL(NV_ENC_CAPS_SEPARATE_COLOUR_PLANE);
				STRINGIFY_VAL(NV_ENC_CAPS_WIDTH_MAX);
				STRINGIFY_VAL(NV_ENC_CAPS_HEIGHT_MAX);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_TEMPORAL_SVC);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_DYN_FORCE_CONSTQP);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_DYN_RCMODE_CHANGE);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_SUBFRAME_READBACK);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_CONSTRAINED_ENCODING);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_INTRA_REFRESH);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_DYNAMIC_SLICE_MODE);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION);
				STRINGIFY_VAL(NV_ENC_CAPS_PREPROC_SUPPORT);
				STRINGIFY_VAL(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT);
				STRINGIFY_VAL(NV_ENC_CAPS_MB_NUM_MAX);
				STRINGIFY_VAL(NV_ENC_CAPS_MB_PER_SEC_MAX);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_SAO);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_MEONLY_MODE);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_LOOKAHEAD);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE);
				STRINGIFY_VAL(NV_ENC_CAPS_NUM_MAX_LTR_FRAMES);
				STRINGIFY_VAL(NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION);
				STRINGIFY_VAL(NV_ENC_CAPS_EXPOSED_COUNT);
			}

			return "Unknown NV_ENC_CAPS value";
		}
	};

	struct LibUnloader {
		void operator()(HMODULE mod) { FreeLibrary(mod); }
	};
	using Library = unique_ptr<remove_pointer_t<HMODULE>, LibUnloader>;

#define LOAD_LIB(name) \
		lib.reset(LoadLibraryA(name)); \
		if (!lib) { \
			blog(LOG_WARNING, "NVENC/Encoder: failed to load library " name); \
			return false; \
		}

#define LOAD_FN(x) \
		x = reinterpret_cast<decltype(x)>(GetProcAddress(lib.get(), #x)); \
		if (!x)  { \
			blog(LOG_WARNING, "NVENC/Encoder: failed to load function " #x); \
			return false; \
		}

	struct CUDAFunctions {
		cuInit_t *cuInit = nullptr;
		cuDeviceGetCount_t *cuDeviceGetCount = nullptr;
		cuDeviceGet_t *cuDeviceGet = nullptr;
		cuDeviceGetName_t *cuDeviceGetName = nullptr;
		cuDeviceComputeCapability_t *cuDeviceComputeCapability = nullptr;
		cuCtxCreate_v2_t *cuCtxCreate = nullptr;
		cuCtxPushCurrent_v2_t *cuCtxPushCurrent = nullptr;
		cuCtxPopCurrent_v2_t *cuCtxPopCurrent = nullptr;
		cuCtxDestroy_v2_t *cuCtxDestroy = nullptr;
		cuMemAlloc_v2_t *cuMemAlloc = nullptr;
		cuMemFree_v2_t *cuMemFree = nullptr;
		cuMemcpy2D_v2_t *cuMemcpy2D = nullptr;
		cuGetErrorName_t *cuGetErrorName = nullptr;
		cuGetErrorString_t *cuGetErrorString = nullptr;

		Library lib;

		bool Load()
		{
			LOAD_LIB(CUDA_LIBNAME);

			auto free_on_error = guard([&]
			{
				lib.reset();
			});

			LOAD_FN(cuInit);
			LOAD_FN(cuDeviceGetCount);
			LOAD_FN(cuDeviceGet);
			LOAD_FN(cuDeviceGetName);
			LOAD_FN(cuDeviceComputeCapability);
			LOAD_FN(cuCtxCreate);
			LOAD_FN(cuCtxPushCurrent);
			LOAD_FN(cuCtxPopCurrent);
			LOAD_FN(cuCtxDestroy);
			LOAD_FN(cuMemAlloc);
			LOAD_FN(cuMemFree);
			LOAD_FN(cuMemcpy2D);
			LOAD_FN(cuGetErrorName);
			LOAD_FN(cuGetErrorString);

			free_on_error.dismiss();

			return true;
		}
	};

	struct NVENCFunctions {
		typedef NVENCSTATUS NVENCAPI NvEncodeAPICreateInstance_t(NV_ENCODE_API_FUNCTION_LIST *functionList);
		typedef NVENCSTATUS NVENCAPI NvEncodeAPIGetMaxSupportedVersion_t(uint32_t* version);

		NvEncodeAPICreateInstance_t *NvEncodeAPICreateInstance = nullptr;
		NvEncodeAPIGetMaxSupportedVersion_t *NvEncodeAPIGetMaxSupportedVersion = nullptr;

		Library lib;

		bool Load()
		{
			LOAD_LIB(NVENC_LIBNAME);

			auto free_on_error = guard([&]
			{
				lib.reset();
			});

			LOAD_FN(NvEncodeAPICreateInstance);
			LOAD_FN(NvEncodeAPIGetMaxSupportedVersion);

			free_on_error.dismiss();

			return true;
		}
	};

#undef LOAD_LIB
#undef LOAD_FN

	struct CUDAResult {
		shared_ptr<CUDAFunctions> funcs;
		CUresult res = CUDA_SUCCESS;
		CUDAResult(shared_ptr<CUDAFunctions> funcs) : funcs(funcs) {}

		CUDAResult &operator=(CUresult res_)
		{
			res = res_;
			return *this;
		}

		explicit operator bool() { return res != CUDA_SUCCESS; }

		const char *Name()
		{
			const char *str = nullptr;
			if (!funcs || funcs->cuGetErrorName(res, &str) != CUDA_SUCCESS || !str)
				return "CUDA_ERROR_INVALID_VALUE";
			return str;
		}

		const char *Description()
		{
			const char *str = nullptr;
			if (!funcs || funcs->cuGetErrorString(res, &str) != CUDA_SUCCESS || !str)
				return "Unknown error";
			return str;
		}
	};

	enum struct H264Profile {
		Baseline,
		Main,
		High
	};

	using CUDAContext = shared_ptr<remove_pointer_t<CUcontext>>;

	struct Surface {
		NV_ENC_INPUT_PTR input = nullptr;

		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t pitch = 0;

		NV_ENC_OUTPUT_PTR output = nullptr;
		NV_ENC_BUFFER_FORMAT format = NV_ENC_BUFFER_FORMAT_NV12_PL;

		unique_ptr<void, HandleDeleter> event;
	};

	struct NVENCEncoder : webrtc::VideoEncoder {
		obs_output_t *output = nullptr;

		webrtc::H264PacketizationMode packetization_mode = webrtc::H264PacketizationMode::SingleNalUnit;
		webrtc::H264BitstreamParser bitstream_parser;

		webrtc::EncodedImageCallback *callback = nullptr;

		boost::optional<int> keyframe_interval;

		shared_ptr<CUDAFunctions> cuda;
		shared_ptr<NVENCFunctions> nvenc;
		NV_ENCODE_API_FUNCTION_LIST funcs = { 0 };

		CUDAContext ctx;


		double keyint_sec = 5.;

		uint32_t bitrate = 0;
		NV_ENC_PARAMS_RC_MODE rc_mode = NV_ENC_PARAMS_RC_VBR_HQ;

		H264Profile profile;


		void *nv_encoder = nullptr;

		NV_ENC_CONFIG encode_config = { 0 };
		NV_ENC_INITIALIZE_PARAMS init_params = { 0 };

		vector<Surface> surfaces;

		deque<Surface*> idle;
		deque<Surface*> processing;
		deque<Surface*> ready;

		vector<uint8_t> headers;

		vector<uint8_t> slice_data;

		unique_ptr<video_scaler> scaler;

		NVENCEncoder(obs_output_t *output, const cricket::VideoCodec &codec, boost::optional<int> keyframe_interval)
			: output(output), keyframe_interval(keyframe_interval)
		{
			info("Created encoder");

			string packetization_mode_str;
			if (codec.GetParam(cricket::kH264FmtpPacketizationMode, &packetization_mode_str) && packetization_mode_str == "1")
				packetization_mode = webrtc::H264PacketizationMode::NonInterleaved;
		}

		// Inherited via VideoEncoder
		int32_t InitEncode(const webrtc::VideoCodec *codec_settings, int32_t number_of_cores, size_t max_payload_size) override
		{
			Release();

			auto h264_settings = codec_settings->H264();

			encode_config.version = NV_ENC_CONFIG_VER;
			encode_config.rcParams.version = NV_ENC_RC_PARAMS_VER;

			init_params.version = NV_ENC_INITIALIZE_PARAMS_VER;
			init_params.encodeGUID = NV_ENC_CODEC_H264_GUID;
			init_params.presetGUID = NV_ENC_PRESET_DEFAULT_GUID;

			init_params.encodeWidth = codec_settings->width;
			init_params.encodeHeight = codec_settings->height;

			init_params.darWidth = init_params.encodeWidth;
			init_params.darHeight = init_params.encodeHeight;

			init_params.enablePTD = 1;

			init_params.encodeConfig = &encode_config;

			auto video = obs_output_video(output);
			if (!video)
				return WEBRTC_VIDEO_CODEC_ERROR;

			auto info = video_output_get_info(video);
			if (!info)
				return WEBRTC_VIDEO_CODEC_ERROR;

			init_params.frameRateNum = info->fps_num;
			init_params.frameRateDen = info->fps_den;

			video_scale_info src;
			if (!obs_output_get_video_conversion(output, &src)) {
				warn("Could not get video conversion");
				return WEBRTC_VIDEO_CODEC_ERROR;
			}

			try {
				if (!LoadFunctions())
					return WEBRTC_VIDEO_CODEC_ERROR;

				CUDAResult res(cuda);

				if (res = cuda->cuInit(0)) {
					warn("cuInit returned %s (%d): %s", res.Name(), res.res, res.Description());
					return WEBRTC_VIDEO_CODEC_ERROR;
				}

				int device_count = 0;
				if (res = cuda->cuDeviceGetCount(&device_count)) {
					warn("cuDeviceGetCount returned %s (%d): %s", res.Name(), res.res, res.Description());
					return WEBRTC_VIDEO_CODEC_ERROR;
				}

				if (!device_count) {
					warn("No CUDA capable devices found");
					return WEBRTC_VIDEO_CODEC_ERROR;
				}


				keyint_sec;

				bitrate = codec_settings->startBitrate;

				profile = H264Profile::Baseline;
				rc_mode = NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ;
				init_params.presetGUID = NV_ENC_PRESET_LOW_LATENCY_DEFAULT_GUID;

				bool allow_async = true;

				auto maybe_retry_without_async = [&]
				{
					if (!init_params.enableEncodeAsync)
						return false;
					init_params.enableEncodeAsync = false;
					allow_async = false;
					warn("Initialization failed with ASYNC_ENCODE enabled, retrying");
					Release();
					return true;
				};

				do {
					bool have_device = false;
					int device = 0;
					for (; device < device_count; device++)
						if (OpenDevice(device, allow_async)) {
							have_device = true;
							break;
						}

					if (!have_device) {
						warn("No valid device found");
						return WEBRTC_VIDEO_CODEC_ERROR;
					}

					if (!InitEncoder(max_payload_size, src.colorspace, src.range)) {
						if (maybe_retry_without_async())
							continue;

						return WEBRTC_VIDEO_CODEC_ERROR;
					}

					if (!InitSurfaces()) {
						if (maybe_retry_without_async())
							continue;

						return WEBRTC_VIDEO_CODEC_ERROR;
					}

					break;
				} while (true);


				{
					auto profile_ = profile == H264Profile::Baseline ? "baseline" :
						profile == H264Profile::Main ? "main" : "high";

#if 0
					info("Encoder settings:\n"
						"\tbitrate:      %d\n"
						"\tkeyint:       %d (%g s)\n"
						"\tpreset:       %s\n"
						"\tprofile:      %s\n"
						"\twidth:        %d\n"
						"\theight:       %d\n"
						"\trate-control: %s\n"
						"\tGPU:          %d",
						enc->bitrate, enc->encode_config.gopLength, enc->keyint_sec, preset_name.c_str(), profile,
						obs_encoder_get_width(encoder), obs_encoder_get_height(encoder),
						enc->rc_mode == NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ ? "CBR" : "VBR",
						device);
#endif
				}

			} catch (const pair<const char*, int> &err) {
				do_log(LOG_WARNING, output, "Failed to create encoder: %s (line %d)", err.first, err.second);
				return WEBRTC_VIDEO_CODEC_ERROR;
			} catch (...) {
				do_log(LOG_WARNING, output, "Failed to create encoder");
				return WEBRTC_VIDEO_CODEC_ERROR;
			}

			{
				video_scale_info dst;
				dst.format = VIDEO_FORMAT_NV12;
				dst.width = codec_settings->width;
				dst.height = codec_settings->height;
				dst.range = src.range;
				dst.colorspace = src.colorspace;

				video_scaler *scaler_ = nullptr;
				auto res = video_scaler_create(&scaler_, &dst, &src, VIDEO_SCALE_DEFAULT);
				if (res) {
					warn("Failed to set up format conversion: %d", res);
					return WEBRTC_VIDEO_CODEC_ERROR;
				}

				scaler.reset(scaler_);
			}

			return WEBRTC_VIDEO_CODEC_OK;
		}

		~NVENCEncoder()
		{
			Release();
		}

		int32_t Release() override
		{
			if (!nvenc)
				return WEBRTC_VIDEO_CODEC_OK;

			scaler.reset();

			if (nv_encoder) {
				CUDAResult res(cuda);
				if (res = cuda->cuCtxPushCurrent(ctx.get())) {
					do_log(LOG_ERROR, output, "~Encoder: cuCtxPushCurrent returned %s (%d): %s", res.Name(), res.res, res.Description());
					return WEBRTC_VIDEO_CODEC_ERROR;
				}

				auto pop_context_impl = [&]
				{
					CUcontext dummy;
					if (res = cuda->cuCtxPopCurrent(&dummy)) {
						do_log(LOG_ERROR, output, "~Encoder: cuCtxPopCurrent returned %s (%d): %s", res.Name(), res.res, res.Description());
						return false;
					}

					return true;
				};

				auto context_guard = guard(pop_context_impl);

				NV_ENC_PIC_PARAMS pic = { 0 };
				pic.version = NV_ENC_PIC_PARAMS_VER;
				pic.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
				funcs.nvEncEncodePicture(nv_encoder, &pic);

				for (auto &surface : surfaces) {
					if (funcs.nvEncUnregisterAsyncEvent && surface.event) {
						NV_ENC_EVENT_PARAMS evt_params{};
						evt_params.version = NV_ENC_EVENT_PARAMS_VER;
						evt_params.completionEvent = surface.event.get();
						funcs.nvEncUnregisterAsyncEvent(nv_encoder, &evt_params);
					}

					if (funcs.nvEncDestroyInputBuffer && surface.input)
						funcs.nvEncDestroyInputBuffer(nv_encoder, surface.input);

					if (funcs.nvEncDestroyBitstreamBuffer && surface.output)
						funcs.nvEncDestroyBitstreamBuffer(nv_encoder, surface.output);
				}

				if (funcs.nvEncDestroyEncoder)
					funcs.nvEncDestroyEncoder(nv_encoder);

				nv_encoder = nullptr;
			}

			surfaces.clear();
			return WEBRTC_VIDEO_CODEC_OK;
		}

		int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *callback_) override
		{
			if (callback)
				return WEBRTC_VIDEO_CODEC_ERROR;

			callback = callback_;
			return WEBRTC_VIDEO_CODEC_OK;
		}

		bool LoadFunctions()
		{
			CUDAFunctions cuda;
			if (!cuda.Load())
				return false;

			NVENCFunctions nvenc;
			if (!nvenc.Load())
				return false;

			uint32_t max_version;
			NVENCStatus sts;
			if (sts = nvenc.NvEncodeAPIGetMaxSupportedVersion(&max_version)) {
				warn("NvEncodeAPIGetMaxSupportedVersion returned %#x (%s)", sts.sts, sts.Name());
				return false;
			}

			info("Loaded NVENC version %d.%d", max_version >> 4, max_version & 0xf);

			if (MakeVersion(NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION) > max_version) {
				warn("Driver does not support required version:\n\tRequired: %d.%d\n\tFound: %d.%d",
					NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION, max_version >> 4, max_version & 0xf);
				return false;
			}

			NV_ENCODE_API_FUNCTION_LIST funcs = { 0 };
			funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
			if (sts = nvenc.NvEncodeAPICreateInstance(&funcs)) {
				warn("NvEncodeAPICreateInstance returned %#x (%s)", sts.sts, sts.Name());
				return false;
			}

			this->cuda = make_shared<CUDAFunctions>(move(cuda));
			this->nvenc = make_shared<NVENCFunctions>(move(nvenc));
			this->funcs = funcs;

			return true;
		}

		bool OpenDevice(int idx, bool allow_async)
		{
			CUDAResult res(cuda);

			auto cuda_error = [&](const char *func)
			{
				warn("%s returned %s (%#x): %s", func, res.Name(), res.res, res.Description());
				return false;
			};

			auto make_version = [](int major, int minor) { return major << 4 | minor; };

			CUdevice device;
			if (res = cuda->cuDeviceGet(&device, idx))
				return cuda_error("cuDeviceGet");

			array<char, 128> name;
			if (res = cuda->cuDeviceGetName(name.data(), name.size(), device))
				return cuda_error("cuDeviceGetName");

			int major, minor;
			if (res = cuda->cuDeviceComputeCapability(&major, &minor, device))
				return cuda_error("cuDeviceComputeCapability");

			auto log_gpu_info = [&](bool supported)
			{
				info("GPU %d (%s) has compute %d.%d, %ssupport%s NVENC", idx, name.data(), major, minor,
					supported ? "" : "does not ",
					supported ? "s" : "");
				return supported;
			};

			if (make_version(major, minor) < make_version(3, 0)) // NVENC requires compute >= 3.0?
				return log_gpu_info(false);

			CUcontext ctx;
			if (res = cuda->cuCtxCreate(&ctx, 0, device))
				return cuda_error("cuCtxCreate");

			this->ctx.reset(ctx, [cuda = cuda](CUcontext ctx) { cuda->cuCtxDestroy(ctx); });

			CUcontext current;
			if (res = cuda->cuCtxPopCurrent(&current))
				return cuda_error("cuCtxPopCurrent");

			auto nvenc_supported = OpenSession() && CheckCodec() && CheckCapabilities(allow_async);

			if (!log_gpu_info(nvenc_supported))
				return false;

			return true;
		}

		bool OpenSession()
		{
			NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = { 0 };
			params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
			params.apiVersion = NVENCAPI_VERSION;
			params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
			params.device = ctx.get();

			void *encoder;
			if (NVENCStatus sts = funcs.nvEncOpenEncodeSessionEx(&params, &encoder)) {
				warn("nvEncOpenEncodeSessionEx returned %#x (%s)", sts.sts, sts.Name());
				return false;
			}

			nv_encoder = encoder;
			return true;
		}

		bool CheckCodec()
		{
			NVENCStatus sts;

			uint32_t count = 0;
			if (sts = funcs.nvEncGetEncodeGUIDCount(nv_encoder, &count)) {
				warn("nvEncGetEncodeGUIDCount returned %#x (%s)", sts.sts, sts.Name());
				return false;
			}

			if (!count) {
				warn("nvEncGetEncodeGUIDCount found 0 GUIDs");
				return false;
			}

			vector<GUID> guids(count);
			if (sts = funcs.nvEncGetEncodeGUIDs(nv_encoder, guids.data(), guids.size(), &count)) {
				warn("nvEncGetEncodeGUIDs returned %#x (%s)", sts.sts, sts.Name());
				return false;
			}

			auto it = find(begin(guids), end(guids), init_params.encodeGUID);
			if (it == end(guids)) {
				warn("encoder does not support H264");
				return false;
			}

			return true;
		}

		bool CheckCapabilities(bool allow_async)
		{
			NV_ENC_CAPS_PARAM params = { 0 };
			params.version = NV_ENC_CAPS_PARAM_VER;
			int val;

			auto check_cap = [&](NVENCCaps cap, auto &&validate)
			{
				params.capsToQuery = cap.cap;
				if (NVENCStatus sts = funcs.nvEncGetEncodeCaps(nv_encoder, init_params.encodeGUID, &params, &val)) {
					warn("nvEncGetEncodeCaps returned %#x (%s) when checking %#x (%s)",
						sts.sts, sts.Name(), cap.cap, cap.Name());
					return false;
				}

				return validate();
			};


			if (!check_cap(NV_ENC_CAPS_WIDTH_MAX, [&]
			{
				auto width = init_params.encodeWidth;
				if (val >= 0 && static_cast<uint32_t>(val) >= width)
					return true;

				warn("width %d not supported (maximum is %d)", width, val);
				return false;
			}))
				return false;

			if (!check_cap(NV_ENC_CAPS_HEIGHT_MAX, [&]
			{
				auto height = init_params.encodeHeight;
				if (val >= 0 && static_cast<uint32_t>(val) >= height)
					return true;

				warn("height %d not supported (maximum is %d)", height, val);
				return false;
			}))
				return false;

			if (!check_cap(NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE, [&]
			{
				if (val)
					return true;

				warn("encoder does not support dynamic bitrate change");
				return false;
			}))
				return false;

			if (allow_async) {
				check_cap(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT, [&]
				{
					info("ASYNC_ENCODE %ssupported", val ? "" : "not ");
					init_params.enableEncodeAsync = val;
					return true;
				});
			}

			return true;
		}

		bool InitEncoder(size_t max_payload_size, video_colorspace colorspace, video_range_type range)
		{
			NV_ENC_PRESET_CONFIG preset_config = { 0 };
			preset_config.version = NV_ENC_PRESET_CONFIG_VER;
			preset_config.presetCfg.version = NV_ENC_CONFIG_VER;

			NVENCStatus sts;
			auto warn_status = [&](const char *func) { sts.Warn(this, func); };

			if (sts = funcs.nvEncGetEncodePresetConfig(nv_encoder, init_params.encodeGUID, init_params.presetGUID, &preset_config)) {
				warn_status("nvEncGetEncodePresetConfig");
				return false;
			}

			encode_config = preset_config.presetCfg;
			encode_config.version = NV_ENC_CONFIG_VER;

			encode_config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;

			encode_config.frameIntervalP = 1;
			encode_config.gopLength = 2 * init_params.frameRateNum / init_params.frameRateDen;

			{
				auto &rc = encode_config.rcParams;
				rc.averageBitRate = bitrate * 1000;
				rc.maxBitRate = bitrate * 1000;
				rc.rateControlMode = rc_mode;
			}

			{

				auto video = obs_output_video(output);
				if (!video)
					return false;

				auto info = video_output_get_info(video);
				if (!info)
					return false;

				auto &h264 = encode_config.encodeCodecConfig.h264Config;
				auto &vui = h264.h264VUIParameters;

				vui.colourMatrix = colorspace == VIDEO_CS_709 ? 1 : 5;
				vui.colourPrimaries = colorspace == VIDEO_CS_709 ? 1 : 5;
				vui.transferCharacteristics = colorspace == VIDEO_CS_709 ? 1 : 5;
				vui.videoFullRangeFlag = range == VIDEO_RANGE_FULL;
				vui.colourDescriptionPresentFlag = 1;
				vui.videoSignalTypePresentFlag = 1;
				vui.chromaSampleLocationBot = 2;
				vui.chromaSampleLocationTop = 2;
				vui.chromaSampleLocationFlag = 1;

				h264.idrPeriod = encode_config.gopLength;

				if (packetization_mode == webrtc::H264PacketizationMode::SingleNalUnit) {
					h264.sliceMode = 3;
					h264.sliceModeData = 1;
				} else {
					h264.sliceMode = 1;
					h264.sliceModeData = max_payload_size;
				}

				h264.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;

				h264.outputBufferingPeriodSEI = 0;
				h264.outputPictureTimingSEI = 0;

				//h264.disableSPSPPS = 1;
				h264.repeatSPSPPS = 1;

				/*if (encode_config.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ) {
					h264.outputBufferingPeriodSEI = 1;
					h264.outputPictureTimingSEI = 1;
				}*/

				//h264.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_DISABLE;
				//h264.fmoMode = NV_ENC_H264_FMO_DISABLE;

				h264.chromaFormatIDC = 1;
			}

			switch (profile) {
			case H264Profile::Baseline:
				encode_config.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
				break;
			case H264Profile::Main:
				encode_config.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
				break;
			case H264Profile::High:
				encode_config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
				break;
			}

			if (sts = funcs.nvEncInitializeEncoder(nv_encoder, &init_params)) {
				warn_status("nvEncInitializeEncoder");
				return false;
			}

			return true;
		}
		
		bool InitSurfaces()
		{
			auto num_surfaces = max(4, encode_config.frameIntervalP * 2 * 2); // 2 NVENC encode sessions per GPU * 2 to decrease likelihood of blocking next group of frames

			surfaces.resize(num_surfaces);

			auto width = init_params.encodeWidth;
			auto height = init_params.encodeHeight;

			NV_ENC_EVENT_PARAMS evt_params{};
			evt_params.version = NV_ENC_EVENT_PARAMS_VER;

			for (auto &surface : surfaces) {
				{
					NV_ENC_CREATE_INPUT_BUFFER alloc_in = { 0 };
					alloc_in.version = NV_ENC_CREATE_INPUT_BUFFER_VER;

					alloc_in.width = (width + 31) & ~31;
					alloc_in.height = (height + 31) & ~31;
					alloc_in.bufferFmt = surface.format;

					if (init_params.enableEncodeAsync) {
						surface.event.reset(CreateEvent(nullptr, false, false, nullptr));

						evt_params.completionEvent = surface.event.get();
						if (NVENCStatus sts = funcs.nvEncRegisterAsyncEvent(nv_encoder, &evt_params)) {
							sts.Warn(this, "nvEncRegisterAsyncEvent");
							return false;
						}
					}

					if (NVENCStatus sts = funcs.nvEncCreateInputBuffer(nv_encoder, &alloc_in)) {
						sts.Warn(this, "nvEncCreateInputBuffer");
						return false;
					}

					surface.input = alloc_in.inputBuffer;
					surface.width = alloc_in.width;
					surface.height = alloc_in.height;
				}

				{
					NV_ENC_CREATE_BITSTREAM_BUFFER alloc_out = { 0 };
					alloc_out.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

					if (NVENCStatus sts = funcs.nvEncCreateBitstreamBuffer(nv_encoder, &alloc_out)) {
						sts.Warn(this, "nvEncCreateBitstreamBuffer");
						return false;
					}

					surface.output = alloc_out.bitstreamBuffer;
				}

				idle.push_back(&surface);
			}

			return true;
		}


		bool UploadFrame(const webrtc::VideoFrame &frame, Surface *surface)
		{
			NV_ENC_LOCK_INPUT_BUFFER lock = { 0 };
			lock.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
			lock.inputBuffer = surface->input;

			NVENCStatus sts;
			if (sts = funcs.nvEncLockInputBuffer(nv_encoder, &lock)) {
				sts.Warn(this, "nvEncLockInputBuffer");
				return false;
			}

			auto frame_buffer = frame.video_frame_buffer();

			surface->pitch = lock.pitch;
			auto base_target = reinterpret_cast<uint8_t*>(lock.bufferDataPtr);

			uint8_t *out[] = {
				base_target,
				base_target + lock.pitch * surface->height
			};

			uint32_t out_linesize[] = {
				lock.pitch,
				lock.pitch
			};

			const uint8_t *input[] = {
				frame_buffer->DataY(),
				frame_buffer->DataU(),
				frame_buffer->DataV()
			};

			const uint32_t in_linesize[] = {
				static_cast<uint32_t>(frame_buffer->StrideY()),
				static_cast<uint32_t>(frame_buffer->StrideU()),
				static_cast<uint32_t>(frame_buffer->StrideV())
			};

			if (!video_scaler_scale(scaler.get(), out, out_linesize, input, in_linesize)) {
				warn("video_scaler_scale failed");
				return false;
			}

			if (sts = funcs.nvEncUnlockInputBuffer(nv_encoder, lock.inputBuffer)) {
				sts.Warn(this, "nvEncUnlockInputBuffer");
				return false;
			}

			return true;
		}

		void ProcessOutput()
		{
			while ((init_params.enableEncodeAsync && !processing.empty()) || !ready.empty()) {
				Surface *out;
				
				if (init_params.enableEncodeAsync) {
					out = processing.front();
					if (WaitForSingleObject(out->event.get(), idle.empty() ? INFINITE : 0) != WAIT_OBJECT_0)
						return;
					processing.pop_front();
					idle.push_back(out);
				} else {
					out = ready.front();
					ready.pop_front();
					idle.push_back(out);
				}

				auto encoded_image = encoded_images.front();
				encoded_images.pop_front();

				NV_ENC_LOCK_BITSTREAM lock = { 0 };
				lock.version = NV_ENC_LOCK_BITSTREAM_VER;

				lock.outputBitstream = out->output;

				if (NVENCStatus sts = funcs.nvEncLockBitstream(nv_encoder, &lock)) {
					sts.Warn(this, "nvEncLockBitstream");
					return;
				}

				auto unlock_bitstream_impl = [&]
				{
					if (NVENCStatus sts = funcs.nvEncUnlockBitstream(nv_encoder, out->output))
						sts.Warn(this, "nvEncUnlockBitstream");
				};

				DEFER{ unlock_bitstream_impl(); };

				webrtc::RTPFragmentationHeader frag_header;
				RTPFragmentize(encoded_image, slice_data, lock, &frag_header);

				encoded_image._frameType = obs_avc_keyframe(reinterpret_cast<uint8_t*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes) ? webrtc::kVideoFrameKey : webrtc::kVideoFrameDelta;
				encoded_image._completeFrame = true;
				encoded_image.qp_ = lock.frameAvgQP;

				webrtc::CodecSpecificInfo codec_specific;
				codec_specific.codecType = webrtc::kVideoCodecH264;
				codec_specific.codecSpecific.H264.packetization_mode = packetization_mode;

				callback->OnEncodedImage(encoded_image, &codec_specific, &frag_header);
			}
		}


		deque<webrtc::EncodedImage> encoded_images;
		int32_t Encode(const webrtc::VideoFrame &frame, const webrtc::CodecSpecificInfo *codec_specific_info, const vector<webrtc::FrameType> *frame_types) override
		{
			bool keyframe = false;
			if (frame_types) {
				if (frame_types->front() == webrtc::kEmptyFrame)
					return WEBRTC_VIDEO_CODEC_OK;
				keyframe = frame_types->front() == webrtc::kVideoFrameKey;
			}

			try {
				if (keyframe) {
					NV_ENC_RECONFIGURE_PARAMS params = { 0 };
					params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
					params.reInitEncodeParams = init_params;

					params.forceIDR = true;

					if (NVENCStatus sts = funcs.nvEncReconfigureEncoder(nv_encoder, &params)) {
						sts.Warn(this, "nvEncReconfigureEncoder");
						warn("Encode: failed to request keyframe");
					}
				}

				bool encode_success = false; // as opposed to NEED_MORE_INPUT

				if (idle.empty() && init_params.enableEncodeAsync)
					ProcessOutput();

				if (idle.empty()) {
					error("Encode: no idle surfaces while trying to encode frame");
					return WEBRTC_VIDEO_CODEC_ERROR;
				}

				auto input = idle.front();

				CUDAResult res(cuda);
				if (res = cuda->cuCtxPushCurrent(ctx.get())) {
					error("Encode: cuCtxPushCurrent returned %s (%d): %s", res.Name(), res.res, res.Description());
					return WEBRTC_VIDEO_CODEC_ERROR;
				}

				auto pop_context_impl = [&]
				{
					CUcontext dummy;
					if (res = cuda->cuCtxPopCurrent(&dummy)) {
						error("Encode: cuCtxPopCurrent returned %s (%d): %s", res.Name(), res.res, res.Description());
						return false;
					}

					return true;
				};

				auto context_guard = guard(pop_context_impl);

				auto pop_context = [&]
				{
					context_guard.dismiss();
					return pop_context_impl();
				};

				if (!UploadFrame(frame, input)) {
					error("UploadFrame failed");
					return WEBRTC_VIDEO_CODEC_ERROR;
				}

				NV_ENC_PIC_PARAMS pic = { 0 };
				pic.version = NV_ENC_PIC_PARAMS_VER;
				pic.inputBuffer = input->input;
				pic.inputWidth = input->width;
				pic.inputHeight = input->height;
				pic.inputPitch = input->pitch;
				pic.outputBitstream = input->output;
				pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
				pic.inputTimeStamp = frame.timestamp();

				pic.codecPicParams.h264PicParams.sliceMode = encode_config.encodeCodecConfig.h264Config.sliceMode;
				pic.codecPicParams.h264PicParams.sliceModeData = encode_config.encodeCodecConfig.h264Config.sliceModeData;

				if (init_params.enableEncodeAsync)
					pic.completionEvent = input->event.get();

				if (NVENCStatus sts = funcs.nvEncEncodePicture(nv_encoder, &pic)) {
					if (sts.sts != NV_ENC_ERR_NEED_MORE_INPUT) {
						sts.Error(this, "nvEncEncodePicture");
						return WEBRTC_VIDEO_CODEC_ERROR;
					}
				} else {
					encode_success = true;
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

				processing.push_back(input);
				idle.pop_front();

				if (!pop_context())
					return WEBRTC_VIDEO_CODEC_ERROR;

				if (encode_success && !init_params.enableEncodeAsync) {
					ready.insert(end(ready), begin(processing), end(processing));
					processing.clear();
				}

				ProcessOutput();

				return WEBRTC_VIDEO_CODEC_OK;

			} catch (...) {
				warn("Encode: unhandled exception");
				return WEBRTC_VIDEO_CODEC_ERROR;
			}
		}

		int32_t SetChannelParameters(uint32_t /*packet_loss*/, int64_t /*rtt*/) override
		{
			return WEBRTC_VIDEO_CODEC_OK;
		}


		int32_t SetRates(uint32_t bitrate_, uint32_t framerate) override
		{
			if (bitrate_ != bitrate) {

				auto old_bitrate = bitrate;
				auto restore_config = guard([&, encode_config = encode_config]
				{
					this->bitrate = old_bitrate;
					this->encode_config = encode_config;
				});

				bitrate = bitrate_;

				NV_ENC_RECONFIGURE_PARAMS params = { 0 };
				params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
				params.reInitEncodeParams = init_params;

				{
					auto &rc = encode_config.rcParams;
					rc.averageBitRate = bitrate * 1000;
					rc.maxBitRate = bitrate * 1000;
				}

				if (NVENCStatus sts = funcs.nvEncReconfigureEncoder(nv_encoder, &params)) {
					sts.Warn(this, "nvEncReconfigureEncoder");
					warn("SetRates: failed to update bitrate from %d to %d", old_bitrate, bitrate);
					return WEBRTC_VIDEO_CODEC_ERROR;
				}

				restore_config.dismiss();
				info("SetRates: changed bitrate from %d to %d", old_bitrate, bitrate);
			}

			return WEBRTC_VIDEO_CODEC_OK;
		}

		ScalingSettings GetScalingSettings() const override
		{
			return ScalingSettings(true);
		}
	};


	void NVENCStatus::Error(NVENCEncoder *enc, const char *func)
	{
		do_log(LOG_ERROR, enc->output, "%s returned %#x (%s)", func, sts, Name());
	}

	void NVENCStatus::Warn(NVENCEncoder *enc, const char *func)
	{
		do_log(LOG_WARNING, enc->output, "%s returned %#x (%s)", func, sts, Name());
	}
}

#undef do_log

#undef error
#undef warn
#undef info
#undef debug


#define do_log(level, format, ...) \
	blog(level, "[WebRTC(NVENC)] " format, ##__VA_ARGS__)

#define error(format, ...) do_log(LOG_ERROR,   format, ##__VA_ARGS__)
#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

bool WebRTCNVENCAvailable()
{
	static once_flag once;
	static bool available = false;

	call_once(once, [&]
	{
		CUDAFunctions cuda;
		if (!cuda.Load()) {
			warn("Failed to load CUDA funcs");
			return;
		}

		NVENCFunctions nvenc;
		if (!nvenc.Load()) {
			warn("Failed to load NVENC funcs");
			return;
		}

		uint32_t max_version;
		NVENCStatus sts;
		if (sts = nvenc.NvEncodeAPIGetMaxSupportedVersion(&max_version)) {
			warn("NvEncodeAPIGetMaxSupportedVersion returned %#x (%s)", sts.sts, sts.Name());
			return;
		}

		if (MakeVersion(NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION) > max_version) {
			warn("Driver does not support required version:\n\tRequired: %d.%d\n\tFound: %d.%d",
				NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION, max_version >> 4, max_version & 0xf);
			return;
		}

		NV_ENCODE_API_FUNCTION_LIST funcs = { 0 };
		funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
		if (sts = nvenc.NvEncodeAPICreateInstance(&funcs)) {
			warn("NvEncodeAPICreateInstance returned %#x (%s)", sts.sts, sts.Name());
			return;
		}

		available = true;
	});

	return available;
}

unique_ptr<webrtc::VideoEncoder> CreateWebRTCNVENCEncoder(obs_output_t *out, const cricket::VideoCodec &codec, boost::optional<int> keyframe_interval)
{
	if (!WebRTCNVENCAvailable())
		return{};

	return make_unique<NVENCEncoder>(out, codec, keyframe_interval);
}

