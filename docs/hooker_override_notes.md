# rpg2000_ogg_hooker override 知見メモ

このメモは、`rpg2000_ogg_hooker` が RPG Maker 2000 / RPGツクール2000 用の
正規 `RPG2000.exe` editor に対して、どの hook を、何の根拠で、どの程度安全側に倒して
入れているかを追うための調査記録です。

対象は `RPG2000_ogg_hooker.exe` と同じフォルダに置かれた RPG Maker 2000 用
`RPG2000.exe` です。RPG Maker 2003 や runtime の `RPG_RT.exe` は対象外です。

## 全体方針

`RPG2000.exe` 自体は disk 上で変更しません。`RPG2000_ogg_hooker.exe` が
`RPG2000.exe` を suspended 起動し、`rpg2k_ogg_hook.dll` を注入してから再開します。

hook の種類は Win32 file API の import slot patch だけです。
RPG2000.exe 内部関数への inline hook は使いません。

通常起動では log file と dialog を出しません。調査が必要な場合だけ、
`--diagnostic` / `--diag` / `/diagnostic` / `/diag` を付けて起動します。

## 音声 OGG overlay の目的

RPG2000 editor の素材選択 UI は `.wav` / `.mp3` / `.mid` を前提としており、
`.ogg` 拡張子を素材として認識しません。

この hooker は、同名 `.wav` が無い場合にだけ `.ogg` を `.wav` 名として editor に見せます。
sample play でその `.wav` が読み込まれた時、一時 WAV へ decode した file に差し替えます。

現在の基本ルール:

- `Music\foo.wav` または `Sound\foo.wav` が存在しない場合だけ、同名 `.ogg` を `.wav` として列挙する。
- 既存の `.wav` / `.mp3` / `.mid` がある場合は元 file を優先する。
- `.mid` 要求は横取りしない。
- `.mid` だけがある素材は RPG2000 本来の MIDI 処理へ任せる。
- OGG は `.wav` 名としてだけ見せる。`.mp3` や `.mid` としては見せない。

## Win32 file API hook

RPG2000.exe は Delphi 5 製の 32-bit ANSI API application です。
import slot を直接 patch します。

hook 対象:

- `CreateFileA`
- `GetFileAttributesA`
- `FindFirstFileA`
- `FindNextFileA`
- `FindClose`

直接 patch している IAT slot:

- `FindFirstFileA`: `0x001481E8`, `0x001483B8`
- `FindNextFileA`: `0x001483B4`
- `FindClose`: `0x001481EC`, `0x001483BC`
- `GetFileAttributesA`: `0x00148384`
- `CreateFileA`: `0x0014821C`, `0x001483E4`

この RPG2000.exe は `OriginalFirstThunk=0` で、loader 後の `FirstThunk` は解決済み関数
pointer になります。そのため、関数名ベースの generic import walk では対象を見つけられませんでした。
現在は `KnownPatchSlot[]` だけを使います。

## FindFirstFileA overlay

素材一覧では `Music\*` / `Sound\*` のような列挙要求が来ます。

hooker は本物の `FindFirstFileA` / `FindNextFileA` 結果に加えて、同じ folder の `.ogg` を
`.wav` 名に変換した fake result を足します。

例:

```text
Music\foo.ogg
-> cFileName = "foo.wav"
```

ただし `foo.wav` / `foo.mp3` / `foo.mid` のいずれかが実在する場合は、fake result を足しません。
既存素材の優先を崩さないためです。

## CreateFileA redirect

sample play などで editor が `Music\foo.wav` / `Sound\foo.wav` を開こうとした時、
同名 `.ogg` が存在すれば一時 WAV を作り、その path へ `CreateFileA` を redirect します。

一時 WAV cache:

```text
%TEMP%\rpg2000_ogg_hooker\<pid>\Music\...
%TEMP%\rpg2000_ogg_hooker\<pid>\Sound\...
```

同一 OGG へ同時アクセスが来た場合は、file 単位の lock で同じ変換完了を待ちます。
cache 合計は 512MB を上限に、超過時は古い WAV から削除します。
正常終了時には当該 pid の cache を削除します。
異常終了で残った古い cache は、次回起動時に掃除します。

## decoder

主経路は Media Foundation です。
Media Foundation で OGG を読めない環境では、同じ folder または PATH 上の `ffmpeg.exe` が
使える場合に fallback します。

この project は `ffmpeg.exe` を同梱しません。配布者が別途同梱する場合は、そのライセンスを
別に確認してください。

## 起動と診断

通常起動:

```powershell
RPG2000_ogg_hooker.exe
```

診断起動:

```powershell
RPG2000_ogg_hooker.exe --diagnostic
```

診断引数は launcher が消費し、`RPG2000.exe` には渡しません。
DLL 側には環境変数 `RPG2000_OGG_HOOKER_DIAGNOSTIC=1` を渡し、同じ条件で log を有効化します。

診断時に見る主な log:

- `launcher start base_dir=...`
- `created RPG2000.exe pid=...`
- `roh_hook direct_patch_done count=...`
- `roh_hook installed session_temp=...`
- `audio_trace api=... path=...`
- `audio requested_wav=... source_ogg=... temp_wav=...`
- `audio_decode_failed ...`
- `RPG2000.exe exited code=...`

## 残しておく注意点

- direct IAT slot は特定の RPG2000.exe build を前提にしている。
- 別 build に対応する場合は、再解析を行い import slot を再確認する。
- `.mid` 要求を OGG に redirect しない。MID-only 素材は本来の MIDI 処理へ任せる。
- path 解決では `Music` / `Sound` より後ろの subfolder tail を保持する。
- 既存 WAV/MP3/MID がある場合は OGG overlay しない。元 file 優先を崩さない。
