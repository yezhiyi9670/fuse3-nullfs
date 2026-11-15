# NullFS

> [!WARNING]
>
> This is created with LLM assistance and is considered experimental. Do not use for mission-critical production purposes.
>
> Files written into the NullFS filesystem is **EXPECTED to be corrupt and to later vanish** without any warning. That's what "Null" means (data is discarded and stored nowhere) and what "NullFS" is expected to do.

A "black hole" filesystem for Linux, built on [FUSE 3](https://github.com/libfuse/libfuse).

It presents a real, writable directory tree in which **no file payload is ever stored**:

* creating files and directories succeeds, at any path and any depth (as long as the
  parent exists, which is a FUSE/VFS constraint — see below);
* `write(2)` succeeds and the bytes are discarded;
* `read(2)` succeeds and returns zeroes, like `/dev/zero`;
* the *logical* file size is remembered, so `ls -l` and `stat` look plausible;
* the volume always reports ~1 PiB free;
* the namespace lives only in memory and disappears when the process exits.

This is the Linux counterpart of [winfsp-nullfs](https://github.com/yezhiyi9670/winfsp-nullfs).
It exists for the case where a piece of software insists on writing large files that will
never be read back.

> [!WARNING]
>
> Files written into nullfs are **expected to be corrupt and to vanish** without warning.
> That is what "null" means: the data is discarded and stored nowhere. If the program
> reads its output back and validates it, it will see zeroes (or, while the page cache
> still holds the pages, the data it just wrote — never trust either).

## Quick start

Dependencies: a C compiler, `make`, and the libfuse 3 development files.

```sh
# Debian / Ubuntu
sudo apt install build-essential libfuse3-dev pkg-config
# Fedora
sudo dnf install gcc make fuse3-devel
# Arch
sudo pacman -S base-devel fuse3
```

Build and mount:

```sh
make
mkdir -p /tmp/sink
./nullfs -f /tmp/sink          # -f = stay in the foreground, Ctrl-C unmounts
```

In another terminal:

```sh
mkdir -p /tmp/sink/a/b/c/d
echo hello > /tmp/sink/a/b/c/d/f.txt
ls -l /tmp/sink/a/b/c/d/f.txt          # 6 bytes, 0 blocks
cat /tmp/sink/a/b/c/d/f.txt            # empty output: zeroes
dd if=/dev/zero of=/tmp/sink/big.bin bs=1M count=4096   # 4 GiB, costs nothing
df -h /tmp/sink                        # ~1 PiB free
rm -rf /tmp/sink/a /tmp/sink/big.bin
```

To unmount:

```sh
fusermount3 -u /tmp/sink       # or: umount /tmp/sink
```

To install system-wide: `sudo make install` (puts `nullfs` in `/usr/local/bin`).

## Options

```
nullfs [options] <mountpoint>

  -B              stateless mode: only directories are remembered; file metadata
                  exists only while a handle is open (see below)
  -n <nodes|0>    evict metadata above this many nodes (default 65536, 0 = never)
  -T <seconds|0>  forget file metadata idle for this long (default 0 = never)
  -h, --help      usage, including the libfuse options
```

Everything else is passed through to libfuse, e.g. `-o allow_other`,
`-o default_permissions`, `-d` (debug), `-f` (foreground), `-s` (single-threaded).

## Two modes

### Metadata mode (default)

The whole tree is remembered, with an optional ceiling:

* when a `create` would push the node count past `-n`, the **oldest closed regular file**
  is dropped and the create proceeds, so the table never grows past `-n` and creating
  files never fails just because the bookkeeping is full;
* only nodes with no open handles are candidates, so an eviction can never pull the rug
  from under an open file — a pinned node is simply skipped;
* if there is no evictable file, the oldest closed **empty directory** goes next, and as a
  last resort the oldest closed directory **together with its whole subtree**;
* the root directory, the parent of the entry being created, and every ancestor of that
  parent are never evicted, because dropping them would break the path being created;
* `-n 0` turns eviction off entirely (unbounded growth, your RAM is the ceiling);
  otherwise `-n` must be at least 2, since the root always occupies one slot;
* `-T <seconds>` additionally expires *file* metadata that has not been created, opened,
  written or truncated for that long (default 0 = never expire). The sweep is lazy: it
  runs from `create`/`mkdir`, at most once every `T/4` seconds (clamped to 1–60 s).

The consequence: an evicted path behaves as though it never existed (`ENOENT`). If the
program recreates it, it comes back as a new, empty file. A generator that writes each
backup once and never revisits it never notices. A program holding a *live* handle keeps
working, because the handle pins the metadata.

### Stateless mode (`-B`)

`-B` removes the metadata almost completely: a file node exists only for the lifetime of
its handle.

|                                        | metadata mode (default) | stateless (`-B`)                    |
| -------------------------------------- | ----------------------- | ----------------------------------- |
| create a file at any path              | yes, remembered         | yes, forgotten when the handle closes |
| name collisions                        | real                    | impossible (the name is free again) |
| file size after close                  | remembered              | forgotten (reopening is `ENOENT`)   |
| directory listing                      | real                    | only directories                    |
| delete / rename                        | real                    | report success, do nothing          |
| memory                                 | bounded by `-n` / `-T`  | O(open handles + directories)       |
| can creation ever fail                 | only if everything is held open | no                          |

**Directories are still remembered** in `-B`. This is not an oversight: FUSE's VFS
resolves every path component before the filesystem is called, so `mkdir -p a/b/c` and
`> a/b/c/f` can only work if `a`, `a/b` and `a/b/c` keep existing. WinFsp can bypass the
lookup entirely (`FspAccessCheckEx` short-circuits when `GetSecurityByName` is not
implemented); FUSE has no such escape hatch, so the closest possible approximation keeps
the directory tree and drops everything else.

The visible difference from the Windows version is therefore: with `-B`, reopening a file
that has been closed gives `ENOENT` (not a 0-byte file), and the directory tree still
grows. If that matters, use metadata mode with a modest `-n`.

## Behaviour and limits

|                                        | |
| -------------------------------------- | --- |
| Directories at arbitrary depth         | yes (kept in memory) |
| `write`, `ftruncate`, preallocation    | succeed; sizes are bookkeeping only |
| Data written                           | discarded; never stored on disk or in memory |
| `read`                                 | zeroes up to the logical size, then EOF |
| Free space reported                    | ~1 PiB, always |
| `unlink` of an open file               | works: the name goes away, the handle keeps working |
| `rename`, `chmod`, `chown`, `utimens`  | work, in memory |
| Symlinks                               | supported |
| Hard links                             | `EPERM` (not emulated) |
| Device nodes, FIFOs, sockets (`mknod`)| `EPERM` (only regular files are accepted) |
| Extended attributes                    | accepted and discarded; `getxattr` returns `ENODATA` |
| `fsync`, `flush`                       | no-ops |
| Persistence                            | none — the whole tree disappears when the process exits |
| RAM per file                           | a couple of hundred bytes plus the name; directories add a hash table |
| Metadata growth                        | bounded by `-n` (default 65536, so on the order of 10 MB) or by `-T` |

Things to be aware of:

* **Reads may return what you just wrote.** Without `-o direct_io`, FUSE keeps a page
  cache, so a read that follows a write of the same file can be served from those pages
  instead of reaching nullfs. This is deliberate: the intended workload never reads back,
  and avoiding `direct_io` lets writes go through the page cache with normal throughput
  and semantics. A read that *does* reach the filesystem always returns zeroes.
* **Do not mount with `-o writeback_cache`.** File sizes are recorded when the write
  reaches the filesystem; with the writeback cache that happens later than the `write(2)`,
  so a `stat` right after a write could report a stale size.
* **Parents must exist.** `mkdir /mnt/x/y/z` fails with `ENOENT` if `/mnt/x/y` does not
  exist — exactly like any other Linux filesystem. Use `mkdir -p`, which creates each
  component in turn and works fine here.
* **Eviction is visible.** With a small `-n`, a path can disappear between two operations
  of the same program. This is inherent to the design (the Windows version has the same
  property) and is why the default is 65536.
* **Unprivileged mounts** work through `fusermount3`; `-o allow_other` additionally
  requires `user_allow_other` in `/etc/fuse.conf`.

## Pointing a defective program at it

Mount nullfs, create the directory the program expects, and bind-mount it over the real
one so the program keeps using the path it already has configured:

```sh
mkdir -p /mnt/sink/backups
sudo mount --bind /mnt/sink/backups /var/lib/whatever/backups
```

Unlike a symlink, a bind mount cannot be resolved away by the program calling
`realpath(3)`, and `umount /var/lib/whatever/backups` later restores the real directory
(whose contents were hidden, not deleted, while the bind was in place).

If the program lets you configure its output directory, skip the bind mount.

## Running it as a service

`/etc/systemd/system/nullfs.service`:

```ini
[Unit]
Description=nullfs black-hole filesystem on /mnt/sink
After=local-fs.target

[Service]
Type=simple
ExecStart=/usr/local/bin/nullfs -f -o allow_other /mnt/sink
ExecStop=/usr/bin/fusermount3 -u /mnt/sink
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Then `sudo systemctl enable --now nullfs`.

## Verifying it

`smoke-test.sh` exercises the headline behaviours against a mounted instance:

```sh
./nullfs -f /mnt/sink &
./smoke-test.sh /mnt/sink
```

## How it is built

The whole filesystem is a single C file (`nullfs.c`, ~1550 lines including comments) using
the libfuse high-level API:

* a tree of `struct node`, with a hash table of children per directory, so path lookup is
  O(depth) and `readdir` is O(children);
* a creation-ordered doubly linked list over all nodes, used by eviction and by `-T`;
* one global mutex around every operation — the payload path is a `memset` and a size
  update, so there is nothing to gain from finer locking;
* handles are `struct node *` in `fi->fh`, and `open_count` is what pins a node against
  eviction; a node whose name was removed while handles were open survives as an orphan
  until the last `release`.

## Licence

GPL-2.0-or-later. Uses the same approach as libfuse's `example/null.c` (Miklos Szeredi)
and the WinFsp `memfs`/nullfs sample.
