/*
 * vs_fullscreen — full-screen quad vertex shader.
 *
 * Takes 2D position (clip-space x / y in [-1, +1]) + texcoord0 and
 * passes them through. Used by every per-pixel GPU effect that
 * draws a full-screen triangle pair to sample an input texture.
 */
$input a_position, a_texcoord0
$output v_texcoord0

#include <bgfx_shader.sh>

void main()
{
	gl_Position = vec4(a_position, 0.0, 1.0);
	v_texcoord0 = a_texcoord0;
}
