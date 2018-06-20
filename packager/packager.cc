// Copyright 2017 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/packager.h"

#include <algorithm>

#include "packager/app/job_manager.h"
#include "packager/app/libcrypto_threading.h"
#include "packager/app/muxer_factory.h"
#include "packager/app/packager_util.h"
#include "packager/app/stream_descriptor.h"
#include "packager/base/at_exit.h"
#include "packager/base/files/file_path.h"
#include "packager/base/logging.h"
#include "packager/base/optional.h"
#include "packager/base/path_service.h"
#include "packager/base/strings/string_util.h"
#include "packager/base/strings/stringprintf.h"
#include "packager/base/threading/simple_thread.h"
#include "packager/base/time/clock.h"
#include "packager/file/file.h"
#include "packager/hls/base/hls_notifier.h"
#include "packager/hls/base/simple_hls_notifier.h"
#include "packager/media/ad_cue_generator/ad_cue_generator.h"
#include "packager/media/base/container_names.h"
#include "packager/media/base/fourccs.h"
#include "packager/media/base/key_source.h"
#include "packager/media/base/language_utils.h"
#include "packager/media/base/muxer.h"
#include "packager/media/base/muxer_options.h"
#include "packager/media/base/muxer_util.h"
#include "packager/media/chunking/chunking_handler.h"
#include "packager/media/chunking/cue_alignment_handler.h"
#include "packager/media/chunking/text_chunker.h"
#include "packager/media/crypto/encryption_handler.h"
#include "packager/media/demuxer/demuxer.h"
#include "packager/media/event/muxer_listener_factory.h"
#include "packager/media/event/vod_media_info_dump_muxer_listener.h"
#include "packager/media/formats/webvtt/text_padder.h"
#include "packager/media/formats/webvtt/text_readers.h"
#include "packager/media/formats/webvtt/webvtt_parser.h"
#include "packager/media/formats/webvtt/webvtt_text_output_handler.h"
#include "packager/media/formats/webvtt/webvtt_to_mp4_handler.h"
#include "packager/media/replicator/replicator.h"
#include "packager/media/trick_play/trick_play_handler.h"
#include "packager/mpd/base/media_info.pb.h"
#include "packager/mpd/base/mpd_builder.h"
#include "packager/mpd/base/simple_mpd_notifier.h"
#include "packager/status_macros.h"
#include "packager/version/version.h"

namespace shaka {

// TODO(kqyang): Clean up namespaces.
using media::Demuxer;
using media::JobManager;
using media::KeySource;
using media::MuxerOptions;
using media::SyncPointQueue;

namespace media {
namespace {

const char kMediaInfoSuffix[] = ".media_info";

MuxerOptions CreateMuxerOptions(const StreamDescriptor& stream,
                                const PackagingParams& params) {
  MuxerOptions options;

  options.mp4_params = params.mp4_output_params;
  options.temp_dir = params.temp_dir;
  options.bandwidth = stream.bandwidth;
  options.output_file_name = stream.output;
  options.segment_template = stream.segment_template;

  return options;
}

MuxerListenerFactory::StreamData ToMuxerListenerData(
    const StreamDescriptor& stream) {
  MuxerListenerFactory::StreamData data;
  data.media_info_output = stream.output;
  data.hls_group_id = stream.hls_group_id;
  data.hls_name = stream.hls_name;
  data.hls_playlist_name = stream.hls_playlist_name;
  data.hls_iframe_playlist_name = stream.hls_iframe_playlist_name;
  return data;
};

// TODO(rkuroiwa): Write TTML and WebVTT parser (demuxing) for a better check
// and for supporting live/segmenting (muxing).  With a demuxer and a muxer,
// CreateAllJobs() shouldn't treat text as a special case.
bool DetermineTextFileCodec(const std::string& file, std::string* out) {
  CHECK(out);

  std::string content;
  if (!File::ReadFileToString(file.c_str(), &content)) {
    LOG(ERROR) << "Failed to open file " << file
               << " to determine file format.";
    return false;
  }

  const uint8_t* content_data =
      reinterpret_cast<const uint8_t*>(content.data());
  MediaContainerName container_name =
      DetermineContainer(content_data, content.size());

  if (container_name == CONTAINER_WEBVTT) {
    *out = "wvtt";
    return true;
  }

  if (container_name == CONTAINER_TTML) {
    *out = "ttml";
    return true;
  }

  return false;
}

MediaContainerName GetOutputFormat(const StreamDescriptor& descriptor) {
  if (!descriptor.output_format.empty()) {
    MediaContainerName format =
        DetermineContainerFromFormatName(descriptor.output_format);
    if (format == CONTAINER_UNKNOWN) {
      LOG(ERROR) << "Unable to determine output format from '"
                 << descriptor.output_format << "'.";
    }
    return format;
  }

  base::Optional<MediaContainerName> format_from_output;
  base::Optional<MediaContainerName> format_from_segment;
  if (!descriptor.output.empty()) {
    format_from_output = DetermineContainerFromFileName(descriptor.output);
    if (format_from_output.value() == CONTAINER_UNKNOWN) {
      LOG(ERROR) << "Unable to determine output format from '"
                 << descriptor.output << "'.";
    }
  }
  if (!descriptor.segment_template.empty()) {
    format_from_segment =
        DetermineContainerFromFileName(descriptor.segment_template);
    if (format_from_segment.value() == CONTAINER_UNKNOWN) {
      LOG(ERROR) << "Unable to determine output format from '"
                 << descriptor.segment_template << "'.";
    }
  }

  if (format_from_output && format_from_segment) {
    if (format_from_output.value() != format_from_segment.value()) {
      LOG(ERROR) << "Output format determined from '" << descriptor.output
                 << "' differs from output format determined from '"
                 << descriptor.segment_template << "'.";
      return CONTAINER_UNKNOWN;
    }
  }

  if (format_from_output)
    return format_from_output.value();
  if (format_from_segment)
    return format_from_segment.value();
  return CONTAINER_UNKNOWN;
}

Status ValidateStreamDescriptor(bool dump_stream_info,
                                const StreamDescriptor& stream) {
  if (stream.input.empty()) {
    return Status(error::INVALID_ARGUMENT, "Stream input not specified.");
  }

  // The only time a stream can have no outputs, is when dump stream info is
  // set.
  if (dump_stream_info && stream.output.empty() &&
      stream.segment_template.empty()) {
    return Status::OK;
  }

  if (stream.output.empty() && stream.segment_template.empty()) {
    return Status(error::INVALID_ARGUMENT,
                  "Streams must specify 'output' or 'segment template'.");
  }

  // Whenever there is output, a stream must be selected.
  if (stream.stream_selector.empty()) {
    return Status(error::INVALID_ARGUMENT,
                  "Stream stream_selector not specified.");
  }

  // If a segment template is provided, it must be valid.
  if (stream.segment_template.length()) {
    RETURN_IF_ERROR(ValidateSegmentTemplate(stream.segment_template));
  }

  if (stream.output.find('$') != std::string::npos) {
    // "$" is only allowed if the output file name is a template, which is
    // used to support one file per Representation per Period when there are
    // Ad Cues.
    RETURN_IF_ERROR(ValidateSegmentTemplate(stream.output));
  }

  // There are some specifics that must be checked based on which format
  // we are writing to.
  const MediaContainerName output_format = GetOutputFormat(stream);

  if (output_format == CONTAINER_UNKNOWN) {
    return Status(error::INVALID_ARGUMENT, "Unsupported output format.");
  } else if (output_format == MediaContainerName::CONTAINER_MPEG2TS) {
    if (stream.segment_template.empty()) {
      return Status(
          error::INVALID_ARGUMENT,
          "Please specify 'segment_template'. Single file TS output is "
          "not supported.");
    }

    // Right now the init segment is saved in |output| for multi-segment
    // content. However, for TS all segments must be self-initializing so
    // there cannot be an init segment.
    if (stream.output.length()) {
      return Status(error::INVALID_ARGUMENT,
                    "All TS segments must be self-initializing. Stream "
                    "descriptors 'output' or 'init_segment' are not allowed.");
    }
  } else if (output_format == CONTAINER_WEBVTT ||
             output_format == CONTAINER_AAC || output_format == CONTAINER_AC3 ||
             output_format == CONTAINER_EAC3) {
    // There is no need for an init segment when outputting because there is no
    // initialization data.
    if (stream.segment_template.length() && stream.output.length()) {
      return Status(
          error::INVALID_ARGUMENT,
          "Segmented WebVTT or PackedAudio output cannot have an init segment. "
          "Do not specify stream descriptors 'output' or 'init_segment' when "
          "using 'segment_template'.");
    }
  } else {
    // For any other format, if there is a segment template, there must be an
    // init segment provided.
    if (stream.segment_template.length() && stream.output.empty()) {
      return Status(error::INVALID_ARGUMENT,
                    "Please specify 'init_segment'. All non-TS multi-segment "
                    "content must provide an init segment.");
    }
  }

  return Status::OK;
}

Status ValidateParams(const PackagingParams& packaging_params,
                      const std::vector<StreamDescriptor>& stream_descriptors) {
  if (!packaging_params.chunking_params.segment_sap_aligned &&
      packaging_params.chunking_params.subsegment_sap_aligned) {
    return Status(error::INVALID_ARGUMENT,
                  "Setting segment_sap_aligned to false but "
                  "subsegment_sap_aligned to true is not allowed.");
  }

  if (stream_descriptors.empty()) {
    return Status(error::INVALID_ARGUMENT,
                  "Stream descriptors cannot be empty.");
  }

  // On demand profile generates single file segment while live profile
  // generates multiple segments specified using segment template.
  const bool on_demand_dash_profile =
      stream_descriptors.begin()->segment_template.empty();
  for (const auto& descriptor : stream_descriptors) {
    if (on_demand_dash_profile != descriptor.segment_template.empty()) {
      return Status(error::INVALID_ARGUMENT,
                    "Inconsistent stream descriptor specification: "
                    "segment_template should be specified for none or all "
                    "stream descriptors.");
    }

    RETURN_IF_ERROR(ValidateStreamDescriptor(
        packaging_params.test_params.dump_stream_info, descriptor));

    if (base::StartsWith(descriptor.input, "udp://",
                         base::CompareCase::SENSITIVE)) {
      const HlsParams& hls_params = packaging_params.hls_params;
      if (!hls_params.master_playlist_output.empty() &&
          hls_params.playlist_type == HlsPlaylistType::kVod) {
        LOG(WARNING)
            << "Seeing UDP input with HLS Playlist Type set to VOD. The "
               "playlists will only be generated when UDP socket is closed. "
               "If you want to do live packaging, --hls_playlist_type needs to "
               "be set to LIVE.";
      }
      // Skip the check for DASH as DASH defaults to 'dynamic' MPD when segment
      // template is provided.
    }
  }

  if (packaging_params.output_media_info && !on_demand_dash_profile) {
    // TODO(rkuroiwa, kqyang): Support partial media info dump for live.
    return Status(error::UNIMPLEMENTED,
                  "--output_media_info is only supported for on-demand profile "
                  "(not using segment_template).");
  }

  return Status::OK;
}

bool StreamDescriptorCompareFn(const StreamDescriptor& a,
                               const StreamDescriptor& b) {
  if (a.input == b.input) {
    if (a.stream_selector == b.stream_selector) {
      // The MPD notifier requires that the main track comes first, so make
      // sure that happens.
      if (a.trick_play_factor == 0 || b.trick_play_factor == 0) {
        return a.trick_play_factor == 0;
      } else {
        return a.trick_play_factor > b.trick_play_factor;
      }
    } else {
      return a.stream_selector < b.stream_selector;
    }
  }

  return a.input < b.input;
}

// A fake clock that always return time 0 (epoch). Should only be used for
// testing.
class FakeClock : public base::Clock {
 public:
  base::Time Now() override { return base::Time(); }
};

bool StreamInfoToTextMediaInfo(const StreamDescriptor& stream_descriptor,
                               MediaInfo* text_media_info) {
  std::string codec;
  if (!DetermineTextFileCodec(stream_descriptor.input, &codec)) {
    LOG(ERROR) << "Failed to determine the text file format for "
               << stream_descriptor.input;
    return false;
  }

  MediaInfo::TextInfo* text_info = text_media_info->mutable_text_info();
  text_info->set_codec(codec);

  const std::string& language = stream_descriptor.language;
  if (!language.empty()) {
    text_info->set_language(language);
  }

  text_media_info->set_media_file_name(stream_descriptor.output);
  text_media_info->set_container_type(MediaInfo::CONTAINER_TEXT);

  if (stream_descriptor.bandwidth != 0) {
    text_media_info->set_bandwidth(stream_descriptor.bandwidth);
  } else {
    // Text files are usually small and since the input is one file; there's no
    // way for the player to do ranged requests. So set this value to something
    // reasonable.
    const int kDefaultTextBandwidth = 256;
    text_media_info->set_bandwidth(kDefaultTextBandwidth);
  }

  return true;
}

/// Create a new demuxer handler for the given stream. If a demuxer cannot be
/// created, an error will be returned. If a demuxer can be created, this
/// |new_demuxer| will be set and Status::OK will be returned.
Status CreateDemuxer(const StreamDescriptor& stream,
                     const PackagingParams& packaging_params,
                     std::shared_ptr<Demuxer>* new_demuxer) {
  std::shared_ptr<Demuxer> demuxer = std::make_shared<Demuxer>(stream.input);
  demuxer->set_dump_stream_info(packaging_params.test_params.dump_stream_info);

  if (packaging_params.decryption_params.key_provider != KeyProvider::kNone) {
    std::unique_ptr<KeySource> decryption_key_source(
        CreateDecryptionKeySource(packaging_params.decryption_params));
    if (!decryption_key_source) {
      return Status(
          error::INVALID_ARGUMENT,
          "Must define decryption key source when defining key provider");
    }
    demuxer->SetKeySource(std::move(decryption_key_source));
  }

  *new_demuxer = std::move(demuxer);
  return Status::OK;
}

std::shared_ptr<MediaHandler> CreateEncryptionHandler(
    const PackagingParams& packaging_params,
    const StreamDescriptor& stream,
    KeySource* key_source) {
  if (stream.skip_encryption) {
    return nullptr;
  }

  if (!key_source) {
    return nullptr;
  }

  // Make a copy so that we can modify it for this specific stream.
  EncryptionParams encryption_params = packaging_params.encryption_params;

  // Use Sample AES in MPEG2TS.
  // TODO(kqyang): Consider adding a new flag to enable Sample AES as we
  // will support CENC in TS in the future.
  if (GetOutputFormat(stream) == CONTAINER_MPEG2TS ||
      GetOutputFormat(stream) == CONTAINER_AAC ||
      GetOutputFormat(stream) == CONTAINER_AC3 ||
      GetOutputFormat(stream) == CONTAINER_EAC3) {
    VLOG(1) << "Use Apple Sample AES encryption for MPEG2TS or Packed Audio.";
    encryption_params.protection_scheme = kAppleSampleAesProtectionScheme;
  }

  if (!stream.drm_label.empty()) {
    const std::string& drm_label = stream.drm_label;
    encryption_params.stream_label_func =
        [drm_label](const EncryptionParams::EncryptedStreamAttributes&) {
          return drm_label;
        };
  } else if (!encryption_params.stream_label_func) {
    const int kDefaultMaxSdPixels = 768 * 576;
    const int kDefaultMaxHdPixels = 1920 * 1080;
    const int kDefaultMaxUhd1Pixels = 4096 * 2160;
    encryption_params.stream_label_func = std::bind(
        &Packager::DefaultStreamLabelFunction, kDefaultMaxSdPixels,
        kDefaultMaxHdPixels, kDefaultMaxUhd1Pixels, std::placeholders::_1);
  }

  return std::make_shared<EncryptionHandler>(encryption_params, key_source);
}

std::unique_ptr<TextChunker> CreateTextChunker(
    const ChunkingParams& chunking_params) {
  const float segment_length_in_seconds =
      chunking_params.segment_duration_in_seconds;
  return std::unique_ptr<TextChunker>(
      new TextChunker(segment_length_in_seconds));
}

Status CreateHlsTextJob(const StreamDescriptor& stream,
                        const PackagingParams& packaging_params,
                        std::unique_ptr<MuxerListener> muxer_listener,
                        SyncPointQueue* sync_points,
                        JobManager* job_manager) {
  DCHECK(muxer_listener);
  DCHECK(job_manager);

  if (stream.segment_template.empty()) {
    return Status(error::INVALID_ARGUMENT,
                  "Cannot output text (" + stream.input +
                      ") to HLS with no segment template");
  }

  // Text files are usually small and since the input is one file;
  // there's no way for the player to do ranged requests. So set this
  // value to something reasonable if it is missing.
  MuxerOptions muxer_options = CreateMuxerOptions(stream, packaging_params);
  muxer_options.bandwidth = stream.bandwidth ? stream.bandwidth : 256;

  auto output = std::make_shared<WebVttTextOutputHandler>(
      muxer_options, std::move(muxer_listener));

  std::unique_ptr<FileReader> reader;
  RETURN_IF_ERROR(FileReader::Open(stream.input, &reader));

  const int64_t kNoDuration = 0;
  auto parser =
      std::make_shared<WebVttParser>(std::move(reader), stream.language);
  auto padder = std::make_shared<TextPadder>(kNoDuration);
  auto cue_aligner = sync_points
                         ? std::make_shared<CueAlignmentHandler>(sync_points)
                         : nullptr;
  auto chunker = CreateTextChunker(packaging_params.chunking_params);

  job_manager->Add("Segmented Text Job", parser);

  return MediaHandler::Chain({std::move(parser), std::move(padder),
                              std::move(cue_aligner), std::move(chunker),
                              std::move(output)});
}

Status CreateWebVttToMp4TextJob(const StreamDescriptor& stream,
                                const PackagingParams& packaging_params,
                                std::unique_ptr<MuxerListener> muxer_listener,
                                SyncPointQueue* sync_points,
                                MuxerFactory* muxer_factory,
                                std::shared_ptr<OriginHandler>* root) {
  std::unique_ptr<FileReader> reader;
  RETURN_IF_ERROR(FileReader::Open(stream.input, &reader));

  const int64_t kNoDuration = 0;
  auto parser =
      std::make_shared<WebVttParser>(std::move(reader), stream.language);
  auto padder = std::make_shared<TextPadder>(kNoDuration);

  auto text_to_mp4 = std::make_shared<WebVttToMp4Handler>();
  auto muxer = muxer_factory->CreateMuxer(GetOutputFormat(stream), stream);
  muxer->SetMuxerListener(std::move(muxer_listener));

  // Optional Cue Alignment Handler
  std::shared_ptr<MediaHandler> cue_aligner;
  if (sync_points) {
    cue_aligner = std::make_shared<CueAlignmentHandler>(sync_points);
  }

  std::shared_ptr<MediaHandler> chunker =
      CreateTextChunker(packaging_params.chunking_params);

  *root = parser;

  return MediaHandler::Chain({std::move(parser), std::move(padder),
                              std::move(cue_aligner), std::move(chunker),
                              std::move(text_to_mp4), std::move(muxer)});
}

Status CreateTextJobs(
    const std::vector<std::reference_wrapper<const StreamDescriptor>>& streams,
    const PackagingParams& packaging_params,
    SyncPointQueue* sync_points,
    MuxerListenerFactory* muxer_listener_factory,
    MuxerFactory* muxer_factory,
    MpdNotifier* mpd_notifier,
    JobManager* job_manager) {
  DCHECK(muxer_listener_factory);
  DCHECK(job_manager);
  for (const StreamDescriptor& stream : streams) {
    // There are currently four options:
    //    TEXT WEBVTT --> TEXT WEBVTT [ supported ]
    //    TEXT WEBVTT --> MP4 WEBVTT  [ supported ]
    //    MP4 WEBVTT  --> MP4 WEBVTT  [ unsupported ]
    //    MP4 WEBVTT  --> TEXT WEBVTT [ unsupported ]
    const auto input_container = DetermineContainerFromFileName(stream.input);
    const auto output_container = GetOutputFormat(stream);

    if (input_container != CONTAINER_WEBVTT) {
      return Status(error::INVALID_ARGUMENT,
                    "Text output format is not support for " + stream.input);
    }

    if (output_container == CONTAINER_MOV) {
      std::unique_ptr<MuxerListener> muxer_listener =
          muxer_listener_factory->CreateListener(ToMuxerListenerData(stream));

      std::shared_ptr<OriginHandler> root;
      RETURN_IF_ERROR(CreateWebVttToMp4TextJob(
          stream, packaging_params, std::move(muxer_listener), sync_points,
          muxer_factory, &root));

      job_manager->Add("MP4 text job", std::move(root));
    } else {
      std::unique_ptr<MuxerListener> hls_listener =
          muxer_listener_factory->CreateHlsListener(
              ToMuxerListenerData(stream));

      // Check input to ensure that output is possible.
      if (hls_listener) {
        if (stream.segment_template.empty() || !stream.output.empty()) {
          return Status(error::INVALID_ARGUMENT,
                        "segment_template needs to be specified for HLS text "
                        "output. Single file output is not supported yet.");
        }
      }

      if (mpd_notifier && !stream.segment_template.empty()) {
        return Status(error::INVALID_ARGUMENT,
                      "Cannot create text output for MPD with segment output.");
      }

      // If we are outputting to HLS, then create the HLS test pipeline that
      // will create segmented text output.
      if (hls_listener) {
        RETURN_IF_ERROR(CreateHlsTextJob(stream, packaging_params,
                                         std::move(hls_listener), sync_points,
                                         job_manager));
      }

      if (!stream.output.empty()) {
        if (!File::Copy(stream.input.c_str(), stream.output.c_str())) {
          std::string error;
          base::StringAppendF(
              &error, "Failed to copy the input file (%s) to output file (%s).",
              stream.input.c_str(), stream.output.c_str());
          return Status(error::FILE_FAILURE, error);
        }

        MediaInfo text_media_info;
        if (!StreamInfoToTextMediaInfo(stream, &text_media_info)) {
          return Status(error::INVALID_ARGUMENT,
                        "Could not create media info for stream.");
        }

        // If we are outputting to MPD, just add the input to the outputted
        // manifest.
        if (mpd_notifier) {
          uint32_t unused;
          if (mpd_notifier->NotifyNewContainer(text_media_info, &unused)) {
            mpd_notifier->Flush();
          } else {
            return Status(error::PARSER_FAILURE,
                          "Failed to process text file " + stream.input);
          }
        }

        if (packaging_params.output_media_info) {
          VodMediaInfoDumpMuxerListener::WriteMediaInfoToFile(
              text_media_info, stream.output + kMediaInfoSuffix);
        }
      }
    }
  }

  return Status::OK;
}

Status CreateAudioVideoJobs(
    const std::vector<std::reference_wrapper<const StreamDescriptor>>& streams,
    const PackagingParams& packaging_params,
    KeySource* encryption_key_source,
    SyncPointQueue* sync_points,
    MuxerListenerFactory* muxer_listener_factory,
    MuxerFactory* muxer_factory,
    JobManager* job_manager) {
  DCHECK(muxer_listener_factory);
  DCHECK(muxer_factory);
  DCHECK(job_manager);
  // Store all the demuxers in a map so that we can look up a stream's demuxer.
  // This is step one in making this part of the pipeline less dependant on
  // order.
  std::map<std::string, std::shared_ptr<Demuxer>> sources;
  std::map<std::string, std::shared_ptr<MediaHandler>> cue_aligners;

  for (const StreamDescriptor& stream : streams) {
    bool seen_input_before = sources.find(stream.input) != sources.end();
    if (seen_input_before) {
      continue;
    }

    RETURN_IF_ERROR(
        CreateDemuxer(stream, packaging_params, &sources[stream.input]));
    cue_aligners[stream.input] =
        sync_points ? std::make_shared<CueAlignmentHandler>(sync_points)
                    : nullptr;
  }

  for (auto& source : sources) {
    job_manager->Add("RemuxJob", source.second);
  }

  // Replicators are shared among all streams with the same input and stream
  // selector.
  std::shared_ptr<MediaHandler> replicator;

  std::string previous_input;
  std::string previous_selector;

  for (const StreamDescriptor& stream : streams) {
    // Get the demuxer for this stream.
    auto& demuxer = sources[stream.input];
    auto& cue_aligner = cue_aligners[stream.input];

    const bool new_input_file = stream.input != previous_input;
    const bool new_stream =
        new_input_file || previous_selector != stream.stream_selector;
    previous_input = stream.input;
    previous_selector = stream.stream_selector;

    // If the stream has no output, then there is no reason setting-up the rest
    // of the pipeline.
    if (stream.output.empty() && stream.segment_template.empty()) {
      continue;
    }

    // Just because it is a different stream descriptor does not mean it is a
    // new stream. Multiple stream descriptors may have the same stream but
    // only differ by trick play factor.
    if (new_stream) {
      if (!stream.language.empty()) {
        demuxer->SetLanguageOverride(stream.stream_selector, stream.language);
      }

      replicator = std::make_shared<Replicator>();
      auto chunker =
          std::make_shared<ChunkingHandler>(packaging_params.chunking_params);
      auto encryptor = CreateEncryptionHandler(packaging_params, stream,
                                               encryption_key_source);

      // TODO(vaage) : Create a nicer way to connect handlers to demuxers.
      if (sync_points) {
        RETURN_IF_ERROR(
            MediaHandler::Chain({cue_aligner, chunker, encryptor, replicator}));
        RETURN_IF_ERROR(
            demuxer->SetHandler(stream.stream_selector, cue_aligner));
      } else {
        RETURN_IF_ERROR(MediaHandler::Chain({chunker, encryptor, replicator}));
        RETURN_IF_ERROR(demuxer->SetHandler(stream.stream_selector, chunker));
      }
    }

    // Create the muxer (output) for this track.
    std::shared_ptr<Muxer> muxer =
        muxer_factory->CreateMuxer(GetOutputFormat(stream), stream);
    if (!muxer) {
      return Status(error::INVALID_ARGUMENT, "Failed to create muxer for " +
                                                 stream.input + ":" +
                                                 stream.stream_selector);
    }

    std::unique_ptr<MuxerListener> muxer_listener =
        muxer_listener_factory->CreateListener(ToMuxerListenerData(stream));
    muxer->SetMuxerListener(std::move(muxer_listener));

    // Trick play is optional.
    std::shared_ptr<MediaHandler> trick_play =
        stream.trick_play_factor
            ? std::make_shared<TrickPlayHandler>(stream.trick_play_factor)
            : nullptr;

    RETURN_IF_ERROR(MediaHandler::Chain({replicator, trick_play, muxer}));
  }

  return Status::OK;
}

Status CreateAllJobs(const std::vector<StreamDescriptor>& stream_descriptors,
                     const PackagingParams& packaging_params,
                     MpdNotifier* mpd_notifier,
                     KeySource* encryption_key_source,
                     SyncPointQueue* sync_points,
                     MuxerListenerFactory* muxer_listener_factory,
                     MuxerFactory* muxer_factory,
                     JobManager* job_manager) {
  DCHECK(muxer_factory);
  DCHECK(muxer_listener_factory);
  DCHECK(job_manager);

  // Group all streams based on which pipeline they will use.
  std::vector<std::reference_wrapper<const StreamDescriptor>> text_streams;
  std::vector<std::reference_wrapper<const StreamDescriptor>>
      audio_video_streams;

  for (const StreamDescriptor& stream : stream_descriptors) {
    // TODO: Find a better way to determine what stream type a stream
    // descriptor is as |stream_selector| may use an index. This would
    // also allow us to use a simpler audio pipeline.
    if (stream.stream_selector == "text") {
      text_streams.push_back(stream);
    } else {
      audio_video_streams.push_back(stream);
    }
  }

  // Audio/Video streams need to be in sorted order so that demuxers and trick
  // play handlers get setup correctly.
  std::sort(audio_video_streams.begin(), audio_video_streams.end(),
            media::StreamDescriptorCompareFn);

  RETURN_IF_ERROR(CreateTextJobs(text_streams, packaging_params, sync_points,
                                 muxer_listener_factory, muxer_factory,
                                 mpd_notifier, job_manager));
  RETURN_IF_ERROR(CreateAudioVideoJobs(
      audio_video_streams, packaging_params, encryption_key_source, sync_points,
      muxer_listener_factory, muxer_factory, job_manager));

  // Initialize processing graph.
  return job_manager->InitializeJobs();
}

}  // namespace
}  // namespace media

struct Packager::PackagerInternal {
  media::FakeClock fake_clock;
  std::unique_ptr<KeySource> encryption_key_source;
  std::unique_ptr<MpdNotifier> mpd_notifier;
  std::unique_ptr<hls::HlsNotifier> hls_notifier;
  BufferCallbackParams buffer_callback_params;
  std::unique_ptr<media::JobManager> job_manager;
};

Packager::Packager() {}

Packager::~Packager() {}

Status Packager::Initialize(
    const PackagingParams& packaging_params,
    const std::vector<StreamDescriptor>& stream_descriptors) {
  // Needed by base::WorkedPool used in ThreadedIoFile.
  static base::AtExitManager exit;
  static media::LibcryptoThreading libcrypto_threading;

  if (internal_)
    return Status(error::INVALID_ARGUMENT, "Already initialized.");

  RETURN_IF_ERROR(media::ValidateParams(packaging_params, stream_descriptors));

  if (!packaging_params.test_params.injected_library_version.empty()) {
    SetPackagerVersionForTesting(
        packaging_params.test_params.injected_library_version);
  }

  std::unique_ptr<PackagerInternal> internal(new PackagerInternal);

  // Create encryption key source if needed.
  if (packaging_params.encryption_params.key_provider != KeyProvider::kNone) {
    internal->encryption_key_source = CreateEncryptionKeySource(
        static_cast<media::FourCC>(
            packaging_params.encryption_params.protection_scheme),
        packaging_params.encryption_params);
    if (!internal->encryption_key_source)
      return Status(error::INVALID_ARGUMENT, "Failed to create key source.");
  }

  // Store callback params to make it available during packaging.
  internal->buffer_callback_params = packaging_params.buffer_callback_params;

  // Update MPD output and HLS output if callback param is specified.
  MpdParams mpd_params = packaging_params.mpd_params;
  HlsParams hls_params = packaging_params.hls_params;
  if (internal->buffer_callback_params.write_func) {
    mpd_params.mpd_output = File::MakeCallbackFileName(
        internal->buffer_callback_params, mpd_params.mpd_output);
    hls_params.master_playlist_output = File::MakeCallbackFileName(
        internal->buffer_callback_params, hls_params.master_playlist_output);
  }
  // Both DASH and HLS require language to follow RFC5646
  // (https://tools.ietf.org/html/rfc5646), which requires the language to be
  // in the shortest form.
  mpd_params.default_language =
      LanguageToShortestForm(mpd_params.default_language);
  hls_params.default_language =
      LanguageToShortestForm(hls_params.default_language);

  if (!mpd_params.mpd_output.empty()) {
    const bool on_demand_dash_profile =
        stream_descriptors.begin()->segment_template.empty();
    const double target_segment_duration =
        packaging_params.chunking_params.segment_duration_in_seconds;
    const MpdOptions mpd_options = media::GetMpdOptions(
        on_demand_dash_profile, mpd_params, target_segment_duration);
    internal->mpd_notifier.reset(new SimpleMpdNotifier(mpd_options));
    if (!internal->mpd_notifier->Init()) {
      LOG(ERROR) << "MpdNotifier failed to initialize.";
      return Status(error::INVALID_ARGUMENT,
                    "Failed to initialize MpdNotifier.");
    }
  }

  if (!hls_params.master_playlist_output.empty()) {
    internal->hls_notifier.reset(new hls::SimpleHlsNotifier(hls_params));
  }

  std::unique_ptr<SyncPointQueue> sync_points;
  if (!packaging_params.ad_cue_generator_params.cue_points.empty()) {
    sync_points.reset(
        new SyncPointQueue(packaging_params.ad_cue_generator_params));
  }
  internal->job_manager.reset(new JobManager(std::move(sync_points)));

  std::vector<StreamDescriptor> streams_for_jobs;

  for (const StreamDescriptor& descriptor : stream_descriptors) {
    // We may need to overwrite some values, so make a copy first.
    StreamDescriptor copy = descriptor;

    if (internal->buffer_callback_params.read_func) {
      copy.input = File::MakeCallbackFileName(internal->buffer_callback_params,
                                              descriptor.input);
    }

    if (internal->buffer_callback_params.write_func) {
      copy.output = File::MakeCallbackFileName(internal->buffer_callback_params,
                                               descriptor.output);
      copy.segment_template = File::MakeCallbackFileName(
          internal->buffer_callback_params, descriptor.segment_template);
    }

    // Update language to ISO_639_2 code if set.
    if (!copy.language.empty()) {
      copy.language = LanguageToISO_639_2(descriptor.language);
      if (copy.language == "und") {
        return Status(
            error::INVALID_ARGUMENT,
            "Unknown/invalid language specified: " + descriptor.language);
      }
    }

    streams_for_jobs.push_back(copy);
  }

  media::MuxerFactory muxer_factory(packaging_params);
  if (packaging_params.test_params.inject_fake_clock) {
    muxer_factory.OverrideClock(&internal->fake_clock);
  }

  media::MuxerListenerFactory muxer_listener_factory(
      packaging_params.output_media_info, internal->mpd_notifier.get(),
      internal->hls_notifier.get());

  RETURN_IF_ERROR(media::CreateAllJobs(
      streams_for_jobs, packaging_params, internal->mpd_notifier.get(),
      internal->encryption_key_source.get(),
      internal->job_manager->sync_points(), &muxer_listener_factory,
      &muxer_factory, internal->job_manager.get()));

  internal_ = std::move(internal);
  return Status::OK;
}

Status Packager::Run() {
  if (!internal_)
    return Status(error::INVALID_ARGUMENT, "Not yet initialized.");

  RETURN_IF_ERROR(internal_->job_manager->RunJobs());

  if (internal_->hls_notifier) {
    if (!internal_->hls_notifier->Flush())
      return Status(error::INVALID_ARGUMENT, "Failed to flush Hls.");
  }
  if (internal_->mpd_notifier) {
    if (!internal_->mpd_notifier->Flush())
      return Status(error::INVALID_ARGUMENT, "Failed to flush Mpd.");
  }
  return Status::OK;
}

void Packager::Cancel() {
  if (!internal_) {
    LOG(INFO) << "Not yet initialized. Return directly.";
    return;
  }
  internal_->job_manager->CancelJobs();
}

std::string Packager::GetLibraryVersion() {
  return GetPackagerVersion();
}

std::string Packager::DefaultStreamLabelFunction(
    int max_sd_pixels,
    int max_hd_pixels,
    int max_uhd1_pixels,
    const EncryptionParams::EncryptedStreamAttributes& stream_attributes) {
  if (stream_attributes.stream_type ==
      EncryptionParams::EncryptedStreamAttributes::kAudio)
    return "AUDIO";
  if (stream_attributes.stream_type ==
      EncryptionParams::EncryptedStreamAttributes::kVideo) {
    const int pixels = stream_attributes.oneof.video.width *
                       stream_attributes.oneof.video.height;
    if (pixels <= max_sd_pixels)
      return "SD";
    if (pixels <= max_hd_pixels)
      return "HD";
    if (pixels <= max_uhd1_pixels)
      return "UHD1";
    return "UHD2";
  }
  return "";
}

}  // namespace shaka
