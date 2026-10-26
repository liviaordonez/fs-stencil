#!/usr/bin/env python3
import os
import re
import struct
import sys

BLOCK = 4096
NUM_INODES = 32
NUM_DATA = 128
INODE_START = 1
DATA_START = INODE_START + NUM_INODES
TOTAL_BLOCKS = DATA_START + NUM_DATA

NFB = 30
NDIRECT = 28
MAGIC = 134
NO_INODE = -1
NAME_LEN = 24

MODE_FREE, MODE_FILE, MODE_DIR = 0, 1, 2
ROOT_INODE = 0


def longs(*vals):
    return struct.pack("<%dq" % len(vals), *vals)


def pack_super(free_inode, nfree, free_blocks, root, version=1):
    assert len(free_blocks) == NFB
    return longs(MAGIC, free_inode, nfree, *free_blocks, root, NUM_INODES, version)


def pack_inode(un, number, count, direct, indirect, mode):
    assert len(direct) == NDIRECT
    return longs(un, number, count, *direct, indirect, mode)


def pack_dirent(ino, next_off, name):
    raw = name.encode()
    assert len(raw) <= NAME_LEN
    return struct.pack("<qq25s7x", ino, next_off, raw)


DIRENT_SIZE = 48


class Image:
    def __init__(self):
        self.blocks = [bytearray(BLOCK) for _ in range(TOTAL_BLOCKS)]

    def put(self, blockno, data):
        assert len(data) <= BLOCK
        self.blocks[blockno][:len(data)] = data

    def bytes(self):
        return b"".join(bytes(b) for b in self.blocks)


def build():
    img = Image()

    # free list of data blocks
    free = [0] * NFB
    nfree = 0
    list_nodes = []

    def free_block(b):
        nonlocal nfree, free
        if nfree == NFB - 1:
            img.put(b, longs(nfree, *free))
            list_nodes.append(b)
            free = [0] * (NFB - 1) + [b]
            nfree = 0
        else:
            free[nfree] = b
            nfree += 1

    root_data = DATA_START
    # free in descending order so that allocation hands out low blocks first
    for b in range(DATA_START + NUM_DATA - 1, root_data, -1):
        free_block(b)

    # root directory: first inode, first data block, "." and ".."
    root_direct = [root_data] + [0] * (NDIRECT - 1)
    img.put(INODE_START + ROOT_INODE,
            pack_inode(2 * DIRENT_SIZE, ROOT_INODE, 2, root_direct, 0, MODE_DIR))
    img.put(root_data,
            pack_dirent(ROOT_INODE, 1 * DIRENT_SIZE, ".") +
            pack_dirent(ROOT_INODE, 2 * DIRENT_SIZE, ".."))

    # all other inodes are free and form a list
    for i in range(ROOT_INODE + 1, NUM_INODES):
        nxt = i + 1 if i + 1 < NUM_INODES else NO_INODE
        img.put(INODE_START + i, pack_inode(nxt, i, 0, [0] * NDIRECT, 0, MODE_FREE))

    # superblock
    img.put(0, pack_super(ROOT_INODE + 1, nfree, free, ROOT_INODE))
    return img, list_nodes


def disk_path():
    if len(sys.argv) > 1:
        return sys.argv[1]
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "config.cfg")) as f:
        m = re.search(r"diskname\s*:\s*(\S+)", f.read())
    return os.path.join(here, "sys", m.group(1))


if __name__ == "__main__":
    path = disk_path()
    img, nodes = build()
    with open(path, "wb") as f:
        f.write(img.bytes())
    print("wrote %s (%d blocks); free-list nodes in blocks %s" % (path, TOTAL_BLOCKS, nodes))
