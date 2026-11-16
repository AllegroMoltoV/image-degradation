// JPEG で何度も保存し直したように映像を劣化させる映像フィルター。
// ガビガビ度 (0-100) を JPEG の品質と保存し直しの回数へ写し、YCbCr への変換・色差の間引き・8x8 の DCT・量子化・逆 DCT を
// GPU のパスで繰り返す。回ごとに 8x8 の格子をずらし、同じ格子で繰り返したときの劣化の頭打ちを避ける。

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
	kParamLuma,
	kParamChroma,
	kParamBasis,
	kParamQtable,
	kParamImageSize,
	kParamChromaSize,
	kParamTargetSize,
	kParamShift,
	kParamCount,
};

constexpr std::array<const char *, kParamCount> kParamNames = {
	"image", "work", "luma", "chroma", "basis", "qtable", "image_size", "chroma_size", "target_size", "shift",
};

constexpr std::array<const char *, 9> kTechniques = {
	"Capture",     "LumaRow",      "LumaQuant", "LumaInvCol", "ChromaRow",
	"ChromaQuant", "ChromaInvCol", "Compose",   "Output",
};

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
		// DCT の途中の値は数百に達し、16 ビットの浮動小数点では刻みが量子化の丸めを狂わせるため 32 ビットで持つ
		luma_[0] = gs_texrender_create(GS_R32F, GS_ZS_NONE);
		luma_[1] = gs_texrender_create(GS_R32F, GS_ZS_NONE);
		chroma_[0] = gs_texrender_create(GS_RG32F, GS_ZS_NONE);
		chroma_[1] = gs_texrender_create(GS_RG32F, GS_ZS_NONE);
		const std::array<float, 64> basis = jpeg_crunch::dct_basis();
		const auto *basis_data = reinterpret_cast<const uint8_t *>(basis.data());
		basis_ = gs_texture_create(8, 8, GS_R32F, 1, &basis_data, 0);
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
		for (gs_texrender_t *render :
		     {work_[0], work_[1], luma_[0], luma_[1], chroma_[0], chroma_[1], source_}) {
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
		if (next.quality != settings_.quality) {
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

		const auto w = static_cast<float>(width);
		const auto h = static_cast<float>(height);
		const uint32_t chroma_w = (width + 1) / 2;
		const uint32_t chroma_h = (height + 1) / 2;
		const auto luma_pw = static_cast<uint32_t>(jpeg_crunch::padded_size(static_cast<int>(width)));
		const auto luma_ph = static_cast<uint32_t>(jpeg_crunch::padded_size(static_cast<int>(height)));
		const auto chroma_pw = static_cast<uint32_t>(jpeg_crunch::padded_size(static_cast<int>(chroma_w)));
		const auto chroma_ph = static_cast<uint32_t>(jpeg_crunch::padded_size(static_cast<int>(chroma_h)));

		// gs_technique_end はパスのたびにパラメーターの値を消すため、パスの直前に毎回すべて設定する
		struct Inputs {
			gs_texture_t *work = nullptr;
			gs_texture_t *luma = nullptr;
			gs_texture_t *chroma = nullptr;
			float shift = 0.0f;
		};
		const auto set_params = [&](const Inputs &in, uint32_t cx, uint32_t cy) {
			// 対象の色はアルファを掛けた線形の値で読む。8 ビットの対象は sRGB の符号化を解いて読み、16 ビットの対象はそのまま読む
			gs_effect_set_texture_srgb(params_[kParamImage], source);
			gs_effect_set_texture(params_[kParamWork], in.work);
			gs_effect_set_texture(params_[kParamLuma], in.luma);
			gs_effect_set_texture(params_[kParamChroma], in.chroma);
			gs_effect_set_texture(params_[kParamBasis], basis_);
			gs_effect_set_texture(params_[kParamQtable], qtable_);
			vec2 size;
			vec2_set(&size, w, h);
			gs_effect_set_vec2(params_[kParamImageSize], &size);
			vec2_set(&size, static_cast<float>(chroma_w), static_cast<float>(chroma_h));
			gs_effect_set_vec2(params_[kParamChromaSize], &size);
			vec2_set(&size, static_cast<float>(cx), static_cast<float>(cy));
			gs_effect_set_vec2(params_[kParamTargetSize], &size);
			gs_effect_set_float(params_[kParamShift], in.shift);
		};
		const auto pass = [&](gs_texrender_t *output, const char *technique, const Inputs &in, uint32_t cx,
				      uint32_t cy) -> gs_texture_t * {
			set_params(in, cx, cy);
			return DrawPass(output, technique, cx, cy);
		};

		gs_texture_t *work = pass(work_[0], "Capture", {}, width, height);
		for (int i = 0; work && i < settings_.passes; ++i) {
			const auto shift = static_cast<float>(jpeg_crunch::kShifts[i]);
			gs_texture_t *luma =
				pass(luma_[0], "LumaRow", {work, nullptr, nullptr, shift}, luma_pw, luma_ph);
			luma = luma ? pass(luma_[1], "LumaQuant", {work, luma, nullptr, shift}, luma_pw, luma_ph)
				    : nullptr;
			luma = luma ? pass(luma_[0], "LumaInvCol", {work, luma, nullptr, shift}, luma_pw, luma_ph)
				    : nullptr;
			gs_texture_t *chroma =
				pass(chroma_[0], "ChromaRow", {work, nullptr, nullptr, shift}, chroma_pw, chroma_ph);
			chroma = chroma ? pass(chroma_[1], "ChromaQuant", {work, nullptr, chroma, shift}, chroma_pw,
					       chroma_ph)
					: nullptr;
			chroma = chroma ? pass(chroma_[0], "ChromaInvCol", {work, nullptr, chroma, shift}, chroma_pw,
					       chroma_ph)
					: nullptr;
			// Compose は work を読まないが、読み書きする texrender を分けるため、回ごとに出力先を入れ替える
			work = luma && chroma ? pass(work_[(i + 1) % 2], "Compose", {nullptr, luma, chroma, shift},
						     width, height)
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
		set_params({work, nullptr, nullptr, 0.0f}, width, height);
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
		const auto luma = jpeg_crunch::scaled_table(jpeg_crunch::kLumaTable, settings_.quality);
		const auto chroma = jpeg_crunch::scaled_table(jpeg_crunch::kChromaTable, settings_.quality);
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
		return work_[0] && work_[1] && luma_[0] && luma_[1] && chroma_[0] && chroma_[1];
	}

	obs_source_t *context_ = nullptr;
	gs_effect_t *effect_ = nullptr;
	std::array<gs_eparam_t *, kParamCount> params_{};
	gs_texrender_t *source_ = nullptr;
	std::array<gs_texrender_t *, 2> work_{};
	std::array<gs_texrender_t *, 2> luma_{};
	std::array<gs_texrender_t *, 2> chroma_{};
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
