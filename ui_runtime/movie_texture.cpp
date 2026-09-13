#include "movie_texture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace bf6::ui_runtime {
namespace {

constexpr const char* kMovieTexture2Guid =
    "3eb6459a-f31e-fc6a-c50c-80bff09d6014";
constexpr const char* kMovieTexture2ShippedGuid =
    "fc6af31e-0cc5-bf80-f09d-6014e2093ece";

template <class T>
bool ReadPod(const std::vector<uint8_t>& data, size_t at, T& value) {
    if (at > data.size() || sizeof(T) > data.size() - at) return false;
    std::memcpy(&value, data.data() + at, sizeof(T));
    return true;
}

bool Take(size_t& cursor, size_t bytes, size_t limit) {
    if (cursor > limit || bytes > limit - cursor) return false;
    cursor += bytes;
    return true;
}

std::string GuidText(const uint8_t* p) {
    uint32_t a = 0;
    uint16_t b = 0, c = 0;
    std::memcpy(&a, p, 4);
    std::memcpy(&b, p + 4, 2);
    std::memcpy(&c, p + 6, 2);
    char out[37]{};
    std::snprintf(out, sizeof(out),
                  "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  a, b, c, p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    return out;
}

std::string GuidRawHex(const uint8_t* p) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (size_t i = 0; i < 16; ++i) {
        out.push_back(kHex[p[i] >> 4]);
        out.push_back(kHex[p[i] & 15]);
    }
    return out;
}

std::string GuidKey(const std::string& text) {
    std::string out;
    out.reserve(32);
    for (char ch : text) {
        if (ch == '-' || ch == '{' || ch == '}') continue;
        if (ch >= 'A' && ch <= 'F') ch = static_cast<char>(ch - 'A' + 'a');
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return {};
        out.push_back(ch);
    }
    return out.size() == 32 ? out : std::string();
}

bool IsZeroGuid(const std::string& text) {
    const std::string key = GuidKey(text);
    return !key.empty() && key.find_first_not_of('0') == std::string::npos;
}

bool IsMovieTexture2(const std::string& guid) {
    const std::string key = GuidKey(guid);
    return key == GuidKey(kMovieTexture2Guid) ||
           key == GuidKey(kMovieTexture2ShippedGuid);
}

std::string MutateGuid(const std::string& guid) {
    std::string fake = guid;
    for (char& ch : fake) {
        if (ch == '-') continue;
        ch = ch == '0' ? '1' : '0';
        return fake;
    }
    return {};
}

bool HasWebMSignature(const std::vector<uint8_t>& bytes) {
    static constexpr uint8_t kEbml[] = {0x1A, 0x45, 0xDF, 0xA3};
    if (bytes.size() < sizeof(kEbml) ||
        std::memcmp(bytes.data(), kEbml, sizeof(kEbml)) != 0) return false;
    const size_t limit = std::min<size_t>(bytes.size(), 4096);
    static constexpr char kDocType[] = "webm";
    return std::search(bytes.begin(), bytes.begin() + limit,
                       std::begin(kDocType), std::end(kDocType) - 1) !=
           bytes.begin() + limit;
}

struct PartitionView {
    std::vector<uint8_t> bytes;
    size_t payload = 0;
    std::string partition_guid;
    std::vector<std::string> type_guids;
    uint32_t exported_count = 0;
    std::vector<uint32_t> instance_offsets;

    bool Parse(std::string& error) {
        if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0) {
            error = "MovieTexture2 asset is not a RIFF partition";
            return false;
        }
        size_t ebxd = 0, efix = 0, efix_size = 0;
        for (size_t at = 12; at + 8 <= bytes.size();) {
            uint32_t size = 0;
            if (!ReadPod(bytes, at + 4, size) || size > bytes.size() - at - 8) {
                error = "RIFF chunk exceeds the asset bounds";
                return false;
            }
            if (std::memcmp(bytes.data() + at, "EBXD", 4) == 0) ebxd = at + 8;
            if (std::memcmp(bytes.data() + at, "EFIX", 4) == 0) {
                efix = at + 8;
                efix_size = size;
            }
            at += 8 + size + (size & 1u);
        }
        if (!ebxd || !efix || efix_size < 20) {
            error = "RIFF partition has no bounded EBXD/EFIX pair";
            return false;
        }
        payload = (ebxd + 15u) & ~size_t(15u);
        if (payload >= bytes.size()) {
            error = "EBXD payload alignment is outside the asset";
            return false;
        }

        size_t cursor = efix;
        const size_t limit = efix + efix_size;
        if (limit > bytes.size() || !Take(cursor, 16, limit)) {
            error = "truncated EFIX partition GUID";
            return false;
        }
        partition_guid = GuidText(bytes.data() + efix);

        uint32_t count = 0;
        if (!ReadPod(bytes, cursor, count) || !Take(cursor, 4, limit) ||
            count > 65536u || size_t(count) * 16u > limit - cursor) {
            error = "invalid EFIX type GUID count";
            return false;
        }
        type_guids.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            type_guids.push_back(GuidText(bytes.data() + cursor));
            cursor += 16;
        }

        uint32_t signature_count = 0;
        if (!ReadPod(bytes, cursor, signature_count) || !Take(cursor, 4, limit) ||
            signature_count > 65536u || size_t(signature_count) * 4u > limit - cursor) {
            error = "invalid EFIX type signature count";
            return false;
        }
        cursor += size_t(signature_count) * 4u;
        if (!ReadPod(bytes, cursor, exported_count) || !Take(cursor, 4, limit)) {
            error = "truncated EFIX exported-instance count";
            return false;
        }
        uint32_t instance_count = 0;
        if (!ReadPod(bytes, cursor, instance_count) || !Take(cursor, 4, limit) ||
            instance_count > 1000000u || size_t(instance_count) * 4u > limit - cursor) {
            error = "invalid EFIX instance-offset count";
            return false;
        }
        if (exported_count > instance_count) {
            error = "exported-instance count exceeds instance count";
            return false;
        }
        instance_offsets.resize(instance_count);
        for (uint32_t& value : instance_offsets) {
            if (!ReadPod(bytes, cursor, value)) {
                error = "truncated EFIX instance-offset table";
                return false;
            }
            cursor += 4;
            if (payload + value >= bytes.size()) {
                error = "instance offset exceeds EBXD bounds";
                return false;
            }
        }
        return true;
    }

    bool CString(size_t position, std::string& out) const {
        int32_t relative = 0;
        if (!ReadPod(bytes, position, relative)) return false;
        if (relative == 0 || relative == -1) {
            out.clear();
            return true;
        }
        const int64_t absolute = static_cast<int64_t>(position) + relative;
        if (absolute < 0 || static_cast<uint64_t>(absolute) >= bytes.size()) return false;
        const auto first = bytes.begin() + static_cast<size_t>(absolute);
        const auto end = std::find(first, bytes.end(), uint8_t{0});
        if (end == bytes.end()) return false;
        out.assign(reinterpret_cast<const char*>(&*first),
                   static_cast<size_t>(end - first));
        return true;
    }

    bool ArrayCount(size_t position, uint32_t& out) const {
        int32_t relative = 0;
        if (!ReadPod(bytes, position, relative)) return false;
        if (relative == 0) { out = 0; return true; }
        const int64_t first = static_cast<int64_t>(position) + relative;
        int32_t count = 0;
        if (first < 4 || static_cast<uint64_t>(first) > bytes.size() ||
            !ReadPod(bytes, static_cast<size_t>(first - 4), count) ||
            count < 0 || count > 1000000) return false;
        out = static_cast<uint32_t>(count);
        return true;
    }
};

MovieReadResult Fail(MovieReadStatus status, std::string error,
                     MovieReadResult result = {}) {
    result.status = status;
    result.error = std::move(error);
    return result;
}

std::string HResultText(long value) {
    char out[32]{};
    std::snprintf(out, sizeof(out), "HRESULT 0x%08lX", value);
    return out;
}

}  // namespace

MovieReadResult ReadMovieTexture2(bf6_ctx* context,
                                  const MovieSourceRequest& request) {
    MovieReadResult result;
    if (request.source_kind != MovieSourceKind::AuthoredMovieTexture2) {
        result.status = MovieReadStatus::DynamicProviderRequired;
        result.error = request.source_kind == MovieSourceKind::DynamicStreamingUrl
            ? "streaming movie URL is a runtime provider value"
            : "movie asset is a runtime provider value";
        return result;
    }
    if (!context || request.authored_asset.empty())
        return Fail(MovieReadStatus::InvalidArgument,
                    "an open context and exact authored asset path are required");

    const uint8_t* raw = nullptr;
    const int64_t raw_size = bf6_read_raw(context, BF6_RAW_EBX,
                                          request.authored_asset.c_str(), &raw);
    if (raw_size <= 0 || !raw)
        return Fail(MovieReadStatus::AssetNotMounted,
                    "exact MovieTexture2 EBX is not present in the current mount");

    PartitionView partition;
    partition.bytes.assign(raw, raw + raw_size);
    if (!partition.Parse(result.error))
        return Fail(MovieReadStatus::MalformedPartition, result.error, std::move(result));

    result.controls.exported_instances = static_cast<int>(partition.exported_count);
    struct Candidate { size_t base; std::string instance_guid; };
    std::vector<Candidate> candidates;
    for (uint32_t i = 0; i < partition.exported_count; ++i) {
        const size_t base = partition.payload + partition.instance_offsets[i];
        uint16_t type_ref = std::numeric_limits<uint16_t>::max();
        if (!ReadPod(partition.bytes, base, type_ref) ||
            type_ref >= partition.type_guids.size()) continue;
        if (!IsMovieTexture2(partition.type_guids[type_ref])) continue;
        if (base < 16) continue;
        candidates.push_back({base, GuidText(partition.bytes.data() + base - 16)});
    }
    result.controls.movie_texture_instances = static_cast<int>(candidates.size());
    if (candidates.empty())
        return Fail(MovieReadStatus::MovieTextureTypeMismatch,
                    "partition has no exported MovieTexture2Asset",
                    std::move(result));

    const Candidate* selected = nullptr;
    const std::string wanted = GuidKey(request.exported_instance_guid);
    if (!request.exported_instance_guid.empty()) {
        if (wanted.empty())
            return Fail(MovieReadStatus::InvalidArgument,
                        "exported instance GUID is malformed", std::move(result));
        for (const Candidate& candidate : candidates)
            if (GuidKey(candidate.instance_guid) == wanted) {
                if (selected)
                    return Fail(MovieReadStatus::InstanceNotUnique,
                                "exported instance GUID matched more than once",
                                std::move(result));
                selected = &candidate;
            }
        if (!selected)
            return Fail(MovieReadStatus::InstanceNotFound,
                        "exact exported MovieTexture2 instance was not found",
                        std::move(result));
    } else {
        if (candidates.size() != 1)
            return Fail(MovieReadStatus::InstanceNotUnique,
                        "partition contains multiple MovieTexture2 exports; exact instance GUID required",
                        std::move(result));
        selected = &candidates.front();
    }
    result.controls.selected_instances = 1;

    MovieTexture& movie = result.movie;
    movie.asset_path = request.authored_asset;
    movie.partition_guid = partition.partition_guid;
    movie.instance_guid = selected->instance_guid;
    const size_t base = selected->base;

    if (!partition.ArrayCount(base + 32, movie.subtitle_stream_count) ||
        !partition.CString(base + 40, movie.movie_filename) ||
        !partition.ArrayCount(base + 48, movie.video_stream_count) ||
        !partition.ArrayCount(base + 56, movie.audio_stream_count) ||
        !partition.CString(base + 64, movie.subtitle_filename) ||
        base + 116 > partition.bytes.size())
        return Fail(MovieReadStatus::MalformedPartition,
                    "MovieTexture2 fixed fields exceed the selected instance",
                    std::move(result));

    const uint8_t* chunk_guid_bytes = partition.bytes.data() + base + 72;
    const uint8_t* subtitle_guid_bytes = partition.bytes.data() + base + 88;
    movie.chunk_guid = GuidText(chunk_guid_bytes);
    movie.subtitle_chunk_guid = GuidText(subtitle_guid_bytes);
    const std::string chunk_key = GuidRawHex(chunk_guid_bytes);
    const std::string subtitle_key = GuidRawHex(subtitle_guid_bytes);
    ReadPod(partition.bytes, base + 104, movie.declared_chunk_size);
    ReadPod(partition.bytes, base + 108, movie.declared_subtitle_chunk_size);
    movie.stereo = partition.bytes[base + 112];
    movie.has_frame_info = partition.bytes[base + 113];
    movie.flipped = partition.bytes[base + 114];
    movie.localized_audio = partition.bytes[base + 115];

    if (!movie.declared_chunk_size || IsZeroGuid(movie.chunk_guid))
        return Fail(MovieReadStatus::MalformedPartition,
                    "MovieTexture2 has no nonempty authored movie chunk",
                    std::move(result));

    const int64_t chunk_size = bf6_read_raw(context, BF6_RAW_CHUNK,
                                             chunk_key.c_str(), &raw);
    if (chunk_size <= 0 || !raw)
        return Fail(MovieReadStatus::ChunkNotMounted,
                    "authored MovieTexture2 chunk is not in the current mount",
                    std::move(result));
    movie.webm.assign(raw, raw + chunk_size);
    if (movie.webm.size() != movie.declared_chunk_size)
        return Fail(MovieReadStatus::ChunkSizeMismatch,
                    "authored movie chunk size does not match its current EBX",
                    std::move(result));
    result.controls.declared_size_matches = 1;
    if (!HasWebMSignature(movie.webm))
        return Fail(MovieReadStatus::ChunkIsNotWebM,
                    "authored movie chunk lacks the EBML/WebM signature",
                    std::move(result));
    result.controls.webm_signature_matches = 1;

    const std::string fake = MutateGuid(chunk_key);
    result.controls.fake_chunk_trials = 1;
    if (!fake.empty() && bf6_read_raw(context, BF6_RAW_CHUNK, fake.c_str(), nullptr) >= 0)
        result.controls.fake_chunk_hits = 1;
    if (result.controls.fake_chunk_hits)
        return Fail(MovieReadStatus::NegativeControlFailed,
                    "mutated chunk GUID unexpectedly resolved",
                    std::move(result));

    if (!IsZeroGuid(movie.subtitle_chunk_guid)) {
        const int64_t subtitle_size = bf6_read_raw(context, BF6_RAW_CHUNK,
            subtitle_key.c_str(), &raw);
        if (subtitle_size < 0 || (subtitle_size > 0 && !raw))
            return Fail(MovieReadStatus::ChunkNotMounted,
                        "authored subtitle chunk is not in the current mount",
                        std::move(result));
        if (static_cast<uint64_t>(subtitle_size) != movie.declared_subtitle_chunk_size)
            return Fail(MovieReadStatus::ChunkSizeMismatch,
                        "authored subtitle chunk size does not match its current EBX",
                        std::move(result));
        movie.subtitle.assign(raw, raw + subtitle_size);
        result.controls.subtitle_size_matches = 1;
    } else if (movie.declared_subtitle_chunk_size != 0) {
        return Fail(MovieReadStatus::ChunkSizeMismatch,
                    "zero subtitle GUID carries a nonzero declared size",
                    std::move(result));
    }

    result.status = MovieReadStatus::Ok;
    return result;
}

const char* MovieReadStatusName(MovieReadStatus status) {
    switch (status) {
    case MovieReadStatus::Ok: return "ok";
    case MovieReadStatus::DynamicProviderRequired: return "dynamic-provider-required";
    case MovieReadStatus::InvalidArgument: return "invalid-argument";
    case MovieReadStatus::AssetNotMounted: return "asset-not-mounted";
    case MovieReadStatus::MalformedPartition: return "malformed-partition";
    case MovieReadStatus::MovieTextureTypeMismatch: return "movie-texture-type-mismatch";
    case MovieReadStatus::InstanceNotUnique: return "instance-not-unique";
    case MovieReadStatus::InstanceNotFound: return "instance-not-found";
    case MovieReadStatus::ChunkNotMounted: return "chunk-not-mounted";
    case MovieReadStatus::ChunkSizeMismatch: return "chunk-size-mismatch";
    case MovieReadStatus::ChunkIsNotWebM: return "chunk-is-not-webm";
    case MovieReadStatus::NegativeControlFailed: return "negative-control-failed";
    }
    return "unknown";
}

#ifdef _WIN32
namespace {

template <class T> void Release(T*& value) {
    if (value) { value->Release(); value = nullptr; }
}

class MediaFoundationMovieDecoder final : public MovieFrameDecoder {
public:
    MediaFoundationMovieDecoder() {
        startup_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    ~MediaFoundationMovieDecoder() override {
        Reset();
        if (startup_) MFShutdown();
    }

    bool Open(const MovieTexture& movie, std::string& error) override {
        Reset();
        if (!startup_) { error = "Media Foundation startup failed"; return false; }
        if (!HasWebMSignature(movie.webm)) {
            error = "decoder input is not the exact in-memory WebM payload";
            return false;
        }
        if (movie.webm.size() > std::numeric_limits<UINT>::max()) {
            error = "WebM exceeds SHCreateMemStream's byte limit";
            return false;
        }

        stream_ = SHCreateMemStream(movie.webm.data(), static_cast<UINT>(movie.webm.size()));
        if (!stream_) { error = "SHCreateMemStream failed"; return false; }
        HRESULT hr = MFCreateMFByteStreamOnStreamEx(stream_, &byte_stream_);
        if (FAILED(hr)) { error = HResultText(hr); Reset(); return false; }
        IMFAttributes* attrs = nullptr;
        if (SUCCEEDED(byte_stream_->QueryInterface(IID_PPV_ARGS(&attrs)))) {
            attrs->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"video/webm");
            attrs->SetString(MF_BYTESTREAM_ORIGIN_NAME, L"bf6-current-install.webm");
            attrs->Release();
        }
        IMFAttributes* reader_attributes = nullptr;
        hr = MFCreateAttributes(&reader_attributes, 2);
        if (SUCCEEDED(hr))
            hr = reader_attributes->SetUINT32(
                MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        if (SUCCEEDED(hr))
            hr = reader_attributes->SetUINT32(
                MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (SUCCEEDED(hr))
            hr = MFCreateSourceReaderFromByteStream(
                byte_stream_, reader_attributes, &reader_);
        Release(reader_attributes);
        if (FAILED(hr)) {
            error = "the installed Media Foundation codec stack cannot open WebM (" +
                    HResultText(hr) + ")";
            Reset();
            return false;
        }

        // Source readers may select every native stream by default. Make the
        // contract explicit so an unread audio stream cannot accumulate while
        // the video-only assets decode, then opt the authored audio stream in
        // independently. Audio negotiation is deliberately non-fatal.
        hr = reader_->SetStreamSelection(
            static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        if (SUCCEEDED(hr))
            hr = reader_->SetStreamSelection(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        if (FAILED(hr)) {
            error = "Media Foundation could not select the WebM video stream (" +
                    HResultText(hr) + ")";
            Reset();
            return false;
        }
        audio_declared_ = movie.audio_stream_count > 0;
        bool audio_selected = false;
        if (audio_declared_)
            audio_selected = SUCCEEDED(reader_->SetStreamSelection(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE));

        GUID native_subtype{};
        IMFMediaType* native = nullptr;
        if (SUCCEEDED(reader_->GetNativeMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native))) {
            native->GetGUID(MF_MT_SUBTYPE, &native_subtype);
            Release(native);
        }

        const struct Candidate { const GUID* subtype; PixelFormat format; const char* name; }
            candidates[] = {
                {&MFVideoFormat_RGB32, PixelFormat::Rgb32, "RGB32"},
                {&MFVideoFormat_NV12, PixelFormat::Nv12, "NV12"},
                {&MFVideoFormat_YUY2, PixelFormat::Yuy2, "YUY2"},
            };
        std::string attempts;
        for (const Candidate& candidate : candidates) {
            IMFMediaType* output = nullptr;
            hr = MFCreateMediaType(&output);
            if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_SUBTYPE, *candidate.subtype);
            if (SUCCEEDED(hr))
                hr = reader_->SetCurrentMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, output);
            Release(output);
            if (!attempts.empty()) attempts += ", ";
            attempts += candidate.name;
            attempts += "=";
            attempts += HResultText(hr);
            if (SUCCEEDED(hr)) {
                pixel_format_ = candidate.format;
                break;
            }
        }
        if (pixel_format_ == PixelFormat::None) {
            error = "WebM source native subtype " + GuidValueText(native_subtype) +
                    " has no installed decode transform (" + attempts + ")";
            Reset();
            return false;
        }

        IMFMediaType* active = nullptr;
        hr = reader_->GetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &active);
        if (SUCCEEDED(hr)) hr = MFGetAttributeSize(active, MF_MT_FRAME_SIZE, &width_, &height_);
        UINT32 raw_stride = 0;
        if (SUCCEEDED(hr) && FAILED(active->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))) {
            raw_stride = pixel_format_ == PixelFormat::Rgb32 ? width_ * 4 :
                         pixel_format_ == PixelFormat::Yuy2 ? width_ * 2 : width_;
        }
        stride_ = static_cast<int32_t>(raw_stride);
        UINT32 matrix = MFVideoTransferMatrix_Unknown;
        UINT32 nominal_range = MFNominalRange_Unknown;
        active->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
        active->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nominal_range);
        SetYuvContract(matrix, nominal_range);
        Release(active);
        if (FAILED(hr) || !width_ || !height_) {
            error = "WebM decoder did not expose a bounded video frame size";
            Reset();
            return false;
        }

        if (audio_selected) {
            IMFMediaType* output = nullptr;
            HRESULT audio_hr = MFCreateMediaType(&output);
            if (SUCCEEDED(audio_hr))
                audio_hr = output->SetGUID(MF_MT_MAJOR_TYPE,
                                           MFMediaType_Audio);
            if (SUCCEEDED(audio_hr))
                audio_hr = output->SetGUID(MF_MT_SUBTYPE,
                                           MFAudioFormat_PCM);
            if (SUCCEEDED(audio_hr))
                audio_hr = output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            if (SUCCEEDED(audio_hr))
                audio_hr = output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (SUCCEEDED(audio_hr))
                audio_hr = reader_->SetCurrentMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                    nullptr, output);
            Release(output);

            IMFMediaType* active_audio = nullptr;
            UINT32 channels = 0, rate = 0, bits = 0;
            if (SUCCEEDED(audio_hr))
                audio_hr = reader_->GetCurrentMediaType(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                    &active_audio);
            if (SUCCEEDED(audio_hr))
                audio_hr = active_audio->GetUINT32(
                    MF_MT_AUDIO_NUM_CHANNELS, &channels);
            if (SUCCEEDED(audio_hr))
                audio_hr = active_audio->GetUINT32(
                    MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            if (SUCCEEDED(audio_hr))
                audio_hr = active_audio->GetUINT32(
                    MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            Release(active_audio);
            if (SUCCEEDED(audio_hr) && channels > 0 && channels <= 2 &&
                rate > 0 && bits == 16) {
                audio_format_.channels = static_cast<uint16_t>(channels);
                audio_format_.sample_rate = rate;
                audio_format_.bits_per_sample = static_cast<uint16_t>(bits);
                audio_available_ = true;
            } else {
                reader_->SetStreamSelection(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                    FALSE);
            }
        }

        PROPVARIANT duration;
        PropVariantInit(&duration);
        if (SUCCEEDED(reader_->GetPresentationAttribute(
                static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                MF_PD_DURATION, &duration))) {
            if (duration.vt == VT_UI8) {
                playback_.duration_known = true;
                playback_.duration_100ns = static_cast<int64_t>(duration.uhVal.QuadPart);
            } else if (duration.vt == VT_I8 && duration.hVal.QuadPart >= 0) {
                playback_.duration_known = true;
                playback_.duration_100ns = duration.hVal.QuadPart;
            }
        }
        PropVariantClear(&duration);
        return true;
    }

    MovieFrameStatus ReadFrame(DecodedMovieFrame& frame,
                               std::string& error) override {
        if (!reader_) { error = "movie decoder is not open"; return MovieFrameStatus::Error; }
        if (terminal_eos_) return MovieFrameStatus::EndOfStream;
        for (;;) {
            DWORD stream_index = 0, flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sample = nullptr;
            const HRESULT hr = reader_->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                0, &stream_index, &flags, &timestamp, &sample);
            if (FAILED(hr)) { error = HResultText(hr); return MovieFrameStatus::Error; }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                Release(sample);
                const EosAction action = HandleEndOfStream(false, error);
                if (action == EosAction::Restart) continue;
                if (action == EosAction::Error)
                    return MovieFrameStatus::Error;
                return MovieFrameStatus::EndOfStream;
            }
            if (!sample) continue;

            IMFMediaBuffer* buffer = nullptr;
            HRESULT copy_hr = sample->ConvertToContiguousBuffer(&buffer);
            LONGLONG duration = 0;
            sample->GetSampleDuration(&duration);
            Release(sample);
            if (FAILED(copy_hr)) { error = HResultText(copy_hr); return MovieFrameStatus::Error; }
            BYTE* data = nullptr;
            LONG pitch = stride_;
            bool locked_2d = false;
            IMF2DBuffer* buffer_2d = nullptr;
            DWORD max_bytes = 0, current_bytes = 0;
            if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer_2d)))) {
                copy_hr = buffer_2d->Lock2D(&data, &pitch);
                locked_2d = SUCCEEDED(copy_hr);
            } else {
                copy_hr = buffer->Lock(&data, &max_bytes, &current_bytes);
                if (SUCCEEDED(copy_hr) && pitch < 0)
                    data += static_cast<size_t>(-pitch) * (height_ - 1u);
            }
            if (FAILED(copy_hr) || !data) {
                Release(buffer_2d);
                Release(buffer);
                error = HResultText(copy_hr);
                return MovieFrameStatus::Error;
            }
            frame.width = width_;
            frame.height = height_;
            frame.stride = static_cast<int32_t>(width_ * 4u);
            frame.timestamp_100ns = timestamp;
            frame.duration_100ns = duration;
            frame.source_pixel_format = PixelFormatName();
            frame.yuv_matrix = yuv_matrix_;
            frame.nominal_range = nominal_range_;
            const bool converted = ConvertToBgra(data, pitch, frame.bgra, error);
            if (locked_2d) buffer_2d->Unlock2D();
            else buffer->Unlock();
            Release(buffer_2d);
            Release(buffer);
            if (converted) {
                const int64_t presented_end = duration > 0 &&
                    timestamp <= std::numeric_limits<int64_t>::max() - duration
                    ? timestamp + duration : timestamp;
                playback_.last_presented_end_100ns =
                    std::max(playback_.last_presented_end_100ns, presented_end);
            }
            return converted ? MovieFrameStatus::Frame : MovieFrameStatus::Error;
        }
    }

    bool HasAudio() const override { return audio_available_; }
    MovieAudioFormat AudioFormat() const override { return audio_format_; }

    MovieAudioStatus ReadAudio(DecodedMovieAudio& audio,
                               std::string& error) override {
        audio = DecodedMovieAudio{};
        if (!reader_) {
            error = "movie decoder is not open";
            return MovieAudioStatus::Error;
        }
        if (!audio_available_) return MovieAudioStatus::Unavailable;
        if (terminal_eos_ || audio_eos_)
            return MovieAudioStatus::EndOfStream;
        for (;;) {
            DWORD stream_index = 0, flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sample = nullptr;
            const HRESULT hr = reader_->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                0, &stream_index, &flags, &timestamp, &sample);
            if (FAILED(hr)) {
                error = HResultText(hr);
                return MovieAudioStatus::Error;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                Release(sample);
                const EosAction action = HandleEndOfStream(true, error);
                if (action == EosAction::Restart) continue;
                if (action == EosAction::Error)
                    return MovieAudioStatus::Error;
                return MovieAudioStatus::EndOfStream;
            }
            if (!sample) continue;

            LONGLONG duration = 0;
            sample->GetSampleDuration(&duration);
            IMFMediaBuffer* buffer = nullptr;
            HRESULT copy_hr = sample->ConvertToContiguousBuffer(&buffer);
            Release(sample);
            if (FAILED(copy_hr)) {
                error = HResultText(copy_hr);
                return MovieAudioStatus::Error;
            }
            BYTE* data = nullptr;
            DWORD max_bytes = 0, current_bytes = 0;
            copy_hr = buffer->Lock(&data, &max_bytes, &current_bytes);
            const uint32_t block_align =
                static_cast<uint32_t>(audio_format_.channels) * 2u;
            if (FAILED(copy_hr) || !data || !block_align ||
                current_bytes == 0 || current_bytes % block_align != 0) {
                if (SUCCEEDED(copy_hr)) buffer->Unlock();
                Release(buffer);
                error = "decoded movie PCM sample is empty or not frame-aligned";
                return MovieAudioStatus::Error;
            }
            audio.timestamp_100ns = timestamp;
            audio.duration_100ns = duration;
            audio.frame_count = current_bytes / block_align;
            audio.pcm.resize(current_bytes / sizeof(int16_t));
            std::memcpy(audio.pcm.data(), data, current_bytes);
            const int64_t presented_end = duration > 0 &&
                timestamp <= std::numeric_limits<int64_t>::max() - duration
                ? timestamp + duration : timestamp;
            playback_.last_presented_end_100ns =
                std::max(playback_.last_presented_end_100ns, presented_end);
            buffer->Unlock();
            Release(buffer);
            error.clear();
            return MovieAudioStatus::Sample;
        }
    }

    void SetPlaybackPolicy(const MoviePlaybackPolicy& policy) override {
        playback_.looping_known = policy.looping_is_authored;
        playback_.looping = policy.looping_is_authored && policy.looping;
    }

    MoviePlaybackMetadata PlaybackMetadata() const override { return playback_; }

    void SetCompletionCallback(MovieCompletionCallback callback) override {
        completion_ = std::move(callback);
    }

private:
    enum class PixelFormat { None, Rgb32, Nv12, Yuy2 };
    enum class EosAction { Wait, Restart, Terminal, Error };

    static std::string GuidValueText(const GUID& guid) {
        char out[39]{};
        std::snprintf(out, sizeof(out),
            "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
            guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
            guid.Data4[6], guid.Data4[7]);
        return out;
    }

    void SetYuvContract(UINT32 matrix, UINT32 nominal_range) {
        // Matrix attributes are exact Media Foundation enum values. Unknown
        // remains explicit and uses the conventional SD/HD fallback only
        // because Media Foundation itself omitted the contract.
        yuv_matrix_ = matrix;
        nominal_range_ = nominal_range;
        if (matrix == MFVideoTransferMatrix_BT601 ||
            matrix == MFVideoTransferMatrix_SMPTE240M) {
            kr_ = 0.299; kb_ = 0.114;
        } else if (matrix == MFVideoTransferMatrix_BT2020_10 ||
                   matrix == MFVideoTransferMatrix_BT2020_12) {
            kr_ = 0.2627; kb_ = 0.0593;
        } else {
            kr_ = 0.2126; kb_ = 0.0722;
        }
        full_range_ = nominal_range == MFNominalRange_0_255;
    }

    const char* PixelFormatName() const {
        switch (pixel_format_) {
        case PixelFormat::Rgb32: return "RGB32";
        case PixelFormat::Nv12: return "NV12";
        case PixelFormat::Yuy2: return "YUY2";
        default: return "none";
        }
    }

    static uint8_t Byte(double value) {
        const long rounded = std::lround(value * 255.0);
        return static_cast<uint8_t>(std::max<long>(0, std::min<long>(255, rounded)));
    }

    void YuvPixel(uint8_t y8, uint8_t u8, uint8_t v8, uint8_t* out) const {
        const double y = full_range_ ? y8 / 255.0 : (static_cast<int>(y8) - 16) / 219.0;
        const double cb = (static_cast<int>(u8) - 128) / (full_range_ ? 255.0 : 224.0);
        const double cr = (static_cast<int>(v8) - 128) / (full_range_ ? 255.0 : 224.0);
        const double kg = 1.0 - kr_ - kb_;
        const double r = y + (2.0 - 2.0 * kr_) * cr;
        const double b = y + (2.0 - 2.0 * kb_) * cb;
        const double g = y - kb_ * (2.0 - 2.0 * kb_) / kg * cb
                           - kr_ * (2.0 - 2.0 * kr_) / kg * cr;
        out[0] = Byte(b); out[1] = Byte(g); out[2] = Byte(r); out[3] = 255;
    }

    bool ConvertToBgra(const uint8_t* scanline0, int32_t pitch,
                       std::vector<uint8_t>& out, std::string& error) const {
        if (!scanline0 || pitch == 0) { error = "decoded surface has zero stride"; return false; }
        out.assign(static_cast<size_t>(width_) * height_ * 4u, 0);
        if (pixel_format_ == PixelFormat::Rgb32) {
            if (static_cast<uint32_t>(std::abs(pitch)) < width_ * 4u) {
                error = "RGB32 stride is smaller than its frame width"; return false;
            }
            for (uint32_t y = 0; y < height_; ++y) {
                const uint8_t* src = scanline0 + static_cast<ptrdiff_t>(y) * pitch;
                uint8_t* dst = out.data() + static_cast<size_t>(y) * width_ * 4u;
                for (uint32_t x = 0; x < width_; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 0];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 2];
                    dst[x * 4 + 3] = 255;
                }
            }
            return true;
        }
        if ((width_ & 1u) || pixel_format_ == PixelFormat::Nv12 && (height_ & 1u)) {
            error = "subsampled decoded surface has odd dimensions"; return false;
        }
        if (pixel_format_ == PixelFormat::Nv12) {
            if (pitch < 0 || static_cast<uint32_t>(pitch) < width_) {
                error = "NV12 decoder returned an unsupported negative/small stride"; return false;
            }
            const uint8_t* uv_base = scanline0 + static_cast<size_t>(pitch) * height_;
            for (uint32_t y = 0; y < height_; ++y) {
                const uint8_t* ys = scanline0 + static_cast<size_t>(pitch) * y;
                const uint8_t* uv = uv_base + static_cast<size_t>(pitch) * (y / 2u);
                uint8_t* dst = out.data() + static_cast<size_t>(y) * width_ * 4u;
                for (uint32_t x = 0; x < width_; ++x)
                    YuvPixel(ys[x], uv[(x & ~1u) + 0], uv[(x & ~1u) + 1], dst + x * 4u);
            }
            return true;
        }
        if (pixel_format_ == PixelFormat::Yuy2) {
            if (static_cast<uint32_t>(std::abs(pitch)) < width_ * 2u) {
                error = "YUY2 stride is smaller than its frame width"; return false;
            }
            for (uint32_t y = 0; y < height_; ++y) {
                const uint8_t* src = scanline0 + static_cast<ptrdiff_t>(y) * pitch;
                uint8_t* dst = out.data() + static_cast<size_t>(y) * width_ * 4u;
                for (uint32_t x = 0; x < width_; x += 2u) {
                    YuvPixel(src[x * 2 + 0], src[x * 2 + 1], src[x * 2 + 3], dst + x * 4u);
                    YuvPixel(src[x * 2 + 2], src[x * 2 + 1], src[x * 2 + 3], dst + (x + 1u) * 4u);
                }
            }
            return true;
        }
        error = "decoded surface subtype is not implemented";
        return false;
    }

    EosAction HandleEndOfStream(bool audio, std::string& error) {
        if (audio) audio_eos_ = true;
        else video_eos_ = true;
        if (!video_eos_ || (audio_available_ && !audio_eos_))
            return EosAction::Wait;

        playback_.end_of_stream = true;
        ++playback_.completed_cycles;
        const bool restart = playback_.looping_known && playback_.looping;
        MovieCompletionEvent event;
        event.completed_cycle = playback_.completed_cycles;
        event.looping_known = playback_.looping_known;
        event.looping = playback_.looping;
        event.restarted = restart;
        event.duration_100ns = playback_.duration_known
            ? playback_.duration_100ns : 0;
        event.last_presented_end_100ns = playback_.last_presented_end_100ns;
        if (completion_) completion_(event);
        if (!restart) {
            terminal_eos_ = true;
            return EosAction::Terminal;
        }

        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = 0;
        const HRESULT seek_hr =
            reader_->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
        if (FAILED(seek_hr)) {
            error = "authored loop restart failed (" +
                    HResultText(seek_hr) + ")";
            terminal_eos_ = true;
            return EosAction::Error;
        }
        playback_.end_of_stream = false;
        video_eos_ = false;
        audio_eos_ = false;
        return EosAction::Restart;
    }

    void Reset() {
        Release(reader_);
        Release(byte_stream_);
        Release(stream_);
        width_ = height_ = 0;
        stride_ = 0;
        pixel_format_ = PixelFormat::None;
        yuv_matrix_ = MFVideoTransferMatrix_Unknown;
        nominal_range_ = MFNominalRange_Unknown;
        playback_.duration_known = false;
        playback_.duration_100ns = 0;
        playback_.end_of_stream = false;
        playback_.completed_cycles = 0;
        playback_.last_presented_end_100ns = 0;
        terminal_eos_ = false;
        video_eos_ = false;
        audio_eos_ = false;
        audio_declared_ = false;
        audio_available_ = false;
        audio_format_ = MovieAudioFormat{};
    }

    bool startup_ = false;
    IStream* stream_ = nullptr;
    IMFByteStream* byte_stream_ = nullptr;
    IMFSourceReader* reader_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    int32_t stride_ = 0;
    PixelFormat pixel_format_ = PixelFormat::None;
    double kr_ = 0.2126, kb_ = 0.0722;
    bool full_range_ = false;
    uint32_t yuv_matrix_ = MFVideoTransferMatrix_Unknown;
    uint32_t nominal_range_ = MFNominalRange_Unknown;
    bool audio_declared_ = false;
    bool audio_available_ = false;
    bool video_eos_ = false;
    bool audio_eos_ = false;
    MovieAudioFormat audio_format_;
    MoviePlaybackMetadata playback_;
    MovieCompletionCallback completion_;
    bool terminal_eos_ = false;
};

}  // namespace

std::unique_ptr<MovieFrameDecoder> CreateMediaFoundationMovieDecoder(
    std::string& error) {
    auto decoder = std::make_unique<MediaFoundationMovieDecoder>();
    error.clear();
    return decoder;
}
#else
std::unique_ptr<MovieFrameDecoder> CreateMediaFoundationMovieDecoder(
    std::string& error) {
    error = "Media Foundation is available only on Windows";
    return {};
}
#endif

}  // namespace bf6::ui_runtime
