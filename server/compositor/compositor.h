/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Copyright 2019-2024, Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.

#pragma once

#include "util/comp_base.h"
#include "util/u_logging.h"

#include "encoder/encoder_settings.h"
#include "foveation.h"
#include "fsr_upscaler.h"
#include "layer_squasher.h"
#include "pacer.h"
#include "utils/wivrn_vk_bundle.h"

#include "main/comp_compositor.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace wivrn
{

class wivrn_session;
class video_encoder;

class compositor : public comp_base
{
public:
	struct image
	{
		std::atomic<bool> busy = false;
		image_allocation image;
		vk::raii::ImageView view_y;
		vk::raii::ImageView view_cbcr;
		to_headset::video_stream_data_shard::view_info_t view_info{};
		uint64_t frame_index;
	};

private:
	struct timings
	{
		std::array<float, 50> values{};
		int index = 0;
		u_var_timing var{
		        .values = {
		                .data = values.data(),
		                .index_ptr = &index,
		                .length = int(values.size()),
		        },
		        .range = 1000,
		        .dynamic_rescale = true,
		        .unit = "µs",
		};

		void add(float us);
	};

	const u_logging_level log_level;
	timings squasher_times;
	timings foveation_times;
	wivrn_session & session;
	vk_bundle vk;
	vk::raii::CommandPool cmd_pool;
	vk::raii::QueryPool query_pool;

	std::array<encoder_settings, 3> settings;

	std::array<image, 4> images;
	vk::raii::CommandBuffer cmd;
	vk::raii::Semaphore sem;
	uint64_t sem_value = 0;

	std::atomic<float> requested_refresh_rate;
	std::atomic<float> frame_rate;
	wivrn::pacer pacer;

	layer_squasher squasher;
	upscaling_selection scaling_selection;
	std::optional<wivrn::fsr_upscaler> spatial_upscaler;
	wivrn::foveation foveation;

	std::array<std::unique_ptr<video_encoder>, 3> encoders;

	std::mutex encode_mutex;
	std::condition_variable encode_cv;
	std::deque<size_t> encode_requests;
	std::jthread encoder_thread;
	std::atomic<bool> paused{false};
	// Serializes frame slots/pacer state with pause and resume. A pause waits for
	// an in-flight commit, then guarantees that no later commit can enqueue work.
	std::mutex frame_mutex;

	struct
	{
		comp_frame waited{.id = -1};
		comp_frame rendering{.id = -1};
	} frame;

	struct frame_generation_history
	{
		image_allocation image;
		std::array<vk::raii::ImageView, 2> views;
		bool valid = false;

		frame_generation_history(vk_bundle &, vk::Extent3D);
	};

	struct frame_generation_backend
	{
		image_allocation output;
		vk::raii::Sampler sampler;
		std::array<vk::raii::ImageView, 2> output_views;
		vk::raii::DescriptorSetLayout ds_layout;
		vk::raii::PipelineLayout layout;
		vk::raii::Pipeline pipeline;
		vk::raii::DescriptorPool descriptor_pool;
		std::array<vk::DescriptorSet, 2> descriptor_sets{};
		bool output_valid = false;

		frame_generation_backend(vk_bundle &, vk::Extent3D);
	};

	struct frame_generation_flow
	{
		image_allocation coarse_motion;
		std::array<vk::raii::ImageView, 2> coarse_motion_views;
		image_allocation motion;
		std::array<vk::raii::ImageView, 2> motion_views;
		vk::raii::Sampler sampler;
		vk::raii::DescriptorSetLayout ds_layout;
		vk::raii::PipelineLayout layout;
		vk::raii::Pipeline pipeline;
		vk::raii::DescriptorPool descriptor_pool;
		std::array<vk::DescriptorSet, 4> descriptor_sets{};
		bool output_valid = false;

		frame_generation_flow(vk_bundle &, vk::Extent3D);
	};

	std::optional<frame_generation_history> frame_history;
	std::optional<frame_generation_backend> frame_generation;
	std::optional<frame_generation_flow> frame_generation_flow_backend;

	struct
	{
		bool requested = false;
		bool handoff_requested = false;
		bool handoff_enabled = false;
		bool double_rate_requested = false;
		bool double_rate_enabled = false;
		bool color_only_requested = false;
		bool history_enabled = false;
		bool backend_enabled = false;
		bool reported = false;
		bool last_inputs_ready = false;
		uint32_t last_ready_views = 0;
		uint64_t frames = 0;
		uint64_t history_captures = 0;
		uint64_t backend_dispatches = 0;
		uint64_t handoff_frames = 0;
		uint64_t double_rate_pairs = 0;
		frame_synthesis_mode mode = frame_synthesis_mode::off;
		frame_synthesis_backend backend = frame_synthesis_backend::auto_select;
		// Half-rate application pacing is shared by both implementations.  Only
		// the server backend also creates and transports an interval frame.
		bool half_rate_enabled = false;
		bool session_disabled = false;
		runtime_fallback_reason fallback_reason = runtime_fallback_reason::none;
		uint32_t auto_over_budget_windows = 0;
		uint32_t auto_headroom_windows = 0;
		uint32_t flow_deadline_windows = 0;
		uint32_t flow_samples = 0;
		uint32_t flow_over_budget = 0;
		uint32_t network_deadline_windows = 0;
		uint32_t feedback_samples = 0;
		uint32_t missed_feedback = 0;
		uint64_t last_feedback_frame_index = UINT64_MAX;
		uint64_t real_frames = 0;
		uint64_t synthetic_frames = 0;
		float optical_flow_ms = 0;
		float missed_interval_percent = 0;
	} frame_generation_probe;

	int64_t previous_display_time_ns = 0;
	std::array<XrPosef, 2> previous_view_poses{};
	std::array<XrFovf, 2> previous_view_fovs{};
	bool previous_view_poses_valid = false;

	// Transport frame IDs must never go backwards when ASW is enabled or
	// disabled at runtime. Keep the application-frame association separately
	// so pacer feedback can still be correlated after assigning a monotonic
	// transport sequence to both real and synthetic frames.
	struct stream_frame_mapping
	{
		uint64_t stream_frame_index = UINT64_MAX;
		int64_t application_frame_id = -1;
		bool application_frame = false;
	};
	static constexpr size_t stream_frame_mapping_count = 1024;
	std::array<stream_frame_mapping, stream_frame_mapping_count> stream_frame_mappings{};
	uint64_t next_stream_frame_index = 0;

	xrt_result_t begin_session(const xrt_begin_session_info * info)
	{
		return XRT_SUCCESS;
	};
	xrt_result_t end_session()
	{
		return XRT_SUCCESS;
	};

	xrt_result_t predict_frame(int64_t * out_frame_id,
	                           int64_t * out_wake_time_ns,
	                           int64_t * out_predicted_gpu_time_ns,
	                           int64_t * out_predicted_display_time_ns,
	                           int64_t * out_predicted_display_period_ns);

	xrt_result_t mark_frame(int64_t frame_id,
	                        xrt_compositor_frame_point point,
	                        int64_t when_ns);

	xrt_result_t begin_frame(int64_t frame_id)
	{
		return XRT_SUCCESS;
	}
	xrt_result_t discard_frame(int64_t frame_id)
	{
		return XRT_SUCCESS;
	};

	xrt_result_t layer_commit(xrt_graphics_sync_handle_t sync_handle);
	void inspect_frame_generation_inputs();
	bool dispatch_frame_generation_motion(vk::raii::CommandBuffer &,
	                                      const std::array<vk::ImageView, 2> &,
	                                      const std::array<XrPosef, 2> &,
	                                      const std::array<XrFovf, 2> &);
	bool dispatch_frame_generation_candidate(vk::raii::CommandBuffer &,
	                                         const std::array<vk::ImageView, 2> &,
	                                         const std::array<XrPosef, 2> &,
	                                         const std::array<XrFovf, 2> &,
	                                         const std::array<XrPosef, 2> &,
	                                         const std::array<XrFovf, 2> &);
	void capture_frame_generation_history(vk::raii::CommandBuffer &);
	bool initialize_frame_synthesis_backend();
	void reset_frame_synthesis_temporal_state();
	void set_frame_synthesis_active(bool active);
	void disable_frame_synthesis_for_session(runtime_fallback_reason reason);
	void update_frame_synthesis_policy(float optical_flow_ms);
	void send_runtime_telemetry();

	xrt_result_t get_display_refresh_rate(float * out_display_refresh_rate_hz);

public:
	xrt_result_t request_display_refresh_rate(float display_refresh_rate_hz);

	static xrt_result_t get_view_config(xrt_compositor_native *,
	                                    xrt_view_type view_type,
	                                    xrt_view_config * out_view_config);

private:
	void destroy()
	{
		// do nothing, actually owned by wivrn_session object
	}

	int acquire_image();

	void encoder_work(std::stop_token);

	void send_video_stream_description();

public:
	using base_t = xrt_compositor;
	compositor(wivrn_session &);
	~compositor();

	xrt_system_compositor_info sys_info() const;

	int64_t get_frame_duration() const
	{
		return pacer.get_frame_duration();
	}

	float get_requested_refresh_rate() const
	{
		return requested_refresh_rate;
	}

	float get_framerate() const
	{
		return frame_rate;
	}
	void set_framerate(float hz);
	void set_frame_synthesis_mode(frame_synthesis_mode mode);
	void set_frame_synthesis_backend(frame_synthesis_backend backend);

	void set_bitrate(uint32_t);
	void update_tracking(const from_headset::tracking &);
	void update_foveation_center_override(const from_headset::override_foveation_center &);

	void resume();
	void pause();

	void on_feedback(const from_headset::feedback &, const clock_offset &);
};

} // namespace wivrn
