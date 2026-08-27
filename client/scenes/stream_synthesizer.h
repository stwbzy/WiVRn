/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <openxr/openxr.h>
#include <vulkan/vulkan_raii.hpp>

namespace scenes
{

/**
 * Isolated client-side, color-only SSW-like synthesizer.
 *
 * The caller owns the OpenXR stream swapchain and supplies its images to the
 * constructor.  The stream swapchain must have been created with
 * XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT because capture_real() copies the
 * completed, real defoveated image into its private history.  The class does
 * not submit, acquire or release an OpenXR swapchain image.
 *
 * Only capture_real() mutates history.  synthesize() never writes a history
 * image, so a generated frame can never become the source for a later flow
 * estimate.  The metadata returned by latest_real_metadata() is deliberately
 * the latest real pose; the integration layer should use it when submitting
 * the projection layer and let OpenXR perform late ATW.
 */
class stream_synthesizer final
{
public:
	static constexpr uint32_t view_count = 2;

	enum class state : uint8_t
	{
		unavailable,
		ready,
	};

	enum class capture_status : uint8_t
	{
		captured,
		reset_and_captured,
		duplicate,
		unavailable,
		invalid,
	};

	enum class synthesis_status : uint8_t
	{
		generated,
		no_history,
		already_generated,
		unavailable,
		invalid,
	};

	struct configuration
	{
		vk::Extent2D extent{};
		vk::Format format = vk::Format::eUndefined;
		// The images must be the array images of the stereo stream swapchain.
		std::span<const vk::Image> output_images{};
		float confidence_threshold = 0.35f;
	};

	struct image_ref
	{
		vk::Image image = nullptr;
		// The layout observed by the caller before this operation.  The
		// synthesizer restores source images to this layout and leaves output
		// images in eColorAttachmentOptimal.
		vk::ImageLayout layout = vk::ImageLayout::eColorAttachmentOptimal;
	};

	struct frame_metadata
	{
		uint64_t frame_id = 0;
		uint64_t connection_generation = 0;
		XrTime display_time = 0;
		std::array<XrPosef, view_count> pose{};
		std::array<XrFovf, view_count> fov{};
		// A discontinuity discards the previous real frame before capturing
		// this one.  The new frame can become the first history entry.
		bool discontinuity = false;
	};

	struct capture_result
	{
		capture_status status = capture_status::unavailable;

		bool accepted() const noexcept
		{
			return status == capture_status::captured || status == capture_status::reset_and_captured;
		}
	};

	struct synthesis_result
	{
		synthesis_status status = synthesis_status::unavailable;

		bool generated() const noexcept
		{
			return status == synthesis_status::generated;
		}
	};

	stream_synthesizer(vk::raii::Device & device, vk::raii::PhysicalDevice & physical_device, configuration config);
	~stream_synthesizer();

	stream_synthesizer(const stream_synthesizer &) = delete;
	stream_synthesizer & operator=(const stream_synthesizer &) = delete;
	stream_synthesizer(stream_synthesizer &&) noexcept;
	stream_synthesizer & operator=(stream_synthesizer &&) noexcept;

	state status() const noexcept;
	bool available() const noexcept
	{
		return status() == state::ready;
	}
	std::string_view unavailable_reason() const noexcept;

	/**
	 * Copy one complete stereo, real defoveated frame into the next ring slot.
	 * The source is an array image with two layers.  A duplicate/non-monotonic
	 * frame ID is rejected and does not change history.  A discontinuity or
	 * connection-generation change clears the old pair before this copy.
	 */
	capture_result capture_real(
	        vk::raii::CommandBuffer & command_buffer,
	        image_ref source,
	        const frame_metadata & metadata);

	/**
	 * Generate at most one intermediate frame for the latest pair of real
	 * frames.  destination must be one of the output images given at
	 * construction and must be a stereo array image.  The destination remains
	 * in eColorAttachmentOptimal for the caller's composition step.
	 */
	synthesis_result synthesize(
	        vk::raii::CommandBuffer & command_buffer,
	        image_ref destination);

	/** Drop both real history images and the one-synthetic-per-real guard. */
	void reset() noexcept;

	/** The latest real metadata, suitable for preserving the ATW pose contract. */
	std::optional<frame_metadata> latest_real_metadata() const;

private:
	struct impl;
	std::unique_ptr<impl> impl_;
};

} // namespace scenes
