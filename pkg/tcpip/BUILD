load("//tools:defs.bzl", "go_library", "go_test")
load("//tools:deps.bzl", "deps_test")
load("//tools/go_generics:defs.bzl", "go_template_instance")

package(
    default_applicable_licenses = ["//:license"],
    licenses = ["notice"],
)

go_template_instance(
    name = "sock_err_list",
    out = "sock_err_list.go",
    package = "tcpip",
    prefix = "sockError",
    template = "//pkg/ilist:generic_list",
    types = {
        "Element": "*SockError",
        "Linker": "*SockError",
    },
)

go_template_instance(
    name = "route_list",
    out = "route_list.go",
    package = "tcpip",
    prefix = "Route",
    template = "//pkg/ilist:generic_list",
    types = {
        "Element": "*Route",
        "Linker": "*Route",
    },
)

go_library(
    name = "tcpip",
    srcs = [
        "errors.go",
        "errors_linux.go",
        "route_list.go",
        "sock_err_list.go",
        "socketops.go",
        "stdclock.go",
        "stdclock_state.go",
        "tcpip.go",
        "tcpip_state.go",
        "timer.go",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//pkg/atomicbitops",
        "//pkg/buffer",
        "//pkg/rand",
        "//pkg/sync",
        "//pkg/waiter",
        "@org_golang_x_sys//unix:go_default_library",
    ],
)

deps_test(
    name = "netstack_deps_test",
    # NOTE: Try not to let allowed or allowed_prefixes to grow large.
    #
    # Netstack is intentionally somewhat separate from the rest of gVisor; it is
    # intended to be usable separately as a library. Therefore we limit its
    # dependencies to a small set of packages. If you're adding dependencies,
    # consider whether netstack should really depend on them.
    allowed = [
        # gVisor deps.
        "//pkg/atomicbitops",
        "//pkg/bits",
        "//pkg/context",
        "//pkg/buffer",
        "//pkg/cpuid",
        "//pkg/gohacks",
        "//pkg/goid",
        "//pkg/ilist",
        "//pkg/linewriter",
        "//pkg/log",
        "//pkg/pool",
        "//pkg/rand",
        "//pkg/rawfile",
        "//pkg/refs",
        "//pkg/sleep",
        "//pkg/state",
        "//pkg/state/wire",
        "//pkg/sync",
        "//pkg/sync/locking",
        "//pkg/waiter",
        "//pkg/xdp",

        # Other deps.
        "@com_github_google_btree//:go_default_library",
        "@org_golang_x_sys//cpu:go_default_library",
        "@org_golang_x_sys//unix:go_default_library",
        "@org_golang_x_time//rate:go_default_library",
    ],
    allowed_prefixes = [
        "//pkg/tcpip",
        "@org_golang_x_sys//internal/unsafeheader",
    ],
    targets = [
        "//pkg/tcpip",
        "//pkg/tcpip/adapters/gonet",
        "//pkg/tcpip/link/channel",
        "//pkg/tcpip/header",
        "//pkg/tcpip/link/fdbased",
        "//pkg/tcpip/link/loopback",
        "//pkg/tcpip/link/qdisc/fifo",
        "//pkg/tcpip/link/sniffer",
        "//pkg/tcpip/network/arp",
        "//pkg/tcpip/network/ipv4",
        "//pkg/tcpip/network/ipv6",
        "//pkg/tcpip/stack",
        "//pkg/tcpip/transport/icmp",
        "//pkg/tcpip/transport/raw",
        "//pkg/tcpip/transport/tcp",
        "//pkg/tcpip/transport/udp",
    ],
)

go_test(
    name = "tcpip_test",
    size = "small",
    srcs = [
        "errors_test.go",
        "tcpip_test.go",
    ],
    library = ":tcpip",
    deps = [
        "@com_github_google_go_cmp//cmp:go_default_library",
        "@org_golang_x_sys//unix:go_default_library",
    ],
)

go_test(
    name = "tcpip_x_test",
    size = "small",
    srcs = ["timer_test.go"],
    deps = [":tcpip"],
)
