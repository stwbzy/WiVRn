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

#pragma once

#include "crypto.h"
#include "wivrn_packets.h"
#include "wivrn_sockets.h"
#include <chrono>
#include <poll.h>
#include <shared_mutex>

using namespace wivrn;

class handshake_error : public std::exception
{
	std::string message;

public:
	const char * what() const noexcept override;
	handshake_error(std::string_view message);
};

class server_busy_error : public handshake_error
{
public:
	using handshake_error::handshake_error;
};

class wivrn_session
{
public:
	using control_socket_t = typed_socket<TCP, to_headset::packets, from_headset::packets>;
	using stream_socket_t = typed_socket<UDP, to_headset::packets, from_headset::packets>;

private:
	control_socket_t control;
	stream_socket_t stream;

	std::atomic<uint64_t> bytes_sent_ = 0;
	std::atomic<uint64_t> bytes_received_ = 0;
	std::atomic<bool> active = true;
	std::atomic<bool> handshaking = true;
	std::atomic<uint64_t> generation_ = 0;
	std::atomic<uint64_t> discarded_packets_ = 0;
	mutable std::shared_mutex socket_mutex;
	int port;
	bool tcp_only;
	crypto::key * headset_keypair;
	std::function<std::string(int fd)> pin_enter;
	std::string resume_token;

	template <typename T>
	void handshake(T address, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter);

public:
	std::variant<in_addr, in6_addr> address;

	wivrn_session(in6_addr address, int port, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter);
	wivrn_session(in_addr address, int port, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter);
	wivrn_session(const wivrn_session &) = delete;
	wivrn_session & operator=(const wivrn_session &) = delete;

	template <typename T>
	void send_control(T && packet)
	{
		if (!active.load() || handshaking.load())
			return;
		std::shared_lock lock(socket_mutex);
		if (!active.load() || handshaking.load())
			return;
		try
		{
			if constexpr (requires { packet.connection_generation = uint64_t{}; })
				packet.connection_generation = generation_.load();
			bytes_sent_ += control.send(std::forward<T>(packet));
		}
		catch (const std::system_error &)
		{
			active = false;
		}
		catch (const socket_shutdown &)
		{
			active = false;
		}
	}

	template <typename T>
	void send_stream(T && packet)
	{
		if (!active.load() || handshaking.load())
			return;
		std::shared_lock lock(socket_mutex);
		if (!active.load() || handshaking.load())
			return;
		try
		{
			if constexpr (requires { packet.connection_generation = uint64_t{}; })
				packet.connection_generation = generation_.load();
			if (stream)
				bytes_sent_ += stream.send(std::forward<T>(packet));
			else
				bytes_sent_ += control.send(std::forward<T>(packet));
		}
		catch (const std::system_error &)
		{
			active = false;
		}
		catch (const socket_shutdown &)
		{
			active = false;
		}
	}

	template <typename T>
	int poll(T && visitor, std::chrono::milliseconds timeout)
	{
		if (!active.load())
			return -1;
		pollfd fds[2] = {};
		fds[0].events = POLLIN;
		fds[0].fd = stream.get_fd();
		fds[1].events = POLLIN;
		fds[1].fd = control.get_fd();

		try
		{
			while (auto packet = stream.receive_pending(&bytes_received_))
				std::visit(std::forward<T>(visitor), std::move(*packet));
		}
		catch (const deserialization_error &)
		{
			++discarded_packets_;
		}
		catch (const socket_shutdown &)
		{
			active = false;
			return -1;
		}
		catch (const std::system_error &)
		{
			active = false;
			return -1;
		}
		try
		{
			while (auto packet = control.receive_pending(&bytes_received_))
				std::visit(std::forward<T>(visitor), std::move(*packet));
		}
		catch (const socket_shutdown &)
		{
			active = false;
			return -1;
		}
		catch (const std::system_error &)
		{
			active = false;
			return -1;
		}

		int r = ::poll(fds, std::size(fds), timeout.count());
		if (r < 0)
		{
			if (errno == EINTR)
				return 0;
			active = false;
			return -1;
		}

		if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL))
		{
			active = false;
			return -1;
		}

		if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL))
		{
			active = false;
			return -1;
		}

		if (fds[0].revents & POLLIN)
		{
			try
			{
				auto packet = stream.receive(&bytes_received_);
				if (packet)
					std::visit(std::forward<T>(visitor), std::move(*packet));
			}
			catch (const deserialization_error &)
			{
				++discarded_packets_;
			}
		}

		if (fds[1].revents & POLLIN)
		{
			try
			{
				auto packet = control.receive(&bytes_received_);
				if (packet)
					std::visit(std::forward<T>(visitor), std::move(*packet));
			}
			catch (const socket_shutdown &)
			{
				active = false;
				return -1;
			}
			catch (const std::system_error &)
			{
				active = false;
				return -1;
			}
		}

		return r;
	}

	void reconnect();
	void shutdown();
	bool is_active() const { return active.load(); }
	bool has_stream() const { return bool(stream); }
	uint64_t generation() const { return generation_.load(); }
	uint64_t discarded_packets() const { return discarded_packets_.load(); }
	void discard_packet() { ++discarded_packets_; }

	uint64_t bytes_received() const
	{
		return bytes_received_;
	}

	uint64_t bytes_sent() const
	{
		return bytes_sent_;
	}
};
