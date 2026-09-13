#ifndef BF6_UI_RUNTIME_MOVIE_TEXTURE_H
#define BF6_UI_RUNTIME_MOVIE_TEXTURE_H

#include "bf6_core.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bf6::ui_runtime {

// Authored Rime movies and provider-fed movies are different contracts.  An
// empty authored_asset never means "find a nearby movie".
enum class MovieSourceKind {
    AuthoredMovieTexture2,
    DynamicMovieProperty,
    DynamicStreamingUrl,
};

enum class MovieReadStatus {
    Ok,
    DynamicProviderRequired,
    InvalidArgument,
    AssetNotMounted,
    MalformedPartition,
    MovieTextureTypeMismatch,
    InstanceNotUnique,
    InstanceNotFound,
    ChunkNotMounted,
    ChunkSizeMismatch,
    ChunkIsNotWebM,
    NegativeControlFailed,
};

struct MovieSourceRequest {
    MovieSourceKind source_kind = MovieSourceKind::AuthoredMovieTexture2;

    // Exact live mount path supplied by the Rime Movie PointerRef.  A caller
    // normally gets this from bf6_rime_node::image_asset.
    std::string authored_asset;

    // Optional exact exported-instance half of the Rime PointerRef.  If it is
    // absent, the partition is accepted only when it contains exactly one
    // exported MovieTexture2Asset.
    std::string exported_instance_guid;

    // Proven source field id for a provider-fed Movie/Url.  This is retained
    // for a future provider handoff and is never used to choose an asset.
    uint32_t dynamic_source_field = 0;
};

struct MovieTextureControls {
    int exported_instances = 0;
    int movie_texture_instances = 0;
    int selected_instances = 0;
    int declared_size_matches = 0;
    int webm_signature_matches = 0;
    int fake_chunk_trials = 0;
    int fake_chunk_hits = 0;
    int subtitle_size_matches = 0;
};

struct MovieTexture {
    std::string asset_path;
    std::string partition_guid;
    std::string instance_guid;
    std::string movie_filename;
    std::string subtitle_filename;
    std::string chunk_guid;
    std::string subtitle_chunk_guid;

    uint32_t declared_chunk_size = 0;
    uint32_t declared_subtitle_chunk_size = 0;
    uint32_t video_stream_count = 0;
    uint32_t audio_stream_count = 0;
    uint32_t subtitle_stream_count = 0;

    uint8_t stereo = 0;
    uint8_t has_frame_info = 0;
    uint8_t flipped = 0;
    uint8_t localized_audio = 0;

    // Exact decompressed chunk bytes from the current mounted install.  They
    // are copied out of libbf6's one-slot raw buffer and never touch disk.
    std::vector<uint8_t> webm;
    std::vector<uint8_t> subtitle;
};

struct MovieReadResult {
    MovieReadStatus status = MovieReadStatus::InvalidArgument;
    MovieTexture movie;
    MovieTextureControls controls;
    std::string error;

    bool ok() const { return status == MovieReadStatus::Ok; }
    bool needs_dynamic_provider() const {
        return status == MovieReadStatus::DynamicProviderRequired;
    }
};

// Reads one exact MovieTexture2Asset and its raw WebM chunk from the current
// bf6_ctx mount.  Research TSV/JSON and exported media are not inputs.
MovieReadResult ReadMovieTexture2(bf6_ctx* context,
                                  const MovieSourceRequest& request);

struct DecodedMovieFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t stride = 0;
    int64_t timestamp_100ns = 0;
    int64_t duration_100ns = 0;
    std::string source_pixel_format;
    uint32_t yuv_matrix = 0;
    uint32_t nominal_range = 0;
    std::vector<uint8_t> bgra;
};

struct MovieAudioFormat {
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
};

struct DecodedMovieAudio {
    int64_t timestamp_100ns = 0;
    int64_t duration_100ns = 0;
    uint32_t frame_count = 0;
    std::vector<int16_t> pcm;
};

enum class MovieFrameStatus { Frame, EndOfStream, Error };
enum class MovieAudioStatus { Sample, EndOfStream, Unavailable, Error };

struct MoviePlaybackPolicy {
    // Loop is authored on RimeMovieElementData, not MovieTexture2Asset.  The
    // caller must set looping_is_authored only after reading that exact field.
    bool looping_is_authored = false;
    bool looping = false;
};

struct MoviePlaybackMetadata {
    bool duration_known = false;
    int64_t duration_100ns = 0;
    bool looping_known = false;
    bool looping = false;
    bool end_of_stream = false;
    uint64_t completed_cycles = 0;
    int64_t last_presented_end_100ns = 0;
};

struct MovieCompletionEvent {
    uint64_t completed_cycle = 0;
    bool looping_known = false;
    bool looping = false;
    bool restarted = false;
    int64_t duration_100ns = 0;
    int64_t last_presented_end_100ns = 0;
};

using MovieCompletionCallback = std::function<void(const MovieCompletionEvent&)>;

// Decoder seam used by the renderer.  It accepts the exact in-memory WebM;
// it does not require an exported temporary file.  On Windows the factory
// creates a Media Foundation source-reader implementation when the installed
// OS codec stack has a WebM byte-stream handler and VP8/VP9 decoder.
class MovieFrameDecoder {
public:
    virtual ~MovieFrameDecoder() = default;
    virtual bool Open(const MovieTexture& movie, std::string& error) = 0;
    virtual MovieFrameStatus ReadFrame(DecodedMovieFrame& frame,
                                       std::string& error) = 0;
    virtual bool HasAudio() const = 0;
    virtual MovieAudioFormat AudioFormat() const = 0;
    virtual MovieAudioStatus ReadAudio(DecodedMovieAudio& audio,
                                       std::string& error) = 0;
    virtual void SetPlaybackPolicy(const MoviePlaybackPolicy& policy) = 0;
    virtual MoviePlaybackMetadata PlaybackMetadata() const = 0;
    virtual void SetCompletionCallback(MovieCompletionCallback callback) = 0;
};

std::unique_ptr<MovieFrameDecoder> CreateMediaFoundationMovieDecoder(
    std::string& error);

const char* MovieReadStatusName(MovieReadStatus status);

}  // namespace bf6::ui_runtime

#endif
