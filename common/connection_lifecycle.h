/*
 * WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace wivrn
{

enum class server_connection_state : uint8_t
{
	listening,
	handshaking,
	streaming,
	reconnecting,
	paused,
	stopping,
	failed,
};

enum class client_connection_state : uint8_t
{
	lobby,
	connecting,
	streaming,
	reconnecting,
	disconnecting,
};

enum class connection_close_reason : uint8_t
{
	none,
	user_requested,
	peer_closed,
	network_timeout,
	transport_error,
	protocol_error,
	incompatible_resume,
	server_shutdown,
};

enum class disconnect_mode : uint8_t
{
	default_mode,
	pause,
	terminate,
};

enum class disconnect_policy : uint8_t
{
	pause,
	terminate,
};

constexpr std::string_view to_string(server_connection_state state)
{
	switch (state)
	{
		case server_connection_state::listening:
			return "listening";
		case server_connection_state::handshaking:
			return "handshaking";
		case server_connection_state::streaming:
			return "streaming";
		case server_connection_state::reconnecting:
			return "reconnecting";
		case server_connection_state::paused:
			return "paused";
		case server_connection_state::stopping:
			return "stopping";
		case server_connection_state::failed:
			return "failed";
	}
	return "failed";
}

constexpr std::string_view to_string(client_connection_state state)
{
	switch (state)
	{
		case client_connection_state::lobby:
			return "lobby";
		case client_connection_state::connecting:
			return "connecting";
		case client_connection_state::streaming:
			return "streaming";
		case client_connection_state::reconnecting:
			return "reconnecting";
		case client_connection_state::disconnecting:
			return "disconnecting";
	}
	return "lobby";
}

constexpr std::string_view to_string(connection_close_reason reason)
{
	switch (reason)
	{
		case connection_close_reason::none:
			return "none";
		case connection_close_reason::user_requested:
			return "user_requested";
		case connection_close_reason::peer_closed:
			return "peer_closed";
		case connection_close_reason::network_timeout:
			return "network_timeout";
		case connection_close_reason::transport_error:
			return "transport_error";
		case connection_close_reason::protocol_error:
			return "protocol_error";
		case connection_close_reason::incompatible_resume:
			return "incompatible_resume";
		case connection_close_reason::server_shutdown:
			return "server_shutdown";
	}
	return "protocol_error";
}

constexpr std::string_view to_string(disconnect_mode mode)
{
	switch (mode)
	{
		case disconnect_mode::default_mode:
			return "default";
		case disconnect_mode::pause:
			return "pause";
		case disconnect_mode::terminate:
			return "terminate";
	}
	return "default";
}

constexpr disconnect_mode resolve(disconnect_mode mode, disconnect_policy policy)
{
	if (mode != disconnect_mode::default_mode)
		return mode;
	return policy == disconnect_policy::pause ? disconnect_mode::pause : disconnect_mode::terminate;
}

constexpr bool valid_transition(server_connection_state from, server_connection_state to)
{
	if (from == to)
		return true;
	if (to == server_connection_state::stopping)
		return true;
	switch (from)
	{
		case server_connection_state::listening:
			return to == server_connection_state::handshaking || to == server_connection_state::failed;
		case server_connection_state::handshaking:
			return to == server_connection_state::listening || to == server_connection_state::streaming ||
			       to == server_connection_state::reconnecting || to == server_connection_state::paused ||
			       to == server_connection_state::failed;
		case server_connection_state::streaming:
			return to == server_connection_state::reconnecting || to == server_connection_state::paused ||
			       to == server_connection_state::failed;
		case server_connection_state::reconnecting:
			return to == server_connection_state::handshaking || to == server_connection_state::streaming ||
			       to == server_connection_state::paused || to == server_connection_state::failed;
		case server_connection_state::paused:
			return to == server_connection_state::handshaking || to == server_connection_state::reconnecting ||
			       to == server_connection_state::streaming || to == server_connection_state::failed;
		case server_connection_state::failed:
			return to == server_connection_state::listening;
		case server_connection_state::stopping:
			return false;
	}
	return false;
}

constexpr bool valid_transition(client_connection_state from, client_connection_state to)
{
	if (from == to)
		return true;
	if (to == client_connection_state::disconnecting)
		return true;
	switch (from)
	{
		case client_connection_state::lobby:
			return to == client_connection_state::connecting;
		case client_connection_state::connecting:
			return to == client_connection_state::streaming || to == client_connection_state::reconnecting ||
			       to == client_connection_state::lobby;
		case client_connection_state::streaming:
			return to == client_connection_state::reconnecting || to == client_connection_state::lobby;
		case client_connection_state::reconnecting:
			return to == client_connection_state::streaming || to == client_connection_state::lobby;
		case client_connection_state::disconnecting:
			return to == client_connection_state::lobby;
	}
	return false;
}

struct connection_counters
{
	uint64_t attempts = 0;
	uint64_t successful_reconnections = 0;
	uint64_t reconnection_expirations = 0;
	uint64_t discarded_packets = 0;
};

struct connection_transition
{
	server_connection_state state;
	connection_close_reason reason;
	uint64_t generation;
	uint64_t previous_state_duration_ms;
	connection_counters counters;
};

struct client_connection_transition
{
	client_connection_state state;
	connection_close_reason reason;
	uint64_t generation;
	uint64_t previous_state_duration_ms;
};

// The session network thread owns all mutations. The mutex makes snapshots safe for
// compositor/audio/IPC observers and prevents duplicate transition events.
class connection_lifecycle
{
	mutable std::mutex mutex;
	server_connection_state state_ = server_connection_state::listening;
	connection_close_reason reason_ = connection_close_reason::none;
	uint64_t generation_ = 0;
	connection_counters counters_{};
	std::chrono::steady_clock::time_point entered_ = std::chrono::steady_clock::now();

public:
	connection_transition snapshot() const
	{
		std::lock_guard lock(mutex);
		return {state_, reason_, generation_, 0, counters_};
	}

	// Returns false for a duplicate. Invalid transitions are programmer errors.
	std::optional<connection_transition> transition(
	        server_connection_state state,
	        connection_close_reason reason = connection_close_reason::none,
	        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
	{
		std::lock_guard lock(mutex);
		if (state_ == state)
			return std::nullopt;
		if (!valid_transition(state_, state))
			throw std::logic_error("invalid connection state transition");
		const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - entered_).count();
		if (state == server_connection_state::handshaking)
			++counters_.attempts;
		if (state == server_connection_state::streaming)
		{
			if (state_ == server_connection_state::reconnecting || state_ == server_connection_state::paused || generation_ > 0)
				++counters_.successful_reconnections;
			++generation_;
		}
		if (state == server_connection_state::failed && reason == connection_close_reason::network_timeout)
			++counters_.reconnection_expirations;
		state_ = state;
		reason_ = reason;
		entered_ = now;
		return connection_transition{
		        .state = state_,
		        .reason = reason_,
		        .generation = generation_,
		        .previous_state_duration_ms = static_cast<uint64_t>(duration < 0 ? 0 : duration),
		        .counters = counters_,
		};
	}

	void discard_packet()
	{
		std::lock_guard lock(mutex);
		++counters_.discarded_packets;
	}

	void set_discarded_packets(uint64_t value)
	{
		std::lock_guard lock(mutex);
		counters_.discarded_packets = value;
	}
};

// The stream scene owns mutations, while the UI and decoder threads may take
// snapshots. Keeping this separate from the media state (initializing/stalled)
// makes the externally visible connection lifecycle deterministic.
class client_connection_lifecycle
{
	mutable std::mutex mutex;
	client_connection_state state_ = client_connection_state::lobby;
	connection_close_reason reason_ = connection_close_reason::none;
	uint64_t generation_ = 0;
	std::chrono::steady_clock::time_point entered_ = std::chrono::steady_clock::now();

public:
	client_connection_transition snapshot() const
	{
		std::lock_guard lock(mutex);
		return {state_, reason_, generation_, 0};
	}

	std::optional<client_connection_transition> transition(
	        client_connection_state state,
	        connection_close_reason reason = connection_close_reason::none,
	        uint64_t generation = 0,
	        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
	{
		std::lock_guard lock(mutex);
		if (state_ == state)
			return std::nullopt;
		if (!valid_transition(state_, state))
			throw std::logic_error("invalid client connection state transition");
		const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - entered_).count();
		state_ = state;
		reason_ = reason;
		generation_ = generation;
		entered_ = now;
		return client_connection_transition{
		        .state = state_,
		        .reason = reason_,
		        .generation = generation_,
		        .previous_state_duration_ms = static_cast<uint64_t>(duration < 0 ? 0 : duration),
		};
	}
};

} // namespace wivrn
