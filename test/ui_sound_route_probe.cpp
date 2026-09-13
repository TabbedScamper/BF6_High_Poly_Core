#include "bf6_core.h"
#include "boot_flow.h"
#include "installed_sound_wave.h"
#include "movie_texture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kStylePlayers =
    "common/sound/ui/cl/styleplayers/bf03_ui_cl_styleplayers_main_prefab_01";
constexpr const char* kUmMainStyle =
    "common/sound/ui/cl/style/um/main/"
    "bf03_ui_cl_style_um_main_main_soundstyle_01";
constexpr const char* kUmMainActivation =
    "common/sound/ui/cl/style/um/main/"
    "bf03_ui_cl_style_um_main_activation_event_01";
constexpr const char* kDefaultPrimaryConfig =
    "common/sound/ui/menunavigation/default/"
    "bf03_ui_menunavigation_default_primaryactivation_config_01";
constexpr const char* kDefaultPrimaryWave =
    "common/sound/ui/menunavigation/default/"
    "bf03_ui_menunavigation_default_primaryactivation_wave_01";
constexpr const char* kDefaultBackConfig =
    "common/sound/ui/menunavigation/default/"
    "bf03_ui_menunavigation_default_goback_config_01";
constexpr const char* kDefaultFocusConfig =
    "common/sound/ui/menunavigation/default/"
    "bf03_ui_menunavigation_default_focus_config_01";
constexpr const char* kFake =
    "common/sound/ui/__control__/not_a_real_sound_asset";

bool has_asset(bf6_ctx* context, const char* name, bool resource = false)
{
    const int count = resource ? bf6_list_res(context, name, nullptr, 0)
                               : bf6_list_ebx(context, name, nullptr, 0);
    if (count != 1) return false;
    bf6_asset row{};
    const int got = resource ? bf6_list_res(context, name, &row, 1)
                             : bf6_list_ebx(context, name, &row, 1);
    return got == 1 && row.name && std::strcmp(row.name, name) == 0;
}

void print_imports(bf6_ctx* context, const std::string& asset,
                   uint32_t field_hash)
{
    constexpr int kStride = 512;
    const int count = bf6_ebx_imports_by_field(
        context, asset.c_str(), field_hash, nullptr, kStride, 0);
    std::vector<char> rows(static_cast<size_t>((std::max)(count, 0)) *
                           kStride, 0);
    const int got = count > 0 ? bf6_ebx_imports_by_field(
        context, asset.c_str(), field_hash, rows.data(), kStride, count) : 0;
    std::printf("imports asset=%s field=0x%08x count=%d\n",
                asset.c_str(), field_hash, got);
    for (int i = 0; i < got; ++i)
        std::printf("  %s\n", rows.data() + static_cast<size_t>(i) * kStride);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3 ||
        (argc == 3 && std::strcmp(argv[2], "--play") != 0)) {
        std::fprintf(stderr,
            "usage: ui_sound_route_probe <Steam BF6 game dir> [--play]\n");
        return 2;
    }
    const bool play = argc == 3;
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, static_cast<int>(sizeof(error)));
    if (!context) { std::fprintf(stderr, "open: %s\n", error); return 1; }
    if (!bf6_mount_all(context, 0, error, static_cast<int>(sizeof(error)))) {
        std::fprintf(stderr, "mount: %s\n", error);
        bf6_close(context);
        return 1;
    }

    const bool exact_style = has_asset(context, kUmMainStyle);
    const bool exact_event = has_asset(context, kUmMainActivation);
    const bool exact_players = has_asset(context, kStylePlayers);
    const bool exact_config = has_asset(context, kDefaultPrimaryConfig);
    const bool exact_wave_ebx = has_asset(context, kDefaultPrimaryWave);
    const bool exact_wave_res = has_asset(context, kDefaultPrimaryWave, true);
    const bool fake_ebx = has_asset(context, kFake);
    const bool fake_res = has_asset(context, kFake, true);
    bf6_asset wave_res_row{};
    const int wave_res_rows = bf6_list_res(
        context, kDefaultPrimaryWave, &wave_res_row, 1);
    const int direct_res_chunks = bf6_res_chunks(
        context, kDefaultPrimaryWave, nullptr, 0, nullptr);

    const int event_count =
        bf6_rime_event_connections(context, kStylePlayers, nullptr, 0);
    std::vector<bf6_rime_event_connection> events(
        static_cast<size_t>(std::max(event_count, 0)));
    const int event_got = event_count > 0
        ? bf6_rime_event_connections(context, kStylePlayers,
                                     events.data(), event_count)
        : event_count;
    const int property_count =
        bf6_rime_connections(context, kStylePlayers, nullptr, 0);

    // NameHash read from the current UM Main Activation event record.  It is
    // deliberately not treated as a schematic EventSpec id unless it occurs
    // in the authored style-player connection table.
    constexpr uint32_t kUmMainActivationNameHash = 0x4EBCC102u;
    int name_hash_edges = 0;
    int xor_name_hash_edges = 0;
    for (const auto& edge : events) {
        if (edge.source_event == kUmMainActivationNameHash ||
            edge.target_event == kUmMainActivationNameHash)
            ++name_hash_edges;
        if (edge.source_event == (kUmMainActivationNameHash ^ 1u) ||
            edge.target_event == (kUmMainActivationNameHash ^ 1u))
            ++xor_name_hash_edges;
    }

    std::printf(
        "assets style/event/players/config/wave-ebx/wave-res=%d/%d/%d/%d/%d/%d "
        "fake-ebx/res=%d/%d\n",
        exact_style, exact_event, exact_players, exact_config,
        exact_wave_ebx, exact_wave_res, fake_ebx, fake_res);
    std::printf("wave-res rows/type/bytes/direct-chunks=%d/0x%08x/%u/%d\n",
                wave_res_rows, wave_res_row.type, wave_res_row.size,
                direct_res_chunks);
    std::printf(
        "styleplayers event/property=%d/%d event-got=%d "
        "event-namehash-edges=%d xor-control=%d\n",
        event_count, property_count, event_got,
        name_hash_edges, xor_name_hash_edges);

    bf6::ui::boot::DirectInstallSource boot_source(context);
    bf6::ui::boot::Runtime boot_runtime;
    std::string boot_error;
    const bool boot_loaded = boot_runtime.load(boot_source, &boot_error);
    int authored_boot_movies = 0;
    int readable_boot_movies = 0;
    int movies_declaring_audio = 0;
    int withaudio_streams = -1;
    int startscreen_streams = -1;
    if (boot_loaded) {
        for (const auto& surface : boot_runtime.surfaces()) {
            for (const auto& binding : surface.media) {
                if (binding.source !=
                        bf6::ui::boot::MediaSource::AuthoredMovieAsset ||
                    binding.movie_asset.empty())
                    continue;
                ++authored_boot_movies;
                bf6::ui_runtime::MovieSourceRequest request;
                request.authored_asset = binding.movie_asset;
                const auto movie =
                    bf6::ui_runtime::ReadMovieTexture2(context, request);
                if (!movie.ok()) continue;
                ++readable_boot_movies;
                movies_declaring_audio += movie.movie.audio_stream_count > 0;
                if (binding.movie_asset.find("withaudio") != std::string::npos)
                    withaudio_streams =
                        static_cast<int>(movie.movie.audio_stream_count);
                if (binding.movie_asset.find("startscreenloop") != std::string::npos)
                    startscreen_streams =
                        static_cast<int>(movie.movie.audio_stream_count);
                std::printf("boot-movie asset=%s streams=%u/%u/%u bytes=%zu\n",
                            binding.movie_asset.c_str(),
                            movie.movie.video_stream_count,
                            movie.movie.audio_stream_count,
                            movie.movie.subtitle_stream_count,
                            movie.movie.webm.size());
            }
        }
    }
    bf6::ui_runtime::MovieSourceRequest fake_movie_request;
    fake_movie_request.authored_asset = kFake;
    const auto fake_movie =
        bf6::ui_runtime::ReadMovieTexture2(context, fake_movie_request);
    std::printf(
        "boot loaded=%d authored/readable/with-audio=%d/%d/%d "
        "logo-audio=%d startscreen-audio=%d fake-movie=%s%s%s\n",
        boot_loaded, authored_boot_movies, readable_boot_movies,
        movies_declaring_audio, withaudio_streams, startscreen_streams,
        bf6::ui_runtime::MovieReadStatusName(fake_movie.status),
        boot_error.empty() ? "" : " error=", boot_error.c_str());

    bf6::ui_runtime::InstalledSoundConfigRequest sound_request;
    sound_request.config_asset = kDefaultPrimaryConfig;
    const auto decoded_sound =
        bf6::ui_runtime::ReadInstalledSoundConfig(context, sound_request);
    bf6::ui_runtime::InstalledSoundConfigRequest fake_sound_request;
    fake_sound_request.config_asset = kFake;
    const auto fake_sound =
        bf6::ui_runtime::ReadInstalledSoundConfig(context, fake_sound_request);
    auto range_request = sound_request;
    range_request.variation_index = UINT32_MAX;
    const auto range_control =
        bf6::ui_runtime::ReadInstalledSoundConfig(context, range_request);
    auto secondary_request = sound_request;
    secondary_request.config_asset = kDefaultBackConfig;
    const auto decoded_back =
        bf6::ui_runtime::ReadInstalledSoundConfig(context, secondary_request);
    secondary_request.config_asset = kDefaultFocusConfig;
    const auto decoded_focus =
        bf6::ui_runtime::ReadInstalledSoundConfig(context, secondary_request);
    std::printf(
        "decoded-sound status=%s codec=0x%02x channels=%u rate=%u "
        "frames=%u pcm=%zu chunk=%s chunk-bytes=%llu offset=%llu header=%u controls=%d/%d/%d/%d/%d/%d "
        "config-imports/xor=%d/%d fake=%s range=%s%s%s\n",
        bf6::ui_runtime::InstalledSoundStatusName(decoded_sound.status),
        decoded_sound.wave.codec, decoded_sound.wave.channels,
        decoded_sound.wave.sample_rate, decoded_sound.wave.sample_count,
        decoded_sound.wave.pcm.size(), decoded_sound.wave.chunk_guid.c_str(),
        static_cast<unsigned long long>(decoded_sound.wave.chunk_size),
        static_cast<unsigned long long>(decoded_sound.wave.sample_offset),
        decoded_sound.wave.sps_header_size,
        decoded_sound.controls.exact_resource_matches,
        decoded_sound.controls.core_datasets,
        decoded_sound.controls.chunk_guid_matches,
        decoded_sound.controls.mutated_chunk_trials,
        decoded_sound.controls.mutated_chunk_hits,
        decoded_sound.controls.shifted_sps_hits,
        decoded_sound.controls.config_wave_imports,
        decoded_sound.controls.config_xor_import_hits,
        bf6::ui_runtime::InstalledSoundStatusName(fake_sound.status),
        bf6::ui_runtime::InstalledSoundStatusName(range_control.status),
        decoded_sound.error.empty() ? "" : " error=",
        decoded_sound.error.c_str());
    std::printf(
        "secondary-sounds back=%s/%u/%zu focus=%s/%u/%zu\n",
        bf6::ui_runtime::InstalledSoundStatusName(decoded_back.status),
        decoded_back.wave.sample_count, decoded_back.wave.pcm.size(),
        bf6::ui_runtime::InstalledSoundStatusName(decoded_focus.status),
        decoded_focus.wave.sample_count, decoded_focus.wave.pcm.size());

    // The current install has twelve default menu-navigation configs. Keep
    // the runtime route keyed by those configs, not by the eleven separately
    // mounted wave EBX assets (turn-on/off resolve their authored wave through
    // the config ImportRef). A fabricated thirteenth leaf is the control.
    static constexpr std::array<const char*, 12> kDefaultLeaves = {{
        "primaryactivation", "goback", "focus", "hoverin",
        "enumselection", "slidersclickdown", "turnon", "turnoff",
        "actionfailed", "secondaryactivation", "mute", "unmute",
    }};
    int default_configs_ok = 0;
    int default_controls_ok = 0;
    uint32_t max_default_variations = 0;
    for (const char* leaf : kDefaultLeaves) {
        bf6::ui_runtime::InstalledSoundConfigRequest request;
        request.config_asset =
            std::string("common/sound/ui/menunavigation/default/") +
            "bf03_ui_menunavigation_default_" + leaf + "_config_01";
        const auto decoded =
            bf6::ui_runtime::ReadInstalledSoundConfig(context, request);
        default_configs_ok += decoded.ok();
        default_controls_ok += decoded.ok() &&
            decoded.controls.config_wave_imports > 0 &&
            decoded.controls.config_xor_import_hits == 0 &&
            decoded.controls.mutated_chunk_trials == 1 &&
            decoded.controls.mutated_chunk_hits == 0 &&
            decoded.controls.shifted_sps_hits == 0;
        max_default_variations = (std::max)(
            max_default_variations, decoded.wave.variation_count);
        std::printf(
            "default-config %-20s status=%-24s wave=%s variations=%u "
            "frames=%u controls=xor%d/haptic%d/chunk%d/shift%d\n",
            leaf,
            bf6::ui_runtime::InstalledSoundStatusName(decoded.status),
            decoded.wave.wave_asset.c_str(), decoded.wave.variation_count,
            decoded.wave.sample_count,
            decoded.controls.config_xor_import_hits,
            decoded.controls.config_haptic_imports,
            decoded.controls.mutated_chunk_hits,
            decoded.controls.shifted_sps_hits);
    }
    bf6::ui_runtime::InstalledSoundConfigRequest absent_default;
    absent_default.config_asset =
        "common/sound/ui/menunavigation/default/"
        "bf03_ui_menunavigation_default_codexcontrol_config_01";
    const auto absent_default_result =
        bf6::ui_runtime::ReadInstalledSoundConfig(context, absent_default);
    const std::string action_failed =
        "common/sound/ui/menunavigation/default/"
        "bf03_ui_menunavigation_default_actionfailed_config_01";
    print_imports(context, action_failed, 0x49D2E039u);
    print_imports(context, action_failed, 0x73E9A894u);
    const std::string secondary_config =
        "common/sound/ui/menunavigation/default/"
        "bf03_ui_menunavigation_default_secondaryactivation_config_01";
    bf6_wave_selection* secondary_selector =
        bf6_wave_selection_read(context, secondary_config.c_str());
    std::printf("secondary-selector config=%d\n",
                secondary_selector ? secondary_selector->count : -1);
    if (secondary_selector) bf6_free(context, secondary_selector);
    bf6_wave_selection* secondary_wave_selector = bf6_wave_selection_read(
        context,
        "common/sound/flow/eor/shared/"
        "bf03_flow_eor_shared_playercardtoggle_markers_01");
    std::printf("secondary-selector wave=%d\n",
                secondary_wave_selector ? secondary_wave_selector->count : -1);
    if (secondary_wave_selector && secondary_wave_selector->count > 0) {
        const bf6_wave_selector& selector =
            secondary_wave_selector->selectors[0];
        std::printf(
            "secondary-selector behavior=%d history=%u scoring=%d random=%d\n",
            selector.behavior, selector.history_entry_count,
            selector.scoring_mode, selector.randomize_candidates);
    }
    if (secondary_wave_selector) bf6_free(context, secondary_wave_selector);
    std::printf(
        "default-configs ok/controls=%d/%d max-variations=%u control=%s\n",
        default_configs_ok, default_controls_ok, max_default_variations,
        bf6::ui_runtime::InstalledSoundStatusName(
            absent_default_result.status));

    bool playback_ok = true;
    if (play) {
        bf6::ui_runtime::PcmOneShotPlayer player;
        std::string playback_error;
        uint32_t overlapping_voices = 0;
        playback_ok = decoded_sound.ok() &&
            player.Play(decoded_sound.wave, 0.0f, playback_error) &&
            decoded_focus.ok() &&
            player.Play(decoded_focus.wave, 0.0f, playback_error);
        overlapping_voices = player.ActiveVoiceCount();
        playback_ok = playback_ok && overlapping_voices == 2;
        if (playback_ok) {
            const uint64_t duration_ms =
                uint64_t((std::max)(decoded_sound.wave.sample_count,
                                    decoded_focus.wave.sample_count)) * 1000u /
                decoded_sound.wave.sample_rate;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(duration_ms + 25u));
            player.Stop();
        }
        bf6::ui_runtime::PcmStreamPlayer stream;
        bool stream_ok = stream.Open(decoded_sound.wave.channels,
                                     decoded_sound.wave.sample_rate, 0.0f,
                                     playback_error) &&
            stream.Submit(decoded_sound.wave.pcm, playback_error) &&
            stream.Start(playback_error);
        if (stream_ok) {
            const uint64_t duration_ms =
                uint64_t(decoded_sound.wave.sample_count) * 1000u /
                decoded_sound.wave.sample_rate;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(duration_ms + 25u));
            stream_ok = stream.IsDrained();
        }
        stream.Stop();
        playback_ok = playback_ok && stream_ok;
        std::printf("xaudio2-playback=%s voices=%u stream=%d%s%s\n",
            playback_ok ? "ok" : "failed",
            overlapping_voices, stream_ok ? 1 : 0,
            playback_error.empty() ? "" : " error=",
            playback_error.c_str());
    }

    const bool pass = exact_style && exact_event && exact_players &&
        exact_config && exact_wave_ebx && exact_wave_res &&
        !fake_ebx && !fake_res && event_count >= 0 &&
        wave_res_rows == 1 && wave_res_row.type == 0xB2C465F6u &&
        event_got == event_count && property_count >= 0 &&
        xor_name_hash_edges == 0 && boot_loaded &&
        authored_boot_movies > 0 &&
        readable_boot_movies == authored_boot_movies &&
        withaudio_streams > 0 && startscreen_streams == 0 &&
        fake_movie.status == bf6::ui_runtime::MovieReadStatus::AssetNotMounted &&
        decoded_sound.ok() && decoded_sound.controls.exact_resource_matches == 1 &&
        decoded_sound.controls.core_datasets == 3 &&
        decoded_sound.controls.chunk_guid_matches == 1 &&
        decoded_sound.controls.mutated_chunk_trials == 1 &&
        decoded_sound.controls.mutated_chunk_hits == 0 &&
        decoded_sound.controls.shifted_sps_hits == 0 &&
        decoded_sound.controls.config_wave_imports == 2 &&
        decoded_sound.controls.config_xor_import_hits == 0 &&
        decoded_sound.wave.wave_asset == kDefaultPrimaryWave &&
        decoded_sound.wave.codec == 0x14 &&
        decoded_sound.wave.channels == 2 &&
        decoded_sound.wave.sample_rate == 48000 &&
        decoded_sound.wave.sample_count == 4029 &&
        decoded_sound.wave.pcm.size() ==
            static_cast<size_t>(decoded_sound.wave.sample_count) *
            decoded_sound.wave.channels &&
        fake_sound.status == bf6::ui_runtime::InstalledSoundStatus::ConfigNotMounted &&
        range_control.status == bf6::ui_runtime::InstalledSoundStatus::VariationOutOfRange &&
        decoded_back.ok() && decoded_back.controls.config_xor_import_hits == 0 &&
        decoded_back.wave.pcm.size() ==
            static_cast<size_t>(decoded_back.wave.sample_count) *
            decoded_back.wave.channels &&
        decoded_focus.ok() && decoded_focus.controls.config_xor_import_hits == 0 &&
        decoded_focus.wave.pcm.size() ==
            static_cast<size_t>(decoded_focus.wave.sample_count) *
            decoded_focus.wave.channels &&
        default_configs_ok == static_cast<int>(kDefaultLeaves.size()) &&
        default_controls_ok == static_cast<int>(kDefaultLeaves.size()) &&
        max_default_variations > 0 &&
        absent_default_result.status ==
            bf6::ui_runtime::InstalledSoundStatus::ConfigNotMounted &&
        playback_ok;
    bf6_close(context);
    return pass ? 0 : 1;
}
