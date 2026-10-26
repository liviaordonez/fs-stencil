#define pid_t kernel_pid_t
#include "svfs.h"
#include "vfs.h"
#include "vnode.h"
#include "dirent.h"
#include "slab_allocator.h"
#include "sys/disk/disk.h"
#include "sys/mem/mem.h"
#undef pid_t

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* create a file or directory in the fs */
static int create(const char *dirpath, const char *name, int is_dir, vnode_t **out) {
    vnode_t *dir;
    if (namev_open(vfs_root_fs.fs_root, dirpath, &dir) != 0) {
        return -1;
    }
    vnode_ops_t *ops = dir->vn_ops;
    if (is_dir) {
        return ops->mkdir ? ops->mkdir(dir, name, strlen(name), out) : -1;
    }
    return ops->mknod ? ops->mknod(dir, name, strlen(name), out) : -1;
}

/* write every cached file's data to disk */
static void flush_all(void) {
    for (list_link_t *l = vfs_root_fs.vnode_list.head; l != NULL; l = l->next) {
        mobj_flush(&((vnode_t *) l->parent)->vn_mobj);
    }
}

/* flush, then mount again from the disk image */
static void reboot(void) {
    flush_all();
    svfs_init();
}

static void fill(char *buf, long n, unsigned seed) {
    for (long i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        buf[i] = (char) (seed >> 16);
    }
}

static void test_fs_maker(void) {
    printf("1. fs-maker: raw disk image\n");
    char blk[SV_BLOCK_SIZE];
    svfs_super_t sb;
    read_disk_block(0, blk);
    memcpy(&sb, blk, sizeof(sb));

    assert(svfs_check_super(&sb) == 0 && "superblock is valid (magic, counts, root inode)");

    // free inodes
    long n = 0, ino = sb.svs_free_inode, all_free = 1;
    while (ino != -1 && n <= 32) {
        svfs_inode_t in;
        read_disk_block(1 + ino, blk);
        memcpy(&in, blk, sizeof(in));
        if (in.svi_count != 0 || in.svi_number != ino || (n > 0 && ino != n + 1)) {
            all_free = 0;
        }
        n++;
        ino = in.svi_un.svi_next_free;
    }
    assert(n == 31 && ino == -1 && all_free && "free inodes 1..31 are chained to each other, ending in -1");

    // free data blocks: the superblock segment, then every list block it links to
    char seen[161] = {0};
    long total = 0, nodes = 0, dup = 0, bad = 0, w[1 + SV_NFBLKS_PER_NODE];
    long entries[SV_NFBLKS_PER_NODE];
    long count = sb.svs_nfree;
    memcpy(entries, sb.svs_free_blocks, sizeof(entries));
    while (1) {
        for (long i = 0; i < count; i++) {
            long b = entries[i];
            if (b < 33 || b > 160) {
                bad++;
            } else if (seen[b]) {
                dup++;
            } else {
                seen[b] = 1;
                total++;
            }
        }
        long link = entries[SV_NFBLKS_PER_NODE - 1];
        if (link == 0 || nodes > 10) {
            break;
        }
        nodes++;
        read_disk_block(link, blk);
        memcpy(w, blk, sizeof(w));
        count = w[0];
        memcpy(entries, &w[1], sizeof(entries));
        if (link >= 33 && link <= 160 && !seen[link]) {
            seen[link] = 1;
            total++;
        }
    }
    assert(bad == 0 && dup == 0 && "free list has no out-of-range or repeated blocks");
    assert(nodes >= 1 && "at least one data block is used to track the overflow of the free list");
    assert(total == 127 && !seen[33] && "all 127 non-root data blocks are reachable; the root's block is not free");

    // root directory
    svfs_inode_t root;
    dirent_t d[2];
    read_disk_block(1, blk);
    memcpy(&root, blk, sizeof(root));
    read_disk_block(33, blk);
    memcpy(d, blk, sizeof(d));
    assert(root.svi_count >= 2 && root.svi_direct_blocks[0] == 33 && "root is inode 0, a directory, stored in block 33");
    assert(root.svi_un.svi_size == 2 * sizeof(dirent_t) && !strcmp(d[0].d_name, ".") && !strcmp(d[1].d_name, "..") &&
              d[0].d_ino == 0 && d[1].d_ino == 0 &&
          "root contains '.' and '..', both pointing at itself");
}

static void test_files(void) {
    printf("2. mobj / svfs / namev\n");
    vnode_t *root = vfs_root_fs.fs_root, *a, *b, *f, *v;

    assert(root != NULL && root->vn_mode == DIRECTORY && "root is a directory");
    assert(create("/", "a", 1, &a) == 0 && create("/a", "b", 1, &b) == 0 && "mkdir /a and /a/b");
    assert(create("/a/b", "f", 0, &f) == 0 && "mknod /a/b/f");
    assert(create("/a", "b", 1, &v) != 0 && "creating a name that exists fails");
    assert(create("/a/b/f", "x", 0, &v) != 0 && "cannot create inside a regular file");
    assert(namev_open(root, "/a/b/f", &v) == 0 && v == f && "namev_open finds the same vnode");
    assert(namev_open(b, "../b/./f", &v) == 0 && v == f && "relative path with . and ..");
    assert(namev_open(root, "/a/nope", &v) != 0 && "missing name fails");
    assert(namev_get_parent(b, &v) == 0 && v == a && "get_parent(/a/b) is /a");
    assert(namev_is_descendent(b, root) == 1 && namev_is_descendent(a, b) == 0 && "is_descendent");

    dirent_t *d;
    assert(a->vn_ops->readdir(a, 0, &d) == 0 && !strcmp(d->d_name, ".") && "readdir(/a)[0] is '.'");
    assert(a->vn_ops->readdir(a, sizeof(dirent_t), &d) == 0 && !strcmp(d->d_name, "..") && "readdir(/a)[1] is '..'");
    assert(a->vn_ops->readdir(a, 2 * sizeof(dirent_t), &d) == 0 && !strcmp(d->d_name, "b") && "readdir(/a)[2] is 'b'");
    assert(a->vn_ops->readdir(a, 3 * sizeof(dirent_t), &d) != 0 && "readdir past the end fails");

    static char model[20000], got[20000], tmp[5000];
    fill(model, 20000, 1);
    assert(f->vn_ops->write(f, 0, model, 20000) == 0 && "write 20000 bytes");
    fill(tmp, 5000, 2);
    memcpy(model + 3000, tmp, 5000);
    assert(f->vn_ops->write(f, 3000, tmp, 5000) == 0 && "overwrite 5000 bytes at 3000 (crosses a page)");
    assert(f->vn_len == 20000 && "length is still 20000");
    assert(f->vn_ops->read(f, 0, got, 20000) == 0 && !memcmp(got, model, 20000) && "read back matches");
    assert(f->vn_ops->read(f, 4090, got, 100) == 0 && !memcmp(got, model + 4090, 100) && "read across a page boundary");
    assert(f->vn_ops->read(f, 19990, got, 11) != 0 && "read past the end fails");

    // 200000 bytes = 49 pages. more than the 28 direct blocks, so the indirect block is used
    vnode_t *big;
    long BIG = 200000;
    char *bp = malloc(BIG), *rb = malloc(BIG);
    fill(bp, BIG, 7);
    assert(create("/", "big", 0, &big) == 0 && big->vn_ops->write(big, 0, bp, BIG) == 0 && "write a 200000 byte file");
    assert(((svfs_node_t *) big)->inode.svi_indirect_block != 0 && "it uses the indirect block");
    assert(big->vn_ops->read(big, 0, rb, BIG) == 0 && !memcmp(rb, bp, BIG) && "read all of it back");
    free(bp);
    free(rb);
}

static void test_host_roundtrip(const char *host_in) {
    printf("3. host file -> fs -> reboot -> host file\n");
    const char *host_out = "/tmp/fs_test_out.bin";
    char gen[] = "/tmp/fs_test_in.bin";
    long size;
    char *data;

    if (host_in == NULL) { // make a host file to use
        host_in = gen;
        size = 30000;
        data = malloc(size);
        fill(data, size, 99);
        FILE *g = fopen(host_in, "wb");
        fwrite(data, 1, size, g);
        fclose(g);
        free(data);
    }
    FILE *f = fopen(host_in, "rb");
    if (!f) {
        perror(host_in);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(size);
    fread(data, 1, size, f);
    fclose(f);

    // host -> fs
    vnode_t *v;
    assert(create("/", "hostfile", 0, &v) == 0 && v->vn_ops->write(v, 0, data, size) == 0 && "write the host file into the fs");

    reboot();

    // fs -> host
    assert(namev_open(vfs_root_fs.fs_root, "/hostfile", &v) == 0 && v->vn_len == size && "after reboot, /hostfile exists with the right size");
    char *back = malloc(size);
    assert(v->vn_ops->read(v, 0, back, size) == 0 && "read it from the disk image");
    FILE *o = fopen(host_out, "wb");
    fwrite(back, 1, size, o);
    fclose(o);
    assert(memcmp(back, data, size) == 0 && "contents are byte-identical to the original");

    // earlier files survived the reboot too
    vnode_t *big;
    assert(namev_open(vfs_root_fs.fs_root, "/big", &big) == 0 && big->vn_len == 200000 && "/big (indirect block) survived the reboot");
    assert(namev_open(vfs_root_fs.fs_root, "/a/b/f", &v) == 0 && v->vn_len == 20000 && "/a/b/f survived the reboot");
    printf("        (copy on the host: %s)\n", host_out);
    free(data);
    free(back);
}

static void test_limits(void) {
    printf("4. limits\n");
    vnode_t *v, *fillf;
    assert(create("/", "fill", 0, &fillf) == 0 && "mknod /fill");
    char *z = calloc(1, 4096);
    long pages = 0;
    while (fillf->vn_ops->write(fillf, pages * 4096, z, 4096) == 0) {
        pages++;
    }
    free(z);
    assert(pages > 0 && pages < 128 && "writing until the disk is full eventually fails");

    int made = 0;
    char name[16];
    for (int i = 0; i < 40; i++) {
        sprintf(name, "n%d", i);
        if (create("/", name, 0, &v) != 0) {
            break;
        }
        made++;
    }
    assert(made > 0 && made < 40 && "creating files until the inodes run out eventually fails");
    assert(create("/", "one_more", 0, &v) != 0 && "no free inode: mknod fails");
    reboot();
    assert(vfs_root_fs.fs_root != NULL && "a completely full file system still mounts");
}

int main(int argc, char **argv) {
    if (mem_init() != 0 || disk_init() != 0) {
        fprintf(stderr, "could not initialise memory/disk\n");
        return 2;
    }
    slab_init();
    svfs_init();
    if (vfs_root_fs.fs_root == NULL) {
        fprintf(stderr, "bad superblock -- run `make test` (it builds a fresh image first)\n");
        return 2;
    }

    test_fs_maker();
    test_files();
    test_host_roundtrip(argc > 1 ? argv[1] : NULL);
    test_limits();

    printf("\nAll checks passed successfully.\n");
    return 0;
}