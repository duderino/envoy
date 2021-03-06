licenses(["notice"])  # Apache 2

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_cc_test_library",
    "envoy_package",
)

envoy_package()

exports_files(["filesystem_setup.sh"])

filegroup(
    name = "filesystem_test_data",
    srcs = glob(["test_data/**"]),
)

envoy_cc_test_library(
    name = "utility_lib",
    hdrs = [
        "utility.h",
    ],
    deps = [
        "//source/common/runtime:runtime_lib",
    ],
)

envoy_cc_test(
    name = "runtime_impl_test",
    srcs = ["runtime_impl_test.cc"],
    data = glob(["test_data/**"]) + ["filesystem_setup.sh"],
    deps = [
        "//source/common/config:runtime_utility_lib",
        "//source/common/runtime:runtime_lib",
        "//source/common/stats:isolated_store_lib",
        "//source/common/stats:stats_lib",
        "//test/mocks/event:event_mocks",
        "//test/mocks/filesystem:filesystem_mocks",
        "//test/mocks/init:init_mocks",
        "//test/mocks/local_info:local_info_mocks",
        "//test/mocks/protobuf:protobuf_mocks",
        "//test/mocks/runtime:runtime_mocks",
        "//test/mocks/thread_local:thread_local_mocks",
        "//test/mocks/upstream:upstream_mocks",
        "//test/test_common:environment_lib",
    ],
)

envoy_cc_test(
    name = "runtime_flag_override_test",
    srcs = ["runtime_flag_override_test.cc"],
    args = [
        "--runtime-feature-override-for-tests=envoy.reloadable_features.test_feature_false",
    ],
    coverage = False,
    deps = [
        "//source/common/runtime:runtime_lib",
    ],
)

envoy_cc_test(
    name = "uuid_util_test",
    srcs = ["uuid_util_test.cc"],
    deps = [
        "//source/common/runtime:runtime_lib",
        "//source/common/runtime:uuid_util_lib",
    ],
)
