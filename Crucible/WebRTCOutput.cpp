
#include <webrtc/api/peerconnectioninterface.h>
#include <webrtc/base/ssladapter.h>
#include <webrtc/base/win32socketinit.h>
#include <webrtc/base/win32socketserver.h>
#include <webrtc/media/base/audiosource.h>
#include <webrtc/media/engine/webrtcvideoencoderfactory.h>
#include <webrtc/modules/audio_device/include/audio_device.h>
#include <webrtc/pc/localaudiosource.h>
#include <webrtc/video_encoder.h>

#include <memory>
#include <vector>

#include <obs.hpp>
#include <obs-output.h>
#include <media-io/audio-resampler.h>
#include <util/dstr.hpp>

#include "OBSHelpers.hpp"
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

namespace std {
	template <>
	struct default_delete<audio_resampler_t> {
		void operator()(audio_resampler_t *resampler)
		{
			audio_resampler_destroy(resampler);
		}
	};
}


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

		struct Timestamps {
			rtc::Optional<uint64_t> audio;
			rtc::Optional<uint64_t> video;
		};

		ProtectedObject<Timestamps> next_timestamps;

		bool warned_about_audio_timestamp = false;
		uint64_t video_frame_time;

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

	struct RTCFakeAudioDeviceModule : webrtc::AudioDeviceModule {
		RTCOutput *out;

		// Inherited via AudioDeviceModule
		virtual int64_t TimeUntilNextProcess() override
		{
			return 500; // 500 ms
		}

		virtual void Process() override { }

		virtual int32_t ActiveAudioLayer(AudioLayer *audioLayer) const override
		{
			return -1;
		}

		virtual ErrorCode LastError() const override
		{
			return ErrorCode();
		}

		virtual int32_t RegisterEventObserver(webrtc::AudioDeviceObserver *eventCallback) override
		{
			return 0;
		}

		virtual int32_t RegisterAudioCallback(webrtc::AudioTransport *audioCallback) override
		{
			return 0;
		}

		virtual int32_t Init() override
		{
			return 0;
		}

		virtual int32_t Terminate() override
		{
			return 0;
		}

		virtual bool Initialized() const override
		{
			return true;
		}

		virtual int16_t PlayoutDevices() override
		{
			return 0;
		}

		virtual int16_t RecordingDevices() override
		{
			return 0;
		}

		virtual int32_t PlayoutDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize], char guid[webrtc::kAdmMaxGuidSize]) override
		{
			return -1;
		}

		virtual int32_t RecordingDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize], char guid[webrtc::kAdmMaxGuidSize]) override
		{
			return -1;
		}

		virtual int32_t SetPlayoutDevice(uint16_t index) override
		{
			return -1;
		}

		virtual int32_t SetPlayoutDevice(WindowsDeviceType device) override
		{
			return -1;
		}

		virtual int32_t SetRecordingDevice(uint16_t index) override
		{
			return -1;
		}

		virtual int32_t SetRecordingDevice(WindowsDeviceType device) override
		{
			return -1;
		}
		virtual int32_t PlayoutIsAvailable(bool * available) override
		{
			return -1;
		}

		virtual int32_t InitPlayout() override
		{
			return -1;
		}

		virtual bool PlayoutIsInitialized() const override
		{
			return false;
		}

		virtual int32_t RecordingIsAvailable(bool *available) override
		{
			if (available)
				*available = true;
			return 0;
		}
		virtual int32_t InitRecording() override
		{
			return 0;
		}

		virtual bool RecordingIsInitialized() const override
		{
			return true;
		}

		virtual int32_t StartPlayout() override
		{
			return int32_t();
		}
		virtual int32_t StopPlayout() override
		{
			return -1;
		}
		virtual bool Playing() const override
		{
			return false;
		}

		virtual int32_t StartRecording() override
		{
			return 0;
		}
		virtual int32_t StopRecording() override
		{
			return 0;
		}

		virtual bool Recording() const override
		{
			return true;
		}

		virtual int32_t SetAGC(bool enable) override
		{
			return -1;
		}
		virtual bool AGC() const override
		{
			return false;
		}

		virtual int32_t SetWaveOutVolume(uint16_t volumeLeft, uint16_t volumeRight) override
		{
			return -1;
		}
		virtual int32_t WaveOutVolume(uint16_t * volumeLeft, uint16_t * volumeRight) const override
		{
			return -1;
		}
		virtual int32_t InitSpeaker() override
		{
			return -1;
		}

		virtual bool SpeakerIsInitialized() const override
		{
			return false;
		}

		virtual int32_t InitMicrophone() override
		{
			return -1;
		}
		virtual bool MicrophoneIsInitialized() const override
		{
			return false;
		}

		virtual int32_t SpeakerVolumeIsAvailable(bool * available) override
		{
			return -1;
		}

		virtual int32_t SetSpeakerVolume(uint32_t volume) override
		{
			return -1;
		}

		virtual int32_t SpeakerVolume(uint32_t * volume) const override
		{
			return -1;
		}
		virtual int32_t MaxSpeakerVolume(uint32_t * maxVolume) const override
		{
			return -1;
		}
		virtual int32_t MinSpeakerVolume(uint32_t * minVolume) const override
		{
			return -1;
		}
		virtual int32_t SpeakerVolumeStepSize(uint16_t * stepSize) const override
		{
			return -1;
		}
		virtual int32_t MicrophoneVolumeIsAvailable(bool * available) override
		{
			return -1;
		}
		virtual int32_t SetMicrophoneVolume(uint32_t volume) override
		{
			return -1;
		}
		virtual int32_t MicrophoneVolume(uint32_t * volume) const override
		{
			return -1;
		}
		virtual int32_t MaxMicrophoneVolume(uint32_t * maxVolume) const override
		{
			return -1;
		}
		virtual int32_t MinMicrophoneVolume(uint32_t * minVolume) const override
		{
			return -1;
		}
		virtual int32_t MicrophoneVolumeStepSize(uint16_t * stepSize) const override
		{
			return -1;
		}
		virtual int32_t SpeakerMuteIsAvailable(bool * available) override
		{
			return -1;
		}
		virtual int32_t SetSpeakerMute(bool enable) override
		{
			return -1;
		}
		virtual int32_t SpeakerMute(bool * enabled) const override
		{
			return -1;
		}
		virtual int32_t MicrophoneMuteIsAvailable(bool * available) override
		{
			return -1;
		}
		virtual int32_t SetMicrophoneMute(bool enable) override
		{
			return -1;
		}
		virtual int32_t MicrophoneMute(bool * enabled) const override
		{
			return -1;
		}
		virtual int32_t MicrophoneBoostIsAvailable(bool * available) override
		{
			return -1;
		}
		virtual int32_t SetMicrophoneBoost(bool enable) override
		{
			return -1;
		}
		virtual int32_t MicrophoneBoost(bool * enabled) const override
		{
			return -1;
		}
		virtual int32_t StereoPlayoutIsAvailable(bool * available) const override
		{
			return -1;
		}
		virtual int32_t SetStereoPlayout(bool enable) override
		{
			return -1;
		}
		virtual int32_t StereoPlayout(bool * enabled) const override
		{
			return -1;
		}
		virtual int32_t StereoRecordingIsAvailable(bool * available) const override
		{
			if (available)
				*available = true;
			return 0;
		}

		virtual int32_t SetStereoRecording(bool enable) override
		{
			return 0;
		}
		virtual int32_t StereoRecording(bool * enabled) const override
		{
			if (enabled)
				*enabled = true;
			return 0;
		}
		virtual int32_t SetRecordingChannel(const ChannelType channel) override
		{
			return -1;
		}
		virtual int32_t RecordingChannel(ChannelType * channel) const override
		{
			return -1;
		}
		virtual int32_t SetPlayoutBuffer(const BufferType type, uint16_t sizeMS = 0) override
		{
			return -1;
		}
		virtual int32_t PlayoutBuffer(BufferType * type, uint16_t * sizeMS) const override
		{
			return -1;
		}
		virtual int32_t PlayoutDelay(uint16_t * delayMS) const override
		{
			return -1;
		}
		virtual int32_t RecordingDelay(uint16_t * delayMS) const override
		{
			return -1;
		}
		virtual int32_t CPULoad(uint16_t * load) const override
		{
			if (load)
				*load = 0;
			return 0;
		}
		virtual int32_t StartRawOutputFileRecording(const char pcmFileNameUTF8[webrtc::kAdmMaxFileNameSize]) override
		{
			return -1;
		}
		virtual int32_t StopRawOutputFileRecording() override
		{
			return -1;
		}
		virtual int32_t StartRawInputFileRecording(const char pcmFileNameUTF8[webrtc::kAdmMaxFileNameSize]) override
		{
			return -1;
		}
		virtual int32_t StopRawInputFileRecording() override
		{
			return -1;
		}
		virtual int32_t SetRecordingSampleRate(const uint32_t samplesPerSec) override
		{
			return 0;
		}
		virtual int32_t RecordingSampleRate(uint32_t * samplesPerSec) const override
		{
			if (samplesPerSec)
				*samplesPerSec = 48000;
			return 0;
		}
		virtual int32_t SetPlayoutSampleRate(const uint32_t samplesPerSec) override
		{
			return -1;
		}
		virtual int32_t PlayoutSampleRate(uint32_t * samplesPerSec) const override
		{
			return -1;
		}
		virtual int32_t ResetAudioDevice() override
		{
			return 0;
		}
		virtual int32_t SetLoudspeakerStatus(bool enable) override
		{
			return -1;
		}
		virtual int32_t GetLoudspeakerStatus(bool * enabled) const override
		{
			return -1;
		}
		virtual bool BuiltInAECIsAvailable() const override
		{
			return false;
		}
		virtual bool BuiltInAGCIsAvailable() const override
		{
			return false;
		}
		virtual bool BuiltInNSIsAvailable() const override
		{
			return false;
		}
		virtual int32_t EnableBuiltInAEC(bool enable) override
		{
			return -1;
		}
		virtual int32_t EnableBuiltInAGC(bool enable) override
		{
			return -1;
		}
		virtual int32_t EnableBuiltInNS(bool enable) override
		{
			return -1;
		}
	};

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

	struct RTCVideoSource : cricket::VideoCapturer {
		bool have_video_info = false;
		shared_ptr<void> monitor;
		shared_ptr<RTCVideoSource> self;

		RTCOutput *out;

		int width = 0;
		int height = 0;

		RTCVideoSource()
		{
			monitor.reset(reinterpret_cast<void*>(1), [](void *) {});
			self = shared_ptr<RTCVideoSource>(monitor, this);
		}

		RTCVideoSource &operator=(const RTCVideoSource &) = delete;
		RTCVideoSource(const RTCVideoSource &) = delete;

		bool Start()
		{
			obs_video_info ovi{};
			have_video_info = obs_get_video_info(&ovi);
			if (!have_video_info)
				return false;

			width = ovi.output_width;
			height = ovi.output_height;
			return true;
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

		void OnSinkWantsChanged(const rtc::VideoSinkWants &wants) override
		{
			auto compute_res = [](const rtc::Optional<int> &pixel_count)->rtc::Optional<pair<int, int>>
			{
				if (pixel_count) {
					auto base = 1280. * 720;
					auto ratio = sqrt(base / *pixel_count);
					return rtc::Optional<pair<int, int>>(make_pair(static_cast<int>(1280 / ratio), static_cast<int>(720 / ratio)));
				}
				return{};
			};

			if (wants.max_pixel_count || wants.target_pixel_count) {
				auto max = wants.max_pixel_count.value_or(-1);
				auto target = wants.target_pixel_count.value_or(-1);
				auto max_res = compute_res(wants.max_pixel_count).value_or(make_pair(-1, -1));
				auto target_res = compute_res(wants.target_pixel_count).value_or(make_pair(-1, -1));
				info("Wants changed: {max: %d (%dx%d), target: %d (%dx%d)}",
					max, max_res.first, max_res.second,
					target, target_res.first, target_res.second);
			}

			cricket::VideoCapturer::OnSinkWantsChanged(wants);
		}
	};

	struct RTCAudioSource : webrtc::LocalAudioSource {
		bool have_audio_info = false;
		shared_ptr<void> monitor;
		shared_ptr<RTCAudioSource> self;

		RTCOutput *out;

		static const speaker_layout speakers = SPEAKERS_STEREO;
		static const audio_format audio_format = AUDIO_FORMAT_16BIT;
		static const uint32_t samples_per_sec = 48000;

		unique_ptr<audio_resampler_t> resampler;

		rtc::Optional<uint64_t> max_output_timestamp;
		rtc::Optional<uint64_t> buffer_start_timestamp;
		vector<uint8_t> audio_buffer;
		vector<uint8_t> audio_out_buffer;

		RTCAudioSource(RTCOutput *out)
			: out(out)
		{
			monitor.reset(reinterpret_cast<void*>(1), [](void *) {});
			self = shared_ptr<RTCAudioSource>(monitor, this);

			SetAudioOptions();
		}

		RTCAudioSource &operator=(const RTCAudioSource &) = delete;
		RTCAudioSource(const RTCAudioSource &) = delete;

		bool Start()
		{
			max_output_timestamp.reset();
			buffer_start_timestamp.reset();
			have_audio_info = false;

			auto audio = obs_output_audio(out->output);
			if (!audio)
				return false;

			auto aoi_ = audio_output_get_info(audio);
			if (!aoi_)
				return false;

			auto aoi = *aoi_;
			have_audio_info = true;

			if (aoi.samples_per_sec != samples_per_sec || aoi.format != audio_format || aoi.speakers != speakers) {
				resample_info src{}, dst{};
				src.format = aoi.format;
				src.samples_per_sec = aoi.samples_per_sec;
				src.speakers = aoi.speakers;

				dst.format = audio_format;
				dst.samples_per_sec = samples_per_sec;
				dst.speakers = speakers;

				resampler.reset(audio_resampler_create(&dst, &src));
			}

			return true;
		}

		void ReceiveAudio(audio_data *frames, rtc::Optional<uint64_t> output_timestamp)
		{
			if (!sink)
				return;

			if (!have_audio_info)
				return;

			if (!buffer_start_timestamp)
				buffer_start_timestamp.emplace(frames->timestamp);

			auto out_bytes_per_sample = get_audio_channels(speakers) * get_audio_bytes_per_channel(audio_format);

			if (!resampler) {
				audio_buffer.insert(end(audio_buffer), frames->data[0], frames->data[0] + frames->frames * out_bytes_per_sample);

			} else {
				uint8_t  *output[MAX_AV_PLANES] = { 0 };
				uint32_t out_frames;
				uint64_t offset;

				audio_resampler_resample(resampler.get(), output, &out_frames, &offset, frames->data, frames->frames);

				audio_buffer.insert(end(audio_buffer), output[0], output[0] + out_frames * out_bytes_per_sample);
			}

			max_output_timestamp = output_timestamp;
			if (!max_output_timestamp)
				return;
			
			auto out_chunk_size = out_bytes_per_sample * (samples_per_sec / 100);
			auto out_chunk_duration = 10'000'000; // 10 ms as ns
			bool did_output = false;
			while (audio_buffer.size() >= out_chunk_size && (*buffer_start_timestamp + out_chunk_duration) <= *max_output_timestamp) {
				audio_out_buffer.assign(begin(audio_buffer), begin(audio_buffer) + out_chunk_size);
				audio_buffer.erase(begin(audio_buffer), begin(audio_buffer) + out_chunk_size);
				sink->OnData(audio_out_buffer.data(), 16, samples_per_sec, speakers, samples_per_sec / 100);
				did_output = true;
				buffer_start_timestamp.emplace(*buffer_start_timestamp + out_chunk_duration);
			}
		}

		// Inherited via AudioSourceInterface
		void RegisterObserver(webrtc::ObserverInterface *observer) override
		{
		}

		void UnregisterObserver(webrtc::ObserverInterface *observer) override
		{
		}

		SourceState state() const override
		{
			return kLive;
		}

		bool remote() const override
		{
			return false;
		}

		webrtc::AudioTrackSinkInterface *sink = nullptr;
		void AddSink(webrtc::AudioTrackSinkInterface *sink_)
		{
			if (!sink)
				sink = sink_;
		}

		void RemoveSink(webrtc::AudioTrackSinkInterface *sink_)
		{
			if (sink == sink_)
				sink = nullptr;
		}

		cricket::AudioOptions options_;
		void SetAudioOptions()
		{
			options_.aecm_generate_comfort_noise.emplace(false);
			options_.auto_gain_control.emplace(false);
			options_.echo_cancellation.emplace(false);
			options_.highpass_filter.emplace(false);
			options_.intelligibility_enhancer.emplace(false);
			options_.level_control.emplace(false);
			options_.noise_suppression.emplace(false);
			options_.residual_echo_detector.emplace(false);
			options_.tx_agc_limiter.emplace(false);
			options_.typing_detection.emplace(false);
		}

		const cricket::AudioOptions &options() const override
		{
			return options_;
		}
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

		weak_ptr<RTCAudioSource> audio_source;
		weak_ptr<RTCVideoSource> video_source;

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
			worker_and_network_thread = rtc::Thread::CreateWithSocketServer();
			worker_and_network_thread->Start();

			rtc::scoped_refptr<RTCFakeAudioDeviceModule> adm = new rtc::RefCountedObject<RTCFakeAudioDeviceModule>();
			adm->out = out;
			peer_connection_factory.swap(webrtc::CreatePeerConnectionFactory(
				worker_and_network_thread.get(),
				rtc::ThreadManager::Instance()->CurrentThread(),
				adm,
				nullptr,
				nullptr));
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

			{
				rtc::scoped_refptr<RTCAudioSource> source = new rtc::RefCountedObject<RTCAudioSource>(out);
				audio_source = source->self;
				stream->AddTrack(peer_connection_factory->CreateAudioTrack("audio", source));
			}
			{
				auto source = new RTCVideoSource;
				source->out = out;
				video_source = source->self;
				stream->AddTrack(peer_connection_factory->CreateVideoTrack("video", peer_connection_factory->CreateVideoSource(source)));
			}

			if (!peer_connection->AddStream(stream))
				throw "Failed to add stream to peer_connection";

			streams.push_back(stream);
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

		shared_ptr<void> init_signal(CreateEvent(nullptr, true, false, nullptr), HandleDeleter());
		shared_ptr<void> ready_signal(CreateEvent(nullptr, true, false, nullptr), HandleDeleter());

		exception_ptr ptr;

		signal_thread.make_joinable = [&]
		{
			auto handle = rtc_thread_handle.Lock();
			if (handle && *handle)
				(*handle)->Quit();
		};

		signal_thread.Run([&, init_signal, ready_signal]
		{
			rtc::Win32Thread rtc_thread;
			rtc::ThreadManager::Instance()->SetCurrentThread(&rtc_thread);

			{
				auto handle = rtc_thread_handle.Lock();
				*handle = rtc::ThreadManager::Instance()->CurrentThread();
			}
			DEFER{ *rtc_thread_handle.Lock() = nullptr; };

			SetEvent(init_signal.get());

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

		if (WaitForSingleObject(init_signal.get(), 2000) != WAIT_OBJECT_0)
			blog(LOG_ERROR, "signal_thread did not set its handle in time, crucible will likely crash");

		if (WaitForSingleObject(ready_signal.get(), 1500) != WAIT_OBJECT_0)
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

	struct Observer : webrtc::CreateSessionDescriptionObserver {
		string err;
		unique_ptr<webrtc::SessionDescriptionInterface> desc;
		function<void(Observer*)> continuation;

		void Continue()
		{
			if (continuation)
				continuation(this);
		}

		void OnSuccess(webrtc::SessionDescriptionInterface *desc_) override
		{
			desc.reset(desc_);
			Continue();
		}

		void OnFailure(const std::string &error) override
		{
			err = error;
			Continue();
		}
	};
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

	shared_ptr<void> answer_complete_signal(CreateEvent(nullptr, true, false, nullptr), HandleDeleter());
	auto err = make_shared<string>();
	OBSData description = static_cast<obs_data_t*>(calldata_ptr(data, "description"));

	string type = type_;
	string sdp = sdp_;
	out->PostRTCMessage([=]
	{
		auto fail = [&](const char *format, ...)
		{
			DStr str;
			va_list args;
			va_start(args, format);
			dstr_vprintf(str, format, args);
			va_end(args);

			warn("HandleRemoteOffer: %s", str);
			*err = str;
			SetEvent(answer_complete_signal.get());
		};

		if (!out->out->peer_connection)
			return fail("Got remote offer without active peer_connection");

		{
			webrtc::SdpParseError err;
			auto desc = webrtc::CreateSessionDescription(type, sdp, &err);
			if (!desc)
				return fail("Failed to parse session description: '%s': %s", err.line.c_str(), err.description.c_str());

			out->out->peer_connection->SetRemoteDescription(DummySetSessionDescriptionObserver::Create(), desc);
		}

		if (type == "offer") {
			rtc::scoped_refptr<Observer> observer = new rtc::RefCountedObject<Observer>();
			observer->continuation = [=](Observer *observer)
			{
				DEFER{ SetEvent(answer_complete_signal.get()); };

				auto fail = [=](string str)
				{
					warn("HandleRemoteOffer: %s", str.c_str());
					*err = str;
				};

				if (!observer->err.empty())
					return fail(observer->err);

				string sdp;
				if (observer->desc->ToString(&sdp)) {
					obs_data_set_string(description, "type", observer->desc->type().c_str());
					obs_data_set_string(description, "sdp", sdp.c_str());

					out->out->peer_connection->SetLocalDescription(DummySetSessionDescriptionObserver::Create(), observer->desc.release());

				} else
					return fail("observer->desc->ToString(&sdp) failed");
			};

			webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
			options.offer_to_receive_audio = 0;
			options.offer_to_receive_video = 0;
			options.ice_restart = true;

			out->out->peer_connection->CreateAnswer(observer, options);
		}
	});

	auto fail = [&](string err)
	{
		warn("HandleRemoteOffer: %s", err.c_str());
		calldata_set_string(data, "error", err.c_str());
		return;
	};

	if (WaitForSingleObject(answer_complete_signal.get(), 5000) != WAIT_OBJECT_0)
		fail("Timeout while waiting for answer_complete_signal");

	if (!err->empty())
		calldata_set_string(data, "error", err->c_str());
}

static void AddRemoteIceCandidate(void *context, calldata_t *data)
{
	auto out = cast(context);

	auto sdp_mid = calldata_string(data, "sdp_mid");
	auto sdp_mline_index = static_cast<int>(calldata_int(data, "sdp_mline_index"));
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

static void CreateOffer(void *context, calldata_t *data)
{
	auto out = cast(context);
	shared_ptr<void> offer_complete_signal(CreateEvent(nullptr, true, false, nullptr), HandleDeleter());
	auto err = make_shared<string>();
	OBSData description = static_cast<obs_data_t*>(calldata_ptr(data, "description"));

	out->PostRTCMessage([out, offer_complete_signal, err, description]
	{
		auto fail = [&](string str)
		{
			warn("CreateOffer: %s", str.c_str());
			*err = move(str);
			SetEvent(offer_complete_signal.get());
		};

		if (!out->out->peer_connection)
			return fail("Got create_offer call without active peer_connection");

		rtc::scoped_refptr<Observer> observer = new rtc::RefCountedObject<Observer>();
		observer->continuation = [=](Observer *observer)
		{
			DEFER{ SetEvent(offer_complete_signal.get()); };

			auto fail = [=](string str)
			{
				warn("CreateOffer: %s", str.c_str());
				*err = str;
			};

			if (!observer->err.empty())
				return fail(observer->err);

			string sdp;
			if (observer->desc->ToString(&sdp)) {
				obs_data_set_string(description, "type", observer->desc->type().c_str());
				obs_data_set_string(description, "sdp", sdp.c_str());

				out->out->peer_connection->SetLocalDescription(DummySetSessionDescriptionObserver::Create(), observer->desc.release());
			} else
				return fail("observer->desc->ToString(&sdp) failed");
		};

		webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
		options.offer_to_receive_audio = 0;
		options.offer_to_receive_video = 0;
		out->out->peer_connection->CreateOffer(observer, options);
	});

	auto fail = [&](string err)
	{
		warn("CreateOffer: %s", err.c_str());
		calldata_set_string(data, "error", err.c_str());
		return;
	};

	if (WaitForSingleObject(offer_complete_signal.get(), 5000) != WAIT_OBJECT_0)
		fail("Timeout while waiting for offer_complete_signal");

	if (!err->empty())
		calldata_set_string(data, "error", err->c_str());
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

		proc_handler_add(handler, "void handle_remote_offer(string type, string sdp, in out ptr description, out string error)", HandleRemoteOffer, out);
		proc_handler_add(handler, "void add_remote_ice_candidate(string sdp_mid, int sdp_mline_index, string sdp)", AddRemoteIceCandidate, out);
		proc_handler_add(handler, "void create_offer(in out ptr description, out string error)", CreateOffer, out);
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

	{
		auto audio = out->out->audio_source.lock();
		if (!audio)
			return false;

		if (!audio->Start())
			return false;
	}

	{
		auto video = out->out->video_source.lock();
		if (!video)
			return false;

		if (!video->Start())
			return false;
	}

	{
		auto video = obs_output_video(out->output);
		out->video_frame_time = video_output_get_frame_time(video);
		auto format = video_output_get_format(video);
		if (format != VIDEO_FORMAT_I420)
			return false;
	}

	{
		auto timestamps = out->next_timestamps.Lock();
		if (timestamps) {
			timestamps->audio.reset();
			timestamps->video.reset();
		}

		out->warned_about_audio_timestamp = false;
	}

	return obs_output_begin_data_capture(out->output, 0);
}

static void ReceivePacketRTC(void *data, encoder_packet *packet)
{
}

static void ReceiveVideoRTC(void *data, video_data *frame)
{
	auto out = cast(data);

	{
		auto timestamps = out->next_timestamps.Lock();
		if (timestamps) {
			timestamps->video.emplace(frame->timestamp + out->video_frame_time); // this should allow sending audio up to the next frame, to slightly reduce overall delay

			if (!out->warned_about_audio_timestamp && timestamps->audio && *timestamps->audio < frame->timestamp) {
				warn("Audio timestamps are behind video timestamps: %llu < %llu", *timestamps->audio, frame->timestamp);
				out->warned_about_audio_timestamp = true;
			}
		}
	}

	auto src = out->out->video_source.lock();
	if (src)
		src->ReceiveVideo(frame);
}

static void ReceiveAudioRTC(void *data, audio_data *frames)
{
	auto out = cast(data);

	rtc::Optional<uint64_t> video_timestamp;
	{
		auto timestamps = out->next_timestamps.Lock();
		if (timestamps) {
			timestamps->audio.emplace(frames->timestamp);
			video_timestamp = timestamps->video;
		}
	}

	auto src = out->out->audio_source.lock();
	if (src)
		src->ReceiveAudio(frames, move(video_timestamp));
}

void RegisterWebRTCOutput()
{
	obs_output_info ooi{};
	ooi.id = "webrtc_output";
	ooi.flags = OBS_OUTPUT_AV;// | OBS_OUTPUT_ENCODED;
	ooi.get_name = [](auto) { return "WebRTC Output"; };
	ooi.create = CreateRTC;
	ooi.destroy = DestroyRTC;
	ooi.start = StartRTC;
	ooi.stop = StopRTC;
	//ooi.encoded_packet = ReceivePacketRTC;
	ooi.raw_video = ReceiveVideoRTC;
	ooi.raw_audio = ReceiveAudioRTC;
	obs_register_output(&ooi);
}

void PumpWebRTCMessages()
{

}
