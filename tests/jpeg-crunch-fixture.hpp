#pragma once

// 統合テストとベンチマークで共通に使う部品。フロントエンドなしでの libobs の起動とプラグインの読み込み、
// テスト用の入力ソースの登録、フィルターの作成、出力の読み出し、目視の確認用の画像の書き出しと、入力に使う画像の読み込みを行う。

#include <obs.h>
#if defined(__linux__)
#include <obs-nix-platform.h>
#include <X11/Xlib.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Windows は D3D11 と OpenGL、Linux は X11 + EGL の OpenGL、macOS は OpenGL の描画モジュールを渡す
namespace obs_test {

// 成功すれば true。失敗したときは obs_shutdown まで済ませて false を返す
inline bool start(const char *graphics_module, const char *core_data, uint32_t width, uint32_t height)
{
#if defined(__linux__)
	// Linux の OpenGL 描画は X11 のディスプレイと EGL を使う。CI では xvfb のディスプレイを開く
	Display *display = XOpenDisplay(nullptr);
	if (!display) {
		std::cerr << "failed to open X display\n";
		return false;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(display);
#endif
	if (!obs_startup("en-US", nullptr, nullptr)) {
		std::cerr << "failed to start OBS\n";
		return false;
	}
	// obs_add_data_path は非推奨だが、フロントエンドなしで libobs の既定エフェクトを読ませる手段がほかにない
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
	std::string core_data_path = core_data;
	core_data_path += '/';
	obs_add_data_path(core_data_path.c_str());
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
	obs_video_info video = {};
	video.graphics_module = graphics_module;
	video.fps_num = 60;
	video.fps_den = 1;
	video.base_width = width;
	video.base_height = height;
	video.output_width = width;
	video.output_height = height;
	video.output_format = VIDEO_FORMAT_RGBA;
	if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) {
		std::cerr << "failed to initialize OBS video\n";
		obs_shutdown();
		return false;
	}
	// OpenGL では、OBS の描画スレッドが最初のフレームを進める前にテストのスレッドから描画すると、
	// 出力全体が空になることがある。描画スレッドが 2 フレーム進んだのを確かめてから戻る
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (obs_get_total_frames() < 2) {
		if (std::chrono::steady_clock::now() > deadline) {
			std::cerr << "OBS graphics thread did not start rendering within five seconds\n";
			obs_shutdown();
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return true;
}

// プラグインを読み込んで初期化する。失敗したときは false を返す
inline bool load_plugin(const char *plugin_path, const char *module_data)
{
	obs_module_t *module = nullptr;
	if (obs_open_module(&module, plugin_path, module_data) != MODULE_SUCCESS || !obs_init_module(module)) {
		std::cerr << "failed to load plugin module\n";
		return false;
	}
	return true;
}

} // namespace obs_test

namespace jpeg_crunch_test {

constexpr const char *kFilterId = "simple_jpeg_crunch_filter";
constexpr const char *kTestSourceId = "simple_jpeg_crunch_test_source";

// RGBA の画素。出力の色はアルファを掛けた値、テスト用ソースに渡す色はアルファを掛けない値
struct Image {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> rgba;

	const uint8_t *at(uint32_t x, uint32_t y) const { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }
	uint8_t *at(uint32_t x, uint32_t y) { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }
};

using Painter = std::function<void(Image &image)>;

// 次に作るテスト用ソースの大きさ、描き方、色空間
struct SourceSpec {
	uint32_t width = 64;
	uint32_t height = 48;
	Painter paint;
	gs_color_space space = GS_CS_SRGB;
};

// 描き方に従って、アルファを掛けない RGBA の画像を作る
inline Image paint_image(const SourceSpec &spec)
{
	Image image;
	image.width = spec.width;
	image.height = spec.height;
	image.rgba.assign(static_cast<size_t>(spec.width) * spec.height * 4, 0);
	if (spec.paint) {
		spec.paint(image);
	}
	return image;
}

inline SourceSpec next_spec;

struct TestSource {
	gs_texture_t *texture = nullptr;
	SourceSpec spec;
};

inline bool rebuild_texture(TestSource &source)
{
	const Image image = paint_image(source.spec);
	const uint8_t *data = image.rgba.data();
	obs_enter_graphics();
	gs_texture_destroy(source.texture);
	source.texture = gs_texture_create(image.width, image.height, GS_RGBA, 1, &data, 0);
	obs_leave_graphics();
	return source.texture != nullptr;
}

// OBS の画像のソースと同じく、テクスチャを sRGB の符号化を解いて読み、線形の値で描く。
// 16 ビットの出力先には線形の値が、8 ビットの出力先には sRGB で符号化した値が書かれる。
// 宣言する色空間は spec.space で変えられる。フィルターの色空間ごとの経路の検査に使う
inline void register_test_source()
{
	obs_source_info info = {};
	info.id = kTestSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return "JPEG crunch test source";
	};
	info.create = [](obs_data_t *, obs_source_t *) -> void * {
		auto *source = new TestSource;
		source->spec = next_spec;
		if (!rebuild_texture(*source)) {
			delete source;
			return nullptr;
		}
		return source;
	};
	info.destroy = [](void *data) {
		auto *source = static_cast<TestSource *>(data);
		obs_enter_graphics();
		gs_texture_destroy(source->texture);
		obs_leave_graphics();
		delete source;
	};
	info.get_width = [](void *data) {
		return static_cast<TestSource *>(data)->spec.width;
	};
	info.get_height = [](void *data) {
		return static_cast<TestSource *>(data)->spec.height;
	};
	info.video_render = [](void *data, gs_effect_t *effect) {
		auto *source = static_cast<TestSource *>(data);
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);
		gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), source->texture);
		gs_draw_sprite(source->texture, 0, source->spec.width, source->spec.height);
		gs_enable_framebuffer_srgb(previous);
	};
	info.video_get_color_space = [](void *data, size_t, const gs_color_space *) {
		return static_cast<TestSource *>(data)->spec.space;
	};
	obs_register_source(&info);
}

inline obs_source_t *create_test_source(const SourceSpec &spec, const char *name = "source")
{
	next_spec = spec;
	return obs_source_create(kTestSourceId, name, nullptr, nullptr);
}

inline obs_data_t *crunch_settings(int crunch)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "crunch", crunch);
	return settings;
}

// 色が x と y で変わる不透明なグラデーション
inline void paint_gradient(Image &image)
{
	for (uint32_t y = 0; y < image.height; ++y) {
		for (uint32_t x = 0; x < image.width; ++x) {
			uint8_t *p = image.at(x, y);
			p[0] = static_cast<uint8_t>((x * 255) / std::max<uint32_t>(1, image.width - 1));
			p[1] = static_cast<uint8_t>((y * 255) / std::max<uint32_t>(1, image.height - 1));
			p[2] = static_cast<uint8_t>(((x + y) * 7) % 256);
			p[3] = 255;
		}
	}
}

// 写真に近い要素を持つ不透明な絵。JPEG の劣化が目立つ要素として、なめらかな色の変化、色の境界、細い線、
// 細かい模様を含む。大きさに合わせて拡大縮小する
inline void paint_scene(Image &image)
{
	const float w = static_cast<float>(image.width);
	const float h = static_cast<float>(image.height);
	for (uint32_t y = 0; y < image.height; ++y) {
		for (uint32_t x = 0; x < image.width; ++x) {
			const float u = (static_cast<float>(x) + 0.5f) / w;
			const float v = (static_cast<float>(y) + 0.5f) / h;
			// 空のような縦のグラデーション
			float r = 70.0f + 120.0f * v;
			float g = 130.0f + 80.0f * v;
			float b = 230.0f - 60.0f * v;
			// 赤い円 (色の境界と色差の間引きが目立つ)
			const float dx = (u - 0.3f) * w / h;
			const float dy = v - 0.55f;
			if (dx * dx + dy * dy < 0.06f) {
				r = 220.0f;
				g = 40.0f + 60.0f * v;
				b = 50.0f;
			}
			// 緑の地面と細かい縞 (高い周波数)
			if (v > 0.75f) {
				const float stripe = std::sin(u * w * 0.9f) * 25.0f;
				r = 60.0f + stripe;
				g = 150.0f + stripe;
				b = 60.0f;
			}
			// 斜めの細い黒い線 (輪郭のノイズが目立つ)
			const float line = std::fabs((u - 0.55f) * w - (v - 0.1f) * h * 0.6f);
			if (line < 1.0f && v < 0.75f) {
				r = g = b = 20.0f;
			}
			// 白い四角の中の文字のような市松模様
			if (u > 0.65f && u < 0.9f && v > 0.2f && v < 0.45f) {
				const bool dark = ((x / 2) + (y / 3)) % 3 == 0;
				r = g = b = dark ? 30.0f : 245.0f;
			}
			uint8_t *p = image.at(x, y);
			p[0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
			p[1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
			p[2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
			p[3] = 255;
		}
	}
}

// アルファが横方向に 0 から 255 まで変わる絵。色は paint_scene と同じ
inline void paint_translucent(Image &image)
{
	paint_scene(image);
	for (uint32_t y = 0; y < image.height; ++y) {
		for (uint32_t x = 0; x < image.width; ++x) {
			image.at(x, y)[3] = static_cast<uint8_t>((x * 255) / std::max<uint32_t>(1, image.width - 1));
		}
	}
}

// 左半分が paint_scene の不透明な絵、右半分が完全な透明 (色は黒) の絵
inline void paint_half_transparent(Image &image)
{
	paint_scene(image);
	for (uint32_t y = 0; y < image.height; ++y) {
		for (uint32_t x = image.width / 2; x < image.width; ++x) {
			uint8_t *p = image.at(x, y);
			p[0] = p[1] = p[2] = p[3] = 0;
		}
	}
}

// 非圧縮の 24 ビットか 32 ビットの BMP を読む。失敗したときは空の画像を返す
inline Image read_bmp(const std::string &path)
{
	Image image;
	std::ifstream file(path, std::ios::binary);
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	const auto get32 = [&bytes](size_t offset) {
		uint32_t value = 0;
		for (int i = 0; i < 4; ++i) {
			value |= static_cast<uint32_t>(bytes[offset + i]) << (8 * i);
		}
		return value;
	};
	if (bytes.size() < 54 || bytes[0] != 'B' || bytes[1] != 'M') {
		return image;
	}
	const uint32_t offset = get32(10);
	const auto width = static_cast<int32_t>(get32(18));
	const auto raw_height = static_cast<int32_t>(get32(22));
	const uint32_t bpp = bytes[28] | (bytes[29] << 8);
	const uint32_t compression = get32(30);
	if (width <= 0 || raw_height == 0 || (bpp != 24 && bpp != 32) || (compression != 0 && compression != 3)) {
		return image;
	}
	const bool bottom_up = raw_height > 0;
	const auto height = static_cast<uint32_t>(bottom_up ? raw_height : -raw_height);
	const uint32_t pixel_bytes = bpp / 8;
	const uint32_t row_size = (static_cast<uint32_t>(width) * pixel_bytes + 3) & ~3u;
	if (bytes.size() < offset + static_cast<size_t>(row_size) * height) {
		return image;
	}
	image.width = static_cast<uint32_t>(width);
	image.height = height;
	image.rgba.resize(static_cast<size_t>(image.width) * height * 4);
	for (uint32_t y = 0; y < height; ++y) {
		const uint32_t row = bottom_up ? height - 1 - y : y;
		const uint8_t *src = &bytes[offset + static_cast<size_t>(row) * row_size];
		for (uint32_t x = 0; x < image.width; ++x) {
			uint8_t *p = image.at(x, y);
			p[0] = src[x * pixel_bytes + 2];
			p[1] = src[x * pixel_bytes + 1];
			p[2] = src[x * pixel_bytes + 0];
			p[3] = 255;
		}
	}
	return image;
}

// OBS の描画スレッドがフレームを count 個進めるまで待つ。設定の変更は次のフレームまでに反映される
inline bool wait_frames(uint64_t count)
{
	const uint64_t target = obs_get_total_frames() + count;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (obs_get_total_frames() < target) {
		if (std::chrono::steady_clock::now() > deadline) {
			std::cerr << "OBS graphics thread did not advance within five seconds\n";
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return true;
}

inline void mark_updated(void *data, calldata_t *)
{
	static_cast<std::atomic_bool *>(data)->store(true);
}

// 設定を渡し、OBS が update を呼ぶまで最大 2 秒待つ
inline bool update_and_wait(obs_source_t *source, obs_data_t *settings)
{
	std::atomic_bool applied = false;
	signal_handler_t *signals = obs_source_get_signal_handler(source);
	signal_handler_connect(signals, "update", mark_updated, &applied);
	obs_source_update(source, settings);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!applied.load() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	signal_handler_disconnect(signals, "update", mark_updated, &applied);
	return applied.load();
}

// ソースの出力を、ソースの大きさのまま読み出す。合成せずに出力をそのまま書き込む
inline bool render(obs_source_t *source, Image &image)
{
	bool ok = false;
	obs_enter_graphics();
	image.width = obs_source_get_width(source);
	image.height = obs_source_get_height(source);
	gs_texrender_t *output = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *staging = gs_stagesurface_create(image.width, image.height, GS_RGBA);
	if (output && staging && gs_texrender_begin(output, image.width, image.height)) {
		vec4 clear = {};
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(image.width), 0.0f, static_cast<float>(image.height), -100.0f,
			 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_video_render(source);
		gs_blend_state_pop();
		gs_texrender_end(output);
		gs_stage_texture(staging, gs_texrender_get_texture(output));
		uint8_t *mapped = nullptr;
		uint32_t stride = 0;
		if (gs_stagesurface_map(staging, &mapped, &stride)) {
			image.rgba.resize(static_cast<size_t>(image.width) * image.height * 4);
			for (uint32_t y = 0; y < image.height; ++y) {
				std::copy(mapped + static_cast<size_t>(y) * stride,
					  mapped + static_cast<size_t>(y) * stride + image.width * 4,
					  image.rgba.begin() + static_cast<size_t>(y) * image.width * 4);
			}
			gs_stagesurface_unmap(staging);
			ok = true;
		}
	}
	if (!ok) {
		std::cerr << "failed to render the source\n";
	}
	gs_stagesurface_destroy(staging);
	gs_texrender_destroy(output);
	obs_leave_graphics();
	return ok;
}

// 24 ビットの BMP を書き出す。pixel(x, y) は上から数えた位置の RGB を返す
template<typename PixelFn>
inline bool write_bmp(const std::string &path, uint32_t width, uint32_t height, PixelFn pixel)
{
	const uint32_t row_size = (width * 3 + 3) & ~3u;
	const uint32_t data_size = row_size * height;
	uint8_t header[54] = {'B', 'M'};
	const auto put32 = [&header](size_t offset, uint32_t value) {
		for (int i = 0; i < 4; ++i) {
			header[offset + i] = static_cast<uint8_t>(value >> (8 * i));
		}
	};
	put32(2, 54 + data_size);
	put32(10, 54);
	put32(14, 40);
	put32(18, width);
	put32(22, height);
	header[26] = 1;
	header[28] = 24;
	put32(34, data_size);
	std::ofstream file(path, std::ios::binary);
	if (!file) {
		return false;
	}
	file.write(reinterpret_cast<const char *>(header), sizeof(header));
	std::vector<uint8_t> row(row_size, 0);
	// BMP は下の行から並べる。各画素は B・G・R の順
	for (uint32_t y = height; y-- > 0;) {
		for (uint32_t x = 0; x < width; ++x) {
			const auto rgb = pixel(x, y);
			row[x * 3 + 0] = rgb[2];
			row[x * 3 + 1] = rgb[1];
			row[x * 3 + 2] = rgb[0];
		}
		file.write(reinterpret_cast<const char *>(row.data()), row.size());
	}
	return static_cast<bool>(file);
}

// 環境変数 JPEG_CRUNCH_TEST_OUTPUT_DIR があるときだけ、目視の確認のために出力を白い背景に合成して書き出す
inline void save_image(const std::string &name, const Image &image)
{
	const char *dir = std::getenv("JPEG_CRUNCH_TEST_OUTPUT_DIR");
	if (!dir || image.rgba.empty()) {
		return;
	}
	const std::string path = std::string(dir) + "/" + name + ".bmp";
	// 色はアルファを掛けた値のため、白い背景には (1 - アルファ) 分の白を足す
	const bool ok = write_bmp(path, image.width, image.height, [&image](uint32_t x, uint32_t y) {
		const uint8_t *p = image.at(x, y);
		std::array<uint8_t, 3> rgb;
		for (int c = 0; c < 3; ++c) {
			rgb[c] = static_cast<uint8_t>(std::min(255, p[c] + (255 - p[3])));
		}
		return rgb;
	});
	std::cout << (ok ? "saved " : "failed to save ") << path << '\n';
}

// 失敗した検査を数えて報告する。同じ種類の失敗が大量に出ても読めるよう、表示は先頭の 20 件に絞る
struct Checker {
	int failures = 0;
	int printed = 0;

	void expect(bool condition, const std::string &message)
	{
		if (!condition) {
			if (printed < 20) {
				std::cerr << message << '\n';
				++printed;
			}
			++failures;
		}
	}
};

} // namespace jpeg_crunch_test
