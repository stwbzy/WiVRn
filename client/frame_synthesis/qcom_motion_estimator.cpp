/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "qcom_motion_estimator.h"

#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace frame_synthesis
{
namespace
{

constexpr uint32_t gl_extensions = 0x1F03;
constexpr uint32_t gl_no_error = 0x0000;
constexpr uint32_t gl_motion_estimation_search_block_x_qcom = 0x8C90;
constexpr uint32_t gl_motion_estimation_search_block_y_qcom = 0x8C91;

#if defined(__ANDROID__)
constexpr uint32_t gl_texture_2d = 0x0DE1;
constexpr uint32_t gl_texture_external_oes = 0x8D65;
constexpr uint32_t gl_texture_min_filter = 0x2801;
constexpr uint32_t gl_texture_mag_filter = 0x2800;
constexpr uint32_t gl_texture_wrap_s = 0x2802;
constexpr uint32_t gl_texture_wrap_t = 0x2803;
constexpr uint32_t gl_linear = 0x2601;
constexpr uint32_t gl_clamp_to_edge = 0x812F;
constexpr uint32_t gl_r8 = 0x8229;
constexpr uint32_t gl_framebuffer = 0x8D40;
constexpr uint32_t gl_color_attachment0 = 0x8CE0;
constexpr uint32_t gl_framebuffer_complete = 0x8CD5;
constexpr uint32_t gl_vertex_shader = 0x8B31;
constexpr uint32_t gl_fragment_shader = 0x8B30;
constexpr uint32_t gl_compile_status = 0x8B81;
constexpr uint32_t gl_link_status = 0x8B82;
constexpr uint32_t gl_triangles = 0x0004;
constexpr uint32_t gl_texture0 = 0x84C0;
#endif

template<typename Function>
Function load_function(const qcom_gl_api & api, const char * name)
{
	if (not api.get_proc_address)
		return nullptr;
	return reinterpret_cast<Function>(api.get_proc_address(name));
}

bool has_extension(std::string_view extensions, std::string_view wanted)
{
	size_t begin = 0;
	while (begin < extensions.size())
	{
		while (begin < extensions.size() && extensions[begin] == ' ')
			++begin;
		const size_t end = extensions.find(' ', begin);
		const size_t token_end = end == std::string_view::npos ? extensions.size() : end;
		if (extensions.substr(begin, token_end - begin) == wanted)
			return true;
		if (end == std::string_view::npos)
			break;
		begin = end + 1;
	}
	return false;
}

void clear_errors(const qcom_gl_api & api)
{
	if (not api.get_error)
		return;
	for (unsigned int i = 0; i < 32; ++i)
	{
		if (api.get_error() == gl_no_error)
			break;
	}
}

std::string format_gl_error(uint32_t error)
{
	std::ostringstream stream;
	stream << "GL error 0x" << std::hex << std::uppercase << error;
	return stream.str();
}

#if defined(__ANDROID__)

using android_get_proc_address_fn = void * (*)(const char *);

struct android_egl_loader
{
	void * egl_module = nullptr;
	void * gles_module = nullptr;
	android_get_proc_address_fn egl_get_proc_address = nullptr;
};

android_egl_loader & get_android_egl_loader()
{
	static android_egl_loader loader = [] {
		android_egl_loader result;
		result.egl_module = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
		result.gles_module = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
		if (not result.gles_module)
			result.gles_module = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
		if (result.egl_module)
			result.egl_get_proc_address =
			        reinterpret_cast<android_get_proc_address_fn>(dlsym(result.egl_module, "eglGetProcAddress"));
		return result;
	}();
	return loader;
}

void * load_android_proc(const char * name)
{
	auto & loader = get_android_egl_loader();
	if (loader.egl_get_proc_address)
	{
		if (void * proc = loader.egl_get_proc_address(name))
			return proc;
	}
	if (loader.egl_module)
	{
		if (void * proc = dlsym(loader.egl_module, name))
			return proc;
	}
	return loader.gles_module ? dlsym(loader.gles_module, name) : nullptr;
}

template<typename Function>
Function load_android_function(const char * name)
{
	return reinterpret_cast<Function>(load_android_proc(name));
}

struct gl_functions
{
	using gen_textures_fn = void (*)(int32_t, uint32_t *);
	using delete_textures_fn = void (*)(int32_t, const uint32_t *);
	using bind_texture_fn = void (*)(uint32_t, uint32_t);
	using tex_parameteri_fn = void (*)(uint32_t, uint32_t, int32_t);
	using tex_storage_2d_fn = void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
	using egl_image_target_texture_fn = void (*)(uint32_t, EGLImageKHR);
	using gen_framebuffers_fn = void (*)(int32_t, uint32_t *);
	using delete_framebuffers_fn = void (*)(int32_t, const uint32_t *);
	using bind_framebuffer_fn = void (*)(uint32_t, uint32_t);
	using framebuffer_texture_2d_fn = void (*)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
	using check_framebuffer_status_fn = uint32_t (*)(uint32_t);
	using viewport_fn = void (*)(int32_t, int32_t, int32_t, int32_t);
	using create_shader_fn = uint32_t (*)(uint32_t);
	using shader_source_fn = void (*)(uint32_t, int32_t, const char * const *, const int32_t *);
	using compile_shader_fn = void (*)(uint32_t);
	using get_shader_iv_fn = void (*)(uint32_t, uint32_t, int32_t *);
	using get_shader_info_log_fn = void (*)(uint32_t, int32_t, int32_t *, char *);
	using delete_shader_fn = void (*)(uint32_t);
	using create_program_fn = uint32_t (*)();
	using attach_shader_fn = void (*)(uint32_t, uint32_t);
	using link_program_fn = void (*)(uint32_t);
	using get_program_iv_fn = void (*)(uint32_t, uint32_t, int32_t *);
	using get_program_info_log_fn = void (*)(uint32_t, int32_t, int32_t *, char *);
	using delete_program_fn = void (*)(uint32_t);
	using use_program_fn = void (*)(uint32_t);
	using get_uniform_location_fn = int32_t (*)(uint32_t, const char *);
	using uniform_1i_fn = void (*)(int32_t, int32_t);
	using active_texture_fn = void (*)(uint32_t);
	using draw_arrays_fn = void (*)(uint32_t, int32_t, int32_t);
	using flush_fn = void (*)();

	qcom_gl_api qcom;
	gen_textures_fn gen_textures = nullptr;
	delete_textures_fn delete_textures = nullptr;
	bind_texture_fn bind_texture = nullptr;
	tex_parameteri_fn tex_parameteri = nullptr;
	tex_storage_2d_fn tex_storage_2d = nullptr;
	egl_image_target_texture_fn egl_image_target_texture = nullptr;
	gen_framebuffers_fn gen_framebuffers = nullptr;
	delete_framebuffers_fn delete_framebuffers = nullptr;
	bind_framebuffer_fn bind_framebuffer = nullptr;
	framebuffer_texture_2d_fn framebuffer_texture_2d = nullptr;
	check_framebuffer_status_fn check_framebuffer_status = nullptr;
	viewport_fn viewport = nullptr;
	create_shader_fn create_shader = nullptr;
	shader_source_fn shader_source = nullptr;
	compile_shader_fn compile_shader = nullptr;
	get_shader_iv_fn get_shader_iv = nullptr;
	get_shader_info_log_fn get_shader_info_log = nullptr;
	delete_shader_fn delete_shader = nullptr;
	create_program_fn create_program = nullptr;
	attach_shader_fn attach_shader = nullptr;
	link_program_fn link_program = nullptr;
	get_program_iv_fn get_program_iv = nullptr;
	get_program_info_log_fn get_program_info_log = nullptr;
	delete_program_fn delete_program = nullptr;
	use_program_fn use_program = nullptr;
	get_uniform_location_fn get_uniform_location = nullptr;
	uniform_1i_fn uniform_1i = nullptr;
	active_texture_fn active_texture = nullptr;
	draw_arrays_fn draw_arrays = nullptr;
	flush_fn flush = nullptr;

	template<typename Function>
	static Function load(const qcom_gl_api & api, const char * name)
	{
		return ::frame_synthesis::load_function<Function>(api, name);
	}

	bool load_all()
	{
		qcom.get_string = load<qcom_gl_api::get_string_fn>(qcom, "glGetString");
		qcom.get_integer = load<qcom_gl_api::get_integer_fn>(qcom, "glGetIntegerv");
		qcom.get_error = load<qcom_gl_api::get_error_fn>(qcom, "glGetError");
		gen_textures = load<gen_textures_fn>(qcom, "glGenTextures");
		delete_textures = load<delete_textures_fn>(qcom, "glDeleteTextures");
		bind_texture = load<bind_texture_fn>(qcom, "glBindTexture");
		tex_parameteri = load<tex_parameteri_fn>(qcom, "glTexParameteri");
		tex_storage_2d = load<tex_storage_2d_fn>(qcom, "glTexStorage2D");
		egl_image_target_texture = load<egl_image_target_texture_fn>(qcom, "glEGLImageTargetTexture2DOES");
		gen_framebuffers = load<gen_framebuffers_fn>(qcom, "glGenFramebuffers");
		delete_framebuffers = load<delete_framebuffers_fn>(qcom, "glDeleteFramebuffers");
		bind_framebuffer = load<bind_framebuffer_fn>(qcom, "glBindFramebuffer");
		framebuffer_texture_2d = load<framebuffer_texture_2d_fn>(qcom, "glFramebufferTexture2D");
		check_framebuffer_status = load<check_framebuffer_status_fn>(qcom, "glCheckFramebufferStatus");
		viewport = load<viewport_fn>(qcom, "glViewport");
		create_shader = load<create_shader_fn>(qcom, "glCreateShader");
		shader_source = load<shader_source_fn>(qcom, "glShaderSource");
		compile_shader = load<compile_shader_fn>(qcom, "glCompileShader");
		get_shader_iv = load<get_shader_iv_fn>(qcom, "glGetShaderiv");
		get_shader_info_log = load<get_shader_info_log_fn>(qcom, "glGetShaderInfoLog");
		delete_shader = load<delete_shader_fn>(qcom, "glDeleteShader");
		create_program = load<create_program_fn>(qcom, "glCreateProgram");
		attach_shader = load<attach_shader_fn>(qcom, "glAttachShader");
		link_program = load<link_program_fn>(qcom, "glLinkProgram");
		get_program_iv = load<get_program_iv_fn>(qcom, "glGetProgramiv");
		get_program_info_log = load<get_program_info_log_fn>(qcom, "glGetProgramInfoLog");
		delete_program = load<delete_program_fn>(qcom, "glDeleteProgram");
		use_program = load<use_program_fn>(qcom, "glUseProgram");
		get_uniform_location = load<get_uniform_location_fn>(qcom, "glGetUniformLocation");
		uniform_1i = load<uniform_1i_fn>(qcom, "glUniform1i");
		active_texture = load<active_texture_fn>(qcom, "glActiveTexture");
		draw_arrays = load<draw_arrays_fn>(qcom, "glDrawArrays");
		flush = load<flush_fn>(qcom, "glFlush");
		return qcom.get_string && qcom.get_integer && qcom.get_error && gen_textures && delete_textures && bind_texture &&
		       tex_parameteri && tex_storage_2d && egl_image_target_texture && gen_framebuffers && delete_framebuffers &&
		       bind_framebuffer && framebuffer_texture_2d && check_framebuffer_status && viewport && create_shader &&
		       shader_source && compile_shader && get_shader_iv && get_shader_info_log && delete_shader && create_program &&
		       attach_shader && link_program && get_program_iv && get_program_info_log && delete_program && use_program &&
		       get_uniform_location && uniform_1i && active_texture && draw_arrays && flush;
	}
};

struct egl_functions
{
	PFNEGLGETDISPLAYPROC get_display = nullptr;
	PFNEGLGETERRORPROC get_error = nullptr;
	PFNEGLINITIALIZEPROC initialize = nullptr;
	PFNEGLTERMINATEPROC terminate = nullptr;
	PFNEGLBINDAPIPROC bind_api = nullptr;
	PFNEGLCHOOSECONFIGPROC choose_config = nullptr;
	PFNEGLCREATEPBUFFERSURFACEPROC create_pbuffer_surface = nullptr;
	PFNEGLCREATECONTEXTPROC create_context = nullptr;
	PFNEGLMAKECURRENTPROC make_current = nullptr;
	PFNEGLDESTROYSURFACEPROC destroy_surface = nullptr;
	PFNEGLDESTROYCONTEXTPROC destroy_context = nullptr;
	PFNEGLQUERYSTRINGPROC query_string = nullptr;
	PFNEGLGETCURRENTDISPLAYPROC get_current_display = nullptr;
	PFNEGLGETCURRENTSURFACEPROC get_current_surface = nullptr;
	PFNEGLGETCURRENTCONTEXTPROC get_current_context = nullptr;
	PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
	PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC get_native_client_buffer = nullptr;
	PFNEGLCREATESYNCKHRPROC create_sync = nullptr;
	PFNEGLDESTROYSYNCKHRPROC destroy_sync = nullptr;
	PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_native_fence = nullptr;

	template<typename Function>
	static Function load(const char * name)
	{
		return load_android_function<Function>(name);
	}

	bool load_core()
	{
		get_display = load<PFNEGLGETDISPLAYPROC>("eglGetDisplay");
		get_error = load<PFNEGLGETERRORPROC>("eglGetError");
		initialize = load<PFNEGLINITIALIZEPROC>("eglInitialize");
		terminate = load<PFNEGLTERMINATEPROC>("eglTerminate");
		bind_api = load<PFNEGLBINDAPIPROC>("eglBindAPI");
		choose_config = load<PFNEGLCHOOSECONFIGPROC>("eglChooseConfig");
		create_pbuffer_surface = load<PFNEGLCREATEPBUFFERSURFACEPROC>("eglCreatePbufferSurface");
		create_context = load<PFNEGLCREATECONTEXTPROC>("eglCreateContext");
		make_current = load<PFNEGLMAKECURRENTPROC>("eglMakeCurrent");
		destroy_surface = load<PFNEGLDESTROYSURFACEPROC>("eglDestroySurface");
		destroy_context = load<PFNEGLDESTROYCONTEXTPROC>("eglDestroyContext");
		query_string = load<PFNEGLQUERYSTRINGPROC>("eglQueryString");
		get_current_display = load<PFNEGLGETCURRENTDISPLAYPROC>("eglGetCurrentDisplay");
		get_current_surface = load<PFNEGLGETCURRENTSURFACEPROC>("eglGetCurrentSurface");
		get_current_context = load<PFNEGLGETCURRENTCONTEXTPROC>("eglGetCurrentContext");
		return get_display && get_error && initialize && terminate && bind_api && choose_config && create_pbuffer_surface &&
		       create_context && make_current && destroy_surface && destroy_context && query_string && get_current_display &&
		       get_current_surface && get_current_context;
	}

	bool load_extensions()
	{
		create_image = load<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
		destroy_image = load<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
		get_native_client_buffer = load<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>("eglGetNativeClientBufferANDROID");
		create_sync = load<PFNEGLCREATESYNCKHRPROC>("eglCreateSyncKHR");
		destroy_sync = load<PFNEGLDESTROYSYNCKHRPROC>("eglDestroySyncKHR");
		dup_native_fence = load<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>("eglDupNativeFenceFDANDROID");
		return create_image && destroy_image && get_native_client_buffer && create_sync && destroy_sync && dup_native_fence;
	}
};

#endif

} // namespace

#if defined(__ANDROID__)

class qcom_motion_estimator::egl_context
{
public:
	struct imported_texture
	{
		AHardwareBuffer * buffer = nullptr;
		EGLImageKHR image = EGL_NO_IMAGE_KHR;
		uint32_t texture = 0;
		uint32_t target = 0;
	};

	bool initialize(std::string & reason)
	{
		if (not egl_.load_core())
		{
			reason = "required EGL core procedures are unavailable";
			return false;
		}

		display_ = egl_.get_display(EGL_DEFAULT_DISPLAY);
		if (display_ == EGL_NO_DISPLAY || not egl_.initialize(display_, &major_, &minor_))
		{
			reason = "eglInitialize failed";
			return false;
		}

		const char * extensions = egl_.query_string(display_, EGL_EXTENSIONS);
        if (not extensions)
        {
            reason = "EGL_EXTENSIONS is unavailable";
            return false;
        }
        const std::string_view extension_list{extensions};
        if (not has_extension(extension_list, "EGL_KHR_image_base") ||
            not has_extension(extension_list, "EGL_ANDROID_get_native_client_buffer") ||
            not has_extension(extension_list, "EGL_ANDROID_image_native_buffer") ||
            not has_extension(extension_list, "EGL_KHR_fence_sync") ||
            not has_extension(extension_list, "EGL_ANDROID_native_fence_sync"))
        {
            reason = "required EGL AHardwareBuffer/native-fence extensions are unavailable";
            return false;
        }
        if (not egl_.load_extensions())
        {
            reason = "required EGL image/sync procedures are unavailable";
            return false;
        }

        previous_display_ = egl_.get_current_display();
        previous_draw_ = egl_.get_current_surface(EGL_DRAW);
        previous_read_ = egl_.get_current_surface(EGL_READ);
        previous_context_ = egl_.get_current_context();

        if (not egl_.bind_api(EGL_OPENGL_ES_API))
        {
            reason = "eglBindAPI(EGL_OPENGL_ES_API) failed";
            return false;
        }

        constexpr EGLint config_attributes[] = {
                EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
        };
        EGLConfig config = nullptr;
        EGLint config_count = 0;
        if (not egl_.choose_config(display_, config_attributes, &config, 1, &config_count) || config_count != 1)
        {
            reason = "no EGL ES 3 pbuffer config is available";
            return false;
        }

        constexpr EGLint pbuffer_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface_ = egl_.create_pbuffer_surface(display_, config, pbuffer_attributes);
        if (surface_ == EGL_NO_SURFACE)
        {
            reason = "eglCreatePbufferSurface failed";
            return false;
        }

        constexpr EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = egl_.create_context(display_, config, EGL_NO_CONTEXT, context_attributes);
        if (context_ == EGL_NO_CONTEXT || not make_current())
        {
            reason = "eglCreateContext/eglMakeCurrent for ES 3 failed";
            return false;
        }

        gl_.qcom.get_proc_address = load_android_proc;
        if (not gl_.load_all())
        {
            reason = "required GLES ES 3 procedures are unavailable";
            return false;
        }

        const auto * gl_extension_bytes = gl_.qcom.get_string(gl_extensions);
        if (not gl_extension_bytes)
        {
            reason = "GLES GL_EXTENSIONS is unavailable";
            return false;
        }
        const std::string_view gl_extensions_list{reinterpret_cast<const char *>(gl_extension_bytes)};
        if (not has_extension(gl_extensions_list, "GL_OES_EGL_image") ||
            not has_extension(gl_extensions_list, "GL_OES_EGL_image_external") ||
            not has_extension(gl_extensions_list, "GL_OES_EGL_image_external_essl3"))
        {
            reason = "required GLES EGLImage/external-image extensions are unavailable";
            return false;
        }

        if (not compile_luma_program(reason))
            return false;

		AHardwareBuffer_Desc format_probe{
		        .width = 8,
		        .height = 8,
		        .layers = 1,
		        .format = AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT,
		        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER,
		        .stride = 0,
		        .rfu0 = 0,
		        .rfu1 = 0,
		};
        if (not AHardwareBuffer_isSupported(&format_probe))
        {
            reason = "RGBA16F AHardwareBuffer output is not supported";
            return false;
        }

        ready_ = true;
        return true;
    }

	~egl_context()
	{
		if (context_ != EGL_NO_CONTEXT && egl_.make_current)
		{
			make_current();
			if (gl_.delete_program && luma_program_)
				gl_.delete_program(luma_program_);
			luma_program_ = 0;
		}

		if (context_ != EGL_NO_CONTEXT && egl_.make_current)
		{
			if (previous_display_ != EGL_NO_DISPLAY)
				egl_.make_current(previous_display_, previous_draw_, previous_read_, previous_context_);
			else
				egl_.make_current(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		}

		if (context_ != EGL_NO_CONTEXT && egl_.destroy_context)
			egl_.destroy_context(display_, context_);
		if (surface_ != EGL_NO_SURFACE && egl_.destroy_surface)
			egl_.destroy_surface(display_, surface_);
		if (display_ != EGL_NO_DISPLAY && egl_.terminate)
			egl_.terminate(display_);
	}

	qcom_gl_api qcom_api() const { return gl_.qcom; }
	bool ready() const noexcept { return ready_; }

	bool make_current()
	{
		return display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE && context_ != EGL_NO_CONTEXT &&
		       egl_.make_current(display_, surface_, surface_, context_);
	}

	bool import_texture(AHardwareBuffer * buffer, uint32_t target, imported_texture & imported)
	{
		if (not ready_ || not buffer || not egl_.get_native_client_buffer || not egl_.create_image ||
		    not gl_.egl_image_target_texture)
			return false;

		AHardwareBuffer_Desc description{};
		AHardwareBuffer_describe(buffer, &description);
		if (description.layers != 1 || (description.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE) == 0)
			return false;

		EGLClientBuffer native_buffer = egl_.get_native_client_buffer(buffer);
		if (not native_buffer)
			return false;

		AHardwareBuffer_acquire(buffer);
		const EGLint image_attributes[] = {EGL_NONE};
		EGLImageKHR image =
		        egl_.create_image(display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, native_buffer, image_attributes);
		if (image == EGL_NO_IMAGE_KHR)
		{
			AHardwareBuffer_release(buffer);
			return false;
		}

		uint32_t texture = 0;
		gl_.gen_textures(1, &texture);
		if (texture == 0)
		{
			egl_.destroy_image(display_, image);
			AHardwareBuffer_release(buffer);
			return false;
		}

		clear_errors(gl_.qcom);
		gl_.bind_texture(target, texture);
		gl_.tex_parameteri(target, gl_texture_min_filter, static_cast<int32_t>(gl_linear));
		gl_.tex_parameteri(target, gl_texture_mag_filter, static_cast<int32_t>(gl_linear));
		gl_.tex_parameteri(target, gl_texture_wrap_s, static_cast<int32_t>(gl_clamp_to_edge));
		gl_.tex_parameteri(target, gl_texture_wrap_t, static_cast<int32_t>(gl_clamp_to_edge));
		gl_.egl_image_target_texture(target, image);
		if (gl_.qcom.get_error() != gl_no_error)
		{
			gl_.delete_textures(1, &texture);
			egl_.destroy_image(display_, image);
			AHardwareBuffer_release(buffer);
			return false;
		}

		imported = {.buffer = buffer, .image = image, .texture = texture, .target = target};
		return true;
	}

	void destroy_texture(imported_texture & imported)
	{
		if (imported.texture && gl_.delete_textures)
			gl_.delete_textures(1, &imported.texture);
		if (imported.image != EGL_NO_IMAGE_KHR && egl_.destroy_image)
			egl_.destroy_image(display_, imported.image);
		if (imported.buffer)
			AHardwareBuffer_release(imported.buffer);
		imported = {};
	}

	bool create_luma_texture(uint32_t width, uint32_t height, uint32_t & texture)
	{
		if (width == 0 || height == 0)
			return false;
		gl_.gen_textures(1, &texture);
		if (texture == 0)
			return false;
		clear_errors(gl_.qcom);
		gl_.bind_texture(gl_texture_2d, texture);
		gl_.tex_parameteri(gl_texture_2d, gl_texture_min_filter, static_cast<int32_t>(gl_linear));
		gl_.tex_parameteri(gl_texture_2d, gl_texture_mag_filter, static_cast<int32_t>(gl_linear));
		gl_.tex_parameteri(gl_texture_2d, gl_texture_wrap_s, static_cast<int32_t>(gl_clamp_to_edge));
		gl_.tex_parameteri(gl_texture_2d, gl_texture_wrap_t, static_cast<int32_t>(gl_clamp_to_edge));
		gl_.tex_storage_2d(gl_texture_2d, 1, gl_r8, static_cast<int32_t>(width), static_cast<int32_t>(height));
		if (gl_.qcom.get_error() != gl_no_error)
		{
			gl_.delete_textures(1, &texture);
			texture = 0;
			return false;
		}
		return true;
	}

	void destroy_texture(uint32_t & texture)
	{
		if (texture && gl_.delete_textures)
			gl_.delete_textures(1, &texture);
		texture = 0;
	}

	bool render_luma(imported_texture & source, uint32_t luma, uint32_t width, uint32_t height)
	{
		if (not source.texture || not luma || !luma_program_ || width == 0 || height == 0)
			return false;
		uint32_t framebuffer = 0;
		gl_.gen_framebuffers(1, &framebuffer);
		if (framebuffer == 0)
			return false;
		clear_errors(gl_.qcom);
		gl_.bind_framebuffer(gl_framebuffer, framebuffer);
		gl_.framebuffer_texture_2d(gl_framebuffer, gl_color_attachment0, gl_texture_2d, luma, 0);
		const bool complete = gl_.check_framebuffer_status(gl_framebuffer) == gl_framebuffer_complete;
		if (complete)
		{
			gl_.viewport(0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height));
			gl_.use_program(luma_program_);
			gl_.active_texture(gl_texture0);
			gl_.bind_texture(gl_texture_external_oes, source.texture);
			gl_.uniform_1i(luma_sampler_location_, 0);
			gl_.draw_arrays(gl_triangles, 0, 3);
			gl_.bind_texture(gl_texture_external_oes, 0);
			gl_.use_program(0);
		}
		gl_.bind_framebuffer(gl_framebuffer, 0);
		gl_.delete_framebuffers(1, &framebuffer);
		return complete && gl_.qcom.get_error() == gl_no_error;
	}

	bool export_fence(int & fence_fd)
	{
		fence_fd = -1;
		if (not egl_.create_sync || not egl_.destroy_sync || not egl_.dup_native_fence || not gl_.flush)
			return false;
		const EGLint attributes[] = {EGL_NONE};
		EGLSyncKHR sync = egl_.create_sync(display_, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
		if (sync == EGL_NO_SYNC_KHR)
			return false;
		gl_.flush();
		fence_fd = egl_.dup_native_fence(display_, sync);
		egl_.destroy_sync(display_, sync);
		if (fence_fd < 0)
		{
			fence_fd = -1;
			return false;
		}
		return true;
	}

private:
	bool compile_luma_program(std::string & reason)
	{
		static constexpr char vertex_source[] = R"GLSL(#version 300 es
precision highp float;
out vec2 uv;
void main() {
    const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 position = positions[gl_VertexID];
    uv = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
)GLSL";
		static constexpr char fragment_source[] = R"GLSL(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
in vec2 uv;
uniform samplerExternalOES input_image;
layout(location = 0) out float out_luma;
void main() {
    vec3 color = texture(input_image, uv).rgb;
    out_luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
}
)GLSL";

		const auto compile = [&](uint32_t type, const char * source) -> uint32_t {
			uint32_t shader = gl_.create_shader(type);
			if (not shader)
				return 0;
			const char * sources[] = {source};
			gl_.shader_source(shader, 1, sources, nullptr);
			gl_.compile_shader(shader);
			int32_t compiled = 0;
			gl_.get_shader_iv(shader, gl_compile_status, &compiled);
			if (!compiled)
			{
				char log[512]{};
				int32_t length = 0;
				gl_.get_shader_info_log(shader, static_cast<int32_t>(sizeof(log)), &length, log);
				reason = std::string("luma shader compilation failed: ") + log;
				gl_.delete_shader(shader);
				return 0;
			}
			return shader;
		};

		const uint32_t vertex = compile(gl_vertex_shader, vertex_source);
		const uint32_t fragment = compile(gl_fragment_shader, fragment_source);
		if (not vertex || not fragment)
		{
			if (vertex)
				gl_.delete_shader(vertex);
			if (fragment)
				gl_.delete_shader(fragment);
			if (reason.empty())
				reason = "luma shader compilation failed";
			return false;
		}

		luma_program_ = gl_.create_program();
		if (not luma_program_)
		{
			gl_.delete_shader(vertex);
			gl_.delete_shader(fragment);
			reason = "glCreateProgram failed";
			return false;
		}
		gl_.attach_shader(luma_program_, vertex);
		gl_.attach_shader(luma_program_, fragment);
		gl_.link_program(luma_program_);
		gl_.delete_shader(vertex);
		gl_.delete_shader(fragment);
		int32_t linked = 0;
		gl_.get_program_iv(luma_program_, gl_link_status, &linked);
		if (!linked)
		{
			char log[512]{};
			int32_t length = 0;
			gl_.get_program_info_log(luma_program_, static_cast<int32_t>(sizeof(log)), &length, log);
			reason = std::string("luma program link failed: ") + log;
			gl_.delete_program(luma_program_);
			luma_program_ = 0;
			return false;
		}
		luma_sampler_location_ = gl_.get_uniform_location(luma_program_, "input_image");
		if (luma_sampler_location_ < 0)
		{
			reason = "luma shader sampler uniform is unavailable";
			gl_.delete_program(luma_program_);
			luma_program_ = 0;
			return false;
		}
		return gl_.qcom.get_error() == gl_no_error;
	}

	egl_functions egl_;
	gl_functions gl_;
	EGLDisplay display_ = EGL_NO_DISPLAY;
	EGLSurface surface_ = EGL_NO_SURFACE;
	EGLContext context_ = EGL_NO_CONTEXT;
	EGLDisplay previous_display_ = EGL_NO_DISPLAY;
	EGLSurface previous_draw_ = EGL_NO_SURFACE;
	EGLSurface previous_read_ = EGL_NO_SURFACE;
	EGLContext previous_context_ = EGL_NO_CONTEXT;
	EGLint major_ = 0;
	EGLint minor_ = 0;
	uint32_t luma_program_ = 0;
	int32_t luma_sampler_location_ = -1;
	bool ready_ = false;
};

#endif

qcom_flow_output::qcom_flow_output(AHardwareBuffer * buffer,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t block_width,
                                   uint32_t block_height,
                                   int fence_fd) noexcept :
        buffer_(buffer),
        width_(width),
        height_(height),
        block_width_(block_width),
        block_height_(block_height),
        fence_fd_(fence_fd)
{
}

qcom_flow_output::~qcom_flow_output()
{
	reset();
}

qcom_flow_output::qcom_flow_output(qcom_flow_output && other) noexcept :
        buffer_(std::exchange(other.buffer_, nullptr)),
        width_(std::exchange(other.width_, 0)),
        height_(std::exchange(other.height_, 0)),
        block_width_(std::exchange(other.block_width_, 0)),
        block_height_(std::exchange(other.block_height_, 0)),
        fence_fd_(std::exchange(other.fence_fd_, -1))
{
}

qcom_flow_output & qcom_flow_output::operator=(qcom_flow_output && other) noexcept
{
	if (this != &other)
	{
		reset();
		buffer_ = std::exchange(other.buffer_, nullptr);
		width_ = std::exchange(other.width_, 0);
		height_ = std::exchange(other.height_, 0);
		block_width_ = std::exchange(other.block_width_, 0);
		block_height_ = std::exchange(other.block_height_, 0);
		fence_fd_ = std::exchange(other.fence_fd_, -1);
	}
	return *this;
}

void qcom_flow_output::reset() noexcept
{
#if defined(__ANDROID__)
	if (buffer_)
		AHardwareBuffer_release(buffer_);
	if (fence_fd_ >= 0)
		close(fence_fd_);
#endif
	buffer_ = nullptr;
	fence_fd_ = -1;
	width_ = 0;
	height_ = 0;
	block_width_ = 0;
	block_height_ = 0;
}

qcom_motion_estimator::qcom_motion_estimator(qcom_gl_api api) : api_(api)
{
	capabilities_.reason = "QCOM GLES backend has not been probed";
}

qcom_motion_estimator::~qcom_motion_estimator() = default;

std::unique_ptr<qcom_motion_estimator> qcom_motion_estimator::probe()
{
#if defined(__ANDROID__)
	auto context = std::make_unique<egl_context>();
	std::string reason;
	if (not context->initialize(reason))
	{
		auto result = std::make_unique<qcom_motion_estimator>();
		result->capabilities_.reason = std::move(reason);
		return result;
	}
	auto result = probe(context->qcom_api());
	result->egl_context_ = std::move(context);
	if (result->capabilities_.available() && result->capabilities_.search_block_width > 0 &&
	    result->capabilities_.search_block_height > 0 && result->egl_context_->ready())
	{
		result->capabilities_.hardware_buffer_path = true;
		result->capabilities_.reason += "; EGL/AHardwareBuffer path is ready";
	}
	else
	{
		result->capabilities_.reason += "; EGL/AHardwareBuffer path is unavailable";
	}
	return result;
#else
	auto result = std::make_unique<qcom_motion_estimator>();
	result->capabilities_.reason = "QCOM GLES motion estimation is only probed on Android";
	return result;
#endif
}

std::unique_ptr<qcom_motion_estimator> qcom_motion_estimator::probe(qcom_gl_api api)
{
	auto result = std::make_unique<qcom_motion_estimator>(api);
	result->resolve_core_entry_points();
	if (not result->api_.get_string)
	{
		result->capabilities_.reason = "glGetString could not be resolved";
		return result;
	}
	const auto * extension_bytes = result->api_.get_string(gl_extensions);
	if (not extension_bytes)
	{
		result->capabilities_.reason = "no current GLES context (GL_EXTENSIONS is null)";
		return result;
	}
	const std::string_view extensions{reinterpret_cast<const char *>(extension_bytes)};
	if (not has_extension(extensions, extension_name))
	{
		result->capabilities_.reason = "current GLES context does not advertise GL_QCOM_motion_estimation";
		return result;
	}
	result->capabilities_.extension_advertised = true;
	result->estimate_motion_ = load_function<estimate_motion_fn>(result->api_, "glTexEstimateMotionQCOM");
	result->estimate_regions_ = load_function<estimate_regions_fn>(result->api_, "glTexEstimateMotionRegionsQCOM");
	if (not result->estimate_motion_)
	{
		result->capabilities_.reason = "GL_QCOM_motion_estimation is advertised but glTexEstimateMotionQCOM is unavailable";
		return result;
	}
	result->capabilities_.backend = motion_estimator_backend::qcom_gles;
	result->capabilities_.estimates_motion = true;
	result->capabilities_.estimates_regions = result->estimate_regions_ != nullptr;
	result->capabilities_.can_check_errors = result->api_.get_error != nullptr;
	if (not result->capabilities_.can_check_errors)
	{
		result->capabilities_.backend = motion_estimator_backend::none;
		result->capabilities_.estimates_motion = false;
		result->capabilities_.estimates_regions = false;
		result->capabilities_.reason = "GL_QCOM_motion_estimation is advertised but GL error checking is unavailable";
		return result;
	}
	result->query_block_dimensions();
	result->capabilities_.reason = "GL_QCOM_motion_estimation and glTexEstimateMotionQCOM are available";
	if (result->capabilities_.search_block_width == 0 || result->capabilities_.search_block_height == 0)
		result->capabilities_.reason += "; block dimensions were not reported";
	return result;
}

void qcom_motion_estimator::resolve_core_entry_points()
{
	if (not api_.get_proc_address)
		return;
	if (not api_.get_string)
		api_.get_string = load_function<qcom_gl_api::get_string_fn>(api_, "glGetString");
	if (not api_.get_integer)
		api_.get_integer = load_function<qcom_gl_api::get_integer_fn>(api_, "glGetIntegerv");
	if (not api_.get_error)
		api_.get_error = load_function<qcom_gl_api::get_error_fn>(api_, "glGetError");
}

void qcom_motion_estimator::query_block_dimensions()
{
	if (not api_.get_integer)
		return;
	clear_errors(api_);
	int32_t block_width = 0;
	int32_t block_height = 0;
	api_.get_integer(gl_motion_estimation_search_block_x_qcom, &block_width);
	api_.get_integer(gl_motion_estimation_search_block_y_qcom, &block_height);
	const uint32_t error = api_.get_error ? api_.get_error() : gl_no_error;
	if (error != gl_no_error)
		return;
	if (block_width > 0)
		capabilities_.search_block_width = static_cast<uint32_t>(block_width);
	if (block_height > 0)
		capabilities_.search_block_height = static_cast<uint32_t>(block_height);
}

motion_estimation_result qcom_motion_estimator::estimate(const motion_estimation_request & request)
{
	if (not capabilities_.available())
		return {.status = motion_estimation_status::unavailable, .message = capabilities_.reason};
	if (request.reference_texture == 0 || request.target_texture == 0 || request.output_texture == 0)
		return {.status = motion_estimation_status::invalid_request,
		        .message = "reference, target and output texture names must be non-zero"};
	if (request.estimate_regions)
	{
		if (request.mask_texture == 0)
			return {.status = motion_estimation_status::invalid_request,
			        .message = "region estimation requires a non-zero R8UI mask texture"};
		if (not estimate_regions_)
			return {.status = motion_estimation_status::unavailable,
			        .message = "GL_QCOM_motion_estimation region procedure is unavailable"};
	}
	if (not api_.get_error)
		return {.status = motion_estimation_status::unavailable,
		        .message = "GL error checking is unavailable; refusing unverified motion estimation"};
	clear_errors(api_);
	if (request.estimate_regions)
		estimate_regions_(request.reference_texture, request.target_texture, request.output_texture, request.mask_texture);
	else
		estimate_motion_(request.reference_texture, request.target_texture, request.output_texture);
	const uint32_t error = api_.get_error();
	if (error != gl_no_error)
		return {.status = motion_estimation_status::driver_error, .api_error = error, .message = format_gl_error(error)};
	return {.status = motion_estimation_status::generated,
	        .message = request.estimate_regions ? "QCOM region motion estimation submitted" : "QCOM motion estimation submitted"};
}

qcom_ahardware_buffer_result qcom_motion_estimator::estimate_ahardware_buffers(
        AHardwareBuffer * reference,
        AHardwareBuffer * target,
        uint32_t width,
        uint32_t height)
{
#if defined(__ANDROID__)
	if (not capabilities_.hardware_buffer_path || not egl_context_ || not egl_context_->ready())
			return {.status = motion_estimation_status::unavailable, .output = nullptr, .message = "EGL/AHardwareBuffer QCOM path is unavailable"};
		if (not reference || not target || width == 0 || height == 0)
			return {.status = motion_estimation_status::unavailable, .output = nullptr, .message = "invalid AHardwareBuffer input or dimensions"};
		if (not egl_context_->make_current())
			return {.status = motion_estimation_status::unavailable, .output = nullptr, .message = "eglMakeCurrent failed for QCOM estimation"};
	AHardwareBuffer_Desc reference_description{};
	AHardwareBuffer_Desc target_description{};
	AHardwareBuffer_describe(reference, &reference_description);
	AHardwareBuffer_describe(target, &target_description);
	if (reference_description.width != width || reference_description.height != height ||
		    target_description.width != width || target_description.height != height)
			return {.status = motion_estimation_status::unavailable, .output = nullptr, .message = "AHardwareBuffer dimensions do not match request"};
	const uint32_t block_width = capabilities_.search_block_width;
	const uint32_t block_height = capabilities_.search_block_height;
	if (block_width == 0 || block_height == 0)
			return {.status = motion_estimation_status::unavailable, .output = nullptr, .message = "QCOM search block dimensions are unavailable"};
	if (width % block_width != 0 || height % block_height != 0)
			return {.status = motion_estimation_status::unavailable,
			        .output = nullptr,
			        .message = "AHardwareBuffer dimensions must be exact multiples of the QCOM search block"};
	const uint32_t output_width = width / block_width;
	const uint32_t output_height = height / block_height;
	if (output_width == 0 || output_height == 0)
			return {.status = motion_estimation_status::unavailable, .output = nullptr, .message = "invalid reduced flow dimensions"};

	qcom_motion_estimator::egl_context::imported_texture reference_texture;
	qcom_motion_estimator::egl_context::imported_texture target_texture;
	qcom_motion_estimator::egl_context::imported_texture output_texture;
	uint32_t reference_luma = 0;
	uint32_t target_luma = 0;
	AHardwareBuffer * output_buffer = nullptr;
	int fence_fd = -1;
	const auto fail = [&](std::string message) {
		egl_context_->destroy_texture(reference_texture);
		egl_context_->destroy_texture(target_texture);
		egl_context_->destroy_texture(output_texture);
		egl_context_->destroy_texture(reference_luma);
		egl_context_->destroy_texture(target_luma);
		if (output_buffer)
			AHardwareBuffer_release(output_buffer);
		if (fence_fd >= 0)
			close(fence_fd);
		return qcom_ahardware_buffer_result{.status = motion_estimation_status::unavailable,
		                                    .output = nullptr,
		                                    .message = std::move(message)};
	};

	if (not egl_context_->import_texture(reference, gl_texture_external_oes, reference_texture))
		return fail("reference AHardwareBuffer cannot be imported as an EGL external image");
	if (not egl_context_->import_texture(target, gl_texture_external_oes, target_texture))
		return fail("target AHardwareBuffer cannot be imported as an EGL external image");
	if (not egl_context_->create_luma_texture(width, height, reference_luma) ||
	    not egl_context_->create_luma_texture(width, height, target_luma))
		return fail("R8 luma texture allocation failed");
	if (not egl_context_->render_luma(reference_texture, reference_luma, width, height) ||
	    not egl_context_->render_luma(target_texture, target_luma, width, height))
		return fail("external AHardwareBuffer to R8 luma conversion failed");

	AHardwareBuffer_Desc output_description{
	        .width = output_width,
		        .height = output_height,
		        .layers = 1,
		        .format = AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT,
		        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER,
		        .stride = 0,
		        .rfu0 = 0,
		        .rfu1 = 0,
	};
	if (not AHardwareBuffer_isSupported(&output_description) || AHardwareBuffer_allocate(&output_description, &output_buffer) != 0)
		return fail("RGBA16F flow AHardwareBuffer allocation is unavailable");
	if (not egl_context_->import_texture(output_buffer, gl_texture_2d, output_texture))
		return fail("RGBA16F flow AHardwareBuffer cannot be imported as EGL texture");

	const auto estimation = estimate({.reference_texture = reference_luma,
	                                  .target_texture = target_luma,
	                                  .output_texture = output_texture.texture});
	if (not estimation.generated())
		return fail("GL_QCOM_motion_estimation failed: " + estimation.message);
	if (not egl_context_->export_fence(fence_fd))
		return fail("EGL native-fence export is unavailable");

	egl_context_->destroy_texture(reference_texture);
	egl_context_->destroy_texture(target_texture);
	egl_context_->destroy_texture(output_texture);
	egl_context_->destroy_texture(reference_luma);
	egl_context_->destroy_texture(target_luma);

	return {.status = motion_estimation_status::generated,
	        .output = std::unique_ptr<qcom_flow_output>(new qcom_flow_output(
                output_buffer, output_width, output_height, block_width, block_height, fence_fd)),
	        .message = "QCOM motion estimation completed and native fence exported"};
#else
	(void)reference;
	(void)target;
	(void)width;
	(void)height;
	return {.status = motion_estimation_status::unavailable,
	        .output = nullptr,
	        .message = "AHardwareBuffer QCOM path is unavailable outside Android"};
#endif
}

} // namespace frame_synthesis
