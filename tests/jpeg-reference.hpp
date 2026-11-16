#pragma once

// フィルターと同じ計算を CPU で行う参照の実装。jpeg-crunch.effect のパスと同じ順序、同じ式、同じ丸めで計算する。
//
// 丸め (floor(x + 0.5)) は、値がちょうど境目にあると、浮動小数点の僅かな誤差で GPU と CPU の結果が分かれる。
// そこで各値に、その値が依存する丸めの判定のうち、境目に最も近いものとの距離 (余裕) を持たせる。
// DCT と逆 DCT は 8 個の値から 1 個を作るため、余裕は 8 個の最小値として伝播する。
// 余裕が小さい画素は、GPU と CPU で結果が分かれうるため比較から外す。

#include "../src/jpeg-crunch-params.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace jpeg_reference {

constexpr float kNoTie = std::numeric_limits<float>::infinity();

// 3 つの成分を持つ平面。余裕は成分で共有する
struct Plane {
	int width = 0;
	int height = 0;
	std::vector<std::array<float, 3>> value;
	std::vector<float> margin;

	Plane(int w, int h)
		: width(w),
		  height(h),
		  value(size_t(w) * h, {0.0f, 0.0f, 0.0f}),
		  margin(size_t(w) * h, kNoTie)
	{
	}

	std::array<float, 3> &at(int x, int y) { return value[size_t(y) * width + x]; }
	const std::array<float, 3> &at(int x, int y) const { return value[size_t(y) * width + x]; }
	float &margin_at(int x, int y) { return margin[size_t(y) * width + x]; }
	float margin_at(int x, int y) const { return margin[size_t(y) * width + x]; }
};

struct Result {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgb; // 符号化した RGB
	std::vector<float> margin;
};

// 丸めの判定 floor(x + 0.5) の、境目 (小数部が 0.5) からの距離を、丸める前の値の単位で返す
inline float tie_distance(float ratio, float unit)
{
	return std::fabs(ratio - std::floor(ratio) - 0.5f) * unit;
}

inline int block_pos(int v)
{
	return v % 8;
}

inline float srgb_decode(float v)
{
	return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

inline float srgb_encode(float v)
{
	return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

inline std::array<float, 3> ycc_of(const std::array<float, 3> &rgb)
{
	return {rgb[0] * 0.299f + rgb[1] * 0.587f + rgb[2] * 0.114f - 128.0f,
		rgb[0] * -0.168736f + rgb[1] * -0.331264f + rgb[2] * 0.5f,
		rgb[0] * 0.5f + rgb[1] * -0.418688f + rgb[2] * -0.081312f};
}

// input は不透明な画像の符号化した RGB (幅 x 高さ x 3)
inline Result run(const std::vector<uint8_t> &input, int width, int height, int crunch)
{
	using namespace jpeg_crunch;
	const Settings settings = settings_for(crunch);
	const std::array<float, 64> basis = dct_basis();
	const auto basis_at = [&basis](int u, int x) {
		return basis[size_t(u) * 8 + x];
	};
	const std::array<float, 64> qluma = scaled_table(kLumaTable, settings.quality, settings.multiplier);
	const std::array<float, 64> qchroma = scaled_table(kChromaTable, settings.quality, settings.multiplier);
	const int scale = settings.passes > 0 ? settings.scale : 1;
	const int work_w = scaled_size(width, scale);
	const int work_h = scaled_size(height, scale);
	const int coef_w = padded_size(work_w);
	const int coef_h = padded_size(work_h);

	// Capture。線形の値で平均してから符号化し、0-255 の整数に丸める
	Plane work(work_w, work_h);
	for (int wy = 0; wy < work_h; ++wy) {
		for (int wx = 0; wx < work_w; ++wx) {
			std::array<float, 3> sum = {0.0f, 0.0f, 0.0f};
			float count = 0.0f;
			for (int dy = 0; dy < 4; ++dy) {
				for (int dx = 0; dx < 4; ++dx) {
					const int x = wx * scale + dx;
					const int y = wy * scale + dy;
					if (dx < scale && dy < scale && x < width && y < height) {
						for (int c = 0; c < 3; ++c) {
							sum[c] += srgb_decode(
								float(input[(size_t(y) * width + x) * 3 + c]) / 255.0f);
						}
						count += 1.0f;
					}
				}
			}
			float margin = kNoTie;
			for (int c = 0; c < 3; ++c) {
				const float encoded = srgb_encode(std::clamp(sum[c] / count, 0.0f, 1.0f)) * 255.0f;
				work.at(wx, wy)[c] = std::floor(encoded + 0.5f);
				margin = std::min(margin, tie_distance(encoded, 1.0f));
			}
			work.margin_at(wx, wy) = margin;
		}
	}

	for (int pass = 0; pass < settings.passes; ++pass) {
		const int shift = kShifts[pass];
		const bool last = pass + 1 == settings.passes;

		// RowDCT
		Plane row(coef_w, coef_h);
		for (int py = 0; py < coef_h; ++py) {
			const int y = std::clamp(py - shift, 0, work_h - 1);
			for (int px = 0; px < coef_w; ++px) {
				const int u = block_pos(px);
				const int b0 = px - u;
				std::array<float, 3> sum = {0.0f, 0.0f, 0.0f};
				float margin = kNoTie;
				for (int i = 0; i < 8; ++i) {
					const int x = std::clamp(b0 + i - shift, 0, work_w - 1);
					const auto ycc = ycc_of(work.at(x, y));
					for (int c = 0; c < 3; ++c) {
						sum[c] += basis_at(u, i) * ycc[c];
					}
					margin = std::min(margin, work.margin_at(x, y));
				}
				row.at(px, py) = sum;
				row.margin_at(px, py) = margin;
			}
		}

		// ColQuant
		Plane col(coef_w, coef_h);
		for (int py = 0; py < coef_h; ++py) {
			const int v0 = block_pos(py);
			const int b0 = py - v0;
			for (int px = 0; px < coef_w; ++px) {
				const int u = block_pos(px);
				float margin = kNoTie;
				for (int k = 0; k < 8; ++k) {
					margin = std::min(margin, row.margin_at(px, b0 + k));
				}
				std::array<float, 3> result = {0.0f, 0.0f, 0.0f};
				for (int v = 0; v < 8; ++v) {
					const float q[3] = {qluma[size_t(v) * 8 + u], qchroma[size_t(v) * 8 + u],
							    qchroma[size_t(v) * 8 + u]};
					for (int c = 0; c < 3; ++c) {
						float sum = 0.0f;
						for (int j = 0; j < 8; ++j) {
							sum += basis_at(v, j) * row.at(px, b0 + j)[c];
						}
						const float ratio = sum / q[c];
						result[c] += basis_at(v, v0) * (std::floor(ratio + 0.5f) * q[c]);
						margin = std::min(margin, tie_distance(ratio, q[c]));
					}
				}
				col.at(px, py) = result;
				col.margin_at(px, py) = margin;
			}
		}

		// Compose
		Plane next(work_w, work_h);
		for (int y = 0; y < work_h; ++y) {
			for (int x = 0; x < work_w; ++x) {
				const int i = x + shift;
				const int j = y + shift;
				const int u0 = block_pos(i);
				const int b0 = i - u0;
				float luma = 128.0f;
				float margin = kNoTie;
				for (int k = 0; k < 8; ++k) {
					luma += basis_at(k, u0) * col.at(b0 + k, j)[0];
					margin = std::min(margin, col.margin_at(b0 + k, j));
				}
				const int ci = x / 2 * 2 + shift;
				const int cj = y / 2 * 2 + shift;
				const int cu = block_pos(ci);
				const int cb0 = ci - cu;
				float cb = 0.0f;
				float cr = 0.0f;
				for (int m = 0; m < 8; ++m) {
					cb += basis_at(m, cu) * col.at(cb0 + m, cj)[1];
					cr += basis_at(m, cu) * col.at(cb0 + m, cj)[2];
					margin = std::min(margin, col.margin_at(cb0 + m, cj));
				}
				const float rgb[3] = {luma + 1.402f * cr, luma - 0.344136f * cb - 0.714136f * cr,
						      luma + 1.772f * cb};
				for (int c = 0; c < 3; ++c) {
					next.at(x, y)[c] = std::clamp(std::floor(rgb[c] + 0.5f), 0.0f, 255.0f);
					// 最後の回の丸めは出力の 1 階調の差にしかならず、比較の許容差に収まる。
					// 途中の回の丸めは次の回の DCT の入力を変え、量子化の結果を分けうるため余裕に数える
					if (!last && rgb[c] > -0.5f && rgb[c] < 255.5f) {
						margin = std::min(margin, tie_distance(rgb[c], 1.0f));
					}
				}
				next.margin_at(x, y) = margin;
			}
		}
		work = std::move(next);
	}

	// Output。最近傍で元の解像度に戻す
	Result result;
	result.width = width;
	result.height = height;
	result.rgb.resize(size_t(width) * height * 3);
	result.margin.resize(size_t(width) * height);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const size_t i = size_t(y) * width + x;
			for (int c = 0; c < 3; ++c) {
				result.rgb[i * 3 + c] = static_cast<uint8_t>(work.at(x / scale, y / scale)[c]);
			}
			result.margin[i] = work.margin_at(x / scale, y / scale);
		}
	}
	return result;
}

} // namespace jpeg_reference
