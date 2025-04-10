load("//tools:defs.bzl", "go_library", "go_test")
load("//tools/go_generics:defs.bzl", "go_template_instance")

package(
    default_applicable_licenses = ["//:license"],
    licenses = ["notice"],
)

go_template_instance(
    name = "chunk_refs",
    out = "chunk_refs.go",
    package = "buffer",
    prefix = "chunk",
    template = "//pkg/refs:refs_template",
    types = {
        "T": "chunk",
    },
)

go_template_instance(
    name = "view_list",
    out = "view_list.go",
    package = "buffer",
    prefix = "View",
    template = "//pkg/ilist:generic_list",
    types = {
        "Element": "*View",
        "Linker": "*View",
    },
)

go_library(
    name = "buffer",
    srcs = [
        "buffer.go",
        "buffer_state.go",
        "chunk.go",
        "chunk_refs.go",
        "view.go",
        "view_list.go",
        "view_unsafe.go",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//pkg/atomicbitops",
        "//pkg/bits",
        "//pkg/context",
        "//pkg/ilist",
        "//pkg/log",
        "//pkg/pool",
        "//pkg/refs",
        "//pkg/sync",
        "//pkg/tcpip/checksum",
    ],
)

go_test(
    name = "buffer_test",
    srcs = [
        "buffer_test.go",
        "view_test.go",
    ],
    library = ":buffer",
    deps = [
        "//pkg/rand",
        "//pkg/state",
        "//pkg/tcpip/checksum",
        "@com_github_google_go_cmp//cmp:go_default_library",
    ],
)
