# シンプルJPEG劣化 / Simple JPEG Crunch

OBS Studio 向けの、JPEG で何度も保存し直したように映像を劣化させる映像フィルターのプラグインです。

A video filter plugin for OBS Studio that makes the image look as if it had been saved as a JPEG over and over again.

[配信プラグイン倉庫くん](https://soko-kun.allegromoltov.jp/plugins/allegromoltov/simple-jpeg-crunch) にも掲載しています。 / Also listed on [Plugin Soko-kun](https://soko-kun.allegromoltov.jp/plugins/allegromoltov/simple-jpeg-crunch?lang=en).

## 通信とデータ / Network and data

このプラグインはネットワークへ何も送信しません。映像はプラグインの中で GPU が処理し、外部のファイルやサービスを使いません。

This plugin sends nothing over the network. The GPU processes the image inside the plugin, and no external files or services are used.

## Features / 機能

ソースやグループに付ける映像フィルターです。設定はガビガビ度のスライダー 1 本だけです。0 では元の映像のまま、上げるほど 8×8 のブロックの境界が目立ち、色がにじみ、輪郭の周りにノイズが出ます。

実際の JPEG と同じ計算で劣化を作ります。RGB を YCbCr に変換し、色差を半分の解像度に間引き、8×8 のブロックごとに DCT をかけ、JPEG の標準の量子化テーブル (ITU-T T.81 Annex K) で丸めて元に戻します。ガビガビ度を上げると、JPEG の品質を下げながら、この保存し直しを最大 8 回繰り返します。回ごとにブロックの格子をずらすため、同じ格子で繰り返したときのように劣化が止まりません。

描画はすべて GPU のシェーダーで行います。1920×1080 の 1 フレームあたりの GPU 時間は、ガビガビ度 1 (1 回) で約 0.5 ms、既定値の 50 (4 回) で約 1.7 ms、最大の 100 (8 回) で約 3.3 ms です (RTX 3060 Ti、Direct3D 11 で測定)。

A video filter for sources and groups with a single "Crunch" slider. At 0 the image passes through unchanged. Higher values bring out 8×8 block edges, color bleeding, and ringing around edges.

The damage is made with the same math as a real JPEG: RGB is converted to YCbCr, chroma is subsampled to half resolution, each 8×8 block goes through a DCT, and the coefficients are rounded with the standard JPEG quantization tables (ITU-T T.81 Annex K) and converted back. Raising the slider lowers the JPEG quality and repeats this save up to eight times. The block grid shifts on each pass, so the damage keeps growing instead of settling as it would on a fixed grid.

All drawing runs in GPU shaders. GPU time per 1920×1080 frame is about 0.5 ms at Crunch 1 (one pass), about 1.7 ms at the default of 50 (four passes), and about 3.3 ms at the maximum of 100 (eight passes), measured on an RTX 3060 Ti with Direct3D 11.

## Installation / インストール

### Windows

Windows 版は OBS Studio 33 以降向けです。

#### 通常版 OBS への導入

プラグインを OBS Studio の共通プラグインフォルダー `C:\ProgramData\obs-studio\plugins` へ配置します。

1. OBS Studio を終了してください。
2. [Releases](../../releases) から `simple-jpeg-crunch-<version>-windows-x64.zip` をダウンロードし、展開してください。
3. エクスプローラーのアドレスバーへ `%PROGRAMDATA%` を入力し、Enter キーを押してください。隠しフォルダーの表示設定を変更する必要はありません。
4. `obs-studio` フォルダーを開き、その中の `plugins` フォルダーを開いてください。存在しないフォルダーは順に作成してください。
5. 展開した `simple-jpeg-crunch` フォルダーを `plugins` へコピーしてください。管理者の許可を求められた場合は許可してください。
6. OBS Studio を起動し、ソースのフィルターからシンプルJPEG劣化を追加してください。

導入後の構成を次に示します。`simple-jpeg-crunch` フォルダーを二重に作らないでください。PDB ファイルが同梱されている場合も、製品フォルダーごとコピーします。

```text
C:\ProgramData\obs-studio\plugins\
└─ simple-jpeg-crunch\
   ├─ simple-jpeg-crunch.dll
   └─ data\
      ├─ effects\jpeg-crunch.effect
      └─ locale\
         ├─ en-US.ini
         └─ ja-JP.ini
```

更新するときは、OBS Studio を終了し、同じ手順で `simple-jpeg-crunch` フォルダーを上書きコピーしてください。設定やシーンは削除しないでください。アンインストールするときは、OBS Studio を終了して `simple-jpeg-crunch` フォルダーを削除してください。

1. Quit OBS Studio.
2. Download `simple-jpeg-crunch-<version>-windows-x64.zip` from [Releases](../../releases) and extract it.
3. Copy the `simple-jpeg-crunch` folder into `C:\ProgramData\obs-studio\plugins`. Create the `obs-studio` and `plugins` folders if they do not exist.
4. Start OBS Studio and add "Simple JPEG Crunch" from the filters of a source.

To uninstall, quit OBS Studio and delete the `simple-jpeg-crunch` folder.

#### ポータブル版 OBS

ポータブル版では、OBS Studio のルートフォルダーにある `plugins` へ製品フォルダーをコピーします。通常版の `%PROGRAMDATA%` への導入では読み込まれません。

OBS Studio を終了し、`bin` や `data` と同じ階層に `plugins` フォルダーを作成してください。展開した製品フォルダーをコピーし、`plugins\simple-jpeg-crunch\simple-jpeg-crunch.dll` と `plugins\simple-jpeg-crunch\data` を配置してから OBS Studio を起動します。

### Linux (Ubuntu)

Ubuntu 24.04 (x86_64) 向けの `.deb` を Releases に置いています。OBS Studio の公式の PPA (`ppa:obsproject/obs-studio`) から apt で導入した OBS Studio が前提です。Releases から `simple-jpeg-crunch-<version>-x86_64-linux-gnu.deb` をダウンロードし、そのフォルダーで `sudo apt install ./simple-jpeg-crunch-<version>-x86_64-linux-gnu.deb` を実行してください。

Linux 版は、GitHub Actions の Ubuntu 24.04 (Mesa の OpenGL) で描画を自動で検証しています。作者は Linux の実機を持たないため、実機の GPU と OBS Studio では動作を確認していません。

A `.deb` for Ubuntu 24.04 (x86_64) is available on Releases. It requires OBS Studio installed with apt from the official PPA (`ppa:obsproject/obs-studio`). Download `simple-jpeg-crunch-<version>-x86_64-linux-gnu.deb` and run `sudo apt install ./simple-jpeg-crunch-<version>-x86_64-linux-gnu.deb` in that folder.

The Linux build is verified automatically by rendering tests on Ubuntu 24.04 (Mesa OpenGL) in GitHub Actions. The author has no Linux machine, so it has not been tested on real hardware.

### macOS

macOS 版は配布しません。GitHub Actions の macOS 15 (OpenGL) でビルドと描画を検証しているだけです。

There is no macOS release. The build and rendering are only checked on macOS 15 (OpenGL) in GitHub Actions.

## ダウンロードしたファイルの検証 / Verifying downloads

リリースのファイルには、GitHub Actions でビルドしたことを示す build provenance の証明を付けています。[GitHub CLI](https://cli.github.com/) で確かめられます。各ファイルの SHA-256 は、リリースのページの各ファイルの横に表示されます。

Every release file has a build provenance attestation made by GitHub Actions. You can verify it with the [GitHub CLI](https://cli.github.com/). The SHA-256 of each file is shown next to it on the release page.

```
gh attestation verify simple-jpeg-crunch-1.0.0-windows-x64.zip --repo AllegroMoltoV/simple-jpeg-crunch
```

## Usage / 使い方

1. 劣化させたいソースかグループを右クリックし、フィルターを開きます。
2. 映像フィルターの + ボタンから、シンプルJPEG劣化 (Simple JPEG Crunch) を追加します。
3. ガビガビ度で崩れ具合を調整します。

1. Right-click the source or group and open Filters.
2. Add Simple JPEG Crunch with the + button under Effect Filters.
3. Adjust the damage with the Crunch slider.

## Parameters / パラメータ

| Parameter | 日本語 | Range | Default |
|-----------|--------|-------|---------|
| Crunch | ガビガビ度 | 0 ~ 100 | 50 |

ガビガビ度に対応する JPEG の品質と保存し直しの回数は次のとおりです。

| ガビガビ度 / Crunch | 品質 / Quality | 回数 / Saves |
|-----------|--------|-------|
| 0 | 処理しない / off | 0 |
| 1 | 90 | 1 |
| 25 | 69 | 2 |
| 50 | 48 | 4 |
| 75 | 26 | 6 |
| 100 | 5 | 8 |

## Known Behavior / 既知の挙動

- HDR の映像 (Rec. 709 の拡張の色空間) では、OBS Studio の標準のシャープフィルターと同じく、劣化をかけずにそのまま通します。
- 色差は 2×2 の画素で同じ値を使って元の解像度に戻します。そのため、色の境界がブロック状ににじみます。
- 半透明の画素は、色をアルファで割り戻してから劣化させます。アルファは元のまま残します。
- ガビガビ度を上げるほど GPU の負荷が増えます。重いときはガビガビ度を下げてください。

- HDR images (extended Rec. 709 color space) pass through unchanged, as with the built-in Sharpen filter of OBS Studio.
- Chroma is restored by repeating each value over 2×2 pixels, so color edges bleed in blocks.
- Translucent pixels are damaged after their color is divided by alpha. Alpha itself is kept unchanged.
- GPU load rises with the Crunch value. Lower it if the filter is too heavy.

## Build / ビルド

### Requirements / 必要環境

| Platform | Tool |
|----------|------|
| Windows | Visual Studio 17 2022, CMake 3.28+ |
| macOS | Xcode 16.0, CMake 3.28+ |
| Ubuntu 24.04 | ninja-build, pkg-config, build-essential, CMake 3.28+ |

### Build Commands / ビルドコマンド

```bash
# Windows
cmake --preset windows-x64 -DENABLE_POST_BUILD_INSTALL=OFF
cmake --build build_x64 --config RelWithDebInfo

# macOS
cmake --preset macos
cmake --build build_macos --config RelWithDebInfo

# Ubuntu
cmake --preset ubuntu-x86_64
cmake --build build_x86_64 --config RelWithDebInfo
```

Windows で生成物を取り出すには、`cmake --install build_x64 --config RelWithDebInfo --prefix release/RelWithDebInfo` を実行してください。生成した製品フォルダーを前述の手順で導入してください。

Windows では、描画の統合テストを Direct3D 11 と OpenGL の両方で CTest に登録しています。統合テストは、libobs にプラグインを読み込んでフィルターを付け、出力の画素を CPU で同じ計算をした参照の値と比べます。OBS のグラフィックスの DLL を `cmake --build .deps/obs-studio-31.1.1/build_x64 --config Release --target libobs-d3d11 libobs-opengl` で作ってから、`ctest --test-dir build_x64 -C RelWithDebInfo` を実行してください。CI では同じテストを Windows (Direct3D 11)・Ubuntu・macOS (OpenGL) で実行します。

On Windows, rendering tests for Direct3D 11 and OpenGL are registered with CTest. They load the plugin into libobs, add the filter, and compare the output pixels with a CPU reference that does the same math. Build the OBS graphics DLLs with `cmake --build .deps/obs-studio-31.1.1/build_x64 --config Release --target libobs-d3d11 libobs-opengl`, then run `ctest --test-dir build_x64 -C RelWithDebInfo`. CI runs the same tests on Windows (Direct3D 11), Ubuntu, and macOS (OpenGL).

## AI利用開示 / AI Disclosure

### 日本語

v1.0.0 の制作では、Anthropic Claude Code を、調査、ソースコード、ビルド設定、テスト、文書を含むほぼすべての工程で使用しました。コード読解による厳密な人間レビューは実施していません。

### English

Anthropic Claude Code was used for almost the entire development process of v1.0.0, including research, source code, build configuration, tests, and documentation. No rigorous human review of the source code was performed.

## License / ライセンス

GPL-2.0 License

このプラグインは [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) をベースにしています。

This plugin is based on [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).

## Author / 作者

AllegroMoltoV - https://www.allegromoltov.jp

## Changelog / 更新情報

### v1.0.0

- 最初の公開版です。 / First public release.
