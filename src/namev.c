#include "vfs.h"
#include "vnode.h"
#include "dirent.h"

/*
 * TODO: implement me!
 */
long namev_lookup(vnode_t *dir, const char *name, long namelen,
                  vnode_t **out) {
    if (dir == NULL || name == NULL || out == NULL) {
        return -1;
    }
    if (namelen <= 0 || namelen > NAME_LEN) {
        return -1;
    }
    if (dir->vn_ops == NULL || dir->vn_ops->lookup == NULL) {
        return -1;
    }
    return dir->vn_ops->lookup(dir, name, namelen, out);
}

/*
 * TODO: implement me!
 */
long namev_open(vnode_t *base, const char *path, vnode_t **res_vnode) {
    if (path == NULL || res_vnode == NULL || path[0] == '\0') {
        return -1;
    }

    vnode_t *cur = base;
    if (path[0] == '/') {
        cur = vfs_root_fs.fs_root;
    }
    if (cur == NULL) {
        return -1;
    }

    long i = 0;
    while (path[i] != '\0') {
        while (path[i] == '/') {
            i++;
        }
        if (path[i] == '\0') {
            break;
        }

        long start = i;
        while (path[i] != '\0' && path[i] != '/') {
            i++;
        }

        vnode_t *next;
        // if missing component, too long, or `cur` isn't a directory
        if (namev_lookup(cur, path + start, i - start, &next) != 0) {
            return -1;
        }
        cur = next;
    }

    *res_vnode = cur;
    return 0;
}

/*
 * TODO: implement me!
 */
long namev_get_child(vnode_t *base, const char *name, long namelen,
                     vnode_t **out) {
    if (name == NULL || namelen <= 0) {
        return -1;
    }
    if (name[0] == '.' && (namelen == 1 || (namelen == 2 && name[1] == '.'))) {
        return -1;
    }
    return namev_lookup(base, name, namelen, out);
}

/*
 * TODO: implement me!
 */
long namev_get_parent(vnode_t *base, vnode_t **out) {
    return namev_lookup(base, "..", 2, out);
}

/*
 * TODO: implement me!
 */
long namev_is_descendent(vnode_t *a, vnode_t *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }

    vnode_t *cur = a;
    while (1) {
        vnode_t *parent;
        if (namev_get_parent(cur, &parent) != 0) {
            return 0;
        }
        if (parent == b) {
            return 1;
        }
        // if reached the root without meeting b
        if (parent == cur) {
            return 0;
        }
        cur = parent;
    }
}
