#version 450
/* a half-transparent layer (the blend benchmark, vktest bench) */
layout(location = 0) out vec4 color;
void main()
{
	color = vec4(0.2, 0.4, 0.6, 0.5);
}
