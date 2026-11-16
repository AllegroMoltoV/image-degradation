#pragma once

// 劣化の数値の定義。フィルターと、テストの CPU 参照の両方が使う。
// 両者が同じ表と同じ写像を使わないと、テストが実装の誤りではなく定義の食い違いで失敗するため、ここに一本化する。
// 劣化は、ネットで転載を重ねた画像の見た目を軽く近似したもの。処理の流れは jpeg-crunch.effect の冒頭にある。

#include <algorithm>
#include <array>
#include <cmath>

namespace jpeg_crunch {

constexpr int kMinCrunch = 0;
constexpr int kMaxCrunch = 100;
constexpr int kDefaultCrunch = 10;

// ITU-T T.81 Annex K の Table K.1 (輝度) と Table K.2 (色差)。行は縦の周波数 v、列は横の周波数 u
constexpr std::array<int, 64> kLumaTable = {
	16, 11, 10, 16, 24,  40,  51,  61,  //
	12, 12, 14, 19, 26,  58,  60,  55,  //
	14, 13, 16, 24, 40,  57,  69,  56,  //
	14, 17, 22, 29, 51,  87,  80,  62,  //
	18, 22, 37, 56, 68,  109, 103, 77,  //
	24, 35, 55, 64, 81,  104, 113, 92,  //
	49, 64, 78, 87, 103, 121, 120, 101, //
	72, 92, 95, 98, 112, 100, 103, 99,
};
constexpr std::array<int, 64> kChromaTable = {
	17, 18, 24, 47, 99, 99, 99, 99, //
	18, 21, 26, 66, 99, 99, 99, 99, //
	24, 26, 56, 99, 99, 99, 99, 99, //
	47, 66, 99, 99, 99, 99, 99, 99, //
	99, 99, 99, 99, 99, 99, 99, 99, //
	99, 99, 99, 99, 99, 99, 99, 99, //
	99, 99, 99, 99, 99, 99, 99, 99, //
	99, 99, 99, 99, 99, 99, 99, 99,
};

struct Settings {
	float quant_scale = 0.0f; // 量子化の表に掛ける倍率 (%)。0 は処理しないことを示す
	int scale = 1;            // 処理の解像度を 1/scale にする (1-4)。戻すときの補間でぼける
	float saturation = 1.0f;  // 彩度の倍率
	float contrast = 1.0f;    // コントラストの倍率 (128 を中心にする)
	float sharpen = 0.0f;     // シャープの強さ。上下左右の平均との差に掛ける
	int color_shift = 0;      // 色のずれ (処理の解像度での px)。R を左、B を右から読む
};

// ガビガビ度 (0-100) を写す。0 は処理しない。
// ガビガビ度を対数の曲線 t = log(1 + 9g/100) / log(10) で 0-1 に写し、低い範囲ほど変化を大きくする。
// g = 10 で t = 0.28、30 で 0.57、50 で 0.74。各要素の強さは t で決める。
// - 量子化の倍率: t = 0 の 20% から t = 1 の 4,000% へ等比で上げる。JPEG の品質の換算式は品質 15 付近から倍率が
//   急に上がり、崩れ方が急に変わって見えるため、品質を経由せずに倍率を直接決める。100% が JPEG の品質 50 に当たる
// - 解像度: t < 0.4 で等倍、0.65 未満で 1/2、0.85 未満で 1/3、以降は 1/4
// - 彩度: 1 から 2 倍、コントラスト: 1 から 1.5 倍、シャープ: 0 から 2
// - 色のずれ: t < 0.4 で 0、0.7 未満で 1 px、0.9 未満で 2 px、以降は 3 px
inline Settings settings_for(int crunch)
{
	crunch = std::clamp(crunch, kMinCrunch, kMaxCrunch);
	if (crunch == 0) {
		return {};
	}
	const double t = std::log(1.0 + 9.0 * crunch / 100.0) / std::log(10.0);
	Settings s;
	s.quant_scale = static_cast<float>(20.0 * std::pow(200.0, t));
	s.scale = t < 0.4 ? 1 : (t < 0.65 ? 2 : (t < 0.85 ? 3 : 4));
	s.saturation = static_cast<float>(1.0 + t);
	s.contrast = static_cast<float>(1.0 + 0.5 * t);
	s.sharpen = static_cast<float>(2.0 * t);
	s.color_shift = t < 0.4 ? 0 : (t < 0.7 ? 1 : (t < 0.9 ? 2 : 3));
	return s;
}

// JPEG の標準の表に倍率 (%) を掛ける。JPEG の上限 255 は超えてよい。最小は 1
inline std::array<float, 64> scaled_table(const std::array<int, 64> &base, float quant_scale)
{
	std::array<float, 64> table{};
	for (size_t i = 0; i < base.size(); ++i) {
		table[i] = std::max(1.0f, std::floor(static_cast<float>(base[i]) * quant_scale / 100.0f + 0.5f));
	}
	return table;
}

// 処理の解像度での大きさ (切り上げ)
inline int scaled_size(int size, int scale)
{
	return (size + scale - 1) / scale;
}

// 8 点の DCT の基底。basis[u * 8 + x] = C(u) / 2 * cos((2x + 1) u pi / 16)。
// 正規直交なので、順変換は F(u) = sum_x basis[u][x] f(x)、逆変換は f(x) = sum_u basis[u][x] F(u) になる
inline std::array<float, 64> dct_basis()
{
	constexpr double kPi = 3.14159265358979323846;
	std::array<float, 64> basis{};
	for (int u = 0; u < 8; ++u) {
		const double c = u == 0 ? 1.0 / std::sqrt(2.0) : 1.0;
		for (int x = 0; x < 8; ++x) {
			basis[u * 8 + x] = static_cast<float>(c / 2.0 * std::cos((2.0 * x + 1.0) * u * kPi / 16.0));
		}
	}
	return basis;
}

// DCT の中間のテクスチャの大きさ。端にかかるブロックの係数も置けるよう、8 の倍数に切り上げる
inline int padded_size(int size)
{
	return (size + 7) / 8 * 8;
}

} // namespace jpeg_crunch
