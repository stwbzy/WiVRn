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

#include "wivrn_client.h"
#include "application.h"
#include "protocol_version.h"
#include "secrets.h"
#include "smp.h"
#include "spdlog/common.h"
#include "utils/i18n.h"
#include "wivrn_packets.h"
#include <algorithm>
#include <arpa/inet.h>
#include <filesystem>
#include <fstream>
#include <ifaddrs.h>
#include <linux/ipv6.h>
#include <mutex>
#include <net/if.h>
#include <netinet/ip.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>

#ifndef IPTOS_DSCP_EF
// constant is not defined in Android ip.h
#define IPTOS_DSCP_EF 0xb8
#endif

using namespace std::chrono_literals;

const char * handshake_error::what() const noexcept
{
	return message.c_str();
}

handshake_error::handshake_error(std::string_view message) :
        message(message) {}

namespace
{
std::mutex resume_tokens_mutex;
std::unordered_map<std::string, std::string> resume_tokens;
std::optional<std::string> last_resume_token;

bool valid_resume_token(std::string_view token)
{
	return token.size() == 64 && std::ranges::all_of(token, [](char c) {
		       return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	       });
}

std::filesystem::path resume_token_path()
{
	return application::get_config_path() / "resume_token";
}

std::string load_resume_token()
{
	std::ifstream file(resume_token_path(), std::ios::binary);
	if (!file)
		return {};

	std::string token{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
	if (!valid_resume_token(token))
	{
		spdlog::warn("Ignoring an invalid persisted resume token");
		return {};
	}
	return token;
}

void persist_resume_token(std::string_view token)
{
	if (!valid_resume_token(token))
		return;

	const auto path = resume_token_path();
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(token.data(), token.size());
	file.close();
	if (!file)
	{
		spdlog::warn("Unable to persist the resume token");
		return;
	}

	std::error_code error;
	std::filesystem::permissions(
	        path,
	        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
	        std::filesystem::perm_options::replace,
	        error);
	if (error)
		spdlog::warn("Unable to restrict resume token permissions: {}", error.message());
}

template <typename T>
std::string endpoint_key(T address, int port)
{
	char buffer[INET6_ADDRSTRLEN]{};
	constexpr int family = std::is_same_v<T, in_addr> ? AF_INET : AF_INET6;
	if (!inet_ntop(family, &address, buffer, sizeof(buffer)))
		throw std::system_error(errno, std::system_category(), "Cannot format server address");
	return std::string(buffer) + ":" + std::to_string(port);
}

template <typename T>
std::string cached_resume_token(T address, int port)
{
	std::lock_guard lock(resume_tokens_mutex);
	const auto item = resume_tokens.find(endpoint_key(address, port));
	if (item != resume_tokens.end())
		return item->second;

	// The headset may receive a new address, and Android may recreate the
	// process while the PC session is paused. The server binds this opaque
	// token to the authenticated headset key, so the most recently completed
	// session is the safe fallback when the endpoint itself has changed.
	if (!last_resume_token)
		last_resume_token = load_resume_token();
	return *last_resume_token;
}

template <typename T>
void remember_resume_token(T address, int port, const std::string & token)
{
	std::lock_guard lock(resume_tokens_mutex);
	resume_tokens[endpoint_key(address, port)] = token;
	last_resume_token = token;
	persist_resume_token(token);
}

template <typename T>
void init_stream(T & stream)
{
	stream.set_receive_buffer_size(1024 * 1024 * 5);
}
} // namespace

template <typename T>
void wivrn_session::handshake(T address, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter)
{
	// FIXME this comment
	// Wait for handshake on control socket,
	// then send ours on stream or control socket,
	// finally wait for second server handshake

	pollfd fds{};
	fds.events = POLLIN;
	fds.fd = control.get_fd();

	auto receive = [&](std::optional<std::chrono::seconds> timeout = std::nullopt) {
		std::chrono::steady_clock::time_point timeout_abs{};
		if (timeout)
			timeout_abs = std::chrono::steady_clock::now() + *timeout;

		while (true)
		{
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout_abs - std::chrono::steady_clock::now());
			int r = ::poll(&fds, 1, std::max<int>(ms.count(), 100));
			if (r < 0)
				throw std::system_error(errno, std::system_category());

			if (r > 0 && (fds.revents & POLLIN))
			{
				auto packet = control.receive();
				if (not packet)
					continue;

				return std::move(*packet);
			}

			if (std::chrono::steady_clock::now() >= timeout_abs)
				throw std::runtime_error(_("Timeout"));
		}
	};

	bytes_sent_ += control.send(from_headset::crypto_handshake{
	        .protocol_version = wivrn::protocol_version,
	        .public_key = headset_keypair.public_key(),
	        .name = application::get_hmd_traits().model_name(),
	        .resume_token = resume_token,
	});

	to_headset::crypto_handshake crypto_handshake = std::get<to_headset::crypto_handshake>(receive(10s));

	std::string pin = "000000";
	switch (crypto_handshake.state)
	{
		case to_headset::crypto_handshake::crypto_state::encryption_disabled: {
			spdlog::info("Encryption is disabled on server");

			bytes_sent_ += control.send(from_headset::crypto_handshake{});

			to_headset::handshake h{std::get<to_headset::handshake>(receive(10s))};
			generation_ = h.connection_generation;
			resume_token = h.resume_token;
			if (h.stream_port > 0 && !tcp_only)
			{
				stream = decltype(stream)();

				stream.connect(address, h.stream_port);
				init_stream(stream);
			}
			break;
		}

		case to_headset::crypto_handshake::crypto_state::pin_needed:
			pin = pin_enter(control.get_fd());

			// Check the PIN
			try
			{
				crypto::smp pin_check;

				auto msg1 = pin_check.step1(pin);
				bytes_sent_ += control.send(from_headset::pin_check_1{msg1});

				auto msg2 = std::get<to_headset::pin_check_2>(receive(10s)).message;

				auto msg3 = pin_check.step3(msg2);
				bytes_sent_ += control.send(from_headset::pin_check_3{msg3});

				auto msg4 = std::get<to_headset::pin_check_4>(receive(10s)).message;
				bool pin_match = pin_check.step5(msg4);

				if (not pin_match)
					throw std::runtime_error(_("Incorrect PIN"));
			}
			catch (crypto::smp_cheated &)
			{
				throw std::runtime_error(_("Unable to check PIN"));
			}

			[[fallthrough]];

		case to_headset::crypto_handshake::crypto_state::client_already_paired: {
			spdlog::info("Using pin \"{}\"", pin);

			crypto::key server_key = crypto::key::from_public_key(crypto_handshake.public_key);
			secrets s{headset_keypair, server_key, pin};
			control.set_aes_key_and_ivs(s.control_key, s.control_iv_to_headset, s.control_iv_from_headset);

			// Confirm that encryption is set up
			bytes_sent_ += control.send(from_headset::crypto_handshake{});

			to_headset::handshake h{std::get<to_headset::handshake>(receive(10s))};
			generation_ = h.connection_generation;
			resume_token = h.resume_token;
			if (h.stream_port > 0 && !tcp_only)
			{
				stream = decltype(stream)();

				stream.set_aes_key_and_ivs(s.stream_key, s.stream_iv_header_to_headset, s.stream_iv_header_from_headset);
				stream.connect(address, h.stream_port);
				init_stream(stream);
			}
			break;
		}

		case to_headset::crypto_handshake::crypto_state::pairing_disabled:
			spdlog::info("Pairing is disabled on server");
			throw std::runtime_error(_("Pairing is disabled on server"));

		case to_headset::crypto_handshake::crypto_state::incompatible_version:
			spdlog::error("Incompatible protocol versions");
			throw std::runtime_error(_("Incompatible server version"));

		case to_headset::crypto_handshake::crypto_state::server_busy:
			spdlog::info("Server is occupied by a paused headset session");
			throw server_busy_error(_("Server is busy with another headset session"));
	}

	// may be on control socket if forced TCP
	if (stream)
		bytes_sent_ += stream.send(from_headset::handshake{});
	else
		bytes_sent_ += control.send(from_headset::handshake{});

	// Wait for second handshake
	auto timeout = std::chrono::steady_clock::now() + 10s;
	while (true)
	{
		bool completed = false;
		const int result = poll([&](const auto && packet) {
			if constexpr (std::is_same_v<std::remove_cvref_t<decltype(packet)>, to_headset::handshake>)
			{
				generation_ = packet.connection_generation;
				resume_token = packet.resume_token;
				completed = true;
			}
		},
		                        100ms);
		if (completed)
		{
			remember_resume_token(address, port, resume_token);
			return;
		}
		if (result < 0)
			throw socket_shutdown{};

		if (std::chrono::steady_clock::now() >= timeout)
			throw std::runtime_error(_("Timeout"));

		// If using stream socket, the handshake might be lost
		if (stream)
		{
			stream.send(from_headset::handshake{});
		}
	}
}

wivrn_session::wivrn_session(in6_addr address, int port, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter) :
        control(address, port), stream(-1), port(port), tcp_only(tcp_only), headset_keypair(&headset_keypair), pin_enter(std::move(pin_enter)), resume_token(cached_resume_token(address, port)), address(address)
{
	try
	{
		handshake(address, tcp_only, headset_keypair, this->pin_enter);
		handshaking = false;
	}
	catch (std::exception & e)
	{
		throw handshake_error{e.what()};
	}
}

wivrn_session::wivrn_session(in_addr address, int port, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter) :
        control(address, port), stream(-1), port(port), tcp_only(tcp_only), headset_keypair(&headset_keypair), pin_enter(std::move(pin_enter)), resume_token(cached_resume_token(address, port)), address(address)
{
	try
	{
		handshake(address, tcp_only, headset_keypair, this->pin_enter);
		handshaking = false;
	}
	catch (std::exception & e)
	{
		throw handshake_error{e.what()};
	}
}

void wivrn_session::reconnect()
{
	handshaking = true;
	active = false;
	try
	{
		{
			std::unique_lock lock(socket_mutex);
			stream = stream_socket_t(-1);
			std::visit([this](const auto & value) { control = control_socket_t(value, port); }, address);
		}
		active = true;
		std::visit([this](const auto & value) { handshake(value, tcp_only, *headset_keypair, pin_enter); }, address);
		handshaking = false;
	}
	catch (const server_busy_error &)
	{
		active = false;
		handshaking = false;
		throw;
	}
	catch (const std::exception & e)
	{
		active = false;
		handshaking = false;
		throw handshake_error{e.what()};
	}
}

void wivrn_session::shutdown()
{
	active = false;
	std::unique_lock lock(socket_mutex);
	if (stream)
		::shutdown(stream.get_fd(), SHUT_RDWR);
	if (control)
		::shutdown(control.get_fd(), SHUT_RDWR);
}
