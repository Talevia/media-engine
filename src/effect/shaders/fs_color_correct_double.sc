/*
 * fs_color_correct_double — two color-correct passes fused into
 * a single fragment shader.
 *
 * Produced by GpuEffectChain::compile when two adjacent
 * ColorCorrectEffects are detected: the chain's per-pass
 * framebuffer ping-pong would have read/written an intermediate
 * texture; this shader does both passes in one draw call, saving
 * the intermediate sampler + blit bandwidth.
 *
 * u_cc_params_a.xyz = (brightness, contrast, saturation) stage A
 * u_cc_params_b.xyz = same for stage B
 *
 * The math is identical to running fs_color_correct.sc twice in
 * sequence — applying stage A then stage B. Tests verify the
 * fused output matches the two-pass output at ±3 ULP.
 */
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_src, 0);
uniform vec4 u_cc_params_a;
uniform vec4 u_cc_params_b;

void main()
{
	vec4 c = texture2D(s_src, v_texcoord0);

	// Stage A
	c.rgb += u_cc_params_a.x;
	c.rgb = (c.rgb - vec3(0.5, 0.5, 0.5)) * u_cc_params_a.y + vec3(0.5, 0.5, 0.5);
	float la = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
	c.rgb = mix(vec3(la, la, la), c.rgb, u_cc_params_a.z);

	// Stage B
	c.rgb += u_cc_params_b.x;
	c.rgb = (c.rgb - vec3(0.5, 0.5, 0.5)) * u_cc_params_b.y + vec3(0.5, 0.5, 0.5);
	float lb = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
	c.rgb = mix(vec3(lb, lb, lb), c.rgb, u_cc_params_b.z);

	gl_FragColor = c;
}
