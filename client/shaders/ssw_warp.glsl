#version 450

#ifdef VERT_SHADER

layout(location = 0) out vec2 out_uv;

void main()
{
	// Full-screen triangle; no vertex buffer or generated history is needed.
	vec2 position = vec2(
	        (gl_VertexIndex == 1) ? 3.0 : -1.0,
	        (gl_VertexIndex == 2) ? 3.0 : -1.0);
	out_uv = position * 0.5 + 0.5;
	gl_Position = vec4(position, 0.0, 1.0);
}

#endif

#ifdef FRAG_SHADER

layout(set = 0, binding = 0) uniform sampler2DArray current_image;
layout(set = 0, binding = 1) uniform sampler2DArray refined_flow;

layout(push_constant) uniform warp_pc
{
	vec4 input_size; // width, height, inverse width, inverse height
	vec4 parameters; // extrapolation factor, confidence threshold, eye, unused
};

layout(location = 0) in vec2 out_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	float eye = parameters.z;
	vec4 flow = texture(refined_flow, vec3(out_uv, eye));

	// Confidence gating intentionally falls back to the latest complete real
	// image.  ATW remains the runtime's responsibility after this pass.
	if (flow.a < parameters.y)
	{
		out_color = texture(current_image, vec3(out_uv, eye));
		return;
	}

	vec2 warped_uv = clamp(out_uv + flow.xy * parameters.x, vec2(0.0), vec2(1.0));
	out_color = texture(current_image, vec3(warped_uv, eye));
}

#endif
