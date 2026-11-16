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

//* Per-process network byte counters through eBPF (see netio/btop_netio.bpf.c)

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "../btop_log.hpp"
#include "../btop_shared.hpp"

#ifdef NETIO_SUPPORT
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "netio/btop_netio.bpf.h"
#endif

namespace Proc::NetIO {
#ifdef NETIO_SUPPORT
	namespace {
		//? Must match struct netio_bytes in btop_netio.bpf.c
		struct netio_bytes {
			uint64_t rx;
			uint64_t tx;
		};

		bpf_object* obj = nullptr;
		std::vector<bpf_link*> links;
		int map_fd = -1;
		bool use_batch = true;

		//? libbpf prints to stderr by default, which would corrupt the terminal UI
		int libbpf_log(enum libbpf_print_level level, const char* format, va_list args) {
			if (level == LIBBPF_DEBUG) return 0;
			char buf[512];
			vsnprintf(buf, sizeof(buf), format, args);
			std::string msg{buf};
			while (not msg.empty() and (msg.back() == '\n' or msg.back() == ' ')) msg.pop_back();
			Logger::debug("{}", msg);
			return 0;
		}

		void cleanup() {
			for (auto* link : links) bpf_link__destroy(link);
			links.clear();
			if (obj != nullptr) bpf_object__close(obj);
			obj = nullptr;
			map_fd = -1;
		}

		//? Runs before the logger is set up, main() logs <status> once the config is loaded
		bool fail(std::string reason) {
			status = std::move(reason);
			cleanup();
			return false;
		}

		bool read_batch(std::unordered_map<size_t, counters>& out) {
			constexpr uint32_t chunk = 1024;
			static std::vector<uint32_t> keys(chunk);
			static std::vector<netio_bytes> vals(chunk);
			bpf_map_batch_opts opts{};
			opts.sz = sizeof(opts);
			uint32_t in_batch = 0, out_batch = 0;
			bool first = true;
			for (;;) {
				uint32_t count = chunk;
				const int err = bpf_map_lookup_batch(map_fd, first ? nullptr : &in_batch, &out_batch, keys.data(), vals.data(), &count, &opts);
				const int code = (err < 0 ? -err : 0);
				if (code != 0 and code != ENOENT) return false;
				for (uint32_t i = 0; i < count; i++)
					out[keys[i]] = {vals[i].rx, vals[i].tx};
				if (code == ENOENT) return true;
				in_batch = out_batch;
				first = false;
			}
		}

		//? libbpf 0.x returns error codes inside pointers unless strict mode is on, libbpf 1.x returns NULL
		bool bad(const void* ptr) { return ptr == nullptr or libbpf_get_error(ptr) != 0; }

		void read_iter(std::unordered_map<size_t, counters>& out) {
			uint32_t key = 0, next = 0;
			netio_bytes val{};
			for (bool first = true; bpf_map_get_next_key(map_fd, first ? nullptr : &key, &next) == 0; first = false) {
				if (bpf_map_lookup_elem(map_fd, &next, &val) == 0)
					out[next] = {val.rx, val.tx};
				key = next;
			}
		}
	}

	bool init() {
		if (active) return true;
		libbpf_set_print(libbpf_log);

		bpf_object_open_opts opts{};
		opts.sz = sizeof(opts);
		opts.object_name = "btop_netio";
		obj = bpf_object__open_mem(btop_netio_bpf, btop_netio_bpf_len, &opts);
		if (bad(obj)) {
			const long err = (obj == nullptr ? errno : -libbpf_get_error(obj));
			obj = nullptr;
			return fail(fmt::format("could not open eBPF object ({})", strerror((int)err)));
		}

		if (const int err = bpf_object__load(obj); err != 0) {
			if (err == -EPERM or err == -EACCES)
				return fail("missing privileges, run fbktop as root or give it CAP_BPF and CAP_PERFMON (make setcap)");
			return fail(fmt::format("could not load eBPF program ({}), needs Linux 6.3+ with BTF", strerror(-err)));
		}

		bpf_program* prog = nullptr;
		bpf_object__for_each_program(prog, obj) {
			bpf_link* link = bpf_program__attach(prog);
			if (bad(link)) {
				const long err = (link == nullptr ? errno : -libbpf_get_error(link));
				return fail(fmt::format("could not attach {} ({})", bpf_program__name(prog), strerror((int)err)));
			}
			links.push_back(link);
		}

		map_fd = bpf_object__find_map_fd_by_name(obj, "proc_netio");
		if (map_fd < 0) return fail("eBPF map not found");

		status = "active";
		active = true;
		return true;
	}

	void stop() {
		if (not active) return;
		active = false;
		status = "turned off (proc_net = false)";
		cleanup();
	}

	bool read(std::unordered_map<size_t, counters>& out) {
		out.clear();
		if (not active) return false;
		if (use_batch and read_batch(out)) return true;
		//? Batch lookups unsupported or failed, use plain iteration from here on
		if (use_batch) {
			use_batch = false;
			out.clear();
		}
		read_iter(out);
		return true;
	}

	void forget(const vector<size_t>& pids) {
		if (not active) return;
		for (const auto pid : pids) {
			const uint32_t key = static_cast<uint32_t>(pid);
			bpf_map_delete_elem(map_fd, &key);
		}
	}
#else
	bool init() {
		status = "not compiled in (rebuild with libbpf-dev installed)";
		return false;
	}

	bool read(std::unordered_map<size_t, counters>& out) {
		out.clear();
		return false;
	}

	void forget(const vector<size_t>&) {}

	void stop() {}
#endif
}
