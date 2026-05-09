load("//tools:defs.bzl", "go_library")

package(
    default_applicable_licenses = ["//:license"],
    licenses = ["notice"],
)

go_library(
    name = "plugin",
    srcs = ["plugin_linux.go"],
    visibility = ["//shim:__subpackages__"],
    deps = [
        "//pkg/shim/v1",  # keep
        "@com_github_containerd_containerd//pkg/shutdown:go_default_library",
        "@com_github_containerd_containerd//plugin:go_default_library",
        "@com_github_containerd_containerd//runtime/v2/shim:go_default_library",
    ],
)
