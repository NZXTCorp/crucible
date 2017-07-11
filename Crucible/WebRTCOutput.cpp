
#include <webrtc/api/peerconnectioninterface.h>
#include <webrtc/base/ssladapter.h>
#include <webrtc/base/win32socketinit.h>
#include <webrtc/base/win32socketserver.h>
#include <webrtc/media/engine/webrtcvideoencoderfactory.h>
#include <webrtc/video_encoder.h>

#include <memory>
#include <vector>

#include <obs.hpp>
#include <obs-output.h>

#include "ProtectedObject.hpp"
#include "ThreadTools.hpp"
#include "scopeguard.hpp"

using namespace std;

#define do_log(level, output, format, ...) \
	blog(level, "[WebRTC: '%s'] " format, \
			obs_output_get_name(output), ##__VA_ARGS__)

#define error(format, ...) do_log(LOG_ERROR,   output, format, ##__VA_ARGS__)
#define warn(format, ...)  do_log(LOG_WARNING, output, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    output, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   output, format, ##__VA_ARGS__)

//#define USE_OBS_ENCODER


namespace {
	struct DummySetSessionDescriptionObserver : webrtc::SetSessionDescriptionObserver {
		static DummySetSessionDescriptionObserver *Create()
		{
			return new rtc::RefCountedObject<DummySetSessionDescriptionObserver>();
		}

		void OnSuccess() override
		{
			blog(LOG_INFO, "set session description success");
		}

		void OnFailure(const std::string& error) override
		{
			blog(LOG_WARNING, "set session description failed: %s", error.c_str());
		}

	protected:
		DummySetSessionDescriptionObserver() {}
		~DummySetSessionDescriptionObserver() {}
	};

	struct RTCControl;

	struct RTCOutput {
		JoiningThread signal_thread;
		ProtectedObject<rtc::Thread*> rtc_thread_handle = nullptr;

		obs_output_t *output;
		string ice_server_uri;

		rtc::scoped_refptr<RTCControl> out;

		RTCOutput(obs_output_t *output, string ice_server_uri);
		void PostRTCMessage(function<void()> func);
	};

#undef error
#undef warn
#undef info
#undef debug
#define error(format, ...) do_log(LOG_ERROR,   out ? out->output : nullptr, format, ##__VA_ARGS__)
#define warn(format, ...)  do_log(LOG_WARNING, out ? out->output : nullptr, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    out ? out->output : nullptr, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   out ? out->output : nullptr, format, ##__VA_ARGS__)

	struct RTCFrameBuffer : webrtc::VideoFrameBuffer {
		int width_;
		int height_;

		vector<uint8_t> data_y;
		vector<uint8_t> data_u;
		vector<uint8_t> data_v;

		int stride_y;
		int stride_u;
		int stride_v;

		// Inherited via VideoFrameBuffer
		int width() const override
		{
			return width_;
		}

		int height() const override
		{
			return height_;
		}

		const uint8_t *DataY() const override
		{
			return data_y.data();
		}

		const uint8_t *DataU() const override
		{
			return data_u.data();
		}

		const uint8_t *DataV() const override
		{
			return data_v.data();
		}

		int StrideY() const override
		{
			return stride_y;
		}

		int StrideU() const override
		{
			return stride_u;
		}

		int StrideV() const override
		{
			return stride_v;
		}

		void *native_handle() const override
		{
			return nullptr;
		}

		rtc::scoped_refptr<VideoFrameBuffer> NativeToI420Buffer() override
		{
			return this;
		}
	};

	struct RTCSource : cricket::VideoCapturer {
		bool have_video_info = false;
		shared_ptr<void> monitor;
		shared_ptr<RTCSource> self;

		int width = 0;
		int height = 0;

		RTCSource()
		{
			monitor.reset(reinterpret_cast<void*>(1), [](void *) {});
			self = shared_ptr<RTCSource>(monitor, this);

			obs_video_info ovi{};
			have_video_info = obs_get_video_info(&ovi);
			if (!have_video_info)
				return;

			width = ovi.output_width;
			height = ovi.output_height;
		}

		bool GetBestCaptureFormat(const cricket::VideoFormat &desired,
			cricket::VideoFormat *best_format) override
		{
			if (!best_format)
				return false;

			*best_format = desired;
			return true;
		}

		// Inherited via VideoCapturer
		cricket::CaptureState Start(const cricket::VideoFormat &capture_format) override
		{
			return cricket::CS_RUNNING;
		}

		void Stop() override
		{
		}

		bool IsRunning() override
		{
			return capture_state() == cricket::CS_RUNNING;
		}

		bool IsScreencast() const override
		{
			return false;
		}

		bool GetPreferredFourccs(vector<uint32_t> *fourccs) override
		{
			if (!fourccs)
				return false;

			fourccs->clear();
			fourccs->emplace_back(cricket::FOURCC_NV12);
			return true;
		}

		void ReceiveVideo(video_data *data)
		{
			if (!have_video_info)
				return;

			rtc::scoped_refptr<RTCFrameBuffer> buffer = new rtc::RefCountedObject<RTCFrameBuffer>();
			if (!data->data[0] || !data->data[1] || !data->data[2])
				return;

			buffer->data_y.assign(data->data[0], data->data[0] + data->linesize[0] * height);
			buffer->data_u.assign(data->data[1], data->data[1] + data->linesize[1] * height / 2);
			buffer->data_v.assign(data->data[2], data->data[2] + data->linesize[2] * height / 2);

			buffer->width_ = width;
			buffer->height_ = height;

			buffer->stride_y = data->linesize[0];
			buffer->stride_u = data->linesize[1];
			buffer->stride_v = data->linesize[2];

			webrtc::VideoFrame frame(buffer, webrtc::kVideoRotation_0, data->timestamp / 1000);
			OnFrame(frame, width, height);
		}

		//virtual void OnSinkWantsChanged(const rtc::VideoSinkWants& wants);

		RTCSource &operator=(const RTCSource &) = delete;
		RTCSource(const RTCSource &) = delete;
	};

#ifdef USE_OBS_ENCODER
	struct RTCEncoder : webrtc::VideoEncoder {
		int32_t InitEncode(const webrtc::VideoCodec *codec_settings,
			int32_t number_of_cores,
			size_t max_payload_size) override
		{

		}

		int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *callback) override
		{

		}

		int32_t Release() override
		{

		}

		int32_t Encode(const webrtc::VideoFrame &frame,
			const webrtc::CodecSpecificInfo *codec_specific_info,
			const vector<webrtc::FrameType> *frame_types) override
		{

		}

		int32_t SetChannelParameters(uint32_t /*packet_loss*/, int64_t /*rtt*/) override
		{
			return 0;
		}

		int32_t SetRateAllocation(const webrtc::BitrateAllocation &allocation, uint32_t framerate) override
		{
			auto rate = allocation.GetBitrate(0, 0);
			return 0;
		}

		// Any encoder implementation wishing to use the WebRTC provided
		// quality scaler must implement this method.
		ScalingSettings GetScalingSettings() const override
		{
			return ScalingSettings(false);
		}

		int32_t SetPeriodicKeyFrames(bool enable) override
		{
			return enable ? 0 : -1;
		}

		const char* ImplementationName() const override
		{
			return "Crucible video";
		}
	};
#endif

	struct RTCControl :
		webrtc::PeerConnectionObserver,
		webrtc::CreateSessionDescriptionObserver,
		cricket::WebRtcVideoEncoderFactory,
		rtc::MessageHandler {

		RTCOutput *out;

		unique_ptr<rtc::Thread> worker_and_network_thread;

		rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peer_connection_factory;
		rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection;

		vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>> streams;


		weak_ptr<RTCSource> source;

		vector<cricket::VideoCodec> codecs;

		void Init(const string &ice_server_uri)
		{
			codecs.emplace_back("H264");
#ifdef USE_OBS_ENCODER
			worker_and_network_thread = rtc::Thread::CreateWithSocketServer();
			worker_and_network_thread->Start();

			peer_connection_factory = webrtc::CreatePeerConnectionFactory(
				worker_and_network_thread.get(),
				rtc::ThreadManager::Instance()->CurrentThread(),
				nullptr,
				this,
				nullptr);
#else
			peer_connection_factory.swap(webrtc::CreatePeerConnectionFactory());
#endif
			if (!peer_connection_factory)
				throw "Could not create peer_connection_factory";

			webrtc::PeerConnectionInterface::RTCConfiguration config;
			if (!ice_server_uri.empty()) {
				webrtc::PeerConnectionInterface::IceServer server;
				server.uri = ice_server_uri;
				config.servers.push_back(server);
			}

			peer_connection = peer_connection_factory->CreatePeerConnection(config, nullptr, nullptr, this);
			if (!peer_connection)
				throw "Could not create peer_connection";

			auto stream = peer_connection_factory->CreateLocalMediaStream("stream");

			/*stream->AddTrack(peer_connection_factory->CreateAudioTrack(
				"audio", peer_connection_factory->CreateAudioSource(NULL)));*/
			auto source_ = new RTCSource;
			source = source_->self;
			stream->AddTrack(peer_connection_factory->CreateVideoTrack("video", peer_connection_factory->CreateVideoSource(source_)));

			if (!peer_connection->AddStream(stream))
				throw "Failed to add stream to peer_connection";

			streams.push_back(stream);

#if 0
			webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
			options.offer_to_receive_audio = 0;
			options.offer_to_receive_video = 0;

			peer_connection->CreateOffer(this, options);
#endif
		}

		// PeerConnectionObserver
		void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override
		{
		}

		void OnAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override
		{
			info("Stream '%s' added", stream->label().c_str());
		}

		void OnRemoveStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override
		{
			info("Stream '%s' removed", stream->label().c_str());
		}

		void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override
		{
			info("DataChannel '%s' activity", data_channel->label().c_str());
		}

		void OnRenegotiationNeeded() override
		{
			info("OnRenegotiationNeeded");
		}

		void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override
		{
		}

		void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override
		{
		}

		void OnIceCandidate(const webrtc::IceCandidateInterface *candidate) override
		{
			string str;
			if (candidate->ToString(&str)) {
				info("Received ICE candidate: %s", str.c_str());

				calldata_t data{};
				DEFER{ calldata_free(&data); };

				calldata_set_ptr(&data, "output", out->output);
				calldata_set_string(&data, "sdp_mid", candidate->sdp_mid().c_str());
				calldata_set_string(&data, "sdp", str.c_str());
				calldata_set_int(&data, "sdp_mline_index", candidate->sdp_mline_index());
				signal_handler_signal(obs_output_get_signal_handler(out->output), "ice_candidate", &data);

			} else
				warn("Received ICE candidate, but ToString failed");
		}

		void OnIceCandidatesRemoved(const std::vector<cricket::Candidate> &candidates) override
		{
		}

		void OnIceConnectionReceivingChange(bool receiving) override
		{
		}

		void OnAddTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
			const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>> &streams) override
		{
		}

		// Inherited via CreateSessionDescriptionObserver
		void OnSuccess(webrtc::SessionDescriptionInterface *desc) override
		{
			string str;
			desc->ToString(&str);
			auto type = desc->type();

			info("Created session description (%s): %s", type.c_str(), str.empty() ? "" : str.c_str());
			peer_connection->SetLocalDescription(DummySetSessionDescriptionObserver::Create(), desc);

			calldata_t data{};
			DEFER{ calldata_free(&data); };

			calldata_set_ptr(&data, "output", out->output);
			calldata_set_string(&data, "type", type.c_str());
			calldata_set_string(&data, "sdp", str.empty() ? nullptr : str.c_str());
			signal_handler_signal(obs_output_get_signal_handler(out->output), "session_description", &data);
		}

		void OnFailure(const std::string &error) override
		{
			//TODO handle description error
		}


		// Inherited via WebRtcVideoEncoderFactory
		webrtc::VideoEncoder *CreateVideoEncoder(const cricket::VideoCodec &codec) override
		{
			auto str = codec.ToString();
			info("Requested codec: %s", str.c_str());
			return nullptr;
		}

		const vector<cricket::VideoCodec> &supported_codecs() const override
		{
			return codecs;
		}

		bool EncoderTypeHasInternalSource(webrtc::VideoCodecType type) const override
		{
			return true;
		}

		void DestroyVideoEncoder(webrtc::VideoEncoder *encoder) override
		{
		}


		// Inherited via MessageHandler
		void OnMessage(rtc::Message *msg) override
		{
			rtc::UseMessageData<function<void()>>(msg->pdata)();
		}
	};


	RTCOutput::RTCOutput(obs_output_t *output, string ice_server_uri)
		: output(output), ice_server_uri(ice_server_uri)
	{
		signal_thread.make_joinable = [&]
		{
			auto handle = rtc_thread_handle.Lock();
			if (handle && *handle)
				(*handle)->Stop();
		};

		shared_ptr<void> ready_signal(CreateEvent(nullptr, true, false, nullptr), HandleDeleter());

		exception_ptr ptr;

		signal_thread.Run([&, ready_signal]
		{
			rtc::Win32Thread rtc_thread;
			rtc::ThreadManager::Instance()->SetCurrentThread(&rtc_thread);

			{
				auto handle = rtc_thread_handle.Lock();
				*handle = rtc::ThreadManager::Instance()->CurrentThread();
			}
			DEFER{ *rtc_thread_handle.Lock() = nullptr; };

			rtc::InitializeSSL();
			DEFER{ rtc::CleanupSSL(); };

			try {
				out = new rtc::RefCountedObject<RTCControl>();
				if (!out)
					return;

				out->out = this;

				out->Init(this->ice_server_uri);

				SetEvent(ready_signal.get());

			} catch (...) {
				ptr = current_exception();
			}

			rtc_thread.Run();
		});

		if (WaitForSingleObject(ready_signal.get(), 500) != WAIT_OBJECT_0)
			if (ptr)
				rethrow_exception(ptr);
			else
				throw "signal_thread did not finish initialization in time";
	}

	void RTCOutput::PostRTCMessage(function<void()> func)
	{
		auto handle = rtc_thread_handle.Lock();
		if (!handle || !*handle)
			return;

		(*handle)->Post(RTC_FROM_HERE, out, 0, rtc::WrapMessageData(func));
	}
}

static RTCOutput *cast(void *data)
{
	return reinterpret_cast<RTCOutput*>(data);
}

static void HandleRemoteOffer(void *context, calldata_t *data)
{
	auto out = cast(context);

	auto type_ = calldata_string(data, "type");
	auto sdp_ = calldata_string(data, "sdp");
	if (!type_ || !sdp_) {
		warn("Got invalid type(%p)/sdp(%p)");
		return;
	}

	string type = type_;
	string sdp = sdp_;
	out->PostRTCMessage([=]
	{
		if (!out->out->peer_connection) {
			warn("Got remote offer without active peer_connection");
			return;
		}

		webrtc::SdpParseError err;
		auto desc = webrtc::CreateSessionDescription(type, sdp, &err);
		if (!desc) {
			warn("Failed to parse session description: '%s': %s", err.line.c_str(), err.description.c_str());
			return;
		}
		out->out->peer_connection->SetRemoteDescription(DummySetSessionDescriptionObserver::Create(), desc);

		if (type == "offer") {
			webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
			options.offer_to_receive_audio = 0;
			options.offer_to_receive_video = 0;
			options.ice_restart = true;

			out->out->peer_connection->CreateAnswer(out->out, options);
		}
	});
}

static void AddRemoteIceCandidate(void *context, calldata_t *data)
{
	auto out = cast(context);

	auto sdp_mid = calldata_string(data, "sdp_mid");
	auto sdp_mline_index = calldata_int(data, "sdp_mline_index");
	auto sdp = calldata_string(data, "sdp");

	if (!sdp_mid || !sdp) {
		warn("Got invalid sdp_mid(%p)/sdp(%p)", sdp_mid, sdp);
		return;
	}

	webrtc::SdpParseError err;
	shared_ptr<webrtc::IceCandidateInterface> candidate(webrtc::CreateIceCandidate(sdp_mid, sdp_mline_index, sdp, &err));
	if (!candidate) {
		warn("Failed to parse ice candidate: '%s': %s", err.line.c_str(), err.description.c_str());
		return;
	}

	out->PostRTCMessage([=]
	{
		if (!out->out->peer_connection) {
			warn("Got remote ice candidate without active peer_connection");
			return;
		}
		
		if (!out->out->peer_connection->AddIceCandidate(candidate.get())) {
			warn("Failed to apply ice candidate");
		}

		string str;
		if (candidate->ToString(&str))
			info("Added ice candidate: %s", str.c_str());
		else
			info("Added ice candidate (ToString failed)");
	});
}

static const char *signal_prototypes[] = {
	"void session_description(ptr output, string type, string sdp)",
	"void ice_candidate(ptr output, string sdp_mid, int sdp_mline_index, string sdp)",
	nullptr
};

static void AddSignalHandlers(RTCOutput *out)
{
	{
		auto handler = obs_output_get_signal_handler(out->output);

		signal_handler_add_array(handler, signal_prototypes);
	}
	{
		auto handler = obs_output_get_proc_handler(out->output);

		proc_handler_add(handler, "void handle_remote_offer(string sdp)", HandleRemoteOffer, out);
		proc_handler_add(handler, "void add_remote_ice_candidate(string sdp_mid, int sdp_mline_index, string sdp)", AddRemoteIceCandidate, out);
	}
}

static void DestroyRTC(void *data)
try {
	auto out = unique_ptr<RTCOutput>(cast(data));
	out.reset();
} catch (...) {
	auto out = cast(data);
	error("Unspecified error while destroying output");
}

static void *CreateRTC(obs_data_t *settings, obs_output_t *output)
try {
	auto out = make_unique<RTCOutput>(output, obs_data_get_string(settings, "server"));

	if (out)
		AddSignalHandlers(out.get());

	return out.release();
} catch (const char *err) {
	do_log(LOG_WARNING, output, "Error while creating output: %s", err);
	return nullptr;
} catch (...) {
	do_log(LOG_ERROR, output, "Unspecified error while creating output");
	return nullptr;
}

static void StopRTC(void *data)
{
	auto out = cast(data);
	obs_output_end_data_capture(out->output);
}

static bool StartRTC(void *data)
{
	auto out = cast(data);
	obs_output_begin_data_capture(out->output, 0);
	return true;
}

static void ReceivePacketRTC(void *data, encoder_packet *packet)
{
}

static void ReceiveVideoRTC(void *data, video_data *frame)
{
	auto out = cast(data);

	auto src = out->out->source.lock();
	if (src)
		src->ReceiveVideo(frame);
}

void RegisterWebRTCOutput()
{
	obs_output_info ooi{};
	ooi.id = "webrtc_output";
	ooi.flags = OBS_OUTPUT_VIDEO;// | OBS_OUTPUT_ENCODED;
	ooi.get_name = [](auto) { return "WebRTC Output"; };
	ooi.create = CreateRTC;
	ooi.destroy = DestroyRTC;
	ooi.start = StartRTC;
	ooi.stop = StopRTC;
	//ooi.encoded_packet = ReceivePacketRTC;
	ooi.raw_video = ReceiveVideoRTC;
	obs_register_output(&ooi);
}

void PumpWebRTCMessages()
{

}
