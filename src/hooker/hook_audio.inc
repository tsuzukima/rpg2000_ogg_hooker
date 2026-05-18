// hook_dll.cpp の匿名 namespace に include される audio overlay 部分です。
// 独立した cpp ではないため、g_real_* などの状態を共有します。
// OGG を .wav 名として editor に見せ、読み出し時は一時 WAV に差し替えます。
// MID 要求は横取りせず、RPG2000 本来の処理に任せます。既存素材は常に元 file 優先です。
//
// rpg_rt_container 0.1.0 の hook_audio_temp.inc から、game-root-relative 系と
// internal Delphi 関数 hook 系を削った editor 向け slim 版です。

bool HasWildcard(const std::wstring& path) {
  return path.find_first_of(L"*?") != std::wstring::npos;
}

struct AudioOverlayRequestParts {
  std::wstring folder;
  std::filesystem::path relative_tail;
};

bool TryGetAudioOverlayRequestParts(const std::wstring& path,
                                    AudioOverlayRequestParts* out_parts) {
  // Music / Sound の直下だけでなく、Sound\marimba\foo のようなサブフォルダも
  // 同じ audio resource として扱います。
  std::filesystem::path request(path);
  bool found_resource = false;
  AudioOverlayRequestParts parts;
  for (const auto& part : request) {
    const std::wstring lower = Lower(part.wstring());
    if (!found_resource && lower == L"music") {
      parts.folder = L"Music";
      found_resource = true;
      continue;
    }
    if (!found_resource && lower == L"sound") {
      parts.folder = L"Sound";
      found_resource = true;
      continue;
    }
    if (found_resource) {
      if (part == L"." || part == L"..") {
        return false;
      }
      parts.relative_tail /= part;
    }
  }
  if (!found_resource || parts.relative_tail.empty()) {
    return false;
  }
  if (out_parts) {
    *out_parts = parts;
  }
  return true;
}

bool IsAudioOverlayPath(const std::wstring& path) {
  std::filesystem::path p(path);
  const std::wstring extension = Lower(p.extension().wstring());
  if (!extension.empty() && extension != L".wav" &&
      extension.find_first_of(L"*?") == std::wstring::npos) {
    return false;
  }
  return TryGetAudioOverlayRequestParts(path, nullptr);
}

bool IsBareAudioOverlayPath(const std::wstring& path) {
  std::filesystem::path p(path);
  if (p.has_parent_path() || p.is_absolute()) {
    return false;
  }
  const std::wstring extension = Lower(p.extension().wstring());
  return extension.empty() || extension == L".wav" ||
         extension.find_first_of(L"*?") != std::wstring::npos;
}

std::wstring AudioPathFromResourceComponent(const std::wstring& request_path) {
  // editor 側から "..\Music\foo.wav" や "../Sound/hoge" のように渡ることがあります。
  // Windows の現在 directory 基準で解決すると project 外へ出ることがあるため、
  // path 中の Music / Sound component を見つけた場合は editor の起動 folder 直下へ寄せ直します。
  std::filesystem::path request(request_path);
  std::filesystem::path relative_tail;
  const wchar_t* resource_folder = nullptr;
  bool found_resource = false;
  for (const auto& part : request) {
    const std::wstring lower = Lower(part.wstring());
    if (!found_resource && lower == L"music") {
      resource_folder = L"Music";
      found_resource = true;
      continue;
    }
    if (!found_resource && lower == L"sound") {
      resource_folder = L"Sound";
      found_resource = true;
      continue;
    }
    if (found_resource) {
      if (part == L"." || part == L"..") {
        return L"";
      }
      relative_tail /= part;
    }
  }
  if (!found_resource || relative_tail.empty()) {
    return L"";
  }
  return (std::filesystem::path(roh::ExecutableDirectory()) / resource_folder / relative_tail).wstring();
}

bool LooksLikeAudioPath(const std::wstring& path) {
  std::wstring lower = Lower(path);
  return lower.rfind(L"music\\", 0) == 0 || lower.rfind(L"music/", 0) == 0 ||
         lower.find(L"\\music\\") != std::wstring::npos ||
         lower.find(L"/music/") != std::wstring::npos ||
         lower.rfind(L"sound\\", 0) == 0 || lower.rfind(L"sound/", 0) == 0 ||
         lower.find(L"\\sound\\") != std::wstring::npos ||
         lower.find(L"/sound/") != std::wstring::npos;
}

void TraceAudioPath(const wchar_t* api, const std::wstring& path, const std::wstring& detail) {
  if (!LooksLikeAudioPath(path)) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_audio_trace_mutex);
  if (g_audio_trace_count >= 400) {
    return;
  }
  ++g_audio_trace_count;
  roh::Log(L"audio_trace api=" + std::wstring(api) + L" path=\"" + path + L"\" " + detail);
}

bool RealFileExists(const std::wstring& path) {
  // hook 済みの GetFileAttributesW を直接呼ぶと、この hook 自身へ戻ることがあります。
  // 本物 API を保存済みならそれを使い、未初期化時だけ素の API を使います。
  auto fn = g_real_get_file_attributes_w ? g_real_get_file_attributes_w : GetFileAttributesW;
  DWORD attrs = fn(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool RealAudioSourceExists(const std::wstring& request_path) {
  std::filesystem::path request(request_path);
  if (!request.extension().empty()) {
    return RealFileExists(request.wstring());
  }
  for (const wchar_t* existing_extension : {L".wav", L".mp3", L".mid"}) {
    std::filesystem::path existing(request_path);
    existing += existing_extension;
    if (RealFileExists(existing.wstring())) {
      return true;
    }
  }
  return false;
}

std::wstring OggPathForAudioRequest(const std::wstring& full_request) {
  // 1 件の要求 path に対応する OGG があるかを調べます。
  // ここでは曖昧検索はせず、foo.wav -> foo.ogg の exact mapping だけを扱います。
  if (!IsAudioOverlayPath(full_request)) {
    return L"";
  }
  if (RealAudioSourceExists(full_request)) {
    return L"";
  }
  std::filesystem::path ogg(full_request);
  if (ogg.extension().empty()) {
    // editor 側で素材名 (拡張子無し) を渡してくる経路があるため、
    // 既存 WAV/MP3/MID がある場合は互換性優先で OGG overlay しません。
    ogg += L".ogg";
  } else {
    ogg.replace_extension(L".ogg");
  }
  if (!RealFileExists(ogg.wstring())) {
    return L"";
  }
  return ogg.wstring();
}

std::wstring FolderPathForBareAudioRequest(const std::wstring& request_path,
                                           const wchar_t* folder_name) {
  if (!IsBareAudioOverlayPath(request_path)) {
    return L"";
  }
  std::filesystem::path audio = std::filesystem::path(roh::ExecutableDirectory()) / folder_name;
  audio /= std::filesystem::path(request_path).filename();
  return audio.wstring();
}

std::wstring MusicPathForBareAudioRequest(const std::wstring& request_path) {
  return FolderPathForBareAudioRequest(request_path, L"Music");
}

std::wstring SoundPathForBareAudioRequest(const std::wstring& request_path) {
  return FolderPathForBareAudioRequest(request_path, L"Sound");
}

std::wstring OggPathForAudioRequestWithFallback(const std::wstring& request_path,
                                                const std::wstring& full_request,
                                                std::wstring* resolved_request,
                                                bool allow_sound_bare_fallback = true) {
  // まず editor が要求した path をそのまま試します。
  // それが Music\foo.wav / Sound\foo.wav 形式でなければ、
  // 裸名 "foo" を Music\foo、必要なら Sound\foo として試します。
  std::wstring ogg = OggPathForAudioRequest(full_request);
  if (!ogg.empty()) {
    if (resolved_request) {
      *resolved_request = full_request;
    }
    return ogg;
  }

  const std::wstring resource_request = AudioPathFromResourceComponent(request_path);
  if (!resource_request.empty() && resource_request != full_request) {
    ogg = OggPathForAudioRequest(resource_request);
    if (!ogg.empty()) {
      if (resolved_request) {
        *resolved_request = resource_request;
      }
      roh::Log(L"audio resource_path_fallback requested=\"" + request_path + L"\" resolved=\"" +
               resource_request + L"\"");
      return ogg;
    }
  }

  const std::wstring music_request = MusicPathForBareAudioRequest(request_path);
  if (!music_request.empty()) {
    ogg = OggPathForAudioRequest(music_request);
    if (!ogg.empty()) {
      if (resolved_request) {
        *resolved_request = music_request;
      }
      roh::Log(L"audio bare_name_fallback requested=\"" + request_path + L"\" resolved=\"" +
               music_request + L"\"");
      return ogg;
    }
    if (!allow_sound_bare_fallback) {
      return L"";
    }
    if (RealAudioSourceExists(music_request)) {
      return L"";
    }
  }
  if (!allow_sound_bare_fallback) {
    return L"";
  }

  const std::wstring sound_request = SoundPathForBareAudioRequest(request_path);
  if (sound_request.empty()) {
    return L"";
  }
  ogg = OggPathForAudioRequest(sound_request);
  if (!ogg.empty()) {
    if (resolved_request) {
      *resolved_request = sound_request;
    }
    roh::Log(L"audio bare_name_fallback requested=\"" + request_path + L"\" resolved=\"" +
             sound_request + L"\"");
  }
  return ogg;
}

std::wstring TempPathForAudioRequest(const std::wstring& full_request) {
  std::filesystem::path source(full_request);
  if (source.extension().empty()) {
    source += L".wav";
  } else {
    source.replace_extension(L".wav");
  }
  AudioOverlayRequestParts parts;
  const bool has_audio_parts = TryGetAudioOverlayRequestParts(source.wstring(), &parts);
  const std::wstring folder = has_audio_parts ? parts.folder : L"Music";
  std::filesystem::path out(g_session_root);
  out /= folder;
  out /= has_audio_parts ? parts.relative_tail : source.filename();
  return out.wstring();
}

void LogAudioDecision(const std::wstring& requested_wav, const std::wstring& source_ogg,
                      const std::wstring& temp_wav, unsigned long decode_ms,
                      unsigned long long bytes, bool cache_hit) {
  roh::Log(L"audio requested_wav=\"" + requested_wav + L"\" source_ogg=\"" + source_ogg +
           L"\" temp_wav=\"" + temp_wav + L"\" decode_ms=" + std::to_wstring(decode_ms) +
           L" bytes=" + std::to_wstring(bytes) + L" cache_hit=" +
           (cache_hit ? L"true" : L"false"));
}

std::shared_ptr<std::mutex> DecodeLockFor(const std::wstring& source_ogg) {
  std::lock_guard<std::mutex> guard(g_decode_map_mutex);
  auto& lock = g_decode_locks[source_ogg];
  if (!lock) {
    lock = std::make_shared<std::mutex>();
  }
  return lock;
}

void EnforceLruLimit() {
  // 変換済み WAV が 512MB を超えた場合、古い file から削除します。
  // 要求直後の file は更新時刻が新しいため、通常は残ります。
  constexpr std::uint64_t kMaxTempCacheBytes = 512ull * 1024ull * 1024ull;
  const std::uint64_t max_bytes = kMaxTempCacheBytes;
  if (max_bytes == 0 || g_session_root.empty()) {
    return;
  }

  struct Entry {
    std::filesystem::path path;
    std::filesystem::file_time_type time;
    std::uint64_t size = 0;
  };
  std::vector<Entry> files;
  std::uint64_t total = 0;
  std::error_code ec;
  if (!std::filesystem::exists(g_session_root, ec)) {
    return;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(g_session_root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || Lower(entry.path().extension().wstring()) != L".wav") {
      continue;
    }
    Entry item;
    item.path = entry.path();
    item.time = entry.last_write_time(ec);
    item.size = entry.file_size(ec);
    files.push_back(item);
    total += item.size;
  }
  if (total <= max_bytes) {
    return;
  }
  std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) {
    return a.time < b.time;
  });
  for (const auto& file : files) {
    if (total <= max_bytes) {
      break;
    }
    if (DeleteFileW(file.path.wstring().c_str())) {
      total -= file.size;
      roh::Log(L"audio_lru_delete temp_wav=\"" + file.path.wstring() + L"\"");
    }
  }
}

bool EnsureTempWav(const std::wstring& requested_wav, const std::wstring& source_ogg,
                   std::wstring* out_temp_wav) {
  // requested_wav は editor が読むつもりだった WAV path です。
  // source_ogg は代わりに読む OGG path、out_temp_wav は editor へ返す一時 WAV path です。
  const std::wstring temp_wav = TempPathForAudioRequest(requested_wav);
  if (out_temp_wav) {
    *out_temp_wav = temp_wav;
  }
  if (RealFileExists(temp_wav)) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    unsigned long long bytes = 0;
    if (GetFileAttributesExW(temp_wav.c_str(), GetFileExInfoStandard, &data)) {
      bytes = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }
    LogAudioDecision(requested_wav, source_ogg, temp_wav, 0, bytes, true);
    return true;
  }

  auto lock = DecodeLockFor(source_ogg);
  std::lock_guard<std::mutex> guard(*lock);
  if (RealFileExists(temp_wav)) {
    LogAudioDecision(requested_wav, source_ogg, temp_wav, 0, 0, true);
    return true;
  }

  auto result = roh::DecodeOggToWav(source_ogg, temp_wav);
  if (!result.ok) {
    roh::Log(L"audio_decode_failed requested_wav=\"" + requested_wav + L"\" source_ogg=\"" +
             source_ogg + L"\" temp_wav=\"" + temp_wav + L"\" decode_ms=" +
             std::to_wstring(result.decode_ms) + L" error=" + std::to_wstring(result.error) +
             L" message=\"" + result.message + L"\"");
    return false;
  }
  LogAudioDecision(requested_wav, source_ogg, temp_wav, result.decode_ms, result.bytes, false);
  EnforceLruLimit();
  return true;
}

bool TryRedirectAudioRequest(const std::wstring& path, std::wstring* out_temp_wav) {
  // W 系 API 用の入口です。必要な場合だけ一時 WAV を作り、path を返します。
  const std::wstring full = roh::GetFullPath(path);
  std::wstring resolved_request;
  const std::wstring ogg = OggPathForAudioRequestWithFallback(path, full, &resolved_request);
  if (ogg.empty()) {
    return false;
  }
  return EnsureTempWav(resolved_request, ogg, out_temp_wav);
}

bool TryRedirectAudioRequestA(LPCSTR path, std::wstring* out_temp_wav,
                              std::string* out_temp_ansi) {
  // A 系 API 用の入口です。editor 由来の CP932/ANSI path を wide に直してから処理します。
  if (!path) {
    return false;
  }
  std::wstring temp_wav;
  if (!TryRedirectAudioRequest(roh::ToWideAnsiPath(path), &temp_wav)) {
    return false;
  }
  if (out_temp_wav) {
    *out_temp_wav = temp_wav;
  }
  if (out_temp_ansi) {
    *out_temp_ansi = roh::ToAnsiPath(temp_wav);
    if (out_temp_ansi->empty()) {
      return false;
    }
  }
  return true;
}

void CopyFindDataWToA(const WIN32_FIND_DATAW& in, WIN32_FIND_DATAA* out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  out->dwFileAttributes = in.dwFileAttributes;
  out->ftCreationTime = in.ftCreationTime;
  out->ftLastAccessTime = in.ftLastAccessTime;
  out->ftLastWriteTime = in.ftLastWriteTime;
  out->nFileSizeHigh = in.nFileSizeHigh;
  out->nFileSizeLow = in.nFileSizeLow;
  out->dwReserved0 = in.dwReserved0;
  out->dwReserved1 = in.dwReserved1;
  WideCharToMultiByte(CP_ACP, 0, in.cFileName, -1, out->cFileName, MAX_PATH, nullptr, nullptr);
  WideCharToMultiByte(CP_ACP, 0, in.cAlternateFileName, -1, out->cAlternateFileName, 14, nullptr, nullptr);
}

WIN32_FIND_DATAW MakeOverlayFindData(const std::filesystem::path& wav_path,
                                     const std::filesystem::path& ogg_path) {
  WIN32_FIND_DATAW data{};
  auto fn = g_real_find_first_file_w ? g_real_find_first_file_w : FindFirstFileW;
  WIN32_FIND_DATAW ogg_data{};
  HANDLE h = fn(ogg_path.wstring().c_str(), &ogg_data);
  if (h != INVALID_HANDLE_VALUE) {
    data = ogg_data;
    if (g_real_find_close) {
      g_real_find_close(h);
    } else {
      FindClose(h);
    }
  } else {
    data.dwFileAttributes = FILE_ATTRIBUTE_ARCHIVE;
  }
  wcsncpy_s(data.cFileName, wav_path.filename().wstring().c_str(), _TRUNCATE);
  data.cAlternateFileName[0] = L'\0';
  return data;
}

bool AppendFindResultsForAudioPattern(const std::wstring& full_pattern,
                                      std::vector<WIN32_FIND_DATAW>* results) {
  // editor 側の素材管理 dialog は通常 `Music\*.wav` ではなく `Music\*.*` や `Music\*` を
  // 渡してくる (Delphi VCL の `FindFirst` は default mask が `*.*` のため)。
  // この場合、実 enumeration の結果には `.ogg` が `.ogg` のまま含まれ、editor の Pascal 側
  // filter (`.wav` / `.mp3` / `.mid` だけ表示) で削除されてしまいます。
  // ここでは pattern が `.ogg` にも一致する形のときは、実 enumeration の `.ogg` を
  // `.wav` 名へ rename しながら results に入れます。
  if (!results || !IsAudioOverlayPath(full_pattern)) {
    return false;
  }
  const size_t original_count = results->size();

  const bool wildcard = HasWildcard(full_pattern);
  auto real_find_first = g_real_find_first_file_w ? g_real_find_first_file_w : FindFirstFileW;
  auto real_find_next = g_real_find_next_file_w ? g_real_find_next_file_w : FindNextFileW;
  auto real_find_close = g_real_find_close ? g_real_find_close : FindClose;

  std::filesystem::path wav_pattern(full_pattern);
  const std::wstring pattern_extension = Lower(wav_pattern.extension().wstring());

  // 「pattern が .ogg にも一致するか」を判定。例: `*.*` / `*` / 拡張子無し / `*.o*` / `*.???`
  // `*.wav` のように具体拡張子に固定されている時は Pass 1 で .ogg が出てこないので、
  // Pass 2 (`*.ogg` を別 enum) に任せます。
  const bool pattern_matches_ogg =
      pattern_extension.empty() ||
      pattern_extension == L".*" ||
      pattern_extension == L".ogg" ||
      pattern_extension.find(L'?') != std::wstring::npos ||
      (pattern_extension.size() >= 2 && pattern_extension[1] == L'*');

  // 表示名 (lower case) の重複防止。実 .wav と overlay 化した .wav の二重登録を避けます。
  std::set<std::wstring> result_names_lower;

  // Pass 1: 通常の find_first(pattern)。
  WIN32_FIND_DATAW data{};
  HANDLE real = real_find_first(full_pattern.c_str(), &data);
  if (real != INVALID_HANDLE_VALUE) {
    do {
      WIN32_FIND_DATAW item = data;
      const std::wstring file_ext =
          Lower(std::filesystem::path(item.cFileName).extension().wstring());
      if (pattern_matches_ogg && file_ext == L".ogg") {
        // .ogg を .wav 名で見せる。editor の拡張子 filter を通過させるための renaming。
        std::filesystem::path renamed(item.cFileName);
        renamed.replace_extension(L".wav");
        wcsncpy_s(item.cFileName, renamed.filename().wstring().c_str(), _TRUNCATE);
      }
      const std::wstring lower_name = Lower(item.cFileName);
      if (result_names_lower.count(lower_name) != 0) {
        continue;
      }
      results->push_back(item);
      result_names_lower.insert(lower_name);
    } while (real_find_next(real, &data));
    real_find_close(real);
  }

  // Pass 2: 同 folder の *.ogg を別 enum。
  // Pass 1 が `*.wav` のような OGG 非一致 pattern の時はここで .ogg を拾います。
  std::filesystem::path ogg_pattern = wav_pattern;
  if (ogg_pattern.extension().empty()) {
    ogg_pattern += L".ogg";
  } else {
    ogg_pattern.replace_extension(L".ogg");
  }
  WIN32_FIND_DATAW ogg_data{};
  HANDLE ogg_find = real_find_first(ogg_pattern.wstring().c_str(), &ogg_data);
  if (ogg_find != INVALID_HANDLE_VALUE) {
    do {
      std::filesystem::path ogg_file = wav_pattern.parent_path() / ogg_data.cFileName;
      std::filesystem::path wav_file = ogg_file;
      wav_file.replace_extension(L".wav");
      const std::wstring lower_name = Lower(wav_file.filename().wstring());
      if (result_names_lower.count(lower_name) != 0) {
        continue;
      }
      if (RealFileExists(wav_file.wstring())) {
        continue;
      }
      results->push_back(MakeOverlayFindData(wav_file, ogg_file));
      result_names_lower.insert(lower_name);
    } while (wildcard && real_find_next(ogg_find, &ogg_data));
    real_find_close(ogg_find);
  }

  return results->size() > original_count;
}

bool BuildFindResults(const std::wstring& pattern, std::vector<WIN32_FIND_DATAW>* results) {
  if (!results) {
    return false;
  }
  results->clear();

  const std::wstring full_pattern = roh::GetFullPath(pattern);
  if (AppendFindResultsForAudioPattern(full_pattern, results)) {
    return true;
  }
  if (IsAudioOverlayPath(full_pattern)) {
    return false;
  }

  const std::wstring resource_pattern = AudioPathFromResourceComponent(pattern);
  if (!resource_pattern.empty() && AppendFindResultsForAudioPattern(resource_pattern, results)) {
    roh::Log(L"audio find resource_path_fallback requested=\"" + pattern + L"\" resolved=\"" +
             resource_pattern + L"\"");
    return true;
  }

  for (const std::wstring candidate :
       {MusicPathForBareAudioRequest(pattern),
        SoundPathForBareAudioRequest(pattern)}) {
    if (candidate.empty()) {
      continue;
    }
    const size_t before = results->size();
    if (AppendFindResultsForAudioPattern(candidate, results)) {
      roh::Log(L"audio find bare_name_fallback requested=\"" + pattern + L"\" resolved=\"" +
               candidate + L"\"");
    }
    if (!HasWildcard(pattern) && results->size() > before) {
      break;
    }
  }
  return !results->empty();
}
