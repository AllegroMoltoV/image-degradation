// JPEG の劣化のフィルターの出力を画素で検査する統合テスト。
// 出力を、同じ計算を CPU で行う参照 (jpeg-reference.hpp) と比べる。量子化の丸めの境目に近い値に依存する画素は、
// 浮動小数点の僅かな誤差で GPU と CPU の結果が分かれうるため、比較から外す。外した画素の割合にも上限を置く。
// 使い方: integration-test <プラグイン> <プラグインの data> <描画モジュール> <libobs の data> <シナリオ>
// 終了コードは、合格が 0、検査の不合格が 1、起動の失敗が 2。
// preview のシナリオは CTest に登録しない。ガビガビ度ごとの出力を書き出して、依頼者が見た目を確かめるために使う。

#include "jpeg-crunch-fixture.hpp"
#include "jpeg-reference.hpp"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace {

using namespace jpeg_crunch_test;

constexpr uint32_t kCanvasWidth = 64;
constexpr uint32_t kCanvasHeight = 48;
// 余裕 (丸めの境目からの距離) がこれより小さい画素は比較から外す。
// D3D11 と OpenGL の実機 (RTX 3060 Ti) と CI のソフトウェア描画で、余裕が 1e-5 以上の画素は CPU 参照と一致した。
// 幅を広げると、比較から外れる画素が周りの画素へ伝播して増える。
// 不一致が出たときは、出力の largest margin among mismatches を見て広げる
constexpr float kTieEpsilon = 1e-5f;
// 比較できる画素の割合の下限
constexpr double kMinComparable = 0.9;

std::string at(uint32_t x, uint32_t y)
{
	return "(" + std::to_string(x) + ", " + std::to_string(y) + ")";
}

// 出力が入力 (アルファを掛けない色) とほぼ同じであることを確かめる。不透明な入力にだけ使う
void check_identity(Checker &c, const Image &input, const Image &out)
{
	if (out.width != input.width || out.height != input.height) {
		c.expect(false, "output size differs from the input");
		return;
	}
	for (uint32_t y = 0; y < out.height; ++y) {
		for (uint32_t x = 0; x < out.width; ++x) {
			for (int ch = 0; ch < 4; ++ch) {
				c.expect(std::abs(out.at(x, y)[ch] - input.at(x, y)[ch]) <= 1,
					 "pixel " + at(x, y) + " channel " + std::to_string(ch) +
						 " changed: " + std::to_string(out.at(x, y)[ch]) + " (input " +
						 std::to_string(input.at(x, y)[ch]) + ")");
			}
		}
	}
}

// 不透明な入力について、出力を CPU 参照と比べる
void check_reference(Checker &c, const std::string &label, const Image &input, const Image &out, int crunch)
{
	if (out.width != input.width || out.height != input.height) {
		c.expect(false, label + ": output size differs from the input");
		return;
	}
	std::vector<uint8_t> rgb(static_cast<size_t>(input.width) * input.height * 3);
	for (size_t i = 0, n = static_cast<size_t>(input.width) * input.height; i < n; ++i) {
		std::copy_n(&input.rgba[i * 4], 3, &rgb[i * 3]);
	}
	const jpeg_reference::Result ref =
		jpeg_reference::run(rgb, static_cast<int>(input.width), static_cast<int>(input.height), crunch);
	size_t comparable = 0;
	size_t mismatched = 0;
	size_t changed = 0;
	float largest_mismatch_margin = 0.0f;
	for (uint32_t y = 0; y < out.height; ++y) {
		for (uint32_t x = 0; x < out.width; ++x) {
			const size_t i = static_cast<size_t>(y) * out.width + x;
			const uint8_t *o = out.at(x, y);
			c.expect(o[3] == 255, label + ": alpha changed at " + at(x, y));
			bool differs_from_input = false;
			for (int ch = 0; ch < 3; ++ch) {
				differs_from_input |= std::abs(o[ch] - input.rgba[i * 4 + ch]) > 2;
			}
			changed += differs_from_input ? 1 : 0;
			if (ref.margin[i] < kTieEpsilon) {
				continue;
			}
			++comparable;
			bool match = true;
			for (int ch = 0; ch < 3; ++ch) {
				match &= std::abs(o[ch] - ref.rgb[i * 3 + ch]) <= 1;
			}
			if (!match) {
				++mismatched;
				largest_mismatch_margin = std::max(largest_mismatch_margin, ref.margin[i]);
				std::ostringstream message;
				message << label << ": pixel " << at(x, y) << " is (" << int(o[0]) << ", " << int(o[1])
					<< ", " << int(o[2]) << "), reference (" << int(ref.rgb[i * 3]) << ", "
					<< int(ref.rgb[i * 3 + 1]) << ", " << int(ref.rgb[i * 3 + 2]) << "), margin "
					<< ref.margin[i];
				c.expect(false, message.str());
			}
		}
	}
	const double total = static_cast<double>(out.width) * out.height;
	std::cout << std::fixed << std::setprecision(4) << label << ": comparable " << comparable / total
		  << ", mismatched " << mismatched << ", changed " << changed / total;
	if (mismatched > 0) {
		// 境目の幅 kTieEpsilon の校正に使う。不一致の画素の余裕の最大値より広げれば、この不一致は比較から外れる
		std::cout << ", largest margin among mismatches " << largest_mismatch_margin;
	}
	std::cout << '\n';
	c.expect(comparable / total >= kMinComparable,
		 label + ": too few comparable pixels (" + std::to_string(comparable / total) + ")");
	c.expect(changed > 0, label + ": the output is the same as the input");
}

// フィルターを付けたソースの出力を読む。読めなかったときは false
bool render_filtered(Checker &c, const std::string &name, obs_source_t *source, Image &out)
{
	if (!render(source, out)) {
		c.expect(false, "failed to render " + name);
		return false;
	}
	save_image(name, out);
	return true;
}

// テスト用ソースにフィルターを付け、check を呼ぶ
using FilterCheck = std::function<void(Checker &, obs_source_t *source, obs_source_t *filter)>;

void with_filter(Checker &c, const SourceSpec &spec, int crunch, const FilterCheck &check)
{
	obs_source_t *source = create_test_source(spec);
	obs_data_t *settings = crunch_settings(crunch);
	obs_source_t *filter = obs_source_create(kFilterId, "jpeg-crunch", settings, nullptr);
	obs_data_release(settings);
	if (!source || !filter) {
		c.expect(false, "failed to create the test source or the filter");
	} else {
		obs_source_filter_add(source, filter);
		c.expect(wait_frames(2), "frames did not advance");
		check(c, source, filter);
	}
	obs_source_release(filter);
	obs_source_release(source);
}

// 1 つのガビガビ度で出力を読み、CPU 参照と比べる
void reference_scenario(Checker &c, const std::string &name, const SourceSpec &spec, int crunch)
{
	const Image input = paint_image(spec);
	with_filter(c, spec, crunch, [&](Checker &c, obs_source_t *source, obs_source_t *) {
		Image out;
		if (render_filtered(c, name, source, out)) {
			check_reference(c, name, input, out, crunch);
		}
	});
}

// 平均の絶対差。色空間の経路で二重に変換すると、階調が数十ずれる
double mean_abs_diff(const Image &a, const Image &b)
{
	if (a.rgba.size() != b.rgba.size() || a.rgba.empty()) {
		return 1e9;
	}
	double sum = 0.0;
	for (size_t i = 0; i < a.rgba.size(); ++i) {
		sum += std::abs(a.rgba[i] - b.rgba[i]);
	}
	return sum / static_cast<double>(a.rgba.size());
}

const SourceSpec scene = {kCanvasWidth, kCanvasHeight, paint_scene};
// 解像度が等倍 (色のずれ 0、既定値)・1/2 (1 px)・1/3 (2 px)・1/4 (3 px) になるガビガビ度
constexpr int kFullScale = 10;
constexpr int kHalfScale = 30;
constexpr int kThirdScale = 50;
constexpr int kQuarterScale = 90;

struct Scenario {
	const char *name;
	std::function<void(Checker &)> run;
};

const Scenario scenarios[] = {
	// ガビガビ度 0 では処理を飛ばし、元の映像のまま通す
	{"zero",
	 [](Checker &c) {
		 const Image input = paint_image(scene);
		 with_filter(c, scene, 0, [&](Checker &c, obs_source_t *source, obs_source_t *) {
			 Image out;
			 if (render_filtered(c, "zero", source, out)) {
				 check_identity(c, input, out);
			 }
		 });
	 }},
	// 等倍の解像度。8x8 のブロックごとの DCT と量子化、シャープ・彩度・コントラストの結果が CPU 参照と一致する
	{"single",
	 [](Checker &c) {
		 reference_scenario(c, "single", scene, kFullScale);
	 }},
	// 処理の解像度を下げたときの縮小・色のずれ・双線形補間での拡大が CPU 参照と一致する
	{"scaled",
	 [](Checker &c) {
		 reference_scenario(c, "scaled-half", scene, kHalfScale);
		 reference_scenario(c, "scaled-third", scene, kThirdScale);
		 reference_scenario(c, "scaled-quarter", scene, kQuarterScale);
	 }},
	// 最大のガビガビ度。CPU 参照との比較は記録だけにし、アルファの保持と、入力から大きく変わったことを確かめる
	{"max",
	 [](Checker &c) {
		 const Image input = paint_image(scene);
		 with_filter(c, scene, jpeg_crunch::kMaxCrunch, [&](Checker &c, obs_source_t *source, obs_source_t *) {
			 Image out;
			 if (!render_filtered(c, "max", source, out)) {
				 return;
			 }
			 for (uint32_t y = 0; y < out.height; ++y) {
				 for (uint32_t x = 0; x < out.width; ++x) {
					 c.expect(out.at(x, y)[3] == 255, "alpha changed at " + at(x, y));
				 }
			 }
			 const double diff = mean_abs_diff(input, out);
			 std::cout << "max: mean absolute difference from the input " << diff << '\n';
			 c.expect(diff > 2.0, "the output at the maximum is too close to the input");
			 Checker record;
			 check_reference(record, "max (recorded only)", input, out, jpeg_crunch::kMaxCrunch);
		 });
	 }},
	// 半透明の入力でも、出力のアルファは入力のアルファのまま残る
	{"alpha",
	 [](Checker &c) {
		 const SourceSpec spec = {kCanvasWidth, kCanvasHeight, paint_translucent};
		 const Image input = paint_image(spec);
		 with_filter(c, spec, jpeg_crunch::kDefaultCrunch,
			     [&](Checker &c, obs_source_t *source, obs_source_t *) {
				     Image out;
				     if (!render_filtered(c, "alpha", source, out)) {
					     return;
				     }
				     for (uint32_t y = 0; y < out.height; ++y) {
					     for (uint32_t x = 0; x < out.width; ++x) {
						     c.expect(std::abs(out.at(x, y)[3] - input.at(x, y)[3]) <= 1,
							      "alpha at " + at(x, y) + " is " +
								      std::to_string(out.at(x, y)[3]) + " (input " +
								      std::to_string(input.at(x, y)[3]) + ")");
					     }
				     }
			     });
	 }},
	// 幅と高さが 8 の倍数でない入力。端にかかるブロックと、ずらした格子の端のブロックを確かめる
	{"odd-size",
	 [](Checker &c) {
		 const SourceSpec spec = {61, 45, paint_scene};
		 reference_scenario(c, "odd-size-single", spec, kFullScale);
		 reference_scenario(c, "odd-size-half", spec, kHalfScale);
		 reference_scenario(c, "odd-size-third", spec, kThirdScale);
	 }},
	// 16 ビットの線形の入力 (SRGB_16F) は、sRGB へ符号化してから処理し、線形へ戻して出力する。
	// 同じ絵を 8 ビットの sRGB の入力で処理した出力とほぼ同じになる
	{"srgb16f",
	 [](Checker &c) {
		 SourceSpec linear = scene;
		 linear.space = GS_CS_SRGB_16F;
		 Image from_srgb;
		 Image from_linear;
		 with_filter(c, scene, kHalfScale, [&](Checker &c, obs_source_t *source, obs_source_t *) {
			 render_filtered(c, "srgb16f-from-srgb", source, from_srgb);
		 });
		 with_filter(c, linear, kHalfScale, [&](Checker &c, obs_source_t *source, obs_source_t *) {
			 render_filtered(c, "srgb16f-from-linear", source, from_linear);
		 });
		 const double diff = mean_abs_diff(from_srgb, from_linear);
		 std::cout << "srgb16f: mean absolute difference " << diff << '\n';
		 c.expect(diff <= 1.0, "SRGB_16F output differs from the SRGB output by " + std::to_string(diff));
	 }},
	// HDR の 709_EXTENDED では、標準のフィルターと同じく処理を飛ばし、フィルターが無いときと同じ出力にする。
	// 出力先は SDR のため、フィルターが無くても OBS が色を変換する。比べる相手は入力ではなく、フィルターを付ける前の出力
	{"extended",
	 [](Checker &c) {
		 SourceSpec spec = scene;
		 spec.space = GS_CS_709_EXTENDED;
		 obs_source_t *source = create_test_source(spec);
		 obs_data_t *settings = crunch_settings(jpeg_crunch::kMaxCrunch);
		 obs_source_t *filter = obs_source_create(kFilterId, "jpeg-crunch", settings, nullptr);
		 obs_data_release(settings);
		 Image before;
		 Image after;
		 if (!source || !filter) {
			 c.expect(false, "failed to create the test source or the filter");
		 } else if (render_filtered(c, "extended-before", source, before)) {
			 obs_source_filter_add(source, filter);
			 c.expect(wait_frames(2), "frames did not advance");
			 if (render_filtered(c, "extended", source, after)) {
				 check_identity(c, before, after);
			 }
		 }
		 obs_source_release(filter);
		 obs_source_release(source);
	 }},
	// シーンの中で、フィルターを付けたソースだけが劣化する。ほかのソースと、付けたソースの透明な部分は、付ける前と同じ
	{"scene",
	 [](Checker &c) {
		 constexpr uint32_t kLeft = 16;
		 constexpr uint32_t kTop = 12;
		 constexpr uint32_t kWidth = 32;
		 constexpr uint32_t kHeight = 24;
		 obs_scene_t *scene = obs_scene_create("scene");
		 obs_source_t *background =
			 create_test_source({kCanvasWidth, kCanvasHeight, paint_gradient}, "background");
		 obs_source_t *overlay = create_test_source({kWidth, kHeight, paint_half_transparent}, "overlay");
		 obs_sceneitem_t *overlay_item = nullptr;
		 if (scene && background && overlay) {
			 obs_scene_add(scene, background);
			 overlay_item = obs_scene_add(scene, overlay);
			 vec2 pos;
			 vec2_set(&pos, static_cast<float>(kLeft), static_cast<float>(kTop));
			 obs_sceneitem_set_pos(overlay_item, &pos);
		 }
		 obs_source_t *scene_source = scene ? obs_scene_get_source(scene) : nullptr;
		 obs_data_t *settings = crunch_settings(jpeg_crunch::kMaxCrunch);
		 obs_source_t *filter = obs_source_create(kFilterId, "jpeg-crunch", settings, nullptr);
		 obs_data_release(settings);
		 Image before;
		 if (!scene_source || !overlay_item || !filter) {
			 c.expect(false, "failed to create the scene or the filter");
		 } else if (wait_frames(2) && render_filtered(c, "scene-before", scene_source, before)) {
			 obs_source_filter_add(overlay, filter);
			 c.expect(wait_frames(2), "frames did not advance");
			 Image after;
			 if (render_filtered(c, "scene", scene_source, after)) {
				 size_t changed_inside = 0;
				 for (uint32_t y = 0; y < after.height; ++y) {
					 for (uint32_t x = 0; x < after.width; ++x) {
						 const bool opaque_part = x >= kLeft && x < kLeft + kWidth / 2 &&
									  y >= kTop && y < kTop + kHeight;
						 bool same = true;
						 for (int ch = 0; ch < 4; ++ch) {
							 same &= std::abs(after.at(x, y)[ch] - before.at(x, y)[ch]) <=
								 1;
						 }
						 if (opaque_part) {
							 changed_inside += same ? 0 : 1;
						 } else {
							 c.expect(
								 same,
								 "pixel " + at(x, y) +
									 " outside the opaque part of the filtered source changed");
						 }
					 }
				 }
				 c.expect(changed_inside > 0, "the opaque part of the filtered source did not change");
			 }
		 }
		 obs_source_release(filter);
		 obs_source_release(overlay);
		 obs_source_release(background);
		 if (scene) {
			 // OBS 31 以降はキャンバスがシーンを保持するため、キャンバスから外してから解放する
			 obs_source_remove(scene_source);
			 obs_scene_release(scene);
		 }
	 }},
	// ガビガビ度の変更が出力に反映される
	{"settings-update",
	 [](Checker &c) {
		 const Image input = paint_image(scene);
		 with_filter(c, scene, 0, [&](Checker &c, obs_source_t *source, obs_source_t *filter) {
			 Image out;
			 if (render_filtered(c, "settings-update-0", source, out)) {
				 check_identity(c, input, out);
			 }
			 for (const int crunch : {kFullScale, 0}) {
				 obs_data_t *next = crunch_settings(crunch);
				 c.expect(update_and_wait(filter, next), "settings were not applied");
				 obs_data_release(next);
				 const std::string name = "settings-update-" + std::to_string(crunch);
				 if (!render_filtered(c, name, source, out)) {
					 continue;
				 }
				 if (crunch == 0) {
					 check_identity(c, input, out);
				 } else {
					 check_reference(c, name, input, out, crunch);
				 }
			 }
		 });
	 }},
	// 目視の確認用。環境変数 JPEG_CRUNCH_TEST_INPUT に BMP があればそれを、無ければ合成した絵を入力にし、
	// ガビガビ度ごとの出力を JPEG_CRUNCH_TEST_OUTPUT_DIR に書き出す
	{"preview",
	 [](Checker &c) {
		 SourceSpec spec = {640, 360, paint_scene};
		 if (const char *path = std::getenv("JPEG_CRUNCH_TEST_INPUT")) {
			 const Image photo = read_bmp(path);
			 if (photo.rgba.empty()) {
				 c.expect(false, std::string("failed to read ") + path);
				 return;
			 }
			 spec = {photo.width, photo.height, [photo](Image &image) {
					 image = photo;
				 }};
		 }
		 for (const int crunch : {0, 10, 20, 30, 50, 70, 100}) {
			 with_filter(c, spec, crunch, [&](Checker &c, obs_source_t *source, obs_source_t *) {
				 std::ostringstream name;
				 name << "preview-" << std::setw(3) << std::setfill('0') << crunch;
				 Image out;
				 render_filtered(c, name.str(), source, out);
			 });
		 }
	 }},
};

} // namespace

int main(int argc, char **argv)
{
	if (argc != 6) {
		std::cerr
			<< "usage: integration-test <plugin> <plugin data> <graphics module> <libobs data> <scenario>\n";
		return 2;
	}
	const Scenario *scenario = nullptr;
	for (const Scenario &s : scenarios) {
		if (std::strcmp(argv[5], s.name) == 0) {
			scenario = &s;
		}
	}
	if (!scenario) {
		std::cerr << "unknown scenario: " << argv[5] << '\n';
		return 2;
	}
	if (!obs_test::start(argv[3], argv[4], kCanvasWidth, kCanvasHeight)) {
		return 2;
	}
	register_test_source();
	int result = 2;
	if (obs_test::load_plugin(argv[1], argv[2])) {
		Checker checker;
		scenario->run(checker);
		result = checker.failures == 0 ? 0 : 1;
		if (result != 0) {
			std::cerr << checker.failures << " check(s) failed in " << scenario->name << '\n';
		} else {
			std::cout << "PASS " << scenario->name << '\n';
		}
	}
	obs_shutdown();
	return result;
}
