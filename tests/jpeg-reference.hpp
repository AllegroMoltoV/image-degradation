#pragma once

// フィルターと同じ計算を CPU で行う参照の実装。jpeg-crunch.effect のパスと同じ順序、同じ式、同じ丸めで計算する。
//
// 量子化の丸め (floor(x + 0.5)) は、値がちょうど境目にあると、浮動小数点の僅かな誤差で GPU と CPU の結果が分かれる。
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

// c 個の成分を持つ平面。余裕は成分で共有する
struct Plane {
	int width = 0;
	int height = 0;
	int channels = 1;
	std::vector<float> value;
	std::vector<float> margin;

	Plane(int w, int h, int c)
		: width(w),
		  height(h),
		  channels(c),
		  value(size_t(w) * h * c, 0.0f),
		  margin(size_t(w) * h, kNoTie)
	{
	}

	float &at(int x, int y, int c = 0) { return value[(size_t(y) * width + x) * channels + c]; }
	float at(int x, int y, int c = 0) const { return value[(size_t(y) * width + x) * channels + c]; }
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

inline float luma_of(float r, float g, float b)
{
	return r * 0.299f + g * 0.587f + b * 0.114f - 128.0f;
}

// input は符号化した RGB (幅 x 高さ x 3)。アルファを割り戻した後の値を渡す
inline Result run(const std::vector<uint8_t> &input, int width, int height, int crunch)
{
	using namespace jpeg_crunch;
	const Settings settings = settings_for(crunch);
	const std::array<float, 64> basis = dct_basis();
	const auto basis_at = [&basis](int u, int x) {
		return basis[size_t(u) * 8 + x];
	};
	const std::array<float, 64> qluma = scaled_table(kLumaTable, settings.quality);
	const std::array<float, 64> qchroma = scaled_table(kChromaTable, settings.quality);

	Plane work(width, height, 3);
	for (size_t i = 0; i < input.size(); ++i) {
		work.value[i] = static_cast<float>(input[i]);
	}

	const int chroma_w = (width + 1) / 2;
	const int chroma_h = (height + 1) / 2;
	const int luma_pw = padded_size(width);
	const int luma_ph = padded_size(height);
	const int chroma_pw = padded_size(chroma_w);
	const int chroma_ph = padded_size(chroma_h);

	for (int pass = 0; pass < settings.passes; ++pass) {
		const int shift = kShifts[pass];
		const int half_shift = shift / 2;
		const bool last = pass + 1 == settings.passes;

		// LumaRow
		Plane luma_row(luma_pw, luma_ph, 1);
		for (int py = 0; py < luma_ph; ++py) {
			const int y = std::clamp(py - shift, 0, height - 1);
			for (int px = 0; px < luma_pw; ++px) {
				const int u = block_pos(px);
				const int b0 = px - u;
				float sum = 0.0f;
				float margin = kNoTie;
				for (int i = 0; i < 8; ++i) {
					const int x = std::clamp(b0 + i - shift, 0, width - 1);
					sum += basis_at(u, i) *
					       luma_of(work.at(x, y, 0), work.at(x, y, 1), work.at(x, y, 2));
					margin = std::min(margin, work.margin_at(x, y));
				}
				luma_row.at(px, py) = sum;
				luma_row.margin_at(px, py) = margin;
			}
		}

		// LumaQuant
		Plane luma_quant(luma_pw, luma_ph, 1);
		for (int py = 0; py < luma_ph; ++py) {
			const int v = block_pos(py);
			const int b0 = py - v;
			for (int px = 0; px < luma_pw; ++px) {
				const int u = block_pos(px);
				float sum = 0.0f;
				float margin = kNoTie;
				for (int j = 0; j < 8; ++j) {
					sum += basis_at(v, j) * luma_row.at(px, b0 + j);
					margin = std::min(margin, luma_row.margin_at(px, b0 + j));
				}
				const float q = qluma[size_t(v) * 8 + u];
				const float ratio = sum / q;
				luma_quant.at(px, py) = std::floor(ratio + 0.5f) * q;
				luma_quant.margin_at(px, py) = std::min(margin, tie_distance(ratio, q));
			}
		}

		// LumaInvCol
		Plane luma_col(luma_pw, luma_ph, 1);
		for (int py = 0; py < luma_ph; ++py) {
			const int y = block_pos(py);
			const int b0 = py - y;
			for (int px = 0; px < luma_pw; ++px) {
				float sum = 0.0f;
				float margin = kNoTie;
				for (int j = 0; j < 8; ++j) {
					sum += basis_at(j, y) * luma_quant.at(px, b0 + j);
					margin = std::min(margin, luma_quant.margin_at(px, b0 + j));
				}
				luma_col.at(px, py) = sum;
				luma_col.margin_at(px, py) = margin;
			}
		}

		// ChromaRow。色差の画素は画像の 2x2 の画素の平均から求める
		const auto chroma_at = [&](int cx, int cy, float &cb, float &cr, float &margin) {
			const int x0 = cx * 2;
			const int y0 = cy * 2;
			const int x1 = std::min(x0 + 1, width - 1);
			const int y1 = std::min(y0 + 1, height - 1);
			float rgb[3];
			for (int c = 0; c < 3; ++c) {
				rgb[c] = (work.at(x0, y0, c) + work.at(x1, y0, c) + work.at(x0, y1, c) +
					  work.at(x1, y1, c)) *
					 0.25f;
			}
			cb = rgb[0] * -0.168736f + rgb[1] * -0.331264f + rgb[2] * 0.5f;
			cr = rgb[0] * 0.5f + rgb[1] * -0.418688f + rgb[2] * -0.081312f;
			margin = std::min({work.margin_at(x0, y0), work.margin_at(x1, y0), work.margin_at(x0, y1),
					   work.margin_at(x1, y1)});
		};
		Plane chroma_row(chroma_pw, chroma_ph, 2);
		for (int py = 0; py < chroma_ph; ++py) {
			const int cy = std::clamp(py - half_shift, 0, chroma_h - 1);
			for (int px = 0; px < chroma_pw; ++px) {
				const int u = block_pos(px);
				const int b0 = px - u;
				float sum_cb = 0.0f;
				float sum_cr = 0.0f;
				float margin = kNoTie;
				for (int i = 0; i < 8; ++i) {
					const int cx = std::clamp(b0 + i - half_shift, 0, chroma_w - 1);
					float cb = 0.0f;
					float cr = 0.0f;
					float m = kNoTie;
					chroma_at(cx, cy, cb, cr, m);
					sum_cb += basis_at(u, i) * cb;
					sum_cr += basis_at(u, i) * cr;
					margin = std::min(margin, m);
				}
				chroma_row.at(px, py, 0) = sum_cb;
				chroma_row.at(px, py, 1) = sum_cr;
				chroma_row.margin_at(px, py) = margin;
			}
		}

		// ChromaQuant
		Plane chroma_quant(chroma_pw, chroma_ph, 2);
		for (int py = 0; py < chroma_ph; ++py) {
			const int v = block_pos(py);
			const int b0 = py - v;
			for (int px = 0; px < chroma_pw; ++px) {
				const int u = block_pos(px);
				const float q = qchroma[size_t(v) * 8 + u];
				float margin = kNoTie;
				for (int c = 0; c < 2; ++c) {
					float sum = 0.0f;
					for (int j = 0; j < 8; ++j) {
						sum += basis_at(v, j) * chroma_row.at(px, b0 + j, c);
					}
					const float ratio = sum / q;
					chroma_quant.at(px, py, c) = std::floor(ratio + 0.5f) * q;
					margin = std::min(margin, tie_distance(ratio, q));
				}
				for (int j = 0; j < 8; ++j) {
					margin = std::min(margin, chroma_row.margin_at(px, b0 + j));
				}
				chroma_quant.margin_at(px, py) = margin;
			}
		}

		// ChromaInvCol
		Plane chroma_col(chroma_pw, chroma_ph, 2);
		for (int py = 0; py < chroma_ph; ++py) {
			const int y = block_pos(py);
			const int b0 = py - y;
			for (int px = 0; px < chroma_pw; ++px) {
				float margin = kNoTie;
				for (int c = 0; c < 2; ++c) {
					float sum = 0.0f;
					for (int j = 0; j < 8; ++j) {
						sum += basis_at(j, y) * chroma_quant.at(px, b0 + j, c);
					}
					chroma_col.at(px, py, c) = sum;
				}
				for (int j = 0; j < 8; ++j) {
					margin = std::min(margin, chroma_quant.margin_at(px, b0 + j));
				}
				chroma_col.margin_at(px, py) = margin;
			}
		}

		// Compose
		Plane next(width, height, 3);
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				const int i = x + shift;
				const int j = y + shift;
				const int u0 = block_pos(i);
				const int b0 = i - u0;
				float luma = 128.0f;
				float margin = kNoTie;
				for (int k = 0; k < 8; ++k) {
					luma += basis_at(k, u0) * luma_col.at(b0 + k, j);
					margin = std::min(margin, luma_col.margin_at(b0 + k, j));
				}
				const int ci = x / 2 + half_shift;
				const int cj = y / 2 + half_shift;
				const int cu = block_pos(ci);
				const int cb0 = ci - cu;
				float cb = 0.0f;
				float cr = 0.0f;
				for (int m = 0; m < 8; ++m) {
					cb += basis_at(m, cu) * chroma_col.at(cb0 + m, cj, 0);
					cr += basis_at(m, cu) * chroma_col.at(cb0 + m, cj, 1);
					margin = std::min(margin, chroma_col.margin_at(cb0 + m, cj));
				}
				const float rgb[3] = {luma + 1.402f * cr, luma - 0.344136f * cb - 0.714136f * cr,
						      luma + 1.772f * cb};
				for (int c = 0; c < 3; ++c) {
					next.at(x, y, c) = std::clamp(std::floor(rgb[c] + 0.5f), 0.0f, 255.0f);
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

	Result result;
	result.width = width;
	result.height = height;
	result.rgb.resize(size_t(width) * height * 3);
	for (size_t i = 0; i < result.rgb.size(); ++i) {
		result.rgb[i] = static_cast<uint8_t>(work.value[i]);
	}
	result.margin = work.margin;
	return result;
}

} // namespace jpeg_reference
