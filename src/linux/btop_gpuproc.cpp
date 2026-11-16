/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

//* Per-process GPU usage from DRM fdinfo.
//*
//* GPU drivers that follow the kernel's drm-usage-stats format (amdgpu, i915, msm, panfrost, v3d, ...) list
//* "drm-engine-<engine>: <busy ns> ns" for each DRM client in /proc/<pid>/fdinfo/<fd>.
//* A process's GPU% is its busiest engine over the last update, like the GPU column in Windows Task Manager.
//*
//* Cost: finding a process's DRM clients takes a readlink() on every fd it has open, so that scan only runs
//* when a pid first appears, when a known client's fd stops reading back, and otherwise only for processes that
//* used CPU time since the last update (a process cannot submit GPU work without running) and were last scanned
//* rescan_cycles updates ago (6x that if they already have a client). Known clients cost one small read() each.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include "../btop_shared.hpp"

namespace Proc::GpuUse {
	using std::string_view, std::vector;

	namespace {
		constexpr uint64_t rescan_cycles = 5;
		constexpr uint64_t unset = UINT64_MAX;

		struct Client {
			int fd{};
			uint64_t id{};
			vector<uint64_t> last_ns{};	// busy ns per engine index at the last read, unset if not listed yet
		};

		struct State {
			vector<Client> clients{};
			uint64_t scanned_cycle{};
			uint64_t seen_cycle{};
			int64_t last_sample{};		// steady clock ns of the last sample, 0 if none
			bool scanned{};
		};

		struct EngineReading {
			size_t index;
			uint64_t ns;
			uint64_t capacity;
		};

		std::unordered_map<size_t, State> states;
		vector<std::string> engine_names;
		vector<EngineReading> engines;		// engines of the fdinfo file read last
		vector<std::pair<size_t, uint64_t>> capacities;
		vector<double> busy;				// busy ns per engine for the pid being sampled
		vector<Client> scanned_clients;
		uint64_t cycle{};
		int64_t now{};
		char path[96];

		size_t engine_index(string_view name) {
			for (size_t i = 0; i < engine_names.size(); ++i)
				if (engine_names[i] == name) return i;
			engine_names.emplace_back(name);
			return engine_names.size() - 1;
		}

		uint64_t to_u64(string_view s) {
			uint64_t v = 0;
			for (const char c : s) {
				if (c < '0' or c > '9') break;
				v = v * 10 + (c - '0');
			}
			return v;
		}

		//* Read /proc/<pid>/fdinfo/<fd> into <engines>, returns false if it is not a DRM client
		bool read_fdinfo(size_t pid, int fd, uint64_t& id) {
			snprintf(path, sizeof(path), "/proc/%zu/fdinfo/%d", pid, fd);
			const int f = open(path, O_RDONLY | O_CLOEXEC);
			if (f < 0) return false;
			char buf[4096];
			ssize_t len = 0;
			for (ssize_t n; len < (ssize_t)sizeof(buf) and (n = read(f, buf + len, sizeof(buf) - len)) > 0; len += n);
			close(f);
			if (len <= 0) return false;

			id = unset;
			engines.clear();
			capacities.clear();
			string_view text(buf, len);
			while (not text.empty()) {
				const auto eol = text.find('\n');
				const string_view line = text.substr(0, eol);
				text.remove_prefix(eol == string_view::npos ? text.size() : eol + 1);
				if (not line.starts_with("drm-")) continue;
				const auto colon = line.find(':');
				if (colon == string_view::npos) continue;
				const string_view key = line.substr(4, colon - 4);
				string_view value = line.substr(colon + 1);
				while (not value.empty() and (value.front() == ' ' or value.front() == '\t')) value.remove_prefix(1);

				if (key == "client-id")
					id = to_u64(value);
				else if (key.starts_with("engine-capacity-"))
					capacities.emplace_back(engine_index(key.substr(16)), std::max<uint64_t>(1, to_u64(value)));
				else if (key.starts_with("engine-") and value.ends_with(" ns"))
					engines.push_back({engine_index(key.substr(7)), to_u64(value), 1});
			}
			for (const auto& [index, capacity] : capacities)
				for (auto& e : engines)
					if (e.index == index) e.capacity = capacity;
			if (not engines.empty() and not active) active = true;
			return id != unset;
		}

		//* Add the busy time since <c> was last read to <busy>; a new client only records its baseline
		void account(Client& c, bool known) {
			if (busy.size() < engine_names.size()) busy.resize(engine_names.size(), 0.0);
			for (const auto& e : engines) {
				if (c.last_ns.size() <= e.index) c.last_ns.resize(e.index + 1, unset);
				const uint64_t prev = c.last_ns[e.index];
				//? Drivers can leave out engines a client has not used yet, so a known client's first listing counts from 0
				const uint64_t delta = (prev == unset) ? (known ? e.ns : 0) : (e.ns >= prev ? e.ns - prev : 0);
				c.last_ns[e.index] = e.ns;
				busy[e.index] += (double)delta / e.capacity;
			}
		}

		//* Find the DRM clients among <pid>'s open fds, keeping the counters of clients seen before
		void scan(size_t pid, State& st) {
			st.scanned = true;
			st.scanned_cycle = cycle;
			scanned_clients.clear();
			snprintf(path, sizeof(path), "/proc/%zu/fd", pid);
			DIR* dir = opendir(path);
			if (dir == nullptr) {
				st.clients.clear();
				return;
			}
			const int dfd = dirfd(dir);
			char link[32];
			while (const dirent* entry = readdir(dir)) {
				if (entry->d_name[0] < '0' or entry->d_name[0] > '9') continue;
				const ssize_t n = readlinkat(dfd, entry->d_name, link, sizeof(link));
				if (n < 9 or memcmp(link, "/dev/dri/", 9) != 0) continue;
				const int fd = atoi(entry->d_name);
				uint64_t id;
				if (not read_fdinfo(pid, fd, id)) continue;
				//? Several fds can share one client (dup, or one fd per device node of the same client)
				if (std::ranges::any_of(scanned_clients, [id](const Client& c) { return c.id == id; })) continue;
				auto old = std::ranges::find(st.clients, id, &Client::id);
				const bool known = old != st.clients.end();
				Client c = known ? std::move(*old) : Client{};
				c.fd = fd;
				c.id = id;
				account(c, known);
				scanned_clients.push_back(std::move(c));
			}
			closedir(dir);
			st.clients.swap(scanned_clients);
		}

		//* Read the known clients, returns false if one of them is gone or its fd now belongs to something else
		bool read_known(size_t pid, State& st) {
			for (auto& c : st.clients) {
				uint64_t id;
				if (not read_fdinfo(pid, c.fd, id) or id != c.id) return false;
				account(c, true);
			}
			return true;
		}
	}

	void begin() {
		++cycle;
		now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	double sample(size_t pid, bool is_new, bool cpu_active) {
		auto [it, inserted] = states.try_emplace(pid);
		State& st = it->second;
		if (is_new and not inserted) st = State{};
		st.seen_cycle = cycle;

		std::ranges::fill(busy, 0.0);
		//? Processes that already have a client are re-scanned less often, to catch clients opened later
		const bool rescan_due = cpu_active and cycle - st.scanned_cycle >= (st.clients.empty() ? rescan_cycles : rescan_cycles * 6);
		if (not st.scanned or rescan_due or not read_known(pid, st))
			scan(pid, st);

		const int64_t elapsed = (st.last_sample > 0) ? now - st.last_sample : 0;
		st.last_sample = now;
		if (st.clients.empty() or elapsed <= 0) return 0.0;
		const double top = busy.empty() ? 0.0 : *std::ranges::max_element(busy);
		return std::clamp(top * 100.0 / elapsed, 0.0, 100.0);
	}

	void end() {
		std::erase_if(states, [](const auto& entry) { return entry.second.seen_cycle != cycle; });
	}
}
