// EA Layer3 PCM through the pinned vgmstream decoder distributed beside the core.
// The mount and SBle segment selection stay in ReadInstalledSoundWave. Only the
// selected SPS stream goes to the codec helper, never an invented replacement.
// Source and dependency notices: addons/highpoly_toggle/bin/vgmstream/README.md.
static bool decode_installed_eal3(const std::vector<uint8_t>& chunk, size_t offset,
                                 InstalledSoundWave& wave) {
#ifdef _WIN32
    // Slice at the SPS end block. A bank chunk can contain several unrelated
    // segments; feeding all trailing bytes to a decoder loses this boundary.
    size_t end = offset;
    bool terminated = false;
    while (end + 4 <= chunk.size()) {
        const uint8_t tag = chunk[end];
        const size_t length = be32(chunk.data() + end) & 0xffffffu;
        if (length < 4 || length > chunk.size() - end ||
            (tag != 0x48 && tag != 0x44 && tag != 0x45)) return false;
        end += length;
        if (tag == 0x45) { terminated = true; break; }
    }
    if (!terminated) return false;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&decode_installed_eal3), &module)) return false;
    wchar_t location[32768]{};
    const DWORD length = GetModuleFileNameW(module, location, 32768);
    if (!length || length >= 32768) return false;
    std::wstring exe(location, length);
    exe.resize(exe.find_last_of(L"\\/") + 1);
    exe += L"vgmstream\\vgmstream-cli.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    wchar_t temp[MAX_PATH]{}, unique[MAX_PATH]{};
    const DWORD temp_length = GetTempPathW(MAX_PATH, temp);
    if (!temp_length || temp_length >= MAX_PATH || !GetTempFileNameW(temp, L"bf6", 0, unique)) return false;
    struct Files {
        std::wstring lock, sps, wav;
        ~Files() { DeleteFileW(sps.c_str()); DeleteFileW(wav.c_str()); DeleteFileW(lock.c_str()); }
    } files{unique, std::wstring(unique) + L".sps", std::wstring(unique) + L".wav"};
    HANDLE input = CreateFileW(files.sps.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (input == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool saved = WriteFile(input, chunk.data() + offset, DWORD(end - offset), &written, nullptr)
                       && written == end - offset;
    CloseHandle(input);
    if (!saved) return false;
    std::wstring command = L"\"" + exe + L"\" -i -o \"" + files.wav + L"\" \"" + files.sps + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    DWORD code = 1;
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1); // Only this owned codec helper.
        WaitForSingleObject(process.hProcess, 1000);
    } else GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code != 0) return false;
    HANDLE output = CreateFileW(files.wav.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    const uint64_t wanted = uint64_t(wave.sample_count) * wave.channels * 2;
    if (!GetFileSizeEx(output, &size) || size.QuadPart < 44 ||
        uint64_t(size.QuadPart) > wanted + 65536 || wanted > 400000000) {
        CloseHandle(output); return false;
    }
    std::vector<uint8_t> wav(size_t(size.QuadPart));
    DWORD got = 0;
    const bool loaded = ReadFile(output, wav.data(), DWORD(wav.size()), &got, nullptr) && got == wav.size();
    CloseHandle(output);
    if (!loaded || std::memcmp(wav.data(), "RIFF", 4) || std::memcmp(wav.data() + 8, "WAVE", 4)) return false;
    bool format_ok = false;
    size_t data_at = 0;
    for (size_t at = 12; at + 8 <= wav.size();) {
        const size_t bytes = le32(wav.data() + at + 4);
        if (bytes > wav.size() - at - 8) return false;
        const uint8_t* p = wav.data() + at + 8;
        if (!std::memcmp(wav.data() + at, "fmt ", 4) && bytes >= 16)
            format_ok = le16(p) == 1 && le16(p + 2) == wave.channels &&
                        le32(p + 4) == wave.sample_rate && le16(p + 14) == 16;
        if (!std::memcmp(wav.data() + at, "data", 4)) {
            if (bytes != wanted) return false;
            data_at = at + 8;
        }
        at += 8 + bytes + (bytes & 1);
    }
    if (!format_ok || !data_at) return false;
    wave.pcm.resize(size_t(wanted / 2));
    std::memcpy(wave.pcm.data(), wav.data() + data_at, size_t(wanted));
    return true;
#else
    (void)chunk; (void)offset; (void)wave;
    return false;
#endif
}
