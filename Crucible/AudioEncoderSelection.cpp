#include <obs.hpp>

#include "OBSHelpers.hpp"

#include <string>

using namespace std;

namespace {

struct AudioEncoderSettings {
	string id;
	int bitrate;
};

}

static AudioEncoderSettings encoders[] = {
	{ "CoreAudio_AAC", 128 },
	{ "libfdk_aac", 128 },
	{ "mf_aac", 128 },
	{ "ffmpeg_aac", 160 },
};

static AudioEncoderSettings *FindBestSettings()
{
	AudioEncoderSettings *best = end(encoders) - 1;

	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (!id)
			continue;

		for (auto &enc : encoders) {
			if (enc.id != id)
				continue;

			if (&enc >= best)
				break;

			best = &enc;
		}
	}

	blog(LOG_INFO, "Using '%s' with bitrate %d", best->id.c_str(), best->bitrate);

	return best;
}

OBSEncoder CreateAudioEncoder(const char *name)
{
	static AudioEncoderSettings *settings = nullptr;
	if (!settings)
		settings = FindBestSettings();

	auto data = OBSDataCreate();
	obs_data_set_int(data, "bitrate", settings->bitrate);

	return OBSTransferOwned(obs_audio_encoder_create(settings->id.c_str(), name, data, 0, nullptr));
}
