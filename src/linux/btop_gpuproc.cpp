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
//* GPU drivers that follow the kernel's drm-usage-stats format list the usage of each DRM client in /proc/<pid>/fdinfo/<fd>,
//* either as "drm-engine-<engine>: <busy ns> ns" (amdgpu, i915, msm, panfrost, panthor with profiling on, v3d, ...) or as
//* "drm-cycles-<engine>" / "drm-total-cycles-<engine>" GPU clock counters (xe). nouveau and NVIDIA's driver report neither.
//* A process's GPU% is its busiest engine over the last update, like the GPU column in Windows Task Manager.
//* Engines are kept apart per GPU (drm-pdev), so a process using two GPUs shows the busier one, not the sum.
//*
//* Cost: finding a process's DRM clients takes a readlink() on every fd it has open, so that scan only runs
//* when a pid first appears, when a known client's fd stops reading back, and otherwise only for processes that
//* used CPU time since the last update (a process cannot submit GPU work without running) and were last scanned
//* rescan_cycles updates ago (6x that if they already have a client). Known clients cost one small read() each.
//* Nothing is scanned while /dev/dri has no device nodes (headless machines, most VMs).

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
		constexpr uint64_t dri_check_cycles = 30;
		constexpr uint64_t unset = UINT64_MAX;

		struct Client {
			int fd{};
			uint64_t id{};
			vector<uint64_t> last_ns{};		// busy ns per engine index at the last read, unset if not listed yet
			vector<uint64_t> last_cycles{};	// busy GPU cycles per engine index (drm-cycles-*), unset if not listed yet
			vector<uint64_t> last_total{};	// total GPU cycles per engine index (drm-total-cycles-*)
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
			uint64_t ns;		// busy ns, or busy cycles when <cycles>
			uint64_t total;		// total cycles when <cycles>
			uint64_t capacity;
			bool cycles;
		};

		//? Raw "drm-..." values of one fdinfo file before the engine names are tied to their GPU
		struct RawValue {
			string_view name;
			uint64_t value;
		};

		std::unordered_map<size_t, State> states;
		vector<std::string> engine_names;	// "<drm-pdev>/<engine>"
		vector<EngineReading> engines;		// engines of the fdinfo file read last
		vector<RawValue> raw_ns, raw_cycles, raw_total, raw_capacity;
		vector<double> busy;				// busy ns per engine for the pid being sampled
		vector<double> busy_frac;			// busy fraction per engine (cycle counters) for the pid being sampled
		vector<Client> scanned_clients;
		uint64_t cycle{};
		uint64_t dri_checked_cycle{};
		bool has_dri{};
		int64_t now{};
		char path[96];
		std::string key_buf;

		size_t engine_index(string_view pdev, string_view name) {
			key_buf.assign(pdev).append(1, '/').append(name);
			for (size_t i = 0; i < engine_names.size(); ++i)
				if (engine_names[i] == key_buf) return i;
			engine_names.push_back(key_buf);
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

		uint64_t value_for(const vector<RawValue>& values, string_view name, uint64_t fallback) {
			for (const auto& v : values)
				if (v.name == name) return v.value;
			return fallback;
		}

		//* Parse the text of an fdinfo file into <engines>, returns false if it is not a DRM client
		bool parse_fdinfo(string_view text, uint64_t& id) {
			id = unset;
			engines.clear();
			raw_ns.clear();
			raw_cycles.clear();
			raw_total.clear();
			raw_capacity.clear();
			string_view pdev;
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
				else if (key == "pdev")
					pdev = value;
				else if (key.starts_with("engine-capacity-"))
					raw_capacity.push_back({key.substr(16), std::max<uint64_t>(1, to_u64(value))});
				else if (key.starts_with("engine-") and value.ends_with(" ns"))
					raw_ns.push_back({key.substr(7), to_u64(value)});
				else if (key.starts_with("total-cycles-"))
					raw_total.push_back({key.substr(13), to_u64(value)});
				else if (key.starts_with("cycles-"))
					raw_cycles.push_back({key.substr(7), to_u64(value)});
			}
			for (const auto& e : raw_ns)
				engines.push_back({engine_index(pdev, e.name), e.value, 0, value_for(raw_capacity, e.name, 1), false});
			//? Cycle counters only mean something next to the matching total-cycles counter
			for (const auto& e : raw_cycles) {
				const uint64_t total = value_for(raw_total, e.name, unset);
				if (total != unset)
					engines.push_back({engine_index(pdev, e.name), e.value, total, value_for(raw_capacity, e.name, 1), true});
			}
			if (not engines.empty() and not active) active = true;
			return id != unset;
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
			return parse_fdinfo(string_view(buf, len), id);
		}

		uint64_t& slot(vector<uint64_t>& v, size_t index) {
			if (v.size() <= index) v.resize(index + 1, unset);
			return v[index];
		}

		//* Add the usage since <c> was last read to <busy> / <busy_frac>; a new client only records its baseline
		void account(Client& c, bool known) {
			if (busy.size() < engine_names.size()) {
				busy.resize(engine_names.size(), 0.0);
				busy_frac.resize(engine_names.size(), 0.0);
			}
			for (const auto& e : engines) {
				if (e.cycles) {
					auto& prev_cycles = slot(c.last_cycles, e.index);
					auto& prev_total = slot(c.last_total, e.index);
					//? Both counters run on the GPU clock, so their ratio is the busy fraction without needing the CPU-side time
					if (prev_cycles != unset and prev_total != unset and e.total > prev_total and e.ns >= prev_cycles)
						busy_frac[e.index] += (double)(e.ns - prev_cycles) / ((double)(e.total - prev_total) * e.capacity);
					prev_cycles = e.ns;
					prev_total = e.total;
					continue;
				}
				auto& prev = slot(c.last_ns, e.index);
				//? Drivers can leave out engines a client has not used yet, so a known client's first listing counts from 0
				const uint64_t delta = (prev == unset) ? (known ? e.ns : 0) : (e.ns >= prev ? e.ns - prev : 0);
				prev = e.ns;
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

		//* True if /dev/dri has any device node (card*, renderD*)
		bool dri_present() {
			DIR* dir = opendir("/dev/dri");
			if (dir == nullptr) return false;
			bool found = false;
			while (const dirent* entry = readdir(dir)) {
				const string_view name = entry->d_name;
				if (name.starts_with("card") or name.starts_with("renderD")) {
					found = true;
					break;
				}
			}
			closedir(dir);
			return found;
		}
	}

	void begin() {
		++cycle;
		now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		//? Device nodes can appear later (driver loaded after boot), so look again now and then
		if (dri_checked_cycle == 0 or cycle - dri_checked_cycle >= dri_check_cycles) {
			dri_checked_cycle = cycle;
			has_dri = dri_present();
		}
	}

	double sample(size_t pid, bool is_new, bool cpu_active) {
		if (not has_dri) return 0.0;
		auto [it, inserted] = states.try_emplace(pid);
		State& st = it->second;
		if (is_new and not inserted) st = State{};
		st.seen_cycle = cycle;

		std::ranges::fill(busy, 0.0);
		std::ranges::fill(busy_frac, 0.0);
		//? Processes that already have a client are re-scanned less often, to catch clients opened later
		const bool rescan_due = cpu_active and cycle - st.scanned_cycle >= (st.clients.empty() ? rescan_cycles : rescan_cycles * 6);
		if (not st.scanned or rescan_due or not read_known(pid, st))
			scan(pid, st);

		const int64_t elapsed = (st.last_sample > 0) ? now - st.last_sample : 0;
		st.last_sample = now;
		if (st.clients.empty() or elapsed <= 0) return 0.0;
		const double top_ns = busy.empty() ? 0.0 : *std::ranges::max_element(busy) / elapsed;
		const double top_frac = busy_frac.empty() ? 0.0 : *std::ranges::max_element(busy_frac);
		return std::clamp(std::max(top_ns, top_frac) * 100.0, 0.0, 100.0);
	}

	void end() {
		std::erase_if(states, [](const auto& entry) { return entry.second.seen_cycle != cycle; });
	}
}
