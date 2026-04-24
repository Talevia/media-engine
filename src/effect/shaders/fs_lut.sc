/*
 * fs_lut — 3D LUT fragment shader.
 *
 * Samples a per-clip 3D LUT texture (`s_lut`) at the source
 * fragment's RGB and writes the looked-up color. Trilinear
 * filtering is enforced on the sampler at CPU side — the shader
 * is a single texture fetch.
 *
 * Identity LUT (rgb = texture3D(s_lut, rgb) ≈ rgb) produces
 * pass-through, which test_lut_effect pins as the first
 * correctness gate.
 */
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_src, 0);
SAMPLER3D(s_lut, 1);

void main()
{
	vec4 c = texture2D(s_src, v_texcoord0);
	vec3 r = texture3D(s_lut, c.rgb).rgb;
	gl_FragColor = vec4(r, c.a);
}
