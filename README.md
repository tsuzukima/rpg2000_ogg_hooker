# rpg2000_ogg_hooker

`rpg2000_ogg_hooker` は、RPG Maker 2000 / RPGツクール2000 の
`RPG2000.exe` editor を disk 上で書き換えずに起動し、外部 DLL の hook で
`Music` / `Sound` 配下の `.ogg` 素材を `.wav` 素材として見せる非公式ツールです。

このプロジェクトは RPG Maker 2000 / RPGツクール2000 の editor 専用です。
RPG Maker 2003、XP、VX、MV、MZ、EasyRPG Player、`RPG_RT.exe` runtime などは対象外です。

Gotcha Gotcha Games Inc. の公式 editor ではありません。

## License

この repository のソースコードは MIT License で提供されます。
RPG Maker 2000 / RPGツクール2000 と一緒に使う場合の注意は
[RPG2000_OGG_HOOKER_LICENSE.txt](RPG2000_OGG_HOOKER_LICENSE.txt) を参照してください。

## Releases

https://github.com/tsuzukima/rpg2000_ogg_hooker/releases

最新版はここから入手できます。

## 作成について

GPT-5.5 および Opus 4.7 の補助を用いて編集されています。

## できること

- 正規の `RPG2000.exe` を suspended 起動し、`rpg2k_ogg_hook.dll` を注入してから再開
- editor の BGM / SE 選択で `Music\*.ogg` / `Sound\*.ogg` を `.wav` 名として列挙
- sample play 時に OGG を一時 WAV へ変換して editor に読ませる
- 既存の `.wav` / `.mp3` / `.mid` がある場合は、互換性優先で元 file を使う
- `.mid` 要求は横取りせず、RPG2000 本来の MIDI 処理へ任せる
- 通常起動では log file と dialog を出さない
- `--diagnostic` / `--diag` 起動時だけ診断 log と fallback dialog を有効にする

## できないこと

- `RPG2000.exe` editor 以外への注入
- editor の test play で起動される `RPG_RT.exe` への対応
- 素材 import dialog や Shell namespace 経由の file 選択への対応
- `.mid` 要求を同名 `.ogg` に差し替えること

runtime 側の OGG 対応は [RPG_RT_container](../rpg_rt_container/) を使ってください。

## Build

Windows の 32-bit Visual Studio toolchain が必要です。

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release
```

生成される主な成果物:

- `build\Release\RPG2000_ogg_hooker.exe`
- `build\Release\rpg2k_ogg_hook.dll`

## RPG2000.exe のあるフォルダへの配置

正規に利用できる `RPG2000.exe` のフォルダに、次を置きます。

- `RPG2000_ogg_hooker.exe`
- `rpg2k_ogg_hook.dll`

同じフォルダに、正規の `RPG2000.exe` が存在する必要があります。

`rpg2000_ogg_hooker.ini` は不要です。設定ファイルは読みません。

## 起動

通常起動:

上記のファイル配置を行ったあとに `RPG2000_ogg_hooker.exe` を起動して下さい。

診断起動:

```powershell
RPG2000_ogg_hooker.exe --diagnostic
```

`--diagnostic` / `--diag` / `/diagnostic` / `/diag` は hooker 側で消費され、
`RPG2000.exe` には渡されません。その他の引数は `RPG2000.exe` に転送されます。

## OGG overlay

`Music\foo.wav` / `Sound\foo.wav` が無く、同名の `.ogg` がある場合だけ、
一時 WAV に変換して editor に読ませます。

既存の WAV/MP3/MID がある場合は、互換性優先で元 file を使います。
`.mid` だけがある場合は、RPG2000 本来の MIDI 再生に任せます。
`.mid` 要求を同名 `.ogg` へ解決することはありません。

一時 WAV cache:

```text
%TEMP%\rpg2000_ogg_hooker\<pid>\Music\...
%TEMP%\rpg2000_ogg_hooker\<pid>\Sound\...
```

cache 合計は 512MB を上限に、超過時は古い WAV から削除します。
正常終了時には当該 pid の cache を削除します。
異常終了で残った古い cache は、次回起動時に掃除します。

## トラブルシュート

通常起動では log file や fallback dialog は出しません。
調査したい場合だけ `--diagnostic` を付けて起動してください。

診断 log は `rpg2000_ogg_hooker.log`、または launcher folder に書けない場合は
`%TEMP%\rpg2000_ogg_hooker.log` に出ます。

よくある問題:

- `RPG2000.exe was not found next to RPG2000_ogg_hooker.exe`
  - 同じフォルダに `RPG2000.exe` を置いてください。
- `rpg2k_ogg_hook.dll injection failed`
  - 32-bit `rpg2k_ogg_hook.dll` が同じフォルダにあるか確認してください。
- OGG が一覧に出ない
  - `Music` / `Sound` 配下に同名の `.wav` / `.mp3` / `.mid` が無いか確認してください。
  - この hooker は OGG を `.wav` としてだけ見せます。

## Source Layout

- `src/hooker/launcher.cpp`
  - `RPG2000.exe` を suspended 起動し、`rpg2k_ogg_hook.dll` を注入する入口
- `src/hooker/hook_dll.cpp`
  - Win32 file API hook と disasm 由来 IAT slot patch の中心
- `src/hooker/hook_audio.inc`
  - OGG overlay、一時 WAV cache、fake find result
- `src/hooker/mf_ogg_decoder.*`
  - Media Foundation / ffmpeg fallback による OGG -> WAV decode
- `src/hooker/paths.*`
  - path、temp directory、CP932/Unicode 変換
- `src/hooker/log.*`
  - diagnostic-only log
