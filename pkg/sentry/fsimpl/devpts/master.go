// Copyright 2020 The gVisor Authors.
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

package devpts

import (
	"gvisor.dev/gvisor/pkg/abi/linux"
	"gvisor.dev/gvisor/pkg/context"
	"gvisor.dev/gvisor/pkg/errors/linuxerr"
	"gvisor.dev/gvisor/pkg/marshal/primitive"
	"gvisor.dev/gvisor/pkg/sentry/arch"
	"gvisor.dev/gvisor/pkg/sentry/fsimpl/kernfs"
	"gvisor.dev/gvisor/pkg/sentry/kernel"
	"gvisor.dev/gvisor/pkg/sentry/kernel/auth"
	"gvisor.dev/gvisor/pkg/sentry/unimpl"
	"gvisor.dev/gvisor/pkg/sentry/vfs"
	"gvisor.dev/gvisor/pkg/usermem"
	"gvisor.dev/gvisor/pkg/waiter"
)

// masterInode is the inode for the master end of the Terminal.
//
// +stateify savable
type masterInode struct {
	implStatFS
	kernfs.InodeAttrs
	kernfs.InodeNoopRefCount
	kernfs.InodeNotAnonymous
	kernfs.InodeNotDirectory
	kernfs.InodeNotSymlink
	kernfs.InodeFSOwned
	kernfs.InodeWatches

	locks vfs.FileLocks

	// root is the devpts root inode.
	root *rootInode
}

var _ kernfs.Inode = (*masterInode)(nil)

// Open implements kernfs.Inode.Open.
func (mi *masterInode) Open(ctx context.Context, rp *vfs.ResolvingPath, d *kernfs.Dentry, opts vfs.OpenOptions) (*vfs.FileDescription, error) {
	t, err := mi.root.allocateTerminal(ctx, rp.Credentials())
	if err != nil {
		return nil, err
	}

	fd := &masterFileDescription{
		inode: mi,
		t:     t,
	}
	fd.LockFD.Init(&mi.locks)
	if err := fd.vfsfd.Init(fd, opts.Flags, rp.Mount(), d.VFSDentry(), &vfs.FileDescriptionOptions{}); err != nil {
		return nil, err
	}
	return &fd.vfsfd, nil
}

// Stat implements kernfs.Inode.Stat.
func (mi *masterInode) Stat(ctx context.Context, vfsfs *vfs.Filesystem, opts vfs.StatOptions) (linux.Statx, error) {
	statx, err := mi.InodeAttrs.Stat(ctx, vfsfs, opts)
	if err != nil {
		return linux.Statx{}, err
	}
	statx.Blksize = 1024
	statx.RdevMajor = linux.TTYAUX_MAJOR
	statx.RdevMinor = linux.PTMX_MINOR
	return statx, nil
}

// SetStat implements kernfs.Inode.SetStat
func (mi *masterInode) SetStat(ctx context.Context, vfsfs *vfs.Filesystem, creds *auth.Credentials, opts vfs.SetStatOptions) error {
	if opts.Stat.Mask&linux.STATX_SIZE != 0 {
		return linuxerr.EINVAL
	}
	return mi.InodeAttrs.SetStat(ctx, vfsfs, creds, opts)
}

// +stateify savable
type masterFileDescription struct {
	vfsfd vfs.FileDescription
	vfs.FileDescriptionDefaultImpl
	vfs.LockFD

	inode *masterInode
	t     *Terminal
}

var _ vfs.FileDescriptionImpl = (*masterFileDescription)(nil)

// Release implements vfs.FileDescriptionImpl.Release.
func (mfd *masterFileDescription) Release(ctx context.Context) {
	mfd.inode.root.masterClose(ctx, mfd.t)
}

// EventRegister implements waiter.Waitable.EventRegister.
func (mfd *masterFileDescription) EventRegister(e *waiter.Entry) error {
	mfd.t.ld.masterWaiter.EventRegister(e)
	return nil
}

// EventUnregister implements waiter.Waitable.EventUnregister.
func (mfd *masterFileDescription) EventUnregister(e *waiter.Entry) {
	mfd.t.ld.masterWaiter.EventUnregister(e)
}

// Readiness implements waiter.Waitable.Readiness.
func (mfd *masterFileDescription) Readiness(mask waiter.EventMask) waiter.EventMask {
	return mfd.t.ld.masterReadiness()
}

// Epollable implements FileDescriptionImpl.Epollable.
func (mfd *masterFileDescription) Epollable() bool {
	return true
}

// Read implements vfs.FileDescriptionImpl.Read.
func (mfd *masterFileDescription) Read(ctx context.Context, dst usermem.IOSequence, _ vfs.ReadOptions) (int64, error) {
	return mfd.t.ld.outputQueueRead(ctx, dst)
}

// Write implements vfs.FileDescriptionImpl.Write.
func (mfd *masterFileDescription) Write(ctx context.Context, src usermem.IOSequence, _ vfs.WriteOptions) (int64, error) {
	return mfd.t.ld.inputQueueWrite(ctx, src)
}

// Ioctl implements vfs.FileDescriptionImpl.Ioctl.
func (mfd *masterFileDescription) Ioctl(ctx context.Context, io usermem.IO, sysno uintptr, args arch.SyscallArguments) (uintptr, error) {
	t := kernel.TaskFromContext(ctx)
	if t == nil {
		// ioctl(2) may only be called from a task goroutine.
		return 0, linuxerr.ENOTTY
	}

	switch cmd := args[1].Uint(); cmd {
	case linux.FIONREAD: // linux.FIONREAD == linux.TIOCINQ
		// Get the number of bytes in the output queue read buffer.
		return 0, mfd.t.ld.outputQueueReadSize(t, io, args)
	case linux.TCGETS:
		// N.B. TCGETS on the master actually returns the configuration
		// of the replica end.
		return mfd.t.ld.getTermios(t, args)
	case linux.TCSETS:
		// N.B. TCSETS on the master actually affects the configuration
		// of the replica end.
		return mfd.t.ld.setTermios(t, args)
	case linux.TCSETSW:
		// Note that this should drain the output queue first, but we
		// don't implement that yet.
		return mfd.t.ld.setTermios(t, args)
	case linux.TCSETSF:
		// This should drain the output queue and clear the input queue
		// first, but we don't implement that yet.
		return mfd.t.ld.setTermios(t, args)
	case linux.TIOCGPTN:
		nP := primitive.Uint32(mfd.t.n)
		_, err := nP.CopyOut(t, args[2].Pointer())
		return 0, err
	case linux.TIOCSPTLCK:
		// For now just pretend we implement pty locking.
		return 0, nil
	case linux.TIOCGWINSZ:
		return 0, mfd.t.ld.windowSize(t, args)
	case linux.TIOCSWINSZ:
		return 0, mfd.t.ld.setWindowSize(t, args)
	case linux.TIOCSCTTY:
		// Make the given terminal the controlling terminal of the
		// calling process.
		steal := args[2].Int() == 1
		return 0, t.ThreadGroup().SetControllingTTY(ctx, mfd.t.masterKTTY, steal, mfd.vfsfd.IsReadable())
	case linux.TIOCNOTTY:
		// Release this process's controlling terminal.
		return 0, t.ThreadGroup().ReleaseControllingTTY(mfd.t.masterKTTY)
	case linux.TIOCGPGRP:
		// Get the foreground process group id.
		pgid, err := t.ThreadGroup().ForegroundProcessGroupID(mfd.t.masterKTTY)
		if err != nil {
			return 0, err
		}
		ret := primitive.Int32(pgid)
		_, err = ret.CopyOut(t, args[2].Pointer())
		return 0, err
	case linux.TIOCSPGRP:
		// Set the foreground process group id.
		var pgid primitive.Int32
		if _, err := pgid.CopyIn(t, args[2].Pointer()); err != nil {
			return 0, err
		}
		return 0, t.ThreadGroup().SetForegroundProcessGroupID(ctx, mfd.t.masterKTTY, kernel.ProcessGroupID(pgid))
	case linux.TIOCPKT:
		// Enable or disable packet mode.
		var mode primitive.Int32
		if _, err := mode.CopyIn(t, args[2].Pointer()); err != nil {
			return 0, err
		}
		mfd.t.ld.setPacketMode(int(mode))
		return 0, nil
	case linux.TIOCGPKT:
		// Get packet mode.
		mode := mfd.t.ld.getPacketMode()
		ret := primitive.Int32(mode)
		_, err := ret.CopyOut(t, args[2].Pointer())
		return 0, err

	default:
		maybeEmitUnimplementedEvent(ctx, sysno, cmd)
		return 0, linuxerr.ENOTTY
	}
}

// SetStat implements vfs.FileDescriptionImpl.SetStat.
func (mfd *masterFileDescription) SetStat(ctx context.Context, opts vfs.SetStatOptions) error {
	creds := auth.CredentialsFromContext(ctx)
	fs := mfd.vfsfd.VirtualDentry().Mount().Filesystem()
	return mfd.inode.SetStat(ctx, fs, creds, opts)
}

// Stat implements vfs.FileDescriptionImpl.Stat.
func (mfd *masterFileDescription) Stat(ctx context.Context, opts vfs.StatOptions) (linux.Statx, error) {
	fs := mfd.vfsfd.VirtualDentry().Mount().Filesystem()
	return mfd.inode.Stat(ctx, fs, opts)
}

// maybeEmitUnimplementedEvent emits unimplemented event if cmd is valid.
func maybeEmitUnimplementedEvent(ctx context.Context, sysno uintptr, cmd uint32) {
	switch cmd {
	case linux.TCGETS,
		linux.TCSETS,
		linux.TCSETSW,
		linux.TCSETSF,
		linux.TIOCGWINSZ,
		linux.TIOCSWINSZ,
		linux.TIOCSETD,
		linux.TIOCSBRK,
		linux.TIOCCBRK,
		linux.TCSBRK,
		linux.TCSBRKP,
		linux.TIOCSTI,
		linux.TIOCCONS,
		linux.FIONBIO,
		linux.TIOCEXCL,
		linux.TIOCNXCL,
		linux.TIOCGEXCL,
		linux.TIOCGSID,
		linux.TIOCGETD,
		linux.TIOCVHANGUP,
		linux.TIOCGDEV,
		linux.TIOCMGET,
		linux.TIOCMSET,
		linux.TIOCMBIC,
		linux.TIOCMBIS,
		linux.TIOCGICOUNT,
		linux.TCFLSH,
		linux.TIOCSSERIAL,
		linux.TIOCGPTPEER:

		unimpl.EmitUnimplementedEvent(ctx, sysno)
	}
}
