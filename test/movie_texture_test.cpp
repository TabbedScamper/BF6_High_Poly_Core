#include "bf6_core.h"
#include "movie_texture.h"

#include <cstdio>
#include <cstdint>
#include <string>

using namespace bf6::ui_runtime;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: movie_texture_test <Steam BF6 game dir>\n");
        return 2;
    }

    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, static_cast<int>(sizeof(error)));
    if (!context) { std::fprintf(stderr, "open: %s\n", error); return 1; }
    if (!bf6_mount_all(context, 0, error, static_cast<int>(sizeof(error)))) {
        std::fprintf(stderr, "mount: %s\n", error);
        bf6_close(context);
        return 1;
    }

    MovieSourceRequest dynamic;
    dynamic.source_kind = MovieSourceKind::DynamicMovieProperty;
    dynamic.dynamic_source_field = 0x0C84D03Du; // Djb2-XOR("Movie")
    const MovieReadResult unresolved = ReadMovieTexture2(context, dynamic);

    MovieSourceRequest request;
    request.authored_asset =
        "common/ui/notification/rankup/assets/movies/"
        "rankupglitch_forexport_alltogether_05";
    request.exported_instance_guid = "cf5a8be8-4cd4-f859-9bee-9fcac0a3c6bd";
    const MovieReadResult exact = ReadMovieTexture2(context, request);

    MovieSourceRequest fake_instance = request;
    fake_instance.exported_instance_guid = "cf5a8be9-4cd4-f859-9bee-9fcac0a3c6bd";
    const MovieReadResult fake = ReadMovieTexture2(context, fake_instance);

    std::printf(
        "movie status=%s bytes=%zu declared=%u streams=%u/%u/%u "
        "controls typed=%d selected=%d size=%d webm=%d fake-chunk=%d/%d\n",
        MovieReadStatusName(exact.status), exact.movie.webm.size(),
        exact.movie.declared_chunk_size, exact.movie.video_stream_count,
        exact.movie.audio_stream_count, exact.movie.subtitle_stream_count,
        exact.controls.movie_texture_instances, exact.controls.selected_instances,
        exact.controls.declared_size_matches, exact.controls.webm_signature_matches,
        exact.controls.fake_chunk_hits, exact.controls.fake_chunk_trials);
    std::printf("dynamic=%s fake-instance=%s\n",
                MovieReadStatusName(unresolved.status),
                MovieReadStatusName(fake.status));

    std::string decoder_error;
    auto decoder = CreateMediaFoundationMovieDecoder(decoder_error);
    bool decoder_open = false;
    MovieFrameStatus frame_status = MovieFrameStatus::Error;
    DecodedMovieFrame frame;
    int completion_events = 0;
    MovieCompletionEvent completion;
    bool unknown_loop_control = false;
    int decoded_frames = 0;
    if (decoder && exact.ok()) {
        decoder->SetPlaybackPolicy({false, true});
        const MoviePlaybackMetadata unknown_loop = decoder->PlaybackMetadata();
        unknown_loop_control = !unknown_loop.looping_known && !unknown_loop.looping;
        decoder->SetPlaybackPolicy({true, false});
        decoder->SetCompletionCallback([&](const MovieCompletionEvent& event) {
            ++completion_events;
            completion = event;
        });
        decoder_open = decoder->Open(exact.movie, decoder_error);
    }
    if (decoder_open) frame_status = decoder->ReadFrame(frame, decoder_error);
    const bool decoded_frame = frame_status == MovieFrameStatus::Frame;
    std::printf("mf-decoder-open=%d frame=%d %ux%u bytes=%zu source=%s matrix=%u range=%u%s%s\n",
                decoder_open ? 1 : 0, decoded_frame ? 1 : 0,
                frame.width, frame.height, frame.bgra.size(),
                frame.source_pixel_format.c_str(), frame.yuv_matrix,
                frame.nominal_range,
                decoder_error.empty() ? "" : " error=",
                decoder_error.c_str());

    MoviePlaybackMetadata playback;
    bool eos_stable = false;
    int decoded_audio_samples = 0;
    uint64_t decoded_audio_frames = 0;
    MovieAudioStatus audio_status = MovieAudioStatus::Unavailable;
    if (decoder_open && decoded_frame) {
        decoded_frames = 1;
        for (int guard = 0; guard < 10000 && frame_status == MovieFrameStatus::Frame; ++guard) {
            frame_status = decoder->ReadFrame(frame, decoder_error);
            if (frame_status == MovieFrameStatus::Frame) ++decoded_frames;
        }
        if (decoder->HasAudio()) {
            DecodedMovieAudio audio;
            for (int guard = 0; guard < 10000; ++guard) {
                audio_status = decoder->ReadAudio(audio, decoder_error);
                if (audio_status != MovieAudioStatus::Sample) break;
                ++decoded_audio_samples;
                decoded_audio_frames += audio.frame_count;
            }
        }
        playback = decoder->PlaybackMetadata();
        const int events_at_eos = completion_events;
        eos_stable = decoder->ReadFrame(frame, decoder_error) ==
                         MovieFrameStatus::EndOfStream &&
                     completion_events == events_at_eos;
    }
    std::printf(
        "playback frames=%d duration-known=%d duration=%lld eos=%d cycles=%llu "
        "events=%d restarted=%d last-end=%lld unknown-loop-control=%d stable=%d\n",
        decoded_frames, playback.duration_known ? 1 : 0,
        static_cast<long long>(playback.duration_100ns),
        playback.end_of_stream ? 1 : 0,
        static_cast<unsigned long long>(playback.completed_cycles),
        completion_events, completion.restarted ? 1 : 0,
        static_cast<long long>(playback.last_presented_end_100ns),
        unknown_loop_control ? 1 : 0, eos_stable ? 1 : 0);
    std::printf("playback audio=%d frames=%llu status=%d format=%u/%u/%u\n",
                decoded_audio_samples,
                static_cast<unsigned long long>(decoded_audio_frames),
                static_cast<int>(audio_status),
                decoder ? decoder->AudioFormat().channels : 0,
                decoder ? decoder->AudioFormat().sample_rate : 0,
                decoder ? decoder->AudioFormat().bits_per_sample : 0);

    MovieSourceRequest boot_request;
    boot_request.authored_asset =
        "common/ui/bootflow/assets/videos/"
        "gla_bootflow_logo_parade_withaudio";
    const MovieReadResult boot = ReadMovieTexture2(context, boot_request);
    std::string boot_decoder_error;
    auto boot_decoder = CreateMediaFoundationMovieDecoder(boot_decoder_error);
    const bool boot_decoder_open = boot_decoder && boot.ok() &&
        boot_decoder->Open(boot.movie, boot_decoder_error);
    const MovieAudioFormat boot_format = boot_decoder_open
        ? boot_decoder->AudioFormat() : MovieAudioFormat{};
    uint64_t boot_audio_frames = 0;
    int boot_audio_samples = 0;
    MovieAudioStatus boot_audio_status = MovieAudioStatus::Unavailable;
    if (boot_decoder_open && boot_decoder->HasAudio()) {
        DecodedMovieAudio audio;
        for (int guard = 0; guard < 10000; ++guard) {
            boot_audio_status =
                boot_decoder->ReadAudio(audio, boot_decoder_error);
            if (boot_audio_status != MovieAudioStatus::Sample) break;
            ++boot_audio_samples;
            boot_audio_frames += audio.frame_count;
        }
    }

    MovieSourceRequest silent_request;
    silent_request.authored_asset =
        "common/ui/bootflow/assets/videos/"
        "bf6_startscreenloop_3440x1440_dallas";
    const MovieReadResult silent = ReadMovieTexture2(context, silent_request);
    std::string silent_decoder_error;
    auto silent_decoder = CreateMediaFoundationMovieDecoder(
        silent_decoder_error);
    const bool silent_decoder_open = silent_decoder && silent.ok() &&
        silent_decoder->Open(silent.movie, silent_decoder_error);
    std::printf(
        "boot-audio read=%s streams=%u open=%d has=%d format=%u/%u/%u "
        "samples=%d frames=%llu status=%d; silent streams=%u open=%d has=%d%s%s\n",
        MovieReadStatusName(boot.status), boot.movie.audio_stream_count,
        boot_decoder_open ? 1 : 0,
        boot_decoder_open && boot_decoder->HasAudio() ? 1 : 0,
        boot_format.channels, boot_format.sample_rate,
        boot_format.bits_per_sample, boot_audio_samples,
        static_cast<unsigned long long>(boot_audio_frames),
        static_cast<int>(boot_audio_status), silent.movie.audio_stream_count,
        silent_decoder_open ? 1 : 0,
        silent_decoder_open && silent_decoder->HasAudio() ? 1 : 0,
        boot_decoder_error.empty() ? "" : " boot-error=",
        boot_decoder_error.c_str());

    const bool pass = exact.ok() &&
        exact.controls.movie_texture_instances == 1 &&
        exact.controls.selected_instances == 1 &&
        exact.controls.declared_size_matches == 1 &&
        exact.controls.webm_signature_matches == 1 &&
        exact.controls.fake_chunk_trials == 1 &&
        exact.controls.fake_chunk_hits == 0 &&
        unresolved.status == MovieReadStatus::DynamicProviderRequired &&
        unresolved.movie.webm.empty() &&
        fake.status == MovieReadStatus::InstanceNotFound &&
        fake.movie.webm.empty() &&
        (!decoder_open || (decoded_frame && frame.width > 0 && frame.height > 0 &&
          frame.bgra.size() == static_cast<size_t>(frame.width) * frame.height * 4u &&
          frame_status == MovieFrameStatus::EndOfStream && decoded_frames > 0 &&
          (!decoder->HasAudio() ||
           (decoded_audio_samples > 0 && decoded_audio_frames > 0 &&
            audio_status == MovieAudioStatus::EndOfStream)) &&
          playback.looping_known && !playback.looping && playback.end_of_stream &&
          playback.completed_cycles == 1 && completion_events == 1 &&
          completion.completed_cycle == 1 && completion.looping_known &&
          !completion.looping && !completion.restarted &&
          (!playback.duration_known ||
           completion.duration_100ns == playback.duration_100ns) &&
          unknown_loop_control && eos_stable)) &&
        boot.ok() && boot.movie.audio_stream_count == 1 &&
        boot_decoder_open && boot_decoder->HasAudio() &&
        boot_format.channels == 2 && boot_format.sample_rate == 48000 &&
        boot_format.bits_per_sample == 16 && boot_audio_samples > 0 &&
        boot_audio_frames == 1737280 &&
        boot_audio_status == MovieAudioStatus::EndOfStream &&
        silent.ok() && silent.movie.audio_stream_count == 0 &&
        silent_decoder_open && !silent_decoder->HasAudio();
    bf6_close(context);
    return pass ? 0 : 1;
}
