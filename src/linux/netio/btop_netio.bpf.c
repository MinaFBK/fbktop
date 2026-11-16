// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
/*
 * Per-process network byte counters for btop.
 *
 * Hooks the kernel's sock:sock_send_length / sock:sock_recv_length tracepoints
 * (Linux 6.3+). They fire once per sendmsg/recvmsg on any socket with the
 * number of bytes actually moved, in the context of the calling process.
 * Bytes are added to a per-process (tgid) entry in an LRU hash map that btop
 * reads every update.
 *
 * Only AF_INET/AF_INET6 sockets count, and loopback traffic (127.0.0.0/8, ::1,
 * ::ffff:127.0.0.0/104) is skipped so the per-process numbers line up with what
 * the real network interfaces carry.
 *
 * Rebuild with `make netio-bpf` (needs clang + bpftool); the compiled object is
 * embedded in btop through btop_netio.bpf.h, so a normal build needs neither.
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_endian.h>

#define AF_INET  2
#define AF_INET6 10
#define MSG_PEEK 2

struct netio_bytes {
	__u64 rx;
	__u64 tx;
};

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 16384);
	__type(key, __u32);
	__type(value, struct netio_bytes);
} proc_netio SEC(".maps");

static __always_inline bool v4_loopback(__be32 addr) {
	return (bpf_ntohl(addr) >> 24) == 127;
}

//? a0..a3 are the four 32-bit words of an IPv6 address in network byte order
static __always_inline bool v6_loopback(__u32 a0, __u32 a1, __u32 a2, __u32 a3) {
	if (a0 != 0 || a1 != 0) return false;
	if (a2 == 0) return a3 == bpf_htonl(1);                  // ::1
	if (a2 == bpf_htonl(0xffff)) return v4_loopback(a3);     // ::ffff:127.x.x.x
	return false;
}

static __always_inline bool is_loopback(const struct sock *sk, __u16 family) {
	if (family == AF_INET) {
		__be32 daddr = sk->__sk_common.skc_daddr;
		//? Unconnected sockets have no peer, fall back to the bound address
		return v4_loopback(daddr != 0 ? daddr : sk->__sk_common.skc_rcv_saddr);
	}

	__u32 d0 = sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32[0];
	__u32 d1 = sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32[1];
	__u32 d2 = sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32[2];
	__u32 d3 = sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32[3];
	if ((d0 | d1 | d2 | d3) != 0) return v6_loopback(d0, d1, d2, d3);

	__u32 s0 = sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32[0];
	__u32 s1 = sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32[1];
	__u32 s2 = sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32[2];
	__u32 s3 = sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32[3];
	return v6_loopback(s0, s1, s2, s3);
}

static __always_inline void account(const struct sock *sk, int ret, bool rx) {
	if (ret <= 0 || !sk) return;

	__u16 family = sk->__sk_common.skc_family;
	if (family != AF_INET && family != AF_INET6) return;
	if (is_loopback(sk, family)) return;

	__u32 tgid = bpf_get_current_pid_tgid() >> 32;
	struct netio_bytes *val = bpf_map_lookup_elem(&proc_netio, &tgid);
	if (!val) {
		struct netio_bytes zero = {};
		bpf_map_update_elem(&proc_netio, &tgid, &zero, BPF_NOEXIST);
		val = bpf_map_lookup_elem(&proc_netio, &tgid);
		if (!val) return;
	}
	if (rx) __sync_fetch_and_add(&val->rx, (__u64)ret);
	else    __sync_fetch_and_add(&val->tx, (__u64)ret);
}

SEC("tp_btf/sock_send_length")
int BPF_PROG(btop_sock_send, struct sock *sk, int ret, int flags) {
	account(sk, ret, false);
	return 0;
}

SEC("tp_btf/sock_recv_length")
int BPF_PROG(btop_sock_recv, struct sock *sk, int ret, int flags) {
	//? A peek leaves the data queued; it gets counted when actually read
	if (flags & MSG_PEEK) return 0;
	account(sk, ret, true);
	return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
