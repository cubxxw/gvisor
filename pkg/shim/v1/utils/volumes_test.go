// Copyright 2019 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package utils

import (
	"fmt"
	"os"
	"reflect"
	"testing"

	"github.com/mohae/deepcopy"
	specs "github.com/opencontainers/runtime-spec/specs-go"
)

func TestUpdateVolumeAnnotations(t *testing.T) {
	dir, err := os.MkdirTemp("", "test-update-volume-annotations")
	if err != nil {
		t.Fatalf("create tempdir: %v", err)
	}
	defer os.RemoveAll(dir)
	kubeletPodsDir = dir

	const (
		testPodUID             = "testuid"
		testVolumeName         = "testvolume"
		testNonEmptyVolumeName = "nonemptyvolume"
		testLogDirPath         = "/var/log/pods/testns_testname_" + testPodUID
		testLegacyLogDirPath   = "/var/log/pods/" + testPodUID
	)
	testVolumePath := fmt.Sprintf("%s/%s/volumes/%s/%s", dir, testPodUID, emptyDirVolumesDir, testVolumeName)
	if err := os.MkdirAll(testVolumePath, 0755); err != nil {
		t.Fatalf("Create test volume: %v", err)
	}

	testNonEmptyVolumePath := fmt.Sprintf("%s/%s/volumes/%s/%s", dir, testPodUID, emptyDirVolumesDir, testNonEmptyVolumeName)
	if err := os.MkdirAll(testNonEmptyVolumePath, 0755); err != nil {
		t.Fatalf("Create test volume: %v", err)
	}
	if err := os.WriteFile(testNonEmptyVolumePath+"/file", []byte("hello"), 0644); err != nil {
		t.Fatalf("Create test volume: %v", err)
	}

	for _, test := range []struct {
		name      string
		spec      *specs.Spec
		expected  *specs.Spec // If nil, the spec is expected to be unchanged.
		expectErr bool
	}{
		{
			name: "volume annotations for sandbox",
			spec: &specs.Spec{
				Annotations: map[string]string{
					sandboxLogDirAnnotation:                       testLogDirPath,
					ContainerTypeAnnotation:                       containerTypeSandbox,
					volumeKeyPrefix + testVolumeName + ".share":   "pod",
					volumeKeyPrefix + testVolumeName + ".type":    "tmpfs",
					volumeKeyPrefix + testVolumeName + ".options": "ro",
				},
			},
			expected: &specs.Spec{
				Annotations: map[string]string{
					sandboxLogDirAnnotation:                       testLogDirPath,
					ContainerTypeAnnotation:                       containerTypeSandbox,
					volumeKeyPrefix + testVolumeName + ".share":   "pod",
					volumeKeyPrefix + testVolumeName + ".type":    "tmpfs",
					volumeKeyPrefix + testVolumeName + ".options": "ro",
					volumeKeyPrefix + testVolumeName + ".source":  testVolumePath,
				},
			},
		},
		{
			name: "volume annotations for sandbox with legacy log path",
			spec: &specs.Spec{
				Annotations: map[string]string{
					sandboxLogDirAnnotation:                       testLegacyLogDirPath,
					ContainerTypeAnnotation:                       containerTypeSandbox,
					volumeKeyPrefix + testVolumeName + ".share":   "pod",
					volumeKeyPrefix + testVolumeName + ".type":    "tmpfs",
					volumeKeyPrefix + testVolumeName + ".options": "ro",
				},
			},
			expected: &specs.Spec{
				Annotations: map[string]string{
					sandboxLogDirAnnotation:                       testLegacyLogDirPath,
					ContainerTypeAnnotation:                       containerTypeSandbox,
					volumeKeyPrefix + testVolumeName + ".share":   "pod",
					volumeKeyPrefix + testVolumeName + ".type":    "tmpfs",
					volumeKeyPrefix + testVolumeName + ".options": "ro",
					volumeKeyPrefix + testVolumeName + ".source":  testVolumePath,
				},
			},
		},
		{
			name: "tmpfs: volume annotations for container",
			spec: &specs.Spec{
				Mounts: []specs.Mount{
					{
						Destination: "/test",
						Type:        "bind",
						Source:      testVolumePath,
						Options:     []string{"ro"},
					},
					{
						Destination: "/random",
						Type:        "bind",
						Source:      "/random",
						Options:     []string{"ro"},
					},
				},
				Annotations: map[string]string{
					ContainerTypeAnnotation:                       ContainerTypeContainer,
					volumeKeyPrefix + testVolumeName + ".share":   "pod",
					volumeKeyPrefix + testVolumeName + ".type":    "tmpfs",
					volumeKeyPrefix + testVolumeName + ".options": "ro",
				},
			},
			expected: &specs.Spec{
				Mounts: []specs.Mount{
					{
						Destination: "/test",
						Type:        "tmpfs",
						Source:      testVolumePath,
						Options:     []string{"ro"},
					},
					{
						Destination: "/random",
						Type:        "bind",
						Source:      "/random",
						Options:     []string{"ro"},
					},
				},
				Annotations: map[string]string{
					ContainerTypeAnnotation:                       ContainerTypeContainer,
					volumeKeyPrefix + testVolumeName + ".share":   "pod",
					volumeKeyPrefix + testVolumeName + ".type":    "tmpfs",
					volumeKeyPrefix + testVolumeName + ".options": "ro",
				},
			},
		},
		{
			name: "non-empty volume for sandbox",
			spec: &specs.Spec{
				Annotations: map[string]string{
					sandboxLogDirAnnotation:                               testLogDirPath,
					ContainerTypeAnnotation:                               containerTypeSandbox,
					volumeKeyPrefix + testNonEmptyVolumeName + ".share":   "pod",
					volumeKeyPrefix + testNonEmptyVolumeName + ".type":    "tmpfs",
					volumeKeyPrefix + testNonEmptyVolumeName + ".options": "ro",
				},
			},
			expected: &specs.Spec{
				Annotations: map[string]string{
					sandboxLogDirAnnotation:                               testLogDirPath,
					ContainerTypeAnnotation:                               containerTypeSandbox,
					volumeKeyPrefix + testNonEmptyVolumeName + ".share":   "shared",
					volumeKeyPrefix + testNonEmptyVolumeName + ".type":    "bind",
					volumeKeyPrefix + testNonEmptyVolumeName + ".options": "ro",
					volumeKeyPrefix + testNonEmptyVolumeName + ".source":  testNonEmptyVolumePath,
				},
			},
		},
		{
			name: "non-empty volume for container",
			spec: &specs.Spec{
				Mounts: []specs.Mount{
					{
						Destination: "/test",
						Type:        "bind",
						Source:      testNonEmptyVolumePath,
						Options:     []string{"ro"},
					},
				},
				Annotations: map[string]string{
					ContainerTypeAnnotation:                               ContainerTypeContainer,
					volumeKeyPrefix + testNonEmptyVolumeName + ".share":   "pod",
					volumeKeyPrefix + testNonEmptyVolumeName + ".type":    "tmpfs",
					volumeKeyPrefix + testNonEmptyVolumeName + ".options": "ro",
				},
			},
		},
		{
			name: "bind: volume annotations for sandbox",
			spec: &specs.Spec{
				Annotations: map[string]string{
					sandboxLogDirAnnotation:                       testLogDirPath,
					ContainerTypeAnnotation:                       containerTypeSandbox,
					volumeKeyPrefix + testVolumeName + ".share":   "container",
					volumeKeyPrefix + testVolumeName + ".type":    "bind",
					volumeKeyPrefix + testVolumeName + ".options": "ro",
				},
			},
			expected: &specs.Spec{
				Annotations: map[string]string{
					sandboxLogDirAnnotation:                       testLogDirPath,
					ContainerTypeAnnotation:                       containerTypeSandbox,
					volumeKeyPrefix + testVolumeName + ".share":   "container",
					volumeKeyPrefix + testVolumeName + ".type":    "tmpfs",
					volumeKeyPrefix + testVolumeName + ".options": "ro",
					volumeKeyPrefix + testVolumeName + ".source":  testVolumePath,
				},
			},
		},
		{
			name: "bind: volume annotations for container",
			spec: &specs.Spec{
				Mounts: []specs.Mount{
					{
						Destination: "/test",
						Type:        "bind",
						Source:      testVolumePath,
						Options:     []string{"ro"},
					},
				},
				Annotations: map[string]string{
					ContainerTypeAnnotation:                       ContainerTypeContainer,
					volumeKeyPrefix + testVolumeName + ".share":   "container",
					volumeKeyPrefix + testVolumeName + ".type":    "bind",
					volumeKeyPrefix + testVolumeName + ".options": "ro",
				},
			},
		},
		{
			name: "should not return error without pod log directory",
			spec: &specs.Spec{
				Annotations: map[string]string{
					ContainerTypeAnnotation:                       containerTypeSandbox,
					volumeKeyPrefix + testVolumeName + ".share":   "pod",
					volumeKeyPrefix + testVolumeName + ".type":    "tmpfs",
					volumeKeyPrefix + testVolumeName + ".options": "ro",
				},
			},
		},
		{
			name: "should return error if volume path does not exist",
			spec: &specs.Spec{
				Annotations: map[string]string{
					sandboxLogDirAnnotation:              testLogDirPath,
					ContainerTypeAnnotation:              containerTypeSandbox,
					volumeKeyPrefix + "notexist.share":   "pod",
					volumeKeyPrefix + "notexist.type":    "tmpfs",
					volumeKeyPrefix + "notexist.options": "ro",
				},
			},
			expectErr: true,
		},
		{
			name: "no volume annotations for sandbox",
			spec: &specs.Spec{
				Annotations: map[string]string{
					sandboxLogDirAnnotation: testLogDirPath,
					ContainerTypeAnnotation: containerTypeSandbox,
				},
			},
		},
		{
			name: "no volume annotations for container",
			spec: &specs.Spec{
				Mounts: []specs.Mount{
					{
						Destination: "/test",
						Type:        "bind",
						Source:      "/test",
						Options:     []string{"ro"},
					},
					{
						Destination: "/random",
						Type:        "bind",
						Source:      "/random",
						Options:     []string{"ro"},
					},
				},
				Annotations: map[string]string{
					ContainerTypeAnnotation: ContainerTypeContainer,
				},
			},
		},
		{
			name: "bind options removed",
			spec: &specs.Spec{
				Annotations: map[string]string{
					ContainerTypeAnnotation:                       ContainerTypeContainer,
					volumeKeyPrefix + testVolumeName + ".share":   "pod",
					volumeKeyPrefix + testVolumeName + ".type":    "tmpfs",
					volumeKeyPrefix + testVolumeName + ".options": "ro",
					volumeKeyPrefix + testVolumeName + ".source":  testVolumePath,
				},
				Mounts: []specs.Mount{
					{
						Destination: "/dst",
						Type:        "bind",
						Source:      testVolumePath,
						Options:     []string{"ro", "bind", "rbind"},
					},
				},
			},
			expected: &specs.Spec{
				Annotations: map[string]string{
					ContainerTypeAnnotation:                       ContainerTypeContainer,
					volumeKeyPrefix + testVolumeName + ".share":   "pod",
					volumeKeyPrefix + testVolumeName + ".type":    "tmpfs",
					volumeKeyPrefix + testVolumeName + ".options": "ro",
					volumeKeyPrefix + testVolumeName + ".source":  testVolumePath,
				},
				Mounts: []specs.Mount{
					{
						Destination: "/dst",
						Type:        "tmpfs",
						Source:      testVolumePath,
						Options:     []string{"ro"},
					},
				},
			},
		},
		{
			name: "shm-sandbox",
			spec: &specs.Spec{
				Annotations: map[string]string{
					sandboxLogDirAnnotation: testLogDirPath,
					ContainerTypeAnnotation: containerTypeSandbox,
				},
				Mounts: []specs.Mount{
					{
						Destination: "/dev/shm",
						Type:        "bind",
						Source:      testVolumePath,
						Options:     []string{"ro", "foo"},
					},
				},
			},
			expected: &specs.Spec{
				Annotations: map[string]string{
					sandboxLogDirAnnotation:                   testLogDirPath,
					ContainerTypeAnnotation:                   containerTypeSandbox,
					volumeKeyPrefix + devshmName + ".share":   "pod",
					volumeKeyPrefix + devshmName + ".type":    "tmpfs",
					volumeKeyPrefix + devshmName + ".options": "rw",
					volumeKeyPrefix + devshmName + ".source":  testVolumePath,
				},
				Mounts: []specs.Mount{
					{
						Destination: "/dev/shm",
						Type:        "tmpfs",
						Source:      testVolumePath,
						Options:     []string{"ro", "foo"},
					},
				},
			},
		},
		{
			name: "shm-container",
			spec: &specs.Spec{
				Annotations: map[string]string{
					ContainerTypeAnnotation: ContainerTypeContainer,
				},
				Mounts: []specs.Mount{
					{
						Destination: "/dev/shm",
						Type:        "bind",
						Source:      testVolumePath,
						Options:     []string{"ro", "foo"},
					},
				},
			},
			expected: &specs.Spec{
				Annotations: map[string]string{
					ContainerTypeAnnotation: ContainerTypeContainer,
				},
				Mounts: []specs.Mount{
					{
						Destination: "/dev/shm",
						Type:        "tmpfs",
						Source:      testVolumePath,
						Options:     []string{"ro", "foo"},
					},
				},
			},
		},
		{
			name: "shm-duplicate",
			spec: &specs.Spec{
				Annotations: map[string]string{
					ContainerTypeAnnotation: ContainerTypeContainer,
				},
				Mounts: []specs.Mount{
					{
						Destination: "/dev/shm",
						Type:        "bind",
						Source:      testVolumePath,
						Options:     []string{"ro", "foo"},
					},
					{
						Destination: "/dev/shm",
						Type:        "tmpfs",
					},
					{
						Destination: "/home",
						Type:        "bind",
						Source:      "/another/mount",
						Options:     []string{"rw"},
					},
				},
			},
			expected: &specs.Spec{
				Annotations: map[string]string{
					ContainerTypeAnnotation: ContainerTypeContainer,
				},
				Mounts: []specs.Mount{
					{
						Destination: "/dev/shm",
						Type:        "tmpfs",
						Source:      testVolumePath,
						Options:     []string{"ro", "foo"},
					},
					{
						Destination: "/home",
						Type:        "bind",
						Source:      "/another/mount",
						Options:     []string{"rw"},
					},
				},
			},
		},
	} {
		t.Run(test.name, func(t *testing.T) {
			expectUpdate := test.expected != nil
			if test.expected == nil && !test.expectErr {
				test.expected = deepcopy.Copy(test.spec).(*specs.Spec)
			}
			updated, err := UpdateVolumeAnnotations(test.spec)
			if test.expectErr {
				if err == nil {
					t.Fatal("UpdateVolumeAnnotations(spec): nil, want: error")
				}
				return
			}
			if err != nil {
				t.Fatalf("UpdateVolumeAnnotations(spec): %v", err)
			}
			if expectUpdate != updated {
				t.Errorf("want: %v, got: %v", expectUpdate, updated)
			}
			if !reflect.DeepEqual(test.expected, test.spec) {
				t.Fatalf("want: %+v, got: %+v", test.expected, test.spec)
			}
		})
	}
}
