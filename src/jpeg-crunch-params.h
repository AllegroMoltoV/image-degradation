#pragma once

// JPEG の劣化の数値の定義。フィルターと、テストの CPU 参照の両方が使う。
// 両者が同じ表と同じ写像を使わないと、テストが実装の誤りではなく定義の食い違いで失敗するため、ここに一本化する。

#include <algorithm>
#include <array>
#include <cmath>

namespace jpeg_crunch {

constexpr int kMinCrunch = 0;
constexpr int kMaxCrunch = 100;
constexpr int kDefaultCrunch = 50;
// 保存し直しの回数の上限。1920x1080 の GPU 時間を測ってから確定する
constexpr int kMaxPasses = 8;

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

// 回ごとの格子のずらし量 (px)。色差は半分の解像度で処理するため偶数にし、色差の格子も整数の画素でずれるようにする。
// 1 回目は 0 にして、1 回だけのときは通常の JPEG と同じ格子にする
constexpr std::array<int, kMaxPasses> kShifts = {0, 6, 10, 4, 14, 2, 12, 8};
// 中間のテクスチャに足す余白 (px)。ずらし量の最大値以上にする
constexpr int kMaxShift = 14;

struct Settings {
	int quality = 0; // JPEG の品質 1-100。0 は処理しないことを示す
	int passes = 0;  // 保存し直しの回数
};

// ガビガビ度 (0-100) を品質と回数へ写す。0 は処理しない。1 で品質 90 の 1 回、100 で品質 5 の kMaxPasses 回
inline Settings settings_for(int crunch)
{
	crunch = std::clamp(crunch, kMinCrunch, kMaxCrunch);
	if (crunch == 0) {
		return {};
	}
	const int step = crunch - 1;
	Settings s;
	s.quality = 90 - (step * 85 + 49) / 99;
	s.passes = 1 + step * (kMaxPasses - 1) / 99;
	return s;
}

// libjpeg の jpeg_quality_scaling と同じ式で、表を品質に合わせて拡大縮小する
inline std::array<float, 64> scaled_table(const std::array<int, 64> &base, int quality)
{
	quality = std::clamp(quality, 1, 100);
	const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
	std::array<float, 64> table{};
	for (size_t i = 0; i < base.size(); ++i) {
		table[i] = static_cast<float>(std::clamp((base[i] * scale + 50) / 100, 1, 255));
	}
	return table;
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

// 中間のテクスチャの大きさ。ずらした格子の端のブロックの係数も置けるよう、余白を足して 8 の倍数に切り上げる
inline int padded_size(int size)
{
	return (size + kMaxShift + 7) / 8 * 8;
}

} // namespace jpeg_crunch
