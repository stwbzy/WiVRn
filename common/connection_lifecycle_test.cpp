/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "connection_lifecycle.h"

#include <atomic>
#include <cassert>
#include <thread>
#include <vector>

using namespace wivrn;

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
		assert(rejected);
	}

	connection_lifecycle lifecycle;
	assert(!lifecycle.transition(server_connection_state::listening));
	assert(lifecycle.transition(server_connection_state::handshaking));
	assert(lifecycle.transition(server_connection_state::streaming));
	assert(lifecycle.snapshot().generation == 1);
	assert(lifecycle.snapshot().counters.attempts == 1);

	for (int cycle = 0; cycle < 100; ++cycle)
	{
		assert(lifecycle.transition(server_connection_state::paused, connection_close_reason::user_requested));
		assert(!lifecycle.transition(server_connection_state::paused, connection_close_reason::user_requested));
		assert(lifecycle.transition(server_connection_state::handshaking));
		assert(lifecycle.transition(server_connection_state::streaming));
	}

	auto snapshot = lifecycle.snapshot();
	assert(snapshot.generation == 101);
	assert(snapshot.counters.attempts == 101);
	assert(snapshot.counters.successful_reconnections == 100);

	std::vector<std::jthread> writers;
	std::atomic<int> close_events = 0;
	for (int thread = 0; thread < 8; ++thread)
		writers.emplace_back([&] {
			if (lifecycle.transition(server_connection_state::paused, connection_close_reason::peer_closed))
				++close_events;
		});
	writers.clear();
	assert(close_events == 1);
	assert(lifecycle.transition(server_connection_state::handshaking));
	assert(lifecycle.transition(server_connection_state::streaming));

	writers.clear();
	for (int thread = 0; thread < 4; ++thread)
		writers.emplace_back([&] {
			for (int packet = 0; packet < 250; ++packet)
				lifecycle.discard_packet();
		});
	writers.clear();
	assert(lifecycle.snapshot().counters.discarded_packets == 1000);

	assert(valid_transition(client_connection_state::lobby, client_connection_state::connecting));
	assert(valid_transition(client_connection_state::connecting, client_connection_state::streaming));
	assert(valid_transition(client_connection_state::streaming, client_connection_state::reconnecting));
	assert(valid_transition(client_connection_state::reconnecting, client_connection_state::streaming));
	assert(valid_transition(client_connection_state::streaming, client_connection_state::disconnecting));
	assert(valid_transition(client_connection_state::disconnecting, client_connection_state::lobby));
	assert(!valid_transition(client_connection_state::lobby, client_connection_state::streaming));

	assert(resolve(disconnect_mode::default_mode, disconnect_policy::pause) == disconnect_mode::pause);
	assert(resolve(disconnect_mode::default_mode, disconnect_policy::terminate) == disconnect_mode::terminate);
	assert(resolve(disconnect_mode::pause, disconnect_policy::terminate) == disconnect_mode::pause);

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
		assert(rejected);
	}
	assert(client.transition(client_connection_state::connecting));
	assert(client.transition(client_connection_state::streaming, connection_close_reason::none, 1));
	assert(client.transition(client_connection_state::reconnecting, connection_close_reason::network_timeout, 1));
	assert(!client.transition(client_connection_state::reconnecting, connection_close_reason::network_timeout, 1));
	assert(client.transition(client_connection_state::streaming, connection_close_reason::none, 2));
	assert(client.transition(client_connection_state::disconnecting, connection_close_reason::user_requested, 2));
	assert(client.transition(client_connection_state::lobby, connection_close_reason::user_requested, 2));
}
