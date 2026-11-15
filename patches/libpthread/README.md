# libpthread patch set

`build-libpthread-kernel.py` applies every `patches/libpthread/*.patch` to the
pinned libpthread work tree, in sorted order, before compiling its three kernel
sources. This directory is what it finds.

## Why this directory exists at all

UniXNU ships `patches/libpthread/` with one patch. Module resolution is
first-wins and iterates the device repository before the UniXNU checkout
(`scripts/unixnu/feeds.py`, steps 5 and 6), so a device directory named
`libpthread` **replaces** the upstream one rather than adding to it. That is why
`explicit-kernel-address.patch` is carried here byte for byte: without it the
device directory would silently drop the patch that makes kernel addresses
explicit instead of `NULL`.

The two patches are independent — one touches lines around 900, the other
around 670 — and both apply strictly in this sorted order onto
`libpthread-519.120.4` (`c0e60fb5ecfc54cf379c261d3f9572cc22ded891`).

| Patch | Origin | What it does |
| --- | --- | --- |
| `explicit-kernel-address.patch` | carried verbatim from UniXNU | `user_addr_t` gets `0` rather than `NULL` in the workqueue paths |
| `xnu-8796-no-jit-entitlement.patch` | this port | stops advertising `PTHREAD_FEATURE_JIT_ENTITLED` |

## The JIT entitlement gap

This port builds an **iOS 16.5** kernel, `xnu-8796.101.5`. The pinned libpthread
is `libpthread-519.120.4`, the iOS 17 drop, and it is one API ahead of that
kernel in exactly one place:

```c
if (pthread_kern->proc_get_jit_entitled(p)) { ... }
```

`pthread_callbacks_s` in `xnu-8796/bsd/sys/pthread_shims.h` has no
`proc_get_jit_entitled`. It has `proc_get_pthread_jit_allowlist` and
`proc_get_pthread_jit_allowlist2`, which the very next block uses and which do
resolve. So the only thing missing is the JIT-entitlement flag, and iOS 16.5 has
no such concept to report — the correct behaviour on this train is not to
advertise the bit at all.

That is what the patch does: it removes the report and leaves the allowlist
reporting, which is the part the 8796 kernel actually implements.

The gap is bounded and checked rather than assumed. Scanning every
`pthread_kern->` use in the libpthread kernel sources against the 63 members of
the 8796 shim struct finds **60 used, exactly 1 missing**, this one. If the
pinned libpthread moves, re-run that comparison before trusting the patch.

## When these can go back to UniXNU

`explicit-kernel-address.patch` should be deleted here as soon as UniXNU's
`patches/libpthread/` can be referenced instead of shadowed — either by giving
module resolution a merge mode, or by rebasing the shared patch sets onto
`xnu-8796` and dropping the device-local copies. The same applies to
`patches/xnu/series`, which is device-local for the same reason.

`xnu-8796-no-jit-entitlement.patch` should be dropped once the port targets a
train whose XNU exposes `proc_get_jit_entitled`, or kept and renamed to
`xnu-8796-*` alongside the two prelink patches if the port stays on 8796 and
UniXNU gains per-train libpthread series.
