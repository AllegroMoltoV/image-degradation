# 初期調査: 流用元・掲載条件・GPU での JPEG 劣化の実装上の制約

調査日: 2026-11-16

## はじめに

この文書は、simple-jpeg-crunch の実装に着手する前に行った調査の記録です。調査の対象は 3 つです。

- 流用元の simple-mosaic (`D:/work/obs-simple-mosaic`、main の HEAD `9d499e4`) の構成
- 配信プラグイン倉庫くんへの掲載条件と、依頼者に固有の決まり
- OBS のエフェクトの言語で、8×8 の DCT と量子化を多パスで描くときの制約

調査は読み取りだけで行いました。行番号は調査時点のものです。OBS のソースは `D:/work/obs-simple-mosaic/.deps/obs-studio-31.1.1` を読みました。以下で OBS のパスを書くときは、この直下からの相対パスです。

## 調査結果: simple-mosaic の構成

### 流用できるもの

`cmake/`・`build-aux/`・`.github/`・`CMakePresets.json`・`.clang-format`・`.gersemirc`・`.gitignore`・`LICENSE` には、プラグイン固有の名前がありません。名前を置き換えずに使えます。

ただし、Windows でファイルを複製すると実行ビットとシンボリックリンクが失われます。git の index では次の形で保たれています。過去に macOS と Ubuntu の CI が終了コード 126 で失敗した前例があるため、`git update-index --chmod=+x` で実行ビットを戻し、シンボリックリンクは index に mode 120000 で登録します。

| mode | ファイル |
| --- | --- |
| 100755 | `.github/scripts/build-macos`・`build-ubuntu`・`package-macos`・`package-ubuntu`、`build-aux/.run-format.zsh` |
| 120000 | `build-aux/run-clang-format`・`run-gersemi`・`run-swift-format` |

### 名前を置き換えるもの

置き換えの対象は `buildspec.json:39-41`、`CMakeLists.txt:64-70,89-103`、`.github/workflows/render-test.yaml`、`src/`・`data/`・`tests/` の識別子、README です。識別子の例は、ソース ID `simple_mosaic_filter`、ロケールキー `SimpleMosaic`、テストのターゲット `mosaic-integration-test`、環境変数 `MOSAIC_TEST_OUTPUT_DIR` です。

### 描画の骨格

`src/mosaic-filter.cpp` は `obs_source_process_filter_begin` を使わず、texrender で多パスの描画を組んでいます。

1. `RenderTarget` (200-227 行): 対象を `gs_texrender_begin_with_color_space(..., GS_CS_SRGB)` で中間テクスチャへ描く。
2. `DrawPass` (181-198 行): 中間パスを `GS_RGBA16F` の texrender へ、`gs_blend_function(ONE, ZERO)` で描く。
3. 最終パス: OBS の描画先へ `gs_blend_function(ONE, INVSRCALPHA)` で描く。

`gs_technique_end` はパスのたびにパラメーターの値を消します。そのため、パスの直前に毎回すべての値を設定し直しています (143-154 行)。sRGB の明示的な扱いは 209 行の `GS_CS_SRGB` だけです。

### 描画の統合テスト

- `tests/obs-test-env.hpp` が、フロントエンドなしで libobs を起動します。OpenGL では、描画スレッドが 2 フレーム進むまで待たないと出力が空になります (67-77 行)。
- `tests/mosaic-fixture.hpp` が、入力のソース、出力の読み出し、BMP の書き出しを提供します。BMP は環境変数 `MOSAIC_TEST_OUTPUT_DIR` があるときだけ書き出します。
- CTest には、Windows で D3D11 と OpenGL の 2 方式 × 9 シナリオを登録しています (`CMakeLists.txt:81-118`)。
- 描画モジュールは自動では作られません。初回に次を手で実行します。

```
cmake --build .deps/obs-studio-31.1.1/build_x64 --config Release --target libobs-d3d11 libobs-opengl
```

- CI の `render-test.yaml` は、Windows では WARP の D3D11 だけ、Ubuntu では Xvfb と Mesa の OpenGL、macOS ではソフトウェア描画の OpenGL で実行します。
- GPU 時間は `tests/benchmark.cpp` が `gs_timer` で測ります。OpenGL の libobs では `gs_timer_range_create` が NULL を返します。CI のソフトウェア描画では値が使えないため、合否に使っていません。

### D3D11 と OpenGL の差として記録されている知見

- D3D11 は、回数が実行時に決まるループの中の `Sample` を受け付けません。`SampleLevel(..., 0.0)` で読みます (`mosaic.effect:8-9`)。
- OpenGL では C 形式のキャスト `(float)i` がエラーになります。`float(i)` の形で書きます。
- 補間された UV に大きさを掛けた位置は、画素の中心から僅かにずれます。`floor()` で番号へ丸めます。

### 前提と異なっていた点

- simple-mosaic には `.soko-kun/config.json` がありません。依頼者のスキルでは必須とされています。
- リポジトリ内に immutable release の設定はありません。リリースは `draft: true` で作られ、公開は手で行います。
- main の ruleset は名前で指定した旧い形です。依頼者のスキルは、simple-white-noise の新しい形を写す手順を定めています。

## 調査結果: 倉庫くんへの掲載条件

出典は `~/.claude/skills/soko-kun-publish/SKILL.md` (以下 PUB)、`D:/work/workspace-plugin-distributer/plugins/soko-kun-plugin-dev/skills/streaming-plugin-dev/` (以下 SPD)、`D:/work/workspace-plugin-distributer/docs/design/review-criteria.md` (以下 RC) です。PUB:23 によると、公開のスキルや審査の基準と食い違うときは後者にしたがいます。

### 掲載の条件

- すべての配布物に build provenance の証明を付けます。署名したワークフローは最初に検証できた版のものが記録され、以後の版で別のワークフローが署名すると失敗します (RC:33-37)。
- リリースの API の `immutable` が true であることが必要です (RC:34)。設定を有効にした後に公開したリリースだけが不変になります。
- 配布物に exe と msi を含めません (RC:35)。
- GitHub がライセンスを検出でき、直下の CMakeLists.txt が libobs を参照していることが登録の条件です。
- GitHub Free では、artifact attestation は公開リポジトリでしか使えません。リリースの前にリポジトリを公開します。

### 依頼者に固有の決まり

- `.soko-kun/config.json` を必ず置きます。`name` は日本語名と英語名を ` / ` でつなぎ、OBS を入れません (PUB:29-41)。
- `os` は、CI に Linux の動作テストがあれば `["windows", "linux"]`、無ければ `["windows"]` にします (PUB:38、2026-11-16 依頼者判断)。Linux を載せるときは、README の Linux の節に `.deb` の導入方法と、公式の PPA の OBS が前提であることを書きます。
- ruleset は simple-white-noise の `default-branch` と `release-tags` を写します (PUB:47-54)。
- README の冒頭の概要の直後に、倉庫くんへのリンクの行を置きます (PUB:62-70)。

### 最初の掲載の前に固めるもの

掲載の後にワークフローを変えると、次の版が管理者の確認待ちになります (RC:45-48)。アクションの SHA 固定、permissions の最小化、`persist-credentials: false`、dependabot.yml、証明の手順、タグのビルドでの ccache の無効化は、最初の版の前に済ませます。審査の勧告として、CMake に `-fsanitize` と `/GS` か `-fstack-protector` を書くこと (`build-hardening`) も挙がっています。

README・リリースノート・リポジトリ内の文書には、OS の保護機能を外させる案内と、貼り付けて実行するコマンドを書きません。照合は語で行われるため、否定の文脈で保護機能の名前を挙げても重大の指摘になります (RC:58,75-76)。

## 調査結果: エフェクトの言語の制約

### 書けない構文

- 大域の定数配列 `const float q[64] = {...}` は書けません。大域の `float` の初期値はスカラー 1 個しか読めず、書き出しでは初期値が捨てられます (`libobs/graphics/effect-parser.c:1049-1112,1420-1445`)。
- uniform の配列は使えません。GL は `[N]` を書き出さず、値を送る `glUniform1fv` の個数も 1 に固定されています (`libobs-opengl/gl-shaderparser.c:104-118`、`libobs-opengl/gl-shader.c:463,481`)。
- `[unroll]` などの属性、`static`、`{...}` の初期化子リストは、GL でコンパイルエラーになります。関数本体はトークン単位でほぼそのまま GLSL へ写されるためです (`gl-shaderparser.c:451-523`)。
- 関数の中の配列は、宣言と要素ごとの代入に分ければ両方で同じ構文になります。

### 表の渡し方

量子化テーブルと DCT の基底は、CPU で計算して渡します。渡し方は、`float4` の uniform を並べる方法と、小さなテクスチャに置いて `Load` で読む方法の 2 つです。`float4x4` の要素を添字で読むと、D3D11 と GL で転置の関係になると推論されます。これは未実測です。

### 中間テクスチャ

- `GS_R32F`・`GS_RG32F`・`GS_RGBA32F`・`GS_RGBA16F` は、両方の描画方式の変換表に載っています。描画先として使えることは実機で未確認です。
- `GS_RGBA16F` の有効桁は 11 ビットです。行方向の DCT の出力はおよそ ±362 に達し、刻みが 0.25 になります。量子化の前の値を保持する中間には 32F を使います。
- 中間テクスチャは `Load` だけで読みます。GL では `Load` だけに使うテクスチャに point のサンプラーが付きます (`libobs-opengl/gl-subsystem.c:295-302,582-585`)。範囲外の読み込みは 0 を返すため、端の画素の複製は座標を自分で clamp します。
- `gs_texrender_begin` は、同じフレームで描画済みなら false を返します。使い回す texrender は毎回 `gs_texrender_reset` を呼びます (`libobs/graphics/texture-render.c:95,134,140`)。

### 丸め

HLSL の `round` は偶数への丸め、GLSL の `round` は 0.5 の扱いが実装依存です。OBS は `round` を変換しません。`floor(x + 0.5)` の形で明示し、CPU の参照の実装も同じ式にします。

平坦なブロックでは、量子化の前の DC の値がちょうど .5 になる場合が多くあります。float の誤差が 1 ulp あるだけで、GPU と CPU で量子化の結果が変わります。

### 色空間

標準のフィルター (`plugins/obs-filters/sharpness-filter.c:72-143`) では、`obs_source_get_color_space` で色空間を得て、`709_EXTENDED` なら処理を飛ばし、`.video_get_color_space` と `OBS_SOURCE_SRGB` で宣言しています。JPEG はガンマのかかった 8 ビットの値に対して処理するため、`SRGB_16F` の入力は sRGB へ符号化してから処理し、最後に線形へ戻す経路が要ります。

### パスの構成と負荷の見積もり

1920×1080 で、行方向と列方向を 8 タップずつに分けた分離型にすると、1 回の保存し直しが 8 回の描画、テクスチャの読み込みがおよそ 1 億回になります。64 タップの 1 パスの方式はおよそ 3.4 億回で、分離型の 3 倍から 4 倍です。N 回繰り返すと描画は 8N+1 回です。格子のずらしは、各パスでブロックの原点の計算にずらし量を足すだけで済み、パスは増えません。これらは見積もりで、実測していません。

## 考察

設計の第一案は、分離型の DCT を 32F の中間テクスチャで多パスに描く方式がよいと考えています。simple-mosaic の多パスの骨格と、行と列に分けて描画先の画素を多く保つ方針に一致するためです。

自動の検証のうち、画素の完全一致を求めると、量子化の境目の値で D3D11・OpenGL・CPU の結果が分かれて不安定になると推測します。量子化した係数を中間テクスチャから読み出して CPU と突き合わせ、境目に近い係数を除いて一致を求める形が適していると考えています。

## 未確認の事項

- 32F のテクスチャへの描画の可否を、実機と CI のソフトウェア描画で確かめていません。
- GL での 8×8 の格子の原点の上下を確かめていません。高さが 8 の倍数でないソースで、描画方式によって格子がずれる恐れがあります。
- 4:2:0 の縮小と拡大で libjpeg が使う方式は、記憶に基づく情報で、ソースを読んでいません。
- 1920×1080 での GPU 時間は未測定です。
