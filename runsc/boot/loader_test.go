// Copyright 2018 The gVisor Authors.
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

package boot

import (
	"fmt"
	"math/rand"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/moby/sys/capability"
	specs "github.com/opencontainers/runtime-spec/specs-go"
	"golang.org/x/sys/unix"
	"gvisor.dev/gvisor/pkg/abi/linux"
	"gvisor.dev/gvisor/pkg/control/server"
	"gvisor.dev/gvisor/pkg/cpuid"
	"gvisor.dev/gvisor/pkg/fspath"
	"gvisor.dev/gvisor/pkg/log"
	"gvisor.dev/gvisor/pkg/sentry/kernel/auth"
	"gvisor.dev/gvisor/pkg/sentry/seccheck"
	"gvisor.dev/gvisor/pkg/sentry/vfs"
	"gvisor.dev/gvisor/pkg/sync"
	"gvisor.dev/gvisor/pkg/unet"
	"gvisor.dev/gvisor/runsc/config"
	"gvisor.dev/gvisor/runsc/flag"
	"gvisor.dev/gvisor/runsc/fsgofer"
)

func init() {
	log.SetLevel(log.Debug)
	if err := fsgofer.OpenProcSelfFD("/proc/self/fd"); err != nil {
		panic(err)
	}
}

func testConfig() *config.Config {
	testFlags := flag.NewFlagSet("test", flag.ContinueOnError)
	config.RegisterFlags(testFlags)
	conf, err := config.NewFromFlags(testFlags)
	if err != nil {
		panic(err)
	}
	// Change test defaults.
	conf.RootDir = "unused_root_dir"
	conf.Network = config.NetworkNone
	conf.DisableSeccomp = true
	return conf
}

// testSpec returns a simple spec that can be used in tests.
func testSpec() *specs.Spec {
	return &specs.Spec{
		// The host filesystem root is the sandbox root.
		Root: &specs.Root{
			Path:     "/",
			Readonly: true,
		},
		Process: &specs.Process{
			Args: []string{"/bin/true"},
		},
	}
}

// startGofer starts a new gofer routine serving 'root' path. It returns the
// sandbox side of the connection, and a function that when called will stop the
// gofer.
func startGofer(root string, conf *config.Config) (int, func(), error) {
	fds, err := unix.Socketpair(unix.AF_UNIX, unix.SOCK_STREAM|unix.SOCK_CLOEXEC, 0)
	if err != nil {
		return 0, nil, err
	}
	sandboxEnd, goferEnd := fds[0], fds[1]

	socket, err := unet.NewSocket(goferEnd)
	if err != nil {
		unix.Close(sandboxEnd)
		unix.Close(goferEnd)
		return 0, nil, fmt.Errorf("error creating server on FD %d: %v", goferEnd, err)
	}
	server := fsgofer.NewLisafsServer(fsgofer.Config{
		HostUDS:            conf.GetHostUDS(),
		HostFifo:           conf.HostFifo,
		DonateMountPointFD: conf.DirectFS,
	})
	c, err := server.CreateConnection(socket, root, true /* readonly */)
	if err != nil {
		return 0, nil, err
	}
	server.StartConnection(c)
	// Closing the gofer socket will stop the gofer and exit goroutine above.
	cleanup := func() {
		if err := socket.Close(); err != nil {
			log.Warningf("Error closing gofer socket: %v", err)
		}
		server.Wait()
		server.Destroy()
	}
	return sandboxEnd, cleanup, nil
}

func createLoader(conf *config.Config, spec *specs.Spec) (*Loader, func(), error) {
	sock := fmt.Sprintf("\x00loader-test.%010d", rand.Int())
	fd, err := server.CreateSocket(sock)
	if err != nil {
		return nil, nil, fmt.Errorf("failed to create socket: %w", err)
	}
	sandEnd, cleanup, err := startGofer(spec.Root.Path, conf)
	if err != nil {
		return nil, nil, fmt.Errorf("failed to start gofer: %w", err)
	}

	// Loader takes ownership of stdio.
	var stdio []int
	for _, f := range []*os.File{os.Stdin, os.Stdout, os.Stderr} {
		fd := int(f.Fd())
		newFd, err := unix.Dup(fd)
		if err != nil {
			return nil, nil, fmt.Errorf("failed to dup FD %d: %w", fd, err)
		}
		stdio = append(stdio, newFd)
	}

	args := Args{
		ID:              "foo",
		Spec:            spec,
		Conf:            conf,
		ControllerFD:    fd,
		GoferFDs:        []int{sandEnd},
		DevGoferFD:      -1,
		StdioFDs:        stdio,
		GoferMountConfs: []GoferMountConf{{Lower: Lisafs, Upper: NoOverlay}},
		PodInitConfigFD: -1,
		ExecFD:          -1,
	}
	l, err := New(args)
	if err != nil {
		cleanup()
		return nil, nil, fmt.Errorf("boot.New: %w", err)
	}
	return l, cleanup, nil
}

// TestRun runs a simple application in a sandbox and checks that it succeeds.
func TestRun(t *testing.T) {
	l, cleanup, err := createLoader(testConfig(), testSpec())
	if err != nil {
		t.Fatalf("error creating loader: %v", err)
	}

	defer l.Destroy()
	defer cleanup()

	// Start a goroutine to read the start chan result, otherwise Run will
	// block forever.
	var resultChanErr error
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		resultChanErr = <-l.ctrl.manager.startResultChan
		wg.Done()
	}()

	// Run the container.
	if err := l.Run(); err != nil {
		t.Errorf("error running container: %v", err)
	}

	// We should have not gotten an error on the startResultChan.
	wg.Wait()
	if resultChanErr != nil {
		t.Errorf("error on startResultChan: %v", resultChanErr)
	}

	// Wait for the application to exit.  It should succeed.
	if status := l.WaitExit(); !status.Exited() || status.ExitStatus() != 0 {
		t.Errorf("application exited with %s, want exit status 0", status)
	}
}

// TestStartSignal tests that the controller Start message will cause
// WaitForStartSignal to return.
func TestStartSignal(t *testing.T) {
	l, cleanup, err := createLoader(testConfig(), testSpec())
	if err != nil {
		t.Fatalf("error creating loader: %v", err)
	}
	defer l.Destroy()
	defer cleanup()

	// We aren't going to wait on this application, so the control server
	// needs to be shut down manually.
	defer l.ctrl.srv.Stop(time.Hour)

	// Start a goroutine that calls WaitForStartSignal and writes to a
	// channel when it returns.
	waitFinished := make(chan struct{})
	go func() {
		l.WaitForStartSignal()
		// Pretend that Run() executed and returned no error.
		l.ctrl.manager.startResultChan <- nil
		waitFinished <- struct{}{}
	}()

	// Nothing has been written to the channel, so waitFinished should not
	// return.  Give it a little bit of time to make sure the goroutine has
	// started.
	select {
	case <-waitFinished:
		t.Errorf("WaitForStartSignal completed but it should not have")
	case <-time.After(50 * time.Millisecond):
		// OK.
	}

	// Trigger the control server StartRoot method.
	cid := "foo"
	if err := l.ctrl.manager.StartRoot(&cid, nil); err != nil {
		t.Errorf("error calling StartRoot: %v", err)
	}

	// Now WaitForStartSignal should return (within a short amount of
	// time).
	select {
	case <-waitFinished:
		// OK.
	case <-time.After(50 * time.Millisecond):
		t.Errorf("WaitForStartSignal did not complete but it should have")
	}
}

// Test that network=host with raw sockets enabled requires CAP_NET_RAW on the
// host.
func TestHostnetWithRawSockets(t *testing.T) {
	// Drop CAP_NET_RAW from effective capabilities, if we have it.
	pid := os.Getpid()
	caps, err := capability.NewPid2(0)
	if err != nil {
		t.Fatalf("error getting capabilities for pid %d: %v", pid, err)
	}
	if err := caps.Load(); err != nil {
		t.Fatalf("error loading capabilities: %v", err)
	}
	if caps.Get(capability.EFFECTIVE, capability.CAP_NET_RAW) {
		caps.Unset(capability.EFFECTIVE, capability.CAP_NET_RAW)
		if err := caps.Apply(capability.EFFECTIVE); err != nil {
			t.Fatalf("error applying capabilities")
		}
		// Be nice and add it back when we are done.
		defer func() {
			caps.Set(capability.EFFECTIVE, capability.CAP_NET_RAW)
			if err := caps.Apply(capability.EFFECTIVE); err != nil {
				t.Fatalf("error restoring capabilities")
			}
		}()
	}

	// Configure host network with raw sockets.
	conf := testConfig()
	conf.Network = config.NetworkHost
	conf.EnableRaw = true

	// Creating loader should fail.
	l, err := New(Args{
		ID:              "should-fail",
		Spec:            testSpec(),
		Conf:            conf,
		DevGoferFD:      -1,
		PodInitConfigFD: -1,
		ExecFD:          -1,
	})
	if err == nil {
		l.Destroy()
		t.Fatalf("expected loader.New() to fail but it did not")
	}
	// Error message must be about CAP_NET_RAW.
	if !strings.Contains(err.Error(), "CAP_NET_RAW") {
		t.Errorf("expected error to contain CAP_NET_RAW but got %q", err)
	}
}

type CreateMountTestcase struct {
	name string
	// Spec that will be used to create the mount manager.  Note
	// that we can't mount procfs without a kernel, so each spec
	// MUST contain something other than procfs mounted at /proc.
	spec specs.Spec
	// Paths that are expected to exist in the resulting fs.
	expectedPaths []string
}

func createMountTestcases() []*CreateMountTestcase {
	testCases := []*CreateMountTestcase{
		{
			// Only proc.
			name: "only proc mount",
			spec: specs.Spec{
				Root: &specs.Root{
					Path:     os.TempDir(),
					Readonly: true,
				},
				Mounts: []specs.Mount{
					{
						Destination: "/proc",
						Type:        "tmpfs",
					},
				},
			},
			// /proc, /dev, and /sys should always be mounted.
			expectedPaths: []string{"/proc", "/dev", "/sys"},
		},
		{
			// Mount at a deep path, with many components that do
			// not exist in the root.
			name: "deep mount path",
			spec: specs.Spec{
				Root: &specs.Root{
					Path:     os.TempDir(),
					Readonly: true,
				},
				Mounts: []specs.Mount{
					{
						Destination: "/some/very/very/deep/path",
						Type:        "tmpfs",
					},
					{
						Destination: "/proc",
						Type:        "tmpfs",
					},
				},
			},
			// /some/deep/path should be mounted, along with /proc, /dev, and /sys.
			expectedPaths: []string{"/some/very/very/deep/path", "/proc", "/dev", "/sys"},
		},
		{
			// Mounts are nested inside each other.
			name: "nested mounts",
			spec: specs.Spec{
				Root: &specs.Root{
					Path:     os.TempDir(),
					Readonly: true,
				},
				Mounts: []specs.Mount{
					{
						Destination: "/proc",
						Type:        "tmpfs",
					},
					{
						Destination: "/foo",
						Type:        "tmpfs",
					},
					{
						Destination: "/foo/qux",
						Type:        "tmpfs",
					},
					{
						// File mounts with the same prefix.
						Destination: "/foo/qux-quz",
						Type:        "tmpfs",
					},
					{
						Destination: "/foo/bar",
						Type:        "tmpfs",
					},
					{
						Destination: "/foo/bar/baz",
						Type:        "tmpfs",
					},
					{
						// A deep path that is in foo but not the other mounts.
						Destination: "/foo/some/very/very/deep/path",
						Type:        "tmpfs",
					},
				},
			},
			expectedPaths: []string{"/foo", "/foo/bar", "/foo/bar/baz", "/foo/qux",
				"/foo/qux-quz", "/foo/some/very/very/deep/path", "/proc", "/dev", "/sys"},
		},
		{
			name: "mount inside /dev",
			spec: specs.Spec{
				Root: &specs.Root{
					Path:     os.TempDir(),
					Readonly: true,
				},
				Mounts: []specs.Mount{
					{
						Destination: "/proc",
						Type:        "tmpfs",
					},
					{
						Destination: "/dev",
						Type:        "tmpfs",
					},
					{
						// Mount with the same prefix.
						Destination: "/dev/fd-foo",
						Type:        "tmpfs",
					},
					{
						// Unsupported fs type.
						Destination: "/dev/mqueue",
						Type:        "mqueue",
					},
					{
						Destination: "/dev/foo",
						Type:        "tmpfs",
					},
					{
						Destination: "/dev/bar",
						Type:        "tmpfs",
					},
				},
			},
			expectedPaths: []string{"/proc", "/dev", "/dev/fd-foo", "/dev/foo", "/dev/bar", "/sys"},
		},
		{
			name: "mounts inside mandatory mounts",
			spec: specs.Spec{
				Root: &specs.Root{
					Path:     os.TempDir(),
					Readonly: true,
				},
				Mounts: []specs.Mount{
					{
						Destination: "/proc",
						Type:        "tmpfs",
					},
					{
						Destination: "/sys/bar",
						Type:        "tmpfs",
					},
					{
						Destination: "/tmp/baz",
						Type:        "tmpfs",
					},
					{
						Destination: "/dev/goo",
						Type:        "tmpfs",
					},
				},
			},
			expectedPaths: []string{"/proc", "/sys", "/sys/bar", "/tmp", "/tmp/baz", "/dev/goo"},
		},
	}

	return testCases
}

// Test that MountNamespace can be created with various specs.
func TestCreateMountNamespace(t *testing.T) {
	for _, tc := range createMountTestcases() {
		t.Run(tc.name, func(t *testing.T) {
			spec := testSpec()
			spec.Mounts = tc.spec.Mounts
			spec.Root = tc.spec.Root

			t.Logf("Using root: %q", spec.Root.Path)
			l, loaderCleanup, err := createLoader(testConfig(), spec)
			if err != nil {
				t.Fatalf("failed to create loader: %v", err)
			}
			defer l.Destroy()
			defer loaderCleanup()

			l.mu.Lock()
			defer l.mu.Unlock()
			mntr := newContainerMounter(&l.root, l.k, l.mountHints, l.sharedMounts, "", l.sandboxID)
			ctx := l.k.SupervisorContext()
			creds := auth.NewRootCredentials(l.root.procArgs.Credentials.UserNamespace)
			mns, err := mntr.mountAll(ctx, creds, l.root.spec, l.root.conf, &l.root.procArgs)
			if err != nil {
				t.Fatalf("mountAll: %v", err)
			}

			root := mns.Root(ctx)
			defer root.DecRef(ctx)
			for _, p := range tc.expectedPaths {
				target := &vfs.PathOperation{
					Root:  root,
					Start: root,
					Path:  fspath.Parse(p),
				}

				if d, err := l.k.VFS().GetDentryAt(ctx, l.root.procArgs.Credentials, target, &vfs.GetDentryOptions{}); err != nil {
					t.Errorf("expected path %v to exist with spec %v, but got error %v", p, tc.spec, err)
				} else {
					d.DecRef(ctx)
				}
			}
		})
	}
}

type createMountPointTestCase struct {
	name string
	path string
	mode linux.FileMode
	fail bool
}

func TestCreateMountPoint(t *testing.T) {
	var createMountPointTestCases = []createMountPointTestCase{
		{
			name: "File",
			path: "/test/test-file",
			mode: linux.ModeRegular,
		},
		{
			name: "Directory",
			path: "/test/test-dir",
			mode: linux.ModeDirectory,
		},
		{
			name: "FileWithIntDirs",
			path: "/test/a/b/c/test-file",
			mode: linux.ModeRegular,
		},
		{
			name: "DirectoryWithIntDirs",
			path: "/test/d/e/f/g/test-dir",
			mode: linux.ModeDirectory,
		},
		{
			name: "ExistingFile",
			path: "/test/test-file",
			mode: linux.ModeRegular,
		},
		{
			name: "ExistingDirectory",
			path: "/test/test-dir",
			mode: linux.ModeDirectory,
		},
		{
			name: "DirVSFile",
			path: "/test/test-file",
			mode: linux.ModeDirectory,
			fail: true,
		},
		{
			name: "FileVSDir",
			path: "/test/test-dir",
			mode: linux.ModeRegular,
			fail: true,
		},
	}

	spec := testSpec()
	spec.Root = &specs.Root{
		Path: os.TempDir(),
	}
	spec.Mounts = append(spec.Mounts, specs.Mount{
		Destination: "/test",
		Type:        "tmpfs",
	})

	t.Logf("Using root: %q", spec.Root.Path)
	l, loaderCleanup, err := createLoader(testConfig(), spec)
	if err != nil {
		t.Fatalf("failed to create loader: %v", err)
	}
	defer l.Destroy()
	defer loaderCleanup()

	l.mu.Lock()
	defer l.mu.Unlock()
	mntr := newContainerMounter(&l.root, l.k, l.mountHints, l.sharedMounts, "", l.sandboxID)
	ctx := l.k.SupervisorContext()
	creds := auth.NewRootCredentials(l.root.procArgs.Credentials.UserNamespace)
	mns, err := mntr.mountAll(ctx, creds, l.root.spec, l.root.conf, &l.root.procArgs)
	if err != nil {
		t.Fatalf("mountAll: %v", err)
	}

	root := mns.Root(ctx)
	defer root.DecRef(ctx)

	for _, tc := range createMountPointTestCases {
		t.Run(tc.name, func(t *testing.T) {
			if err := mntr.makeMountPoint(ctx, creds, mns, tc.path, tc.mode); err != nil {
				if tc.fail {
					return
				}
				t.Fatalf("makeMountPoint failed: %v", err)
			} else {
				if tc.fail {
					t.Fatalf("makeMountPoint doesn't fail")
				}
			}
			target := &vfs.PathOperation{
				Root:  root,
				Start: root,
				Path:  fspath.Parse(tc.path),
			}

			if mode, err := mntr.getPathMode(ctx, creds, target); err != nil {
				t.Errorf("expected path %v, but got error %v", tc.path, err)
			} else {
				if mode.IsDir() != tc.mode.IsDir() {
					t.Errorf("path mode %v doesn't match the mount mode %v", mode, tc.mode)
				}
			}
		})
	}
}

func TestMain(m *testing.M) {
	cpuid.Initialize()
	seccheck.Initialize()
	os.Exit(m.Run())
}
