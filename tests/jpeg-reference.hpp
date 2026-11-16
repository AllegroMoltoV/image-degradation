#pragma once

// フィルターと同じ計算を CPU で行う参照の実装。jpeg-crunch.effect のパスと同じ順序、同じ式、同じ丸めで計算する。
//
// 丸め (floor(x + 0.5)) は、値がちょうど境目にあると、浮動小数点の僅かな誤差で GPU と CPU の結果が分かれる。
// そこで各値に、その値が依存する丸めの判定のうち、境目に最も近いものとの距離 (余裕) を持たせる。
// 1 つの値は周りの複数の値から作るため、余裕はそれらの最小値として伝播する。
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

	// 座標を端へ寄せて読む
	const std::array<float, 3> &clamped(int x, int y) const
	{
		return at(std::clamp(x, 0, width - 1), std::clamp(y, 0, height - 1));
	}
	float clamped_margin(int x, int y) const
	{
		return margin_at(std::clamp(x, 0, width - 1), std::clamp(y, 0, height - 1));
	}
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

inline std::array<float, 3> ycc_of(const std::array<float, 3> &rgb)
{
	return {rgb[0] * 0.299f + rgb[1] * 0.587f + rgb[2] * 0.114f - 128.0f,
		rgb[0] * -0.168736f + rgb[1] * -0.331264f + rgb[2] * 0.5f,
		rgb[0] * 0.5f + rgb[1] * -0.418688f + rgb[2] * -0.081312f};
}

// 0-255 の整数へ丸めて clamp し、丸めの境目からの距離を margin に反映する
inline float round_channel(float v, float &margin)
{
	if (v > -0.5f && v < 255.5f) {
		margin = std::min(margin, tie_distance(v, 1.0f));
	}
	return std::clamp(std::floor(v + 0.5f), 0.0f, 255.0f);
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
	const std::array<float, 64> qluma = scaled_table(kLumaTable, settings.quant_scale);
	const std::array<float, 64> qchroma = scaled_table(kChromaTable, settings.quant_scale);
	const int scale = settings.scale;
	const int work_w = scaled_size(width, scale);
	const int work_h = scaled_size(height, scale);
	const int coef_w = padded_size(work_w);
	const int coef_h = padded_size(work_h);

	// Capture。符号化した 0-255 の整数のまま平均し、整数に丸める。
	// 画素の数が 2 の累乗なら割り算は GPU でも誤差が無いため、丸めの余裕に数えるのはそれ以外のときだけ
	Plane work(work_w, work_h);
	for (int wy = 0; wy < work_h; ++wy) {
		for (int wx = 0; wx < work_w; ++wx) {
			std::array<float, 3> sum = {0.0f, 0.0f, 0.0f};
			int count = 0;
			for (int dy = 0; dy < scale && dy < 4; ++dy) {
				for (int dx = 0; dx < scale && dx < 4; ++dx) {
					const int x = wx * scale + dx;
					const int y = wy * scale + dy;
					if (x < width && y < height) {
						for (int c = 0; c < 3; ++c) {
							sum[c] += float(input[(size_t(y) * width + x) * 3 + c]);
						}
						++count;
					}
				}
			}
			const bool exact = (count & (count - 1)) == 0;
			float margin = kNoTie;
			for (int c = 0; c < 3; ++c) {
				const float average = sum[c] / float(count);
				if (!exact) {
					margin = std::min(margin, tie_distance(average, 1.0f));
				}
				work.at(wx, wy)[c] = std::floor(average + 0.5f);
			}
			work.margin_at(wx, wy) = margin;
		}
	}

	// RowDCT
	Plane row(coef_w, coef_h);
	for (int py = 0; py < coef_h; ++py) {
		for (int px = 0; px < coef_w; ++px) {
			const int u = block_pos(px);
			const int b0 = px - u;
			std::array<float, 3> sum = {0.0f, 0.0f, 0.0f};
			float margin = kNoTie;
			for (int i = 0; i < 8; ++i) {
				const auto ycc = ycc_of(work.clamped(b0 + i, py));
				for (int c = 0; c < 3; ++c) {
					sum[c] += basis_at(u, i) * ycc[c];
				}
				margin = std::min(margin, work.clamped_margin(b0 + i, py));
			}
			row.at(px, py) = sum;
			row.margin_at(px, py) = margin;
		}
	}

	// ColQuant と ColIDCT
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
	Plane composed(work_w, work_h);
	for (int y = 0; y < work_h; ++y) {
		for (int x = 0; x < work_w; ++x) {
			const int u0 = block_pos(x);
			const int b0 = x - u0;
			float luma = 128.0f;
			float margin = kNoTie;
			for (int k = 0; k < 8; ++k) {
				luma += basis_at(k, u0) * col.at(b0 + k, y)[0];
				margin = std::min(margin, col.margin_at(b0 + k, y));
			}
			const int cx = x / 2 * 2;
			const int cy = y / 2 * 2;
			const int cu = block_pos(cx);
			const int cb0 = cx - cu;
			float cb = 0.0f;
			float cr = 0.0f;
			for (int m = 0; m < 8; ++m) {
				cb += basis_at(m, cu) * col.at(cb0 + m, cy)[1];
				cr += basis_at(m, cu) * col.at(cb0 + m, cy)[2];
				margin = std::min(margin, col.margin_at(cb0 + m, cy));
			}
			const float rgb[3] = {luma + 1.402f * cr, luma - 0.344136f * cb - 0.714136f * cr,
					      luma + 1.772f * cb};
			for (int c = 0; c < 3; ++c) {
				composed.at(x, y)[c] = round_channel(rgb[c], margin);
			}
			composed.margin_at(x, y) = margin;
		}
	}

	// Post。色のずれ、シャープ、彩度、コントラスト
	Plane post(work_w, work_h);
	const int d = settings.color_shift;
	for (int y = 0; y < work_h; ++y) {
		for (int x = 0; x < work_w; ++x) {
			const auto &center = composed.at(x, y);
			float rgb[3] = {composed.clamped(x - d, y)[0], center[1], composed.clamped(x + d, y)[2]};
			const auto &l = composed.clamped(x - 1, y);
			const auto &r = composed.clamped(x + 1, y);
			const auto &t = composed.clamped(x, y - 1);
			const auto &b = composed.clamped(x, y + 1);
			for (int c = 0; c < 3; ++c) {
				const float around = (l[c] + r[c] + t[c] + b[c]) * 0.25f;
				rgb[c] = rgb[c] + (center[c] - around) * settings.sharpen;
			}
			const float luma = rgb[0] * 0.299f + rgb[1] * 0.587f + rgb[2] * 0.114f;
			float margin = std::min({composed.margin_at(x, y), composed.clamped_margin(x - d, y),
						 composed.clamped_margin(x + d, y), composed.clamped_margin(x - 1, y),
						 composed.clamped_margin(x + 1, y), composed.clamped_margin(x, y - 1),
						 composed.clamped_margin(x, y + 1)});
			for (int c = 0; c < 3; ++c) {
				float v = luma + (rgb[c] - luma) * settings.saturation;
				v = (v - 128.0f) * settings.contrast + 128.0f;
				post.at(x, y)[c] = round_channel(v, margin);
			}
			post.margin_at(x, y) = margin;
		}
	}

	// Output。4 点の双線形補間で元の解像度に戻す。最後の 8 ビットへの丸めは描画器が行うため、ここでは丸めの余裕に数えない
	Result result;
	result.width = width;
	result.height = height;
	result.rgb.resize(size_t(width) * height * 3);
	result.margin.resize(size_t(width) * height);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const float fx = (float(x) + 0.5f) / float(scale) - 0.5f;
			const float fy = (float(y) + 0.5f) / float(scale) - 0.5f;
			const float ix = std::floor(fx);
			const float iy = std::floor(fy);
			const float wx = fx - ix;
			const float wy = fy - iy;
			const int x0 = int(ix);
			const int y0 = int(iy);
			const size_t i = size_t(y) * width + x;
			for (int c = 0; c < 3; ++c) {
				const float a = post.clamped(x0, y0)[c];
				const float b = post.clamped(x0 + 1, y0)[c];
				const float cc = post.clamped(x0, y0 + 1)[c];
				const float dd = post.clamped(x0 + 1, y0 + 1)[c];
				const float top = a + (b - a) * wx;
				const float bottom = cc + (dd - cc) * wx;
				const float v = top + (bottom - top) * wy;
				result.rgb[i * 3 + c] =
					static_cast<uint8_t>(std::clamp(std::floor(v + 0.5f), 0.0f, 255.0f));
			}
			result.margin[i] =
				std::min({post.clamped_margin(x0, y0), post.clamped_margin(x0 + 1, y0),
					  post.clamped_margin(x0, y0 + 1), post.clamped_margin(x0 + 1, y0 + 1)});
		}
	}
	return result;
}

} // namespace jpeg_reference
