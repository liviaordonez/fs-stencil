#include "svfs.h"

#include "dirent.h"

fs_ops_t svfs_fsops = { .fs_init = svfs_init,
                        .read_vnode = svfs_read_vnode};

static vnode_ops_t svfs_dir_vops = {.read = NULL,
                                    .write = NULL,
                                    .mknod = svfs_mknod,
                                    .mkdir = svfs_mkdir,
                                    .lookup = svfs_lookup,
                                    .readdir = svfs_readdir,
                                    .read_pframe = svfs_read_pframe,
                                    .write_pframe = svfs_write_pframe,
                                    .flush_pframe = NULL};

static vnode_ops_t svfs_file_vops = {.read = svfs_read,
                                     .write = svfs_write,
                                     .mknod = NULL,
                                     .mkdir = NULL,
                                     .lookup = NULL,
                                     .readdir = NULL,
                                     .read_pframe = svfs_read_pframe,
                                     .write_pframe = svfs_write_pframe,
                                     .flush_pframe = NULL};

static svfs_t svfs_g;
static slab_allocator_t svfs_node_allocator;

/*
 * TODO: implement me!
 *
 * Hints:
 *   - what is the main file system reference? how should our svfs instance
 *     refer to this main system?
 *   - it is your job here to implement both the fields of the svfs_t struct
 *     and the fs_t struct!
 */
void svfs_init() {
    fs_t *fs = &vfs_root_fs;
    spinlock_init(&svfs_g.svfs_mutex);
    svfs_g.svfs_fs = fs;

    fs->fs_ops = &svfs_fsops;
    fs->fs_root = NULL;
    slab_allocator_init(&svfs_node_allocator, sizeof(svfs_node_t));
    fs->vnode_allocator = &svfs_node_allocator;
    list_init(&fs->vnode_list);
    spinlock_init(&fs->vnode_list_mutex);

    char blk[SV_BLOCK_SIZE];
    read_disk_block(SV_SUPER_BLOCK, blk);
    memcpy(&svfs_g.svfs_super, blk, sizeof(svfs_super_t));
    if (svfs_check_super(&svfs_g.svfs_super) != 0) {
        return;
    }

    svfs_node_t *root = (svfs_node_t *) slab_obj_alloc(*fs->vnode_allocator);
    for (long i = 0; i < (long) sizeof(svfs_node_t); i++) {
        ((char *) root)[i] = 0;
    }
    root->inode.svi_number = svfs_g.svfs_super.svs_root_inode;
    fs->fs_ops->read_vnode(fs, &root->vnode);
    if (root->vnode.vn_ops == NULL) {
        return;
    }
    list_insert_back(&fs->vnode_list, &root->vnode.vn_link);
    fs->fs_root = &root->vnode;
}

/*
 * Initialize a svfs_node/vnode after reading a file from disk.
 *
 * TODO: implement me!
 *
 * Hints:
 *   - how do we get data from disk?
 *   - what should the svfs_node/vnode fields be for a given file?
 *   - think about the distinction between normal file and directory!
 */
void svfs_read_vnode(fs_t *fs, vnode_t *vn) {
    // vnode is the first member
    svfs_node_t *node = (svfs_node_t *) vn;
    long ino = node->inode.svi_number;

    vn->vn_ops = NULL;
    if (ino < 0 || ino >= SV_NUM_INODES) {
        return;
    }

    char blk[SV_BLOCK_SIZE];
    svfs_inode_t in;
    read_disk_block(SV_INODE_START_BLOCK + ino, blk);
    memcpy(&in, blk, sizeof(svfs_inode_t));
    if (in.svi_mode == SV_MODE_FREE || in.svi_number != ino) {
        return;
    }

    node->inode = in;
    node->dirtied_inode = 0;

    int is_dir = (in.svi_mode == SV_MODE_DIR);
    vn->vn_fs = fs;
    vn->vn_len = in.svi_un.svi_size;
    vn->vn_mode = is_dir ? DIRECTORY : REGULAR;
    vn->parent = node;
    list_link_init(&vn->vn_link, vn);

    // the mobj's ops are the generic vnode ones
    mobj_ops_t mops = {.read_pframe = vnode_read_pframe,
                       .write_pframe = vnode_write_pframe,
                       .flush_pframe = vnode_flush_pframe};
    mobj_create(&vn->vn_mobj, &mops);
    vn->vn_mobj.parent = vn;

    // set last
    vn->vn_ops = is_dir ? &svfs_dir_vops : &svfs_file_vops;
}


/*
 * Check the super block. Return 0 on success, else -1
 *
 * TODO: implement me!
 */
long svfs_check_super(svfs_super_t *super) {
    if (super == NULL || super->svs_magic != SV_MAGIC_NO) {
        return -1;
    }
    if (super->svs_num_inodes != SV_NUM_INODES) {
        return -1;
    }
    if (super->svs_root_inode < 0 || super->svs_root_inode >= SV_NUM_INODES) {
        return -1;
    }
    if (super->svs_free_inode != SV_NO_INODE &&
        (super->svs_free_inode < 0 || super->svs_free_inode >= SV_NUM_INODES)) {
        return -1;
    }
    if (super->svs_nfree < 0 || super->svs_nfree > SV_NFBLKS_PER_NODE - 1) {
        return -1;
    }
    for (long i = 0; i < super->svs_nfree; i++) {
        long b = super->svs_free_blocks[i];
        if (b < SV_DATA_START_BLOCK || b >= SV_DATA_START_BLOCK + SV_NUM_DATA_BLOCKS) {
            return -1;
        }
    }
    long link = super->svs_free_blocks[SV_NFBLKS_PER_NODE - 1];
    if (link != 0 && (link < SV_DATA_START_BLOCK ||
                      link >= SV_DATA_START_BLOCK + SV_NUM_DATA_BLOCKS)) {
        return -1;
    }
    return 0;
}

/*
 * On file creation, this is called to attach an inode to the newly created
 * file. Choose an unused inode from the file system. Returns 0 on succes,
 * -1 on failure.
 *
 * TODO: implement me!
 *
 * Hints:
 *   - what is the next available inode?
 *   - where do we find the next available inode after we allocate this inode?
 */
long svfs_alloc_inode(svfs_t *svfs) {
    svfs_super_t *sb = &svfs->svfs_super;
    char blk[SV_BLOCK_SIZE];
    svfs_inode_t in;

    spinlock_lock(&svfs->svfs_mutex);
    long ino = sb->svs_free_inode;
    if (ino < 0 || ino >= SV_NUM_INODES) { // SV_NO_INODE: nothing left
        spinlock_unlock(&svfs->svfs_mutex);
        return -1;
    }

    read_disk_block(SV_INODE_START_BLOCK + ino, blk);
    memcpy(&in, blk, sizeof(svfs_inode_t));
    if (in.svi_mode != SV_MODE_FREE) {
        spinlock_unlock(&svfs->svfs_mutex);
        return -1;
    }
    sb->svs_free_inode = in.svi_un.svi_next_free; // next free inode is the new head

    for (long i = 0; i < SV_BLOCK_SIZE; i++) {
        blk[i] = 0;
    }
    memcpy(blk, sb, sizeof(svfs_super_t));
    write_disk_block(SV_SUPER_BLOCK, blk);
    spinlock_unlock(&svfs->svfs_mutex);
    return ino;
}

/*
 * Find a directory entry from a svfs_node with the input name/namelen and
 * pass the associated svfs_node/vnode to the ent pointer. Return 0 on success,
 * else -1.
 *
 * TODO: implement me!
 *
 * Hints:
 *   - how should we read a directory entry?
 */
long svfs_find_dirent(svfs_node_t *dir, const char *name, long namelen,
                      svfs_node_t *ent) {
    if (dir == NULL || ent == NULL || name == NULL ||
        dir->vnode.vn_mode != DIRECTORY || namelen <= 0 || namelen > NAME_LEN) {
        return -1;
    }
    long nents = dir->vnode.vn_len / SV_DIRENT_SIZE;
    for (long i = 0; i < nents; i++) {
        dirent_t d;
        if (svfs_read(&dir->vnode, i * SV_DIRENT_SIZE, &d, SV_DIRENT_SIZE) != 0) {
            return -1;
        }

        long same = 1;
        for (long j = 0; j < namelen; j++) {
            if (d.d_name[j] != name[j]) {
                same = 0;
                break;
            }
        }
        if (same && d.d_name[namelen] == '\0') {
            ent->inode.svi_number = d.d_ino;
            return 0;
        }
    }
    return -1;
}

/*
 * Read len bytes from the file associated with the vnode_t from position pos
 * and save the result into the buffer. Return 0 on success, -1 on failure.
 *
 * TODO: implement me!
 *
 * Hints:
 *   - where is the data?
 *   - how do we access data from a svfs_node/vnode?
 */
long svfs_read(vnode_t *vn, long pos, char *buf, long len) {
    if (vn == NULL || pos < 0 || len < 0 || (len > 0 && buf == NULL)) {
        return -1;
    }
    if (pos + len > vn->vn_len) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    for (long p = pos / SV_BLOCK_SIZE; p <= (pos + len - 1) / SV_BLOCK_SIZE; p++) {
        pframe_t *pf;
        mobj_get_pframe(&vn->vn_mobj, p, &pf);
        if (pf == NULL) {
            return -1;
        }
    }
    return mobj_read(&vn->vn_mobj, pos, len, (char *) buf) == len ? 0 : -1;
}

/*
 * Write len bytes from the buffer into the file at position pos. If the write
 * requires more bytes than are currently available, svfs should allocate
 * another data block to the file. Returns 0 on success, else -1.
 *
 * TODO: implement me!
 *
 * Hints:
 *   - where do new data blocks come from?
 *   - where should we put the data relative to the vnode?
 */
long svfs_write(vnode_t *vn, long pos, const char *buf, long len) {
    if (vn == NULL || pos < 0 || len < 0 || (len > 0 && buf == NULL)) {
        return -1;
    }
    if (pos + len > SV_MAX_FILE_SIZE) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    svfs_node_t *node = (svfs_node_t *) vn;
    svfs_t *svfs = &svfs_g;
    svfs_super_t *sb = &svfs->svfs_super;
    long first = pos / SV_BLOCK_SIZE;
    long last = (pos + len - 1) / SV_BLOCK_SIZE;
    long ret = -1, super_dirty = 0, failed = 0, bno;
    char blk[SV_BLOCK_SIZE];
    long ind[SV_NINDIRECT_BLKS];
    svfs_free_node_t fnode;

    do {
        for (long p = first; p <= last; p++) {
            if (p >= SV_NDIRECT_BLKS && node->inode.svi_indirect_block == 0) {
                // allocate the indirect block
                spinlock_lock(&svfs->svfs_mutex);
                if (sb->svs_nfree > 0) {
                    bno = sb->svs_free_blocks[--sb->svs_nfree];
                } else {
                    bno = sb->svs_free_blocks[SV_NFBLKS_PER_NODE - 1];
                    if (bno >= SV_DATA_START_BLOCK &&
                        bno < SV_DATA_START_BLOCK + SV_NUM_DATA_BLOCKS) {
                        read_disk_block(bno, blk);
                        memcpy(&fnode, blk, sizeof(svfs_free_node_t));
                        sb->svs_nfree = fnode.svf_nfree;
                        memcpy(sb->svs_free_blocks, fnode.svf_free, sizeof(fnode.svf_free));
                    } else {
                        bno = -1; // end of list, disk full
                    }
                }
                spinlock_unlock(&svfs->svfs_mutex);
                if (bno < SV_DATA_START_BLOCK ||
                    bno >= SV_DATA_START_BLOCK + SV_NUM_DATA_BLOCKS) {
                    failed = 1;
                    break;
                }
                for (long i = 0; i < SV_BLOCK_SIZE; i++) {
                    blk[i] = 0;
                }
                write_disk_block(bno, blk);
                super_dirty = 1;
                node->inode.svi_indirect_block = bno;
                node->dirtied_inode = 1;
            }

            long *slot; // where block number for page p is recorded
            if (p < SV_NDIRECT_BLKS) {
                slot = &node->inode.svi_direct_blocks[p];
            } else {
                read_disk_block(node->inode.svi_indirect_block, (char *) ind);
                slot = &ind[p - SV_NDIRECT_BLKS];
            }
            if (*slot != 0) {
                continue; // already has a block
            }

            // allocate data block for page p
            spinlock_lock(&svfs->svfs_mutex);
            if (sb->svs_nfree > 0) {
                bno = sb->svs_free_blocks[--sb->svs_nfree];
            } else {
                bno = sb->svs_free_blocks[SV_NFBLKS_PER_NODE - 1];
                if (bno >= SV_DATA_START_BLOCK &&
                    bno < SV_DATA_START_BLOCK + SV_NUM_DATA_BLOCKS) {
                    read_disk_block(bno, blk);
                    memcpy(&fnode, blk, sizeof(svfs_free_node_t));
                    sb->svs_nfree = fnode.svf_nfree;
                    memcpy(sb->svs_free_blocks, fnode.svf_free, sizeof(fnode.svf_free));
                } else {
                    bno = -1; // end of list, disk full
                }
            }
            spinlock_unlock(&svfs->svfs_mutex);
            if (bno < SV_DATA_START_BLOCK ||
                bno >= SV_DATA_START_BLOCK + SV_NUM_DATA_BLOCKS) {
                failed = 1;
                break;
            }
            for (long i = 0; i < SV_BLOCK_SIZE; i++) {
                blk[i] = 0;
            }
            write_disk_block(bno, blk);
            super_dirty = 1;

            *slot = bno;
            if (p < SV_NDIRECT_BLKS) {
                node->dirtied_inode = 1;
            } else {
                write_disk_block(node->inode.svi_indirect_block, (char *) ind);
            }
        }
        if (failed) {
            break;
        }

        // load the pages, copy in
        for (long p = first; p <= last; p++) {
            pframe_t *pf;
            mobj_get_pframe(&vn->vn_mobj, p, &pf);
            if (pf == NULL) {
                failed = 1;
                break;
            }
        }
        if (failed || mobj_write(&vn->vn_mobj, pos, len, (const char *) buf) != len) {
            break;
        }

        // grow the file
        if (pos + len > vn->vn_len) {
            vn->vn_len = pos + len;
            node->inode.svi_un.svi_size = vn->vn_len;
            node->dirtied_inode = 1;
        }
        ret = 0;
    } while (0);

    if (node->dirtied_inode) {
        for (long i = 0; i < SV_BLOCK_SIZE; i++) {
            blk[i] = 0;
        }
        memcpy(blk, &node->inode, sizeof(svfs_inode_t));
        write_disk_block(SV_INODE_START_BLOCK + node->inode.svi_number, blk);
        node->dirtied_inode = 0;
    }
    if (super_dirty) {
        for (long i = 0; i < SV_BLOCK_SIZE; i++) {
            blk[i] = 0;
        }
        memcpy(blk, sb, sizeof(svfs_super_t));
        write_disk_block(SV_SUPER_BLOCK, blk);
    }
    return ret;
}

/*
 * Creates a new entry for a file (e.g., a call to "touch") for a file that
 * does not yet exist. Creates the file in the provided directory with the
 * the provided name and passes the newly created vnode to the out parameter.
 * Returns 0 on success, -1 on failure.
 *
 * TODO: implement me!
 *
 * Hints:
 *   - don't forget to add the newly created vnode to any global tracking
 *     mechanisms!
 */
long svfs_mknod(vnode_t *dir, const char *name, long namelen, vnode_t **out) {
    if (dir == NULL || out == NULL || name == NULL ||
        dir->vn_mode != DIRECTORY || namelen <= 0 || namelen > NAME_LEN) {
        return -1;
    }
    for (long i = 0; i < namelen; i++) {
        if (name[i] == '\0' || name[i] == '/') {
            return -1;
        }
    }

    svfs_node_t *dnode = (svfs_node_t *) dir;
    fs_t *fs = svfs_g.svfs_fs;

    svfs_node_t probe;
    probe.inode.svi_number = -1;
    if (svfs_find_dirent(dnode, name, namelen, &probe) == 0) {
        return -1; // already exists
    }

    long ino = svfs_alloc_inode(&svfs_g);
    if (ino < 0) {
        return -1;
    }
    long next_free = svfs_g.svfs_super.svs_free_inode;

    // write new inode to disk, then load vnode from it
    char blk[SV_BLOCK_SIZE];
    for (long i = 0; i < SV_BLOCK_SIZE; i++) {
        blk[i] = 0;
    }
    svfs_inode_t in;
    for (long i = 0; i < (long) sizeof(in); i++) {
        ((char *) &in)[i] = 0;
    }
    in.svi_un.svi_size = 0;
    in.svi_number = ino;
    in.svi_count = 1;
    in.svi_mode = SV_MODE_FILE;
    memcpy(blk, &in, sizeof(in));
    write_disk_block(SV_INODE_START_BLOCK + ino, blk);

    svfs_node_t *node = (svfs_node_t *) slab_obj_alloc(*fs->vnode_allocator);
    for (long i = 0; i < (long) sizeof(svfs_node_t); i++) {
        ((char *) node)[i] = 0;
    }
    node->inode.svi_number = ino;
    fs->fs_ops->read_vnode(fs, &node->vnode);

    // add the entry to the directory
    dirent_t d;
    for (long i = 0; i < (long) sizeof(d); i++) {
        ((char *) &d)[i] = 0;
    }
    d.d_ino = ino;
    d.d_off = dir->vn_len + SV_DIRENT_SIZE;
    for (long i = 0; i < namelen; i++) {
        d.d_name[i] = name[i];
    }

    if (node->vnode.vn_ops == NULL ||
        svfs_write(dir, dir->vn_len, &d, SV_DIRENT_SIZE) != 0) {
        // mark the inode free again and make it the list head
        for (long i = 0; i < SV_BLOCK_SIZE; i++) {
            blk[i] = 0;
        }
        in.svi_un.svi_next_free = next_free;
        in.svi_count = 0;
        in.svi_mode = SV_MODE_FREE;
        memcpy(blk, &in, sizeof(in));
        write_disk_block(SV_INODE_START_BLOCK + ino, blk);

        svfs_g.svfs_super.svs_free_inode = ino;
        for (long i = 0; i < SV_BLOCK_SIZE; i++) {
            blk[i] = 0;
        }
        memcpy(blk, &svfs_g.svfs_super, sizeof(svfs_super_t));
        write_disk_block(SV_SUPER_BLOCK, blk);
        return -1;
    }

    spinlock_lock(&fs->vnode_list_mutex);
    list_insert_back(&fs->vnode_list, &node->vnode.vn_link);
    spinlock_unlock(&fs->vnode_list_mutex);

    *out = &node->vnode;
    return 0;
}

/*
 * Creates a new entry for a directory for a file that does not yet exist.
 * Creates the file in the provided directory with theprovided name and passes
 * the newly created vnode to the out parameter.  Returns 0 on success, -1 on
 * failure.
 *
 * TODO: implement me!
 *
 * Hints:
 *   - don't forget to add the newly created vnode to any global tracking
 *     mechanisms!
 */
long svfs_mkdir(vnode_t *dir, const char *name, long namelen, vnode_t **out) {
    if (svfs_mknod(dir, name, namelen, out) != 0) {
        return -1;
    }
    svfs_node_t *node = (svfs_node_t *) *out;
    svfs_node_t *parent = (svfs_node_t *) dir;

    node->inode.svi_mode = SV_MODE_DIR;
    node->inode.svi_count = 2;
    node->dirtied_inode = 1;
    node->vnode.vn_mode = DIRECTORY;
    node->vnode.vn_ops = &svfs_dir_vops;

    const char *names[2] = {".", ".."};
    long targets[2] = {node->inode.svi_number, parent->inode.svi_number};
    for (int k = 0; k < 2; k++) {
        dirent_t d;
        for (long i = 0; i < (long) sizeof(d); i++) {
            ((char *) &d)[i] = 0;
        }
        d.d_ino = targets[k];
        d.d_off = node->vnode.vn_len + SV_DIRENT_SIZE;
        for (long i = 0; names[k][i] != '\0'; i++) {
            d.d_name[i] = names[k][i];
        }
        // svfs_write also writes the node's updated inode to disk
        if (svfs_write(&node->vnode, node->vnode.vn_len, &d, SV_DIRENT_SIZE) != 0) {
            return -1;
        }
    }

    parent->inode.svi_count++;
    char blk[SV_BLOCK_SIZE];
    for (long i = 0; i < SV_BLOCK_SIZE; i++) {
        blk[i] = 0;
    }
    memcpy(blk, &parent->inode, sizeof(svfs_inode_t));
    write_disk_block(SV_INODE_START_BLOCK + parent->inode.svi_number, blk);
    return 0;
}

/*
 * Search for a given entry within a directory. If found, entry is passed back
 * via the out pointer. Returns 0 on success, -1 on failure.
 *
 * TODO: implement me!
 *
 * Hints:
 *   - recall that the data in a directory is a series of dirents (defined in
 *     dirent.h)
 *   - what happens if we can't find the directory?
 */
long svfs_lookup(vnode_t *dir, const char *name, long namelen, vnode_t **out) {
    if (dir == NULL || out == NULL || dir->vn_mode != DIRECTORY) {
        return -1;
    }

    svfs_node_t probe;
    probe.inode.svi_number = -1;
    if (svfs_find_dirent((svfs_node_t *) dir, name, namelen, &probe) != 0) {
        return -1;
    }
    long ino = probe.inode.svi_number;
    fs_t *fs = svfs_g.svfs_fs;

    for (list_link_t *l = fs->vnode_list.head; l != NULL; l = l->next) {
        svfs_node_t *n = (svfs_node_t *) l->parent;
        if (n->inode.svi_number == ino) {
            *out = &n->vnode;
            return 0;
        }
    }

    svfs_node_t *node = (svfs_node_t *) slab_obj_alloc(*fs->vnode_allocator);
    for (long i = 0; i < (long) sizeof(svfs_node_t); i++) {
        ((char *) node)[i] = 0;
    }
    node->inode.svi_number = ino;
    fs->fs_ops->read_vnode(fs, &node->vnode);
    if (node->vnode.vn_ops == NULL) {
        return -1;
    }

    spinlock_lock(&fs->vnode_list_mutex);
    list_insert_back(&fs->vnode_list, &node->vnode.vn_link);
    spinlock_unlock(&fs->vnode_list_mutex);
    *out = &node->vnode;
    return 0;
}

/*
 * Retrieve the directory entry at the location specified by pos in the
 * provided directory. Return 0 on success, -1 on failure.
 *
 * TODO: implement me!
 *
 * Hints:
 *   - index the data associated with the directory
 *   - what happens if pos is not aligned to the dirent?
 */
long svfs_readdir(vnode_t *vnode, long pos, dirent_t **d) {
    if (vnode == NULL || d == NULL || vnode->vn_mode != DIRECTORY) {
        return -1;
    }
    if (pos < 0 || pos % SV_DIRENT_SIZE != 0 ||
        pos + SV_DIRENT_SIZE > vnode->vn_len) {
        return -1;
    }

    slab_allocator_t alloc;
    slab_allocator_init(&alloc, sizeof(dirent_t));
    dirent_t *ent = (dirent_t *) slab_obj_alloc(alloc);
    if (svfs_read(vnode, pos, ent, SV_DIRENT_SIZE) != 0) {
        slab_obj_free(alloc, ent);
        return -1;
    }
    *d = ent;
    return 0;
}

/*
 * Read the data associated with a file from disk. Return 0 on success, -1 on
 * error.
 *
 * TODO: implement me!
 *
 * Hints:
 *   - are there helper functions that can help us out?
 *   - how do we access these helpers?
 */
long svfs_read_pframe(vnode_t *vnode, long pagenum, pframe_t **pfp) {
    if (vnode == NULL || pfp == NULL || *pfp == NULL ||
        pagenum < 0 || pagenum >= SV_MAX_FILE_BLOCKS) {
        return -1;
    }
    svfs_node_t *node = (svfs_node_t *) vnode;

    long b = 0;
    if (pagenum < SV_NDIRECT_BLKS) {
        b = node->inode.svi_direct_blocks[pagenum];
    } else if (node->inode.svi_indirect_block != 0) {
        long ind[SV_NINDIRECT_BLKS];
        read_disk_block(node->inode.svi_indirect_block, (char *) ind);
        b = ind[pagenum - SV_NDIRECT_BLKS];
    }

    if (b == 0) {
        for (long i = 0; i < FRAME_LEN; i++) {
            (*pfp)->bytes[i] = 0;
        }
    } else {
        read_disk_block(b, (*pfp)->bytes);
    }
    return 0;
}

/*
 * Write pframe to disk. Return 0 on success, -1 on error.
 *
 * TODO: implement me!
 */
long svfs_write_pframe(vnode_t *vnode, pframe_t *pf) {
    if (vnode == NULL || pf == NULL ||
        pf->pf_pagenum < 0 || pf->pf_pagenum >= SV_MAX_FILE_BLOCKS) {
        return -1;
    }
    svfs_node_t *node = (svfs_node_t *) vnode;

    long b = 0;
    if (pf->pf_pagenum < SV_NDIRECT_BLKS) {
        b = node->inode.svi_direct_blocks[pf->pf_pagenum];
    } else if (node->inode.svi_indirect_block != 0) {
        long ind[SV_NINDIRECT_BLKS];
        read_disk_block(node->inode.svi_indirect_block, (char *) ind);
        b = ind[pf->pf_pagenum - SV_NDIRECT_BLKS];
    }
    if (b == 0) {
        return -1;
    }

    write_disk_block(b, pf->bytes);
    return 0;
}
