load("//tools:defs.bzl", "go_library", "go_test")

package(
    default_applicable_licenses = ["//:license"],
    licenses = ["notice"],
)

go_library(
    name = "utils",
    srcs = [
        "annotations.go",
        "utils.go",
        "volumes.go",
    ],
    visibility = [
        "//visibility:public",
    ],
    deps = [
        "//runsc/specutils",
        "@com_github_containerd_log//:go_default_library",
        "@com_github_opencontainers_runtime_spec//specs-go:go_default_library",
    ],
)

go_test(
    name = "utils_test",
    size = "small",
    srcs = ["volumes_test.go"],
    library = ":utils",
    deps = [
        "@com_github_mohae_deepcopy//:go_default_library",
        "@com_github_opencontainers_runtime_spec//specs-go:go_default_library",
    ],
)
