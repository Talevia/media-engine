/*
 * fs_color_correct — brightness / contrast / saturation fragment shader.
 *
 * u_color_correct_params.xyz = (brightness, contrast, saturation).
 *   brightness: additive offset (typ. [-1, +1]; 0 = identity)
 *   contrast:   multiplier around 0.5 (typ. [0, 2]; 1 = identity)
 *   saturation: mix factor against Rec-709 luma (typ. [0, 2]; 1 = identity)
 *
 * Identity params (0, 1, 1) produce exact pass-through, which the
 * test_color_correct_effect regression pins byte-for-byte.
 */
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_src, 0);
uniform vec4 u_color_correct_params;

void main()
{
	vec4 c = texture2D(s_src, v_texcoord0);

	// brightness
	c.rgb += u_color_correct_params.x;

	// contrast (pivot at 0.5)
	c.rgb = (c.rgb - vec3(0.5, 0.5, 0.5)) * u_color_correct_params.y + vec3(0.5, 0.5, 0.5);

	// saturation — mix with Rec-709 luma
	float luma = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
	c.rgb = mix(vec3(luma, luma, luma), c.rgb, u_color_correct_params.z);

	gl_FragColor = c;
}
