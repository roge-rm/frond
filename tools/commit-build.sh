#!/bin/bash
# Binaries built once per commit, shared by every worktree and agent.
#   tools/commit-build.sh [path]        print the folder for HEAD, building it first if it's missing
#   tools/commit-build.sh tinydeck      copy that folder to tinydeck once and print the path there
#   tools/commit-build.sh list          the folders on devbox and tinydeck
#   tools/commit-build.sh prune N [--tinydeck] [--dry-run]
#                                       keep the newest N folders (and HEAD's), delete the rest and
#                                       any temporary folders left by a stopped build or copy
# The folder is out-<10 hex of the commit> in the main checkout's build/ and holds frond,
# frond-68k and benchmark-replay, plus COMMIT. Only a clean tree is built and shared: with
# uncommitted changes this builds in the normal places and exits 3 without printing a folder.
# The shared build is made in a fresh detached worktree of the commit
# (~/.worktrees/frond/commit-build-<hash>, removed after), so nothing built earlier with other
# make variables or files ends up in it. Builds go through build/build-slot.sh in the main
# checkout, one at a time. Two callers for one commit take a lock; the second waits and reuses
# the first one's folder. Folders are built and copied under temporary names and renamed when
# done. Prune skips folders a process has open.
# On tinydeck the folders are in /var/lib/lab/frond-speed/out-<hash>/. SPEED_HOST and
# SPEED_SSH_KEY (tools/speed-check.py) override the host and key.
set -eu
cd "$(dirname "$0")/.."
tree=$PWD
main=$(dirname "$(readlink -f "$(git rev-parse --git-common-dir)")")
store=$main/build
host=${SPEED_HOST:-lab@tinydeck}
key=${SPEED_SSH_KEY:-$HOME/.ssh/tinydeck-lab}
remote=/var/lib/lab/frond-speed
rsh=(ssh -i "$key" -o IdentitiesOnly=yes -o BatchMode=yes)
tdeck() { "${rsh[@]}" "$host" "$@"; }

full=$(git rev-parse HEAD)
hash=${full:0:10}
dir=$store/out-$hash

dirty() { [ -n "$(git -C "${1:-.}" status --porcelain)" ]; }
binaries="frond frond-68k benchmark-replay"
complete() { [ -f "$1/COMMIT" ] && for b in $binaries; do [ -x "$1/$b" ] || return 1; done; }

build() {
    if complete "$dir"; then return; fi
    if dirty; then
        echo "commit-build: the tree has uncommitted changes, so there is no shared build for $hash; building in place" >&2
        "$store/build-slot.sh" heavy build make -j"${BUILD_JOBS:-32}" BUILD_JOBS="${BUILD_JOBS:-32}" out/frond out/frond-68k benchmark-replay >&2
        exit 3
    fi
    mkdir -p "$store"
    exec 8>"$store/.out-$hash.lock"
    flock 8
    if complete "$dir"; then return; fi
    src=$HOME/.worktrees/frond/commit-build-$hash
    tmp=$store/.out-$hash.tmp.$$
    scratch=/mnt/scratch/frond/core-commit-build-$hash
    trap 'rm -rf "$tmp" "$scratch"; git worktree remove --force "$src" 2>/dev/null; rm -rf "$src"' EXIT
    git worktree remove --force "$src" 2>/dev/null || rm -rf "$src"
    git worktree prune
    git worktree add -q --detach "$src" "$full"
    # Its build/out and out/ go on the scratch SSD when it's there, deleted with the worktree.
    if [ -d /mnt/scratch/frond ]; then
        rm -rf "$scratch"; mkdir -p "$scratch/obj" "$scratch/out" "$src/build"
        ln -s "$scratch/obj" "$src/build/out"
        ln -s "$scratch/out" "$src/out"
    fi
    echo "commit-build: building $hash in $src" >&2
    (cd "$src" && "$store/build-slot.sh" heavy build make -j"${BUILD_JOBS:-32}" BUILD_JOBS="${BUILD_JOBS:-32}" out/frond out/frond-68k benchmark-replay) >&2
    if dirty "$src" || [ "$(git -C "$src" rev-parse HEAD)" != "$full" ]; then
        echo "commit-build: the build changed tracked files in $src, not sharing it" >&2
        git -C "$src" status --short >&2
        exit 1
    fi
    rm -rf "$tmp"; mkdir -p "$tmp"
    cp "$src/out/frond" "$src/out/frond-68k" "$src/build/out/benchmark-replay" "$tmp/"
    echo "$full" > "$tmp/COMMIT"
    rm -rf "$dir"
    mv "$tmp" "$dir"
}

check_tinydeck() {
    local src
    src=$(tdeck findmnt -no SOURCE /var/lib/lab) || { echo "commit-build: cannot reach tinydeck" >&2; exit 1; }
    [ "$src" = /dev/sdb1 ] || { echo "commit-build: /var/lib/lab on tinydeck is $src, not the SSD" >&2; exit 1; }
}

case "${1:-path}" in
path)
    build
    echo "$dir"
    ;;
tinydeck)
    build
    check_tinydeck
    # One copy per hash at a time from here; the remote temporary name is unique to this call.
    exec 7>"$store/.out-$hash.tinydeck.lock"
    flock 7
    have="test -f $remote/out-$hash/COMMIT"
    for b in $binaries; do have="$have && test -x $remote/out-$hash/$b"; done
    if tdeck "$have"; then
        echo "commit-build: out-$hash is already on tinydeck" >&2
    else
        rtmp=$remote/.out-$hash.tmp.$(hostname -s).$$
        tdeck "rm -rf $rtmp; mkdir -p $rtmp"
        # COMMIT goes last so a half copy never looks finished.
        rsync -a -e "${rsh[*]}" --exclude COMMIT "$dir/" "$host:$rtmp/"
        rsync -a -e "${rsh[*]}" "$dir/COMMIT" "$host:$rtmp/"
        tdeck "rm -rf $remote/out-$hash && mv $rtmp $remote/out-$hash"
        tdeck "$have" || { echo "commit-build: out-$hash on tinydeck is incomplete after the copy" >&2; exit 1; }
    fi
    echo "$remote/out-$hash"
    ;;
list)
    echo "devbox ($store), newest first:"
    for d in $(ls -1dt "$store"/out-[0-9a-f]* 2>/dev/null); do
        echo "  $(basename "$d")  $(du -sh "$d" | cut -f1)  $(date -r "$d" +%F\ %R)"
    done
    echo "tinydeck ($remote), newest first:"
    tdeck "cd $remote && ls -1dt out-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f] 2>/dev/null | while read d; do echo \"  \$d  \$(du -sh \$d | cut -f1)  \$(date -r \$d +'%F %R')\"; done" || true
    ;;
prune)
    keep=${2:?usage: commit-build.sh prune N [--tinydeck] [--dry-run]}
    shift 2
    on_tdeck=0; dry=0
    for a in "$@"; do
        case $a in --tinydeck) on_tdeck=1 ;; --dry-run) dry=1 ;; *) echo "unknown option $a" >&2; exit 2 ;; esac
    done
    n=0
    # Temporary folders from a stopped build or copy, an hour old and not open.
    if [ $on_tdeck = 0 ]; then
        for d in $(find "$store" -maxdepth 1 -name '.out-*.tmp.*' -mmin +60 2>/dev/null) \
                $(find "$HOME/.worktrees/frond" -maxdepth 1 -name 'commit-build-*' -mmin +60 2>/dev/null) \
                $(find /mnt/scratch/frond -maxdepth 1 -name 'core-commit-build-*' -mmin +60 2>/dev/null); do
            if lsof +D "$d" >/dev/null 2>&1; then echo "in use, kept: $d"; continue; fi
            # A build still running holds its lock.
            lk="$store/.out-${d##*commit-build-}.lock"
            if [ -e "$lk" ] && ! flock -n "$lk" true 2>/dev/null; then echo "build running, kept: $d"; continue; fi
            echo "remove $d"
            [ $dry = 1 ] || { git worktree remove --force "$d" 2>/dev/null || rm -rf "$d"; }
        done
        [ $dry = 1 ] || git worktree prune
    else
        check_tinydeck
        tdeck "cd $remote && for d in \$(find . -maxdepth 1 -name '.out-*.tmp*' -mmin +60); do
            if lsof +D \$d >/dev/null 2>&1; then echo \"in use, kept: \$d\"; continue; fi
            echo \"remove $host:$remote/\${d#./}\"
            [ $dry = 1 ] || rm -rf \$d
        done"
    fi
    if [ $on_tdeck = 0 ]; then
        for d in $(ls -1dt "$store"/out-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f] 2>/dev/null); do
            n=$((n + 1))
            [ $n -le "$keep" ] && continue
            [ "$(basename "$d")" = "out-$hash" ] && continue
            if lsof +D "$d" >/dev/null 2>&1; then echo "in use, kept: $d"; continue; fi
            echo "remove $d"
            [ $dry = 1 ] || rm -rf "$d"
        done
    else
        check_tinydeck
        tdeck "cd $remote && n=0 && for d in \$(ls -1dt out-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f] 2>/dev/null); do
            n=\$((n + 1)); [ \$n -le $keep ] && continue
            [ \$d = out-$hash ] && continue
            if lsof +D \$d >/dev/null 2>&1; then echo \"in use, kept: \$d\"; continue; fi
            echo \"remove $host:$remote/\$d\"
            [ $dry = 1 ] || rm -rf \$d
        done"
    fi
    ;;
*)
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
    ;;
esac
