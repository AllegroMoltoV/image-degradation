#pragma once

// JPEG の劣化の数値の定義。フィルターと、テストの CPU 参照の両方が使う。
// 両者が同じ表と同じ写像を使わないと、テストが実装の誤りではなく定義の食い違いで失敗するため、ここに一本化する。
// 劣化は JPEG の計算を近似したもので、軽さと崩れ具合を優先する。近似の内容は jpeg-crunch.effect の冒頭にある。

#include <algorithm>
#include <array>
#include <cmath>

namespace jpeg_crunch {

constexpr int kMinCrunch = 0;
constexpr int kMaxCrunch = 100;
constexpr int kDefaultCrunch = 50;
// 保存し直しの回数の上限
constexpr int kMaxPasses = 4;

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

// 回ごとの格子のずらし量 (処理の解像度での px)。色差は 2x2 の画素で同じ値にするため偶数にし、色差の 2x2 の区切りを保つ。
// 回数は解像度の縮小率と同じで、最大 4 回
// 1 回目は 0 にして、1 回だけのときは通常の JPEG と同じ格子にする
constexpr std::array<int, kMaxPasses> kShifts = {0, 6, 10, 4};
// 中間のテクスチャに足す余白 (px)。ずらし量の最大値以上にする
constexpr int kMaxShift = 10;

struct Settings {
	int quality = 0;         // JPEG の品質 1-100。0 は処理しないことを示す
	int passes = 0;          // 保存し直しの回数
	int scale = 1;           // 処理の解像度を 1/scale にする (1-4)。ブロックが 8 x scale px に見える
	float multiplier = 1.0f; // 量子化の表に掛ける倍率。JPEG の上限 255 を超えて粗くする
};

// ガビガビ度 (0-100) を写す。0 は処理しない。
// 崩れ方の感じ方は、低い範囲で鈍く高い範囲で急になりやすい。そこでガビガビ度を対数の曲線
// t = log(1 + 9g/100) / log(10) で 0-1 に写し、低い範囲ほど変化を大きくする。g = 10 で t = 0.28、30 で 0.57、50 で 0.74。
// GPU の負荷は、処理の解像度での画素の数と回数の積で決まるため、回数は解像度を下げる範囲で増やす。
// - 品質: t = 0 の 90 から t = 0.5 の 5 へ下げ、以降は 5
// - 倍率: t = 0.5 までは 1、以降は t = 1 の 10 へ上げる
// - 解像度と回数: t < 0.6 で等倍・1 回、0.8 未満で 1/2・2 回、0.92 未満で 1/3・3 回、以降は 1/4・4 回
inline Settings settings_for(int crunch)
{
	crunch = std::clamp(crunch, kMinCrunch, kMaxCrunch);
	if (crunch == 0) {
		return {};
	}
	const double t = std::log(1.0 + 9.0 * crunch / 100.0) / std::log(10.0);
	Settings s;
	s.quality = 90 - static_cast<int>(std::lround(85.0 * std::min(t / 0.5, 1.0)));
	s.multiplier = static_cast<float>(1.0 + 9.0 * std::max((t - 0.5) / 0.5, 0.0));
	s.scale = t < 0.6 ? 1 : (t < 0.8 ? 2 : (t < 0.92 ? 3 : 4));
	s.passes = s.scale;
	return s;
}

// libjpeg の jpeg_quality_scaling と同じ式で、表を品質に合わせて拡大縮小し、倍率を掛ける
inline std::array<float, 64> scaled_table(const std::array<int, 64> &base, int quality, float multiplier)
{
	quality = std::clamp(quality, 1, 100);
	const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
	std::array<float, 64> table{};
	for (size_t i = 0; i < base.size(); ++i) {
		table[i] = static_cast<float>(std::clamp((base[i] * scale + 50) / 100, 1, 255)) * multiplier;
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

// DCT の中間のテクスチャの大きさ。ずらした格子の端のブロックの係数も置けるよう、余白を足して 8 の倍数に切り上げる
inline int padded_size(int size)
{
	return (size + kMaxShift + 7) / 8 * 8;
}

} // namespace jpeg_crunch
