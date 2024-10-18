// Copyright 2023 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package specutils

import (
	"fmt"
	"strconv"
	"strings"

	specs "github.com/opencontainers/runtime-spec/specs-go"
	"gvisor.dev/gvisor/pkg/sentry/devices/nvproxy/nvconf"
	"gvisor.dev/gvisor/runsc/config"
)

const (
	// NVIDIA_VISIBLE_DEVICES environment variable controls which GPUs are
	// visible and accessible to the container.
	nvidiaVisibleDevsEnv = "NVIDIA_VISIBLE_DEVICES"
	// NVIDIA_DRIVER_CAPABILITIES environment variable allows to fine-tune which
	// NVIDIA driver components are mounted and accessible within a container.
	nvidiaDriverCapsEnv = "NVIDIA_DRIVER_CAPABILITIES"
	// CUDA_VERSION environment variable indicates the version of the CUDA
	// toolkit installed on in the container image.
	cudaVersionEnv = "CUDA_VERSION"
	// NVIDIA_REQUIRE_CUDA environment variable indicates the CUDA toolkit
	// version that a container needs.
	requireCudaEnv = "NVIDIA_REQUIRE_CUDA"
	// AnnotationNVProxy enables nvproxy.
	AnnotationNVProxy = "dev.gvisor.internal.nvproxy"
)

// NVProxyEnabled checks both the nvproxy annotation and conf.NVProxy to see if nvproxy is enabled.
func NVProxyEnabled(spec *specs.Spec, conf *config.Config) bool {
	if conf.NVProxy {
		return true
	}
	return AnnotationToBool(spec, AnnotationNVProxy)
}

// GPUFunctionalityRequested returns true if the container should have access
// to GPU functionality.
func GPUFunctionalityRequested(spec *specs.Spec, conf *config.Config) bool {
	if !NVProxyEnabled(spec, conf) {
		// nvproxy disabled.
		return false
	}
	// In GKE, the nvidia_gpu device plugin injects NVIDIA devices into
	// spec.Linux.Devices when GPUs are allocated to a container.
	if spec.Linux != nil {
		for _, dev := range spec.Linux.Devices {
			if dev.Path == "/dev/nvidiactl" {
				return true
			}
		}
	}
	return gpuFunctionalityRequestedViaHook(spec, conf)
}

// GPUFunctionalityRequestedViaHook returns true if the container should have
// access to GPU functionality configured via nvidia-container-runtime-hook.
// This hook is used by:
// - Docker when using `--gpus` flag from the CLI.
// - nvidia-container-runtime when using its legacy mode.
func GPUFunctionalityRequestedViaHook(spec *specs.Spec, conf *config.Config) bool {
	if !NVProxyEnabled(spec, conf) {
		// nvproxy disabled.
		return false
	}
	return gpuFunctionalityRequestedViaHook(spec, conf)
}

// Precondition: NVProxyEnabled(spec, conf).
func gpuFunctionalityRequestedViaHook(spec *specs.Spec, conf *config.Config) bool {
	if !isNvidiaHookPresent(spec, conf) {
		return false
	}
	// In Docker mode, GPU access is only requested if NVIDIA_VISIBLE_DEVICES is
	// non-empty and set to a value that doesn't mean "no GPU".
	if spec.Process == nil {
		return false
	}
	nvd, _ := EnvVar(spec.Process.Env, nvidiaVisibleDevsEnv)
	// A value of "none" means "no GPU device, but still access to driver
	// functionality", so it is not a value we check for here.
	return nvd != "" && nvd != "void"
}

func isNvidiaHookPresent(spec *specs.Spec, conf *config.Config) bool {
	if conf.NVProxyDocker {
		// This has the effect of injecting the nvidia-container-runtime-hook.
		return true
	}

	if spec.Hooks != nil {
		for _, h := range spec.Hooks.Prestart {
			if strings.HasSuffix(h.Path, "/nvidia-container-runtime-hook") {
				return true
			}
		}
	}
	return false
}

// ParseNvidiaVisibleDevices parses NVIDIA_VISIBLE_DEVICES env var and returns
// the devices specified in it. This can be passed to nvidia-container-cli.
//
// Precondition: conf.NVProxyDocker && GPUFunctionalityRequested(spec, conf).
func ParseNvidiaVisibleDevices(spec *specs.Spec) (string, error) {
	nvd, _ := EnvVar(spec.Process.Env, nvidiaVisibleDevsEnv)
	if nvd == "none" {
		return "", nil
	}
	if nvd == "all" {
		return "all", nil
	}

	for _, gpuDev := range strings.Split(nvd, ",") {
		// Validate gpuDev. We only support the following formats for now:
		// * GPU indices (e.g. 0,1,2)
		// * GPU UUIDs (e.g. GPU-fef8089b)
		//
		// We do not support MIG devices yet.
		if strings.HasPrefix(gpuDev, "GPU-") {
			continue
		}
		_, err := strconv.ParseUint(gpuDev, 10, 32)
		if err != nil {
			return "", fmt.Errorf("invalid %q in NVIDIA_VISIBLE_DEVICES %q: %w", gpuDev, nvd, err)
		}
	}
	return nvd, nil
}

// NVProxyDriverCapsFromEnv returns the driver capabilities requested by the
// application via the NVIDIA_DRIVER_CAPABILITIES env var. See
// nvidia-container-toolkit/cmd/nvidia-container-runtime-hook/container_config.go:getDriverCapabilities().
func NVProxyDriverCapsFromEnv(spec *specs.Spec, conf *config.Config) (nvconf.DriverCaps, error) {
	// Construct the set of allowed driver capabilities.
	allowedDriverCaps := nvconf.DriverCapsFromString(conf.NVProxyAllowedDriverCapabilities)
	// Resolve "all" to nvconf.SupportedDriverCaps.
	if allowedDriverCaps.HasAll() {
		delete(allowedDriverCaps, nvconf.AllCap)
		for cap := range nvconf.SupportedDriverCaps {
			allowedDriverCaps[cap] = struct{}{}
		}
	}

	// Extract the set of driver capabilities requested by the application.
	driverCapsEnvStr, ok := EnvVar(spec.Process.Env, nvidiaDriverCapsEnv)
	if !ok {
		// Nothing requested. Fallback to default configurations.
		if IsLegacyCudaImage(spec) {
			return allowedDriverCaps, nil
		}
		return nvconf.DefaultDriverCaps, nil
	}
	if len(driverCapsEnvStr) == 0 {
		// Empty. Fallback to nvconf.DefaultDriverCaps.
		return nvconf.DefaultDriverCaps, nil
	}
	envDriverCaps := nvconf.DriverCapsFromString(driverCapsEnvStr)

	// Intersect what's requested with what's allowed. Since allowedDriverCaps
	// doesn't contain "all", the result will also not contain "all".
	driverCaps := allowedDriverCaps.Intersect(envDriverCaps)
	if !envDriverCaps.HasAll() && len(driverCaps) != len(envDriverCaps) {
		return nil, fmt.Errorf("disallowed driver capabilities requested: '%v' (allowed '%v'), update --nvproxy-allowed-driver-capabilities to allow them", envDriverCaps, driverCaps)
	}
	return driverCaps, nil
}

// IsLegacyCudaImage returns true if spec represents a legacy CUDA image.
// See nvidia-container-toolkit/internal/config/image/cuda_image.go:IsLegacy().
func IsLegacyCudaImage(spec *specs.Spec) bool {
	cudaVersion, _ := EnvVar(spec.Process.Env, cudaVersionEnv)
	requireCuda, _ := EnvVar(spec.Process.Env, requireCudaEnv)
	return len(cudaVersion) > 0 && len(requireCuda) == 0
}
