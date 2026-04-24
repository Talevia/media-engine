/*
 * fs_blur_v — vertical 3-tap box blur.
 *
 * Paired with fs_blur_h for a separable 2-pass blur. See
 * fs_blur_h.sc for the full rationale; this shader samples on
 * the Y axis instead of X.
 */
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_src, 0);
uniform vec4 u_blur_texel;

void main()
{
	vec2 uv = v_texcoord0;
	vec2 ty = vec2(0.0, u_blur_texel.y);

	vec4 a = texture2D(s_src, uv - ty);
	vec4 b = texture2D(s_src, uv);
	vec4 c = texture2D(s_src, uv + ty);

	gl_FragColor = (a + b + c) * (1.0 / 3.0);
}
