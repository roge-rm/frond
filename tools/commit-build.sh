#!/bin/bash
# Binaries built once per commit, shared by every worktree and agent.
#   tools/commit-build.sh [path]        print the folder for HEAD, building it first if it's missing
#   tools/commit-build.sh tinydeck      copy that folder to tinydeck once and print the path there
#   tools/commit-build.sh list          the folders on devbox and tinydeck
#   tools/commit-build.sh prune N [--tinydeck] [--dry-run]
#                                       keep the newest N folders (and HEAD's), delete the rest
# The folder is out-<10 hex of the commit> in the main checkout's build/ and holds frond,
# frond-68k and benchmark-replay, plus COMMIT. Only a clean tree is built and shared: with
# uncommitted changes this builds in the normal places and exits 3 without printing a folder.
# Builds go through build/build-slot.sh in the main checkout, one at a time. Two callers for one
# commit take a lock; the second waits and reuses the first one's folder. Folders are built under a
# temporary name and renamed when done. Prune skips folders a process has open.
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

hash=$(git rev-parse --short=10 HEAD)
dir=$store/out-$hash

dirty() { [ -n "$(git status --porcelain)" ]; }

build() {
    if [ -f "$dir/COMMIT" ]; then return; fi
    if dirty; then
        echo "commit-build: the tree has uncommitted changes, so there is no shared build for $hash; building in place" >&2
        "$store/build-slot.sh" heavy build make -j"${BUILD_JOBS:-32}" BUILD_JOBS="${BUILD_JOBS:-32}" out/frond out/frond-68k benchmark-replay >&2
        exit 3
    fi
    mkdir -p "$store"
    exec 8>"$store/.out-$hash.lock"
    flock 8
    if [ -f "$dir/COMMIT" ]; then return; fi
    echo "commit-build: building $hash in $tree" >&2
    "$store/build-slot.sh" heavy build make -j"${BUILD_JOBS:-32}" BUILD_JOBS="${BUILD_JOBS:-32}" out/frond out/frond-68k benchmark-replay >&2
    tmp=$store/.out-$hash.tmp.$$
    rm -rf "$tmp"; mkdir -p "$tmp"
    cp out/frond out/frond-68k build/benchmark-replay "$tmp/"
    git rev-parse HEAD > "$tmp/COMMIT"
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
    if tdeck test -f "$remote/out-$hash/COMMIT"; then
        echo "commit-build: out-$hash is already on tinydeck" >&2
    else
        tdeck "rm -rf $remote/.out-$hash.tmp; mkdir -p $remote/.out-$hash.tmp"
        # COMMIT goes last so a half copy never looks finished.
        rsync -a -e "${rsh[*]}" --exclude COMMIT "$dir/" "$host:$remote/.out-$hash.tmp/"
        rsync -a -e "${rsh[*]}" "$dir/COMMIT" "$host:$remote/.out-$hash.tmp/"
        tdeck "rm -rf $remote/out-$hash; mv $remote/.out-$hash.tmp $remote/out-$hash"
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
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
    ;;
esac
