/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "stream.h"

#include "application.h"
#include "utils/i18n.h"
#include "utils/named_thread.h"

#include <spdlog/spdlog.h>
#include <uni_algo/case.h>

namespace
{
int64_t steady_now_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	               std::chrono::steady_clock::now().time_since_epoch())
	        .count();
}
} // namespace

void scenes::stream::process_packets()
{
#ifdef __ANDROID__
	application::instance().setup_jni();
#endif
	last_control_heartbeat_ns = steady_now_ns();
	last_stream_heartbeat_ns = last_control_heartbeat_ns.load();
	auto next_heartbeat = std::chrono::steady_clock::now();
	static constexpr auto heartbeat_period = std::chrono::seconds(1);
	static constexpr auto heartbeat_timeout = std::chrono::seconds(4);
	while (state_ != state::shutdown)
	{
		try
		{
			connection_close_reason loss_reason = connection_close_reason::peer_closed;
			const int poll_result = network_session->poll(*this, std::chrono::milliseconds(250));
			const auto now = std::chrono::steady_clock::now();
			if (state_ == state::disconnecting && disconnect_ack_received.load())
			{
				exit();
				break;
			}
			if (poll_result >= 0)
			{
				if (now >= next_heartbeat)
				{
					const auto sequence = ++heartbeat_sequence;
					network_session->send_control(from_headset::heartbeat{
					        .connection_generation = network_session->generation(),
					        .sequence = sequence,
					        .stream_channel = false,
					});
					if (network_session->has_stream())
					{
						network_session->send_stream(from_headset::heartbeat{
						        .connection_generation = network_session->generation(),
						        .sequence = sequence,
						        .stream_channel = true,
						});
					}
					next_heartbeat = now + heartbeat_period;
				}

				if (!network_session->is_active())
					loss_reason = connection_close_reason::transport_error;
				else
				{
					const auto control_age = std::chrono::nanoseconds(steady_now_ns() - last_control_heartbeat_ns.load());
					const auto stream_age = std::chrono::nanoseconds(steady_now_ns() - last_stream_heartbeat_ns.load());
					if (control_age <= heartbeat_timeout && (!network_session->has_stream() || stream_age <= heartbeat_timeout))
						continue;
					loss_reason = connection_close_reason::network_timeout;
				}
			}

			if (state_ == state::disconnecting)
			{
				exit();
				break;
			}

			state previous_state = state_.load();
			bool entered_reconnecting = false;
			while (previous_state != state::shutdown && previous_state != state::disconnecting &&
			       previous_state != state::reconnecting)
			{
				if (state_.compare_exchange_weak(previous_state, state::reconnecting))
				{
					entered_reconnecting = true;
					break;
				}
			}
			if (!entered_reconnecting)
			{
				if (previous_state == state::disconnecting)
					exit();
				break;
			}

			reconnect_reason = loss_reason;
			transition_connection(client_connection_state::reconnecting, loss_reason);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			reconnect_deadline_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
			network_session->shutdown();
			reset_stream_resources();
			spdlog::warn("Connection lost ({}); attempting to resume for 30 seconds", to_string(loss_reason));

			static constexpr std::array delays{
			        std::chrono::milliseconds(0),
			        std::chrono::milliseconds(250),
			        std::chrono::milliseconds(500),
			        std::chrono::milliseconds(1000),
			};
			size_t attempt = 0;
			bool resumed = false;
			bool fatal_resume_error = false;
			while (state_ == state::reconnecting && std::chrono::steady_clock::now() < deadline)
			{
				const auto delay = attempt < delays.size() ? delays[attempt] : std::chrono::milliseconds(2000);
				if (delay.count())
				{
					const auto wake = std::min(deadline, std::chrono::steady_clock::now() + delay);
					while (state_ == state::reconnecting && std::chrono::steady_clock::now() < wake)
						std::this_thread::sleep_for(std::chrono::milliseconds(50));
				}
				if (state_ != state::reconnecting || std::chrono::steady_clock::now() >= deadline)
					break;

				++attempt;
				resume_validated = false;
				resume_rejected = false;
				try
				{
					network_session->reconnect();
					network_session->send_control(from_headset::headset_info_packet{reconnect_info});
					if (!network_session->is_active())
						throw handshake_error{"Connection closed while sending resume information"};

					// Transport setup is not enough: the server validates headset
					// capabilities and resumes all resources before announcing streaming.
					while (state_ == state::reconnecting && network_session->is_active() &&
					       !resume_validated.load() && !resume_rejected.load() &&
					       std::chrono::steady_clock::now() < deadline)
					{
						if (network_session->poll(*this, std::chrono::milliseconds(100)) < 0)
							break;
					}
					if (resume_rejected.load())
					{
						fatal_resume_error = true;
						break;
					}
					if (!resume_validated.load())
						throw handshake_error{"Server did not validate the resumed session"};
					resumed = true;
					break;
				}
				catch (const server_busy_error & e)
				{
					spdlog::warn("Reconnect refused: {}", e.what());
					reconnect_reason = connection_close_reason::incompatible_resume;
					auto errors = stream_error_queue.lock();
					errors->emplace(e.what());
					fatal_resume_error = true;
					break;
				}
				catch (const handshake_error & e)
				{
					spdlog::info("Reconnect attempt {} failed: {}", attempt, e.what());
				}
			}

			if (!resumed)
			{
				if (state_ == state::reconnecting && !fatal_resume_error)
				{
					auto errors = stream_error_queue.lock();
					errors->emplace(_("The connection could not be restored within 30 seconds."));
				}
				exit();
				break;
			}

			reconnect_deadline_ns = 0;
			reconnect_reason = connection_close_reason::none;
			last_control_heartbeat_ns = steady_now_ns();
			last_stream_heartbeat_ns = last_control_heartbeat_ns.load();
			next_heartbeat = std::chrono::steady_clock::now();
			last_display_time = 0;
			display_time_phase = 0;
			display_time_period = 0;
			real_display_period = 0;
			set_state(state::initializing);
			spdlog::info("Connection resumed and validated at generation {}", network_session->generation());
		}
		catch (std::exception & e)
		{
			spdlog::error("Protocol error in network thread: {}", e.what());
			{
				auto errors = stream_error_queue.lock();
				errors->emplace(e.what());
			}
			exit();
		}
	}
}

void scenes::stream::operator()(to_headset::disconnect_request && request)
{
	const auto mode = request.mode;
	network_session->send_control(from_headset::disconnect_ack{.mode = mode});
	disconnect_ack_received = true;
	reconnect_reason = request.reason;
	set_state(state::disconnecting);
	transition_connection(client_connection_state::disconnecting, request.reason);
}

void scenes::stream::operator()(to_headset::disconnect_ack &&)
{
	disconnect_ack_received = true;
}

void scenes::stream::operator()(to_headset::connection_state && connection)
{
	if (connection.connection_generation != network_session->generation())
	{
		network_session->discard_packet();
		return;
	}
	if (connection.state == server_connection_state::streaming)
		resume_validated = true;
	if (connection.state == server_connection_state::stopping)
	{
		reconnect_reason = connection.reason;
		disconnect_ack_received = true;
		set_state(state::disconnecting);
		transition_connection(client_connection_state::disconnecting, connection.reason);
		return;
	}
	if (connection.state == server_connection_state::failed)
	{
		resume_rejected = true;
		reconnect_reason = connection.reason;
		auto errors = stream_error_queue.lock();
		errors->emplace(std::string(to_string(connection.reason)));
	}
}

void scenes::stream::operator()(to_headset::heartbeat && heartbeat)
{
	if (heartbeat.connection_generation != network_session->generation())
	{
		network_session->discard_packet();
		return;
	}
	if (heartbeat.stream_channel)
		last_stream_heartbeat_ns = steady_now_ns();
	else
		last_control_heartbeat_ns = steady_now_ns();
}

void scenes::stream::operator()(to_headset::server_message && message)
{
	switch (message.kind)
	{
		case to_headset::server_message::kind::toast:
		case to_headset::server_message::kind::toast_urgent: {
			auto toast = gui_toast.lock();
			toast->emplace(message.msg, message.kind == to_headset::server_message::kind::toast_urgent);

			gui_status_last_change = instance.now();
			break;
		}

		case to_headset::server_message::kind::error: {
			if (state_ == state::reconnecting)
			{
				resume_rejected = true;
				reconnect_reason = connection_close_reason::incompatible_resume;
			}
			auto queue = stream_error_queue.lock();
			queue->emplace(std::move(message.msg));

			break;
		}
	}
}

void scenes::stream::operator()(to_headset::video_stream_data_shard && shard)
{
	if (shard.connection_generation != network_session->generation())
	{
		network_session->discard_packet();
		return;
	}
	std::shared_lock lock(decoder_mutex);
	uint8_t idx = shard.stream_item_idx;
	if (idx >= decoders.size())
	{
		// We don't know (yet?) about this stream, ignore packet
		return;
	}
	decoders[idx].decoder->push_shard(std::move(shard));
}

void scenes::stream::operator()(to_headset::feature_control && control)
{
	switch (control.f)
	{
		case wivrn::to_headset::feature_control::hid_input:
			hid_forwarding = control.state;
			if (not control.state and (application::get_config().forward_keyboard or application::get_config().forward_mouse or application::get_config().forward_gamepad))
			{
				auto toast = gui_toast.lock();
				toast->emplace(_("The server does not allow forwarded input devices"), true);
				gui_status_last_change = instance.now();
			}
			return;
		case wivrn::to_headset::feature_control::microphone:
			if (audio_handle)
				audio_handle->set_mic_state(control.state);
			return;
	}
}

void scenes::stream::operator()(to_headset::audio_stream_description && desc)
{
	audio_handle.emplace(desc, *network_session, instance);
}

void scenes::stream::operator()(to_headset::video_stream_description && desc)
{
	setup(desc);

	if (not tracking_thread)
	{
		tracking_thread = utils::named_thread("tracking_thread", &stream::tracking, this);
	}
}

void scenes::stream::operator()(to_headset::refresh_rate_change && rate)
{
	spdlog::info("refresh rate change request: {}", rate.hz);
	session.set_refresh_rate(rate.hz);
	std::shared_lock lock(decoder_mutex);
	if (video_stream_description)
		video_stream_description->refresh_rate = rate.hz;
}

void scenes::stream::operator()(to_headset::stream_tab_change && tab)
{
	next_gui_status = tab.tab;
}

void scenes::stream::operator()(to_headset::timesync_query && query)
{
	if (query.connection_generation != network_session->generation())
	{
		network_session->discard_packet();
		return;
	}
	network_session->send_stream(from_headset::timesync_response{
	        .query = query.query,
	        .response = instance.now(),
	});
}

void scenes::stream::operator()(audio_data && data)
{
	if (data.connection_generation != network_session->generation())
	{
		network_session->discard_packet();
		return;
	}
	if (audio_handle)
		(*audio_handle)(std::move(data));
}

void scenes::stream::send_feedback(const wivrn::from_headset::feedback & feedback)
{
	try
	{
		network_session->send_control(wivrn::from_headset::feedback{feedback});
	}
	catch (std::exception & e)
	{
		spdlog::warn("Exception while sending feedback packet: {}", e.what());
	}
}

void scenes::stream::operator()(to_headset::application_list && l)
{
	apps(std::move(l));
}

void scenes::stream::operator()(to_headset::application_icon && icon)
{
	apps(std::move(icon));
}

void scenes::stream::operator()(to_headset::running_applications && apps)
{
	*running_applications.lock() = std::move(apps);
}

void scenes::stream::start_application(std::string appid)
{
	network_session->send_control(wivrn::from_headset::start_app{
	        .app_id = std::move(appid),
	});
}
