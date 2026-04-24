/*
 * fs_blur_h — horizontal 3-tap box blur.
 *
 * u_blur_texel.xy = (1/src_width, 1/src_height)
 *
 * Samples at (-u_blur_texel.x, 0), (0, 0), (+u_blur_texel.x, 0)
 * and averages. Edge pixels clamp via BGFX_SAMPLER_U_CLAMP /
 * V_CLAMP on the source texture — out-of-bounds samples read
 * the edge pixel, so a uniform color input produces the same
 * uniform output.
 *
 * Phase-1 radius is fixed at 1 (3 taps). Wider kernels +
 * Gaussian weights arrive when a real visual need surfaces;
 * the separable shape means adding weights is localized.
 */
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_src, 0);
uniform vec4 u_blur_texel;

void main()
{
	vec2 uv = v_texcoord0;
	vec2 tx = vec2(u_blur_texel.x, 0.0);

	vec4 a = texture2D(s_src, uv - tx);
	vec4 b = texture2D(s_src, uv);
	vec4 c = texture2D(s_src, uv + tx);

	gl_FragColor = (a + b + c) * (1.0 / 3.0);
}
