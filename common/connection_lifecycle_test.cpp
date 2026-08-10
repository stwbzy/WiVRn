/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "connection_lifecycle.h"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace wivrn;

namespace
{
template <typename T>
void check(T && condition)
{
	if (!static_cast<bool>(condition))
		throw std::runtime_error("connection lifecycle test check failed");
}
} // namespace

int main()
{
	{
		connection_lifecycle invalid_server;
		bool rejected = false;
		try
		{
			invalid_server.transition(server_connection_state::streaming);
		}
		catch (const std::logic_error &)
		{
			rejected = true;
		}
		check(rejected);
	}

	connection_lifecycle lifecycle;
	check(!lifecycle.transition(server_connection_state::listening));
	check(lifecycle.transition(server_connection_state::handshaking));
	check(lifecycle.transition(server_connection_state::streaming));
	check(lifecycle.snapshot().generation == 1);
	check(lifecycle.snapshot().counters.attempts == 1);

	for (int cycle = 0; cycle < 100; ++cycle)
	{
		check(lifecycle.transition(server_connection_state::paused, connection_close_reason::user_requested));
		check(!lifecycle.transition(server_connection_state::paused, connection_close_reason::user_requested));
		check(lifecycle.transition(server_connection_state::handshaking));
		check(lifecycle.transition(server_connection_state::streaming));
	}

	auto snapshot = lifecycle.snapshot();
	check(snapshot.generation == 101);
	check(snapshot.counters.attempts == 101);
	check(snapshot.counters.successful_reconnections == 100);

	std::vector<std::jthread> writers;
	std::atomic<int> close_events = 0;
	for (int thread = 0; thread < 8; ++thread)
		writers.emplace_back([&] {
			if (lifecycle.transition(server_connection_state::paused, connection_close_reason::peer_closed))
				++close_events;
		});
	writers.clear();
	check(close_events == 1);
	check(lifecycle.transition(server_connection_state::handshaking));
	check(lifecycle.transition(server_connection_state::streaming));

	writers.clear();
	for (int thread = 0; thread < 4; ++thread)
		writers.emplace_back([&] {
			for (int packet = 0; packet < 250; ++packet)
				lifecycle.discard_packet();
		});
	writers.clear();
	check(lifecycle.snapshot().counters.discarded_packets == 1000);

	check(valid_transition(client_connection_state::lobby, client_connection_state::connecting));
	check(valid_transition(client_connection_state::connecting, client_connection_state::streaming));
	check(valid_transition(client_connection_state::streaming, client_connection_state::reconnecting));
	check(valid_transition(client_connection_state::reconnecting, client_connection_state::streaming));
	check(valid_transition(client_connection_state::streaming, client_connection_state::disconnecting));
	check(valid_transition(client_connection_state::disconnecting, client_connection_state::lobby));
	check(!valid_transition(client_connection_state::lobby, client_connection_state::streaming));

	check(resolve(disconnect_mode::default_mode, disconnect_policy::pause) == disconnect_mode::pause);
	check(resolve(disconnect_mode::default_mode, disconnect_policy::terminate) == disconnect_mode::terminate);
	check(resolve(disconnect_mode::pause, disconnect_policy::terminate) == disconnect_mode::pause);

	client_connection_lifecycle client;
	{
		client_connection_lifecycle invalid_client;
		bool rejected = false;
		try
		{
			invalid_client.transition(client_connection_state::streaming);
		}
		catch (const std::logic_error &)
		{
			rejected = true;
		}
		check(rejected);
	}
	check(client.transition(client_connection_state::connecting));
	check(client.transition(client_connection_state::streaming, connection_close_reason::none, 1));
	check(client.transition(client_connection_state::reconnecting, connection_close_reason::network_timeout, 1));
	check(!client.transition(client_connection_state::reconnecting, connection_close_reason::network_timeout, 1));
	check(client.transition(client_connection_state::streaming, connection_close_reason::none, 2));
	check(client.transition(client_connection_state::disconnecting, connection_close_reason::user_requested, 2));
	check(client.transition(client_connection_state::lobby, connection_close_reason::user_requested, 2));
}
