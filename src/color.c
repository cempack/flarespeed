/*
 * Ports of the go-colorful conversions behind lipgloss.Blend1D and
 * ansi.Convert256, from go-colorful (MIT, Lucas Beyer; its HSLuv code from
 * hsluv-go, MIT, Alexei Boronine), charmbracelet/x/ansi (MIT, Charmbracelet,
 * whose 256-color mapping comes from tmux, ISC) and Go's math package
 * (BSD-3-Clause, The Go Authors). See THIRD-PARTY-NOTICES.
 *
 * Constant subexpressions that Go folds exactly are written
 * as a single correctly rounded division (216.0 / 24389.0 rather than
 * 6.0/29.0*6.0/29.0*6.0/29.0), and Go's pure-Go Cbrt and Pow are reproduced
 * so the results agree bit for bit wherever the Go runtime does not use
 * assembly.
 */
#include "color.h"

#include <float.h>
#include <math.h>
#include <string.h>

typedef struct {
	double r, g, b;
} col_t;

/* math.Pi, which as a decimal literal rounds to the same double. */
static const double go_pi = 3.14159265358979323846264338327950288419716939937510582097494459;

static const double d65[3] = {0.95047, 1.00000, 1.08883};
static const double hsluv_d65[3] = {0.95045592705167, 1.0, 1.089057750759878};

static uint64_t bits_of(double x)
{
	uint64_t u;
	memcpy(&u, &x, sizeof(u));
	return u;
}

static double from_bits(uint64_t u)
{
	double x;
	memcpy(&x, &u, sizeof(x));
	return x;
}

/* math.Cbrt from the Go standard library, a modified version of fdlibm's
 * s_cbrt.c, which came with this notice:
 *
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunSoft, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 */
static double go_cbrt(double x)
{
	const uint64_t B1 = 715094163, B2 = 696219795;
	const double C = 5.42857142857142815906e-01, D = -7.05306122448979611050e-01,
		     E = 1.41428571428571436819e+00, F = 1.60714285714285720630e+00,
		     G = 3.57142857142857150787e-01, smallest_normal = 2.22507385850720138309e-308;

	if (x == 0 || isnan(x) || isinf(x))
		return x;
	int sign = 0;
	if (x < 0) {
		x = -x;
		sign = 1;
	}
	double t = from_bits(bits_of(x) / 3 + (B1 << 32));
	if (x < smallest_normal) {
		t = (double)(UINT64_C(1) << 54);
		t *= x;
		t = from_bits(bits_of(t) / 3 + (B2 << 32));
	}
	double r = t * t / x;
	double s = C + r * t;
	t *= G + F / (s + E + D / s);
	t = from_bits((bits_of(t) & (UINT64_C(0xFFFFFFFFC) << 28)) + (UINT64_C(1) << 30));
	s = t * t;
	r = x / s;
	double w = t + t;
	r = (r - t) / (w + r);
	t = t + t * r;
	return sign ? -t : t;
}

/* math.Pow from the Go standard library, for finite x > 0 and finite y. */
static double go_pow(double x, double y)
{
	if (y == 0 || x == 1)
		return 1;
	if (y == 1)
		return x;
	if (y == 0.5)
		return sqrt(x);
	if (y == -0.5)
		return 1 / sqrt(x);

	double yi;
	double yf = modf(fabs(y), &yi);
	double a1 = 1.0;
	int ae = 0;
	if (yf != 0) {
		if (yf > 0.5) {
			yf--;
			yi++;
		}
		a1 = exp(yf * log(x));
	}
	int xe;
	double x1 = frexp(x, &xe);
	for (int64_t i = (int64_t)yi; i != 0; i >>= 1) {
		if (xe < -(1 << 12) || (1 << 12) < xe) {
			ae += xe;
			break;
		}
		if (i & 1) {
			a1 *= x1;
			ae += xe;
		}
		x1 *= x1;
		xe *= 2; /* Go's xe <<= 1; a shift of a negative int is undefined in C */
		if (x1 < .5) {
			x1 += x1;
			xe--;
		}
	}
	if (y < 0) {
		a1 = 1 / a1;
		ae = -ae;
	}
	return ldexp(a1, ae);
}

static double clamp01(double v)
{
	return fmax(0.0, fmin(v, 1.0));
}

static col_t from_rgb16(rgb16_t c)
{
	return (col_t){c.r / 65535.0, c.g / 65535.0, c.b / 65535.0};
}

/* colorful.Color.RGBA, keeping the 16-bit result. */
static rgb16_t to_rgb16(col_t c)
{
	return (rgb16_t){(uint16_t)(uint32_t)(c.r * 65535.0 + 0.5),
			 (uint16_t)(uint32_t)(c.g * 65535.0 + 0.5),
			 (uint16_t)(uint32_t)(c.b * 65535.0 + 0.5)};
}

static double linearize(double v)
{
	if (v <= 0.04045)
		return v / 12.92;
	return go_pow((v + 0.055) / 1.055, 2.4);
}

static double delinearize(double v)
{
	if (v <= 0.0031308)
		return 12.92 * v;
	return 1.055 * go_pow(v, 5.0 / 12.0) - 0.055;
}

static void to_xyz(col_t c, double *x, double *y, double *z)
{
	double r = linearize(c.r), g = linearize(c.g), b = linearize(c.b);
	*x = 0.41239079926595948 * r + 0.35758433938387796 * g + 0.18048078840183429 * b;
	*y = 0.21263900587151036 * r + 0.71516867876775593 * g + 0.072192315360733715 * b;
	*z = 0.019330818715591851 * r + 0.11919477979462599 * g + 0.95053215224966058 * b;
}

static col_t from_xyz(double x, double y, double z)
{
	double r = 3.2409699419045214 * x - 1.5373831775700935 * y - 0.49861076029300328 * z;
	double g = -0.96924363628087983 * x + 1.8759675015077207 * y + 0.041555057407175613 * z;
	double b = 0.055630079696993609 * x - 0.20397695888897657 * y + 1.0569715142428786 * z;
	return (col_t){delinearize(r), delinearize(g), delinearize(b)};
}

static double lab_f(double t)
{
	if (t > 216.0 / 24389.0)
		return go_cbrt(t);
	return t / 3.0 * 29.0 / 6.0 * 29.0 / 6.0 + 4.0 / 29.0;
}

static double lab_finv(double t)
{
	if (t > 6.0 / 29.0)
		return t * t * t;
	return 108.0 / 841.0 * (t - 4.0 / 29.0);
}

static void to_lab(col_t c, double *l, double *a, double *b)
{
	double x, y, z;
	to_xyz(c, &x, &y, &z);
	double fy = lab_f(y / d65[1]);
	*l = 1.16 * fy - 0.16;
	*a = 5.0 * (lab_f(x / d65[0]) - fy);
	*b = 2.0 * (fy - lab_f(z / d65[2]));
}

static col_t from_lab(double l, double a, double b)
{
	double l2 = (l + 0.16) / 1.16;
	double x = d65[0] * lab_finv(l2 + a / 5.0);
	double y = d65[1] * lab_finv(l2);
	double z = d65[2] * lab_finv(l2 - b / 2.0);
	return from_xyz(x, y, z);
}

void color_blend(rgb16_t *out, int steps, rgb16_t from, rgb16_t to)
{
	if (steps <= 0)
		return;
	if (steps <= 2) {
		out[0] = from;
		if (steps == 2)
			out[1] = to;
		return;
	}

	double l1, a1, b1, l2, a2, b2;
	to_lab(from_rgb16(from), &l1, &a1, &b1);
	to_lab(from_rgb16(to), &l2, &a2, &b2);
	double divisor = (double)(steps - 1);
	for (int j = 0; j < steps; j++) {
		double t = (double)j / divisor;
		col_t c = from_lab(l1 + t * (l2 - l1), a1 + t * (a2 - a1), b1 + t * (b2 - b1));
		c = (col_t){clamp01(c.r), clamp01(c.g), clamp01(c.b)};
		out[j] = to_rgb16(c);
	}
}

/* HSLuv, used by Convert256 to judge which palette entry looks closest. */

static void xyz_to_uv(double x, double y, double z, double *u, double *v)
{
	double denom = x + 15.0 * y + 3.0 * z;
	if (denom == 0.0) {
		*u = 0.0;
		*v = 0.0;
	} else {
		*u = 4.0 * x / denom;
		*v = 9.0 * y / denom;
	}
}

static void to_luv_lch(col_t c, double *l, double *chroma, double *h)
{
	double x, y, z;
	to_xyz(c, &x, &y, &z);
	const double *w = hsluv_d65;
	double lum;
	if (y / w[1] <= 216.0 / 24389.0)
		lum = y / w[1] * (24389.0 / 27.0) / 100.0;
	else
		lum = 1.16 * go_cbrt(y / w[1]) - 0.16;
	double ubis, vbis, un, vn;
	xyz_to_uv(x, y, z, &ubis, &vbis);
	xyz_to_uv(w[0], w[1], w[2], &un, &vn);
	double u = 13.0 * lum * (ubis - un);
	double v = 13.0 * lum * (vbis - vn);

	if (fabs(v - u) > 1e-4 && fabs(u) > 1e-4)
		*h = fmod(57.29577951308232087721 * atan2(v, u) + 360.0, 360.0);
	else
		*h = 0.0;
	*l = lum;
	*chroma = sqrt(u * u + v * v);
}

static const double hsluv_m[3][3] = {
	{3.2409699419045214, -1.5373831775700935, -0.49861076029300328},
	{-0.96924363628087983, 1.8759675015077207, 0.041555057407175613},
	{0.055630079696993609, -0.20397695888897657, 1.0569715142428786},
};

static double max_chroma_for_lh(double l, double h)
{
	const double kappa = 903.2962962962963, epsilon = 0.0088564516790356308;
	double h_rad = h / 360.0 * go_pi * 2.0;
	double min_length = DBL_MAX;

	double sub1 = go_pow(l + 16.0, 3.0) / 1560896.0;
	double sub2 = sub1 > epsilon ? sub1 : l / kappa;
	for (int i = 0; i < 3; i++) {
		const double *m = hsluv_m[i];
		for (int k = 0; k < 2; k++) {
			double top1 = (284517.0 * m[0] - 94839.0 * m[2]) * sub2;
			double top2 = (838422.0 * m[2] + 769860.0 * m[1] + 731718.0 * m[0]) * l * sub2 -
				      769860.0 * (double)k * l;
			double bottom = (632260.0 * m[2] - 126452.0 * m[1]) * sub2 + 126452.0 * (double)k;
			double slope = top1 / bottom, intercept = top2 / bottom;
			double length = intercept / (sin(h_rad) - slope * cos(h_rad));
			if (length > 0.0 && length < min_length)
				min_length = length;
		}
	}
	return min_length;
}

static void to_hsluv(col_t c, double *h, double *s, double *l)
{
	double lum, chroma, hue;
	to_luv_lch(c, &lum, &chroma, &hue);
	chroma *= 100.0;
	lum *= 100.0;
	double sat = 0.0;
	if (!(lum > 99.9999999 || lum < 0.00000001))
		sat = chroma / max_chroma_for_lh(lum, hue) * 100.0;
	*h = hue;
	*s = clamp01(sat / 100.0);
	*l = clamp01(lum / 100.0);
}

static double distance_hsluv(col_t a, col_t b)
{
	double h1, s1, l1, h2, s2, l2;
	to_hsluv(a, &h1, &s1, &l1);
	to_hsluv(b, &h2, &s2, &l2);
	double dh = (h1 - h2) / 100.0, ds = s1 - s2, dl = l1 - l2;
	return sqrt(dh * dh + ds * ds + dl * dl);
}

static int to_6cube(double v)
{
	if (v < 48)
		return 0;
	if (v < 115)
		return 1;
	return (int)((v - 35) / 40);
}

int color_to_ansi256(rgb16_t c)
{
	static const int q2c[6] = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};
	col_t col = from_rgb16(c);
	double r = col.r * 255, g = col.g * 255, b = col.b * 255;

	int qr = to_6cube(r), qg = to_6cube(g), qb = to_6cube(b);
	int cr = q2c[qr], cg = q2c[qg], cb = q2c[qb];
	int ci = 36 * qr + 6 * qg + qb;
	if (cr == (int)r && cg == (int)g && cb == (int)b)
		return 16 + ci;

	int grey_avg = (int)(r + g + b) / 3;
	int grey_idx = grey_avg > 238 ? 23 : (grey_avg - 3) / 10;
	int grey = 8 + 10 * grey_idx;

	col_t cube = {cr / 255.0, cg / 255.0, cb / 255.0};
	col_t gray = {grey / 255.0, grey / 255.0, grey / 255.0};
	if (distance_hsluv(col, cube) <= distance_hsluv(col, gray))
		return 16 + ci;
	return 232 + grey_idx;
}

int color_to_ansi16(rgb16_t c)
{
	/* ansi256To16 from github.com/charmbracelet/x/ansi. */
	static const unsigned char to16[256] = {
		0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
		0,  4,  4,  4,  12, 12, 2,  6,  4,  4,  12, 12, 2,  2,  6,  4,
		12, 12, 2,  2,  2,  6,  12, 12, 10, 10, 10, 10, 14, 12, 10, 10,
		10, 10, 10, 14, 1,  5,  4,  4,  12, 12, 3,  8,  4,  4,  12, 12,
		2,  2,  6,  4,  12, 12, 2,  2,  2,  6,  12, 12, 10, 10, 10, 10,
		14, 12, 10, 10, 10, 10, 10, 14, 1,  1,  5,  4,  12, 12, 1,  1,
		5,  4,  12, 12, 3,  3,  8,  4,  12, 12, 2,  2,  2,  6,  12, 12,
		10, 10, 10, 10, 14, 12, 10, 10, 10, 10, 10, 14, 1,  1,  1,  5,
		12, 12, 1,  1,  1,  5,  12, 12, 1,  1,  1,  5,  12, 12, 3,  3,
		3,  7,  12, 12, 10, 10, 10, 10, 14, 12, 10, 10, 10, 10, 10, 14,
		9,  9,  9,  9,  13, 12, 9,  9,  9,  9,  13, 12, 9,  9,  9,  9,
		13, 12, 9,  9,  9,  9,  13, 12, 11, 11, 11, 11, 7,  12, 10, 10,
		10, 10, 10, 14, 9,  9,  9,  9,  9,  13, 9,  9,  9,  9,  9,  13,
		9,  9,  9,  9,  9,  13, 9,  9,  9,  9,  9,  13, 9,  9,  9,  9,
		9,  13, 11, 11, 11, 11, 11, 15, 0,  0,  0,  0,  0,  0,  8,  8,
		8,  8,  8,  8,  7,  7,  7,  7,  7,  7,  15, 15, 15, 15, 15, 15,
	};
	return to16[color_to_ansi256(c)];
}
