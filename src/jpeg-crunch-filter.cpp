// JPEG で何度も保存し直したように映像を劣化させる映像フィルター。
// ガビガビ度 (0-100) を、処理の解像度・JPEG の品質・量子化の倍率・保存し直しの回数へ写す。YCbCr への変換・8x8 の DCT・
// 量子化・逆 DCT を近似して GPU のパスで繰り返す。回ごとに 8x8 の格子をずらし、同じ格子で繰り返したときの劣化の頭打ちを避ける。

#include "jpeg-crunch-params.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <array>
#include <cstdint>
#include <new>

namespace {

constexpr const char *kCrunch = "crunch";

// エフェクトのパラメーター。名前は jpeg-crunch.effect の uniform と一致させる
enum Param {
	kParamImage,
	kParamWork,
	kParamCoef,
	kParamBasis,
	kParamQtable,
	kParamImageSize,
	kParamWorkSize,
	kParamTargetSize,
	kParamScale,
	kParamShift,
	kParamCount,
};

constexpr std::array<const char *, kParamCount> kParamNames = {
	"image", "work", "coef", "basis", "qtable", "image_size", "work_size", "target_size", "scale", "shift",
};

constexpr std::array<const char *, 6> kTechniques = {"Capture", "RowDCT", "ColQuant", "ColIDCT", "Compose", "Output"};

// フィルターが受け付ける色空間。sharpness-filter.c と同じく、HDR の 709_EXTENDED では処理を飛ばす
constexpr std::array<gs_color_space, 3> kSpaces = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};

class JpegCrunchFilter {
public:
	JpegCrunchFilter(obs_data_t *settings, obs_source_t *context) : context_(context)
	{
		obs_enter_graphics();
		char *path = obs_module_file("effects/jpeg-crunch.effect");
		if (path) {
			effect_ = gs_effect_create_from_file(path, nullptr);
			bfree(path);
		}
		if (effect_) {
			for (size_t i = 0; i < params_.size(); ++i) {
				params_[i] = gs_effect_get_param_by_name(effect_, kParamNames[i]);
			}
		}
		// 符号化した RGB は 0-255 の整数のため、16 ビットの浮動小数点で誤差なく持てる
		work_[0] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
		work_[1] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
		// DCT の途中の値は数百に達し、16 ビットの浮動小数点では刻みが量子化の丸めを狂わせるため 32 ビットで持つ。
		// Y・Cb・Cr を 1 枚にまとめ、パスの数を減らす
		coef_[0] = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
		coef_[1] = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
		// DCT の基底を、8 値を 2 回の読み込みで得られる 4 x 8 の RGBA に並べる。
		// (0, u) と (1, u) に順変換の B(u, 0..7)、(2, x) と (3, x) に逆変換の B(0..7, x) を置く
		const std::array<float, 64> basis = jpeg_crunch::dct_basis();
		std::array<float, 128> layout{};
		for (int row = 0; row < 8; ++row) {
			for (int k = 0; k < 8; ++k) {
				layout[size_t(row) * 16 + size_t(k)] = basis[size_t(row) * 8 + size_t(k)];
				layout[size_t(row) * 16 + 8 + size_t(k)] = basis[size_t(k) * 8 + size_t(row)];
			}
		}
		const auto *basis_data = reinterpret_cast<const uint8_t *>(layout.data());
		basis_ = gs_texture_create(4, 8, GS_RGBA32F, 1, &basis_data, 0);
		obs_leave_graphics();
		if (!Ready()) {
			obs_log(LOG_ERROR, "[jpeg-crunch] failed to load effects/jpeg-crunch.effect");
		}
		Update(settings);
	}

	~JpegCrunchFilter()
	{
		obs_enter_graphics();
		gs_texture_destroy(qtable_);
		gs_texture_destroy(basis_);
		for (gs_texrender_t *render : {work_[0], work_[1], coef_[0], coef_[1], source_}) {
			gs_texrender_destroy(render);
		}
		gs_effect_destroy(effect_);
		obs_leave_graphics();
	}

	// OBS は映像のフィルターの update を描画スレッドで、描画の前に呼ぶ。そのため描画と同じスレッドで値を読み書きできる
	void Update(obs_data_t *settings)
	{
		const auto crunch = static_cast<int>(obs_data_get_int(settings, kCrunch));
		const jpeg_crunch::Settings next = jpeg_crunch::settings_for(crunch);
		if (next.quality != settings_.quality || next.multiplier != settings_.multiplier) {
			qtable_dirty_ = true;
		}
		settings_ = next;
	}

	void Render()
	{
		obs_source_t *target = obs_filter_get_target(context_);
		if (!Ready() || !target || settings_.passes == 0) {
			obs_source_skip_video_filter(context_);
			return;
		}
		const gs_color_space space = obs_source_get_color_space(target, kSpaces.size(), kSpaces.data());
		const uint32_t width = obs_source_get_base_width(target);
		const uint32_t height = obs_source_get_base_height(target);
		if (space == GS_CS_709_EXTENDED || width == 0 || height == 0 || !UpdateQtable()) {
			obs_source_skip_video_filter(context_);
			return;
		}
		gs_texture_t *source = RenderTarget(space, width, height);
		if (!source) {
			obs_source_skip_video_filter(context_);
			return;
		}

		const int scale = settings_.scale;
		const auto work_w = static_cast<uint32_t>(jpeg_crunch::scaled_size(static_cast<int>(width), scale));
		const auto work_h = static_cast<uint32_t>(jpeg_crunch::scaled_size(static_cast<int>(height), scale));
		const auto coef_w = static_cast<uint32_t>(jpeg_crunch::padded_size(static_cast<int>(work_w)));
		const auto coef_h = static_cast<uint32_t>(jpeg_crunch::padded_size(static_cast<int>(work_h)));

		// gs_technique_end はパスのたびにパラメーターの値を消すため、パスの直前に毎回すべて設定する
		struct Inputs {
			gs_texture_t *work = nullptr;
			gs_texture_t *coef = nullptr;
			float shift = 0.0f;
		};
		const auto set_params = [&](const Inputs &in, uint32_t cx, uint32_t cy) {
			// 対象の色はアルファを掛けた線形の値で読む。8 ビットの対象は sRGB の符号化を解いて読み、16 ビットの対象はそのまま読む
			gs_effect_set_texture_srgb(params_[kParamImage], source);
			gs_effect_set_texture(params_[kParamWork], in.work);
			gs_effect_set_texture(params_[kParamCoef], in.coef);
			gs_effect_set_texture(params_[kParamBasis], basis_);
			gs_effect_set_texture(params_[kParamQtable], qtable_);
			vec2 size;
			vec2_set(&size, static_cast<float>(width), static_cast<float>(height));
			gs_effect_set_vec2(params_[kParamImageSize], &size);
			vec2_set(&size, static_cast<float>(work_w), static_cast<float>(work_h));
			gs_effect_set_vec2(params_[kParamWorkSize], &size);
			vec2_set(&size, static_cast<float>(cx), static_cast<float>(cy));
			gs_effect_set_vec2(params_[kParamTargetSize], &size);
			gs_effect_set_float(params_[kParamScale], static_cast<float>(scale));
			gs_effect_set_float(params_[kParamShift], in.shift);
		};
		const auto pass = [&](gs_texrender_t *output, const char *technique, const Inputs &in, uint32_t cx,
				      uint32_t cy) -> gs_texture_t * {
			set_params(in, cx, cy);
			return DrawPass(output, technique, cx, cy);
		};

		gs_texture_t *work = pass(work_[0], "Capture", {}, work_w, work_h);
		for (int i = 0; work && i < settings_.passes; ++i) {
			const auto shift = static_cast<float>(jpeg_crunch::kShifts[i]);
			gs_texture_t *coef = pass(coef_[0], "RowDCT", {work, nullptr, shift}, coef_w, coef_h);
			coef = coef ? pass(coef_[1], "ColQuant", {nullptr, coef, shift}, coef_w, coef_h) : nullptr;
			coef = coef ? pass(coef_[0], "ColIDCT", {nullptr, coef, shift}, coef_w, coef_h) : nullptr;
			// 読み書きする texrender を分けるため、回ごとに work の出力先を入れ替える
			work = coef ? pass(work_[(i + 1) % 2], "Compose", {nullptr, coef, shift}, work_w, work_h)
				    : nullptr;
		}
		if (!work) {
			obs_source_skip_video_filter(context_);
			return;
		}

		// 線形の値を出力し、8 ビットの出力先では書き込み時に sRGB へ符号化させる (gpu-delay.c と同じ)。
		// 色はアルファを掛けた値のため、そのまま重ねる
		const bool previous_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		set_params({work, nullptr, 0.0f}, width, height);
		while (gs_effect_loop(effect_, "Output")) {
			gs_draw_sprite(nullptr, 0, width, height);
		}
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(previous_srgb);
	}

	static gs_color_space ColorSpace(void *data, size_t, const gs_color_space *)
	{
		auto *filter = static_cast<JpegCrunchFilter *>(data);
		obs_source_t *target = obs_filter_get_target(filter->context_);
		return target ? obs_source_get_color_space(target, kSpaces.size(), kSpaces.data()) : GS_CS_SRGB;
	}

	static const char *GetName(void *) { return obs_module_text("SimpleJpegCrunch"); }

	static void *Create(obs_data_t *settings, obs_source_t *context)
	{
		return new (std::nothrow) JpegCrunchFilter(settings, context);
	}

	static void Destroy(void *data) { delete static_cast<JpegCrunchFilter *>(data); }

	static void UpdateCallback(void *data, obs_data_t *settings)
	{
		static_cast<JpegCrunchFilter *>(data)->Update(settings);
	}

	static void Defaults(obs_data_t *settings)
	{
		obs_data_set_default_int(settings, kCrunch, jpeg_crunch::kDefaultCrunch);
	}

	static obs_properties_t *Properties(void *)
	{
		obs_properties_t *props = obs_properties_create();
		obs_properties_add_int_slider(props, kCrunch, obs_module_text("Crunch"), jpeg_crunch::kMinCrunch,
					      jpeg_crunch::kMaxCrunch, 1);
		return props;
	}

	static void RenderCallback(void *data, gs_effect_t *) { static_cast<JpegCrunchFilter *>(data)->Render(); }

private:
	// 設定済みのパラメーターで technique を描き、output (cx x cy) に書く。途中の値を壊さないよう、混ぜずに上書きする
	gs_texture_t *DrawPass(gs_texrender_t *output, const char *technique, uint32_t cx, uint32_t cy)
	{
		gs_texrender_reset(output);
		if (!gs_texrender_begin(output, cx, cy)) {
			return nullptr;
		}
		vec4 clear = {};
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(cx), 0.0f, static_cast<float>(cy), -100.0f, 100.0f);
		gs_blend_state_push();
		gs_enable_blending(false);
		while (gs_effect_loop(effect_, technique)) {
			gs_draw_sprite(nullptr, 0, cx, cy);
		}
		gs_blend_state_pop();
		gs_texrender_end(output);
		return gs_texrender_get_texture(output);
	}

	// 対象を、対象の色空間に合う形式の中間のテクスチャへ描く。色はアルファを掛けた値になる。
	// OBS の obs_source_process_filter_begin_with_color_space と同じ手順
	gs_texture_t *RenderTarget(gs_color_space space, uint32_t width, uint32_t height)
	{
		obs_source_t *target = obs_filter_get_target(context_);
		obs_source_t *parent = obs_filter_get_parent(context_);
		if (!target || !parent) {
			return nullptr;
		}
		const gs_color_format format = gs_get_format_from_space(space);
		if (!source_ || gs_texrender_get_format(source_) != format) {
			gs_texrender_destroy(source_);
			source_ = gs_texrender_create(format, GS_ZS_NONE);
		}
		gs_texrender_reset(source_);
		if (!source_ || !gs_texrender_begin_with_color_space(source_, width, height, space)) {
			return nullptr;
		}
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		vec4 clear = {};
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);
		const uint32_t parent_flags = obs_source_get_output_flags(parent);
		if (target == parent && !(parent_flags & OBS_SOURCE_CUSTOM_DRAW) &&
		    !(parent_flags & OBS_SOURCE_ASYNC)) {
			obs_source_default_render(target);
		} else {
			obs_source_video_render(target);
		}
		gs_blend_state_pop();
		gs_texrender_end(source_);
		return gs_texrender_get_texture(source_);
	}

	// 品質が変わったときだけ量子化テーブルのテクスチャを作り直す。上の 8 行が輝度、下の 8 行が色差
	bool UpdateQtable()
	{
		if (!qtable_dirty_ && qtable_) {
			return true;
		}
		std::array<float, 128> table{};
		const auto luma =
			jpeg_crunch::scaled_table(jpeg_crunch::kLumaTable, settings_.quality, settings_.multiplier);
		const auto chroma =
			jpeg_crunch::scaled_table(jpeg_crunch::kChromaTable, settings_.quality, settings_.multiplier);
		std::copy(luma.begin(), luma.end(), table.begin());
		std::copy(chroma.begin(), chroma.end(), table.begin() + 64);
		const auto *data = reinterpret_cast<const uint8_t *>(table.data());
		gs_texture_destroy(qtable_);
		qtable_ = gs_texture_create(8, 16, GS_R32F, 1, &data, 0);
		qtable_dirty_ = qtable_ == nullptr;
		return qtable_ != nullptr;
	}

	bool Ready() const
	{
		if (!effect_ || !basis_) {
			return false;
		}
		for (gs_eparam_t *param : params_) {
			if (!param) {
				return false;
			}
		}
		for (const char *technique : kTechniques) {
			if (!gs_effect_get_technique(effect_, technique)) {
				return false;
			}
		}
		return work_[0] && work_[1] && coef_[0] && coef_[1];
	}

	obs_source_t *context_ = nullptr;
	gs_effect_t *effect_ = nullptr;
	std::array<gs_eparam_t *, kParamCount> params_{};
	gs_texrender_t *source_ = nullptr;
	std::array<gs_texrender_t *, 2> work_{};
	std::array<gs_texrender_t *, 2> coef_{};
	gs_texture_t *basis_ = nullptr;
	gs_texture_t *qtable_ = nullptr;
	bool qtable_dirty_ = true;
	jpeg_crunch::Settings settings_;
};

} // namespace

void register_jpeg_crunch_filter(void)
{
	static obs_source_info info = {};
	info.id = "simple_jpeg_crunch_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = JpegCrunchFilter::GetName;
	info.create = JpegCrunchFilter::Create;
	info.destroy = JpegCrunchFilter::Destroy;
	info.update = JpegCrunchFilter::UpdateCallback;
	info.get_defaults = JpegCrunchFilter::Defaults;
	info.get_properties = JpegCrunchFilter::Properties;
	info.video_render = JpegCrunchFilter::RenderCallback;
	info.video_get_color_space = JpegCrunchFilter::ColorSpace;
	obs_register_source(&info);
}
