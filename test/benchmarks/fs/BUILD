load("//test/benchmarks:defs.bzl", "benchmark_test")

package(
    default_applicable_licenses = ["//:license"],
    licenses = ["notice"],
)

benchmark_test(
    name = "bazel_test",
    srcs = ["bazel_test.go"],
    use_for_pgo = False,
    visibility = ["//:sandbox"],
    deps = [
        "//pkg/cleanup",
        "//pkg/test/dockerutil",
        "//test/benchmarks/fs/fsbench",
        "//test/benchmarks/harness",
        "//test/benchmarks/tools",
        "@com_github_docker_docker//api/types/mount:go_default_library",
    ],
)

benchmark_test(
    name = "fio_test",
    srcs = ["fio_test.go"],
    data = ["//test/runner/fuse"],
    use_for_pgo = False,
    visibility = ["//:sandbox"],
    deps = [
        "//pkg/cleanup",
        "//pkg/test/dockerutil",
        "//test/benchmarks/harness",
        "//test/benchmarks/tools",
        "//test/metricsviz",
        "@com_github_docker_docker//api/types/mount:go_default_library",
    ],
)

benchmark_test(
    name = "rubydev_test",
    srcs = ["rubydev_test.go"],
    use_for_pgo = True,
    visibility = ["//:sandbox"],
    deps = [
        "//pkg/test/dockerutil",
        "//test/benchmarks/fs/fsbench",
        "//test/benchmarks/harness",
        "//test/benchmarks/tools",
    ],
)
