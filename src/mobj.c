#include "mobj.h"

#include "pframe.h"
#include "util/string.h"
#include "slab_allocator.h"

/*
 * TODO: implement me!
 *
 * Hints:
 *   - when we write the ops, we want to make sure that we copy the value
 *     of the parameters into the space that we've been provided
 */
void mobj_create(mobj_t *o, mobj_ops_t *ops) {
    o->mo_ops.read_pframe = ops->read_pframe;
    o->mo_ops.write_pframe = ops->write_pframe;
    o->mo_ops.flush_pframe = ops->flush_pframe;

    list_init(&o->mo_pframes);
    spinlock_init(&o->mo_mutex);
    o->parent = NULL;
}

/*
 * TODO: implement me!
 *
 * Hints:
 *   - in this function, where do we search for frame?
 *   - if we don't find the frame, set the pfp to NULL
 */
void mobj_find_pframe(mobj_t *o, long pagenum, pframe_t **pfp) {
    *pfp = NULL;
    for (list_link_t *l = o->mo_pframes.head; l != NULL; l = l->next) {
        pframe_t *pf = (pframe_t *) l->parent;
        if (pf->pf_pagenum == pagenum) {
            *pfp = pf;
            return;
        }
    }
}

/*
 * TODO: implement me!
 *
 * Hints:
 *   - the frame might be in our list already...
 *   - we have access to a function to read the pframe from disk!
 *   - check how we get data from disk in sys/disk/disk.{h,c}...
 */
void mobj_get_pframe(mobj_t *o, long pagenum, pframe_t **pfp) {
    spinlock_lock(&o->mo_mutex);
    mobj_find_pframe(o, pagenum, pfp);
    if (*pfp != NULL || pagenum < 0) {
        spinlock_unlock(&o->mo_mutex);
        return;
    }

    slab_allocator_t alloc;
    slab_allocator_init(&alloc, sizeof(pframe_t));
    pframe_t *pf = (pframe_t *) slab_obj_alloc(alloc);
    for (long i = 0; i < FRAME_LEN; i++) {
        pf->bytes[i] = 0;
    }
    pf->pf_pagenum = pagenum;
    pf->pf_addr = pf->bytes;
    list_link_init(&pf->pf_link, pf);

    *pfp = pf;
    if (o->mo_ops.read_pframe(o, pagenum, pfp) != 0) {
        slab_obj_free(alloc, pf);
        *pfp = NULL;
        spinlock_unlock(&o->mo_mutex);
        return;
    }

    // ordered insert, go before the first cached page with a larger number
    list_link_t *cur = o->mo_pframes.head;
    while (cur != NULL && ((pframe_t *) cur->parent)->pf_pagenum < pagenum) {
        cur = cur->next;
    }
    if (cur == NULL) {
        list_insert_back(&o->mo_pframes, &pf->pf_link);
    } else if (cur == o->mo_pframes.head) {
        list_insert_front(&o->mo_pframes, &pf->pf_link);
    } else {
        pf->pf_link.prev = cur->prev;
        pf->pf_link.next = cur;
        cur->prev->next = &pf->pf_link;
        cur->prev = &pf->pf_link;
        o->mo_pframes.size++;
    }

    *pfp = pf;
    spinlock_unlock(&o->mo_mutex);
}

/*
 * TODO: implement me!
 *
 * Hints:
 *   - pos describes where to read from in terms of bytes
 *   - len is the maximum number of bytes to read... when can we not read all
 *     of the way up to len?
 *   - save what we read to buf
 */
long mobj_read(mobj_t *o, long pos, long len, char *buf) {
    if (o == NULL || buf == NULL || pos < 0 || len < 0) {
        return -1;
    }

    long done = 0;
    spinlock_lock(&o->mo_mutex);
    while (done < len) {
        long cur = pos + done;
        long off = cur % FRAME_LEN;

        pframe_t *pf;
        mobj_find_pframe(o, cur / FRAME_LEN, &pf);
        if (pf == NULL) {
            break;
        }

        long n = FRAME_LEN - off;
        if (n > len - done) {
            n = len - done;
        }
        memcpy(buf + done, pf->bytes + off, n);
        done += n;
    }
    spinlock_unlock(&o->mo_mutex);
    return done;
}

/*
 * TODO: implement me!
 *
 * Hints:
 *   - pos describes where to write to in terms of bytes
 *   - len is the maximum number of bytes to write... when can we not read all
 *     of the way up to len?
 *   - save what we read to buf
 */
long mobj_write(mobj_t *o, long pos, long len, const char *buf) {
    if (o == NULL || buf == NULL || pos < 0 || len < 0) {
        return -1;
    }

    long done = 0;
    spinlock_lock(&o->mo_mutex);
    while (done < len) {
        long cur = pos + done;
        long off = cur % FRAME_LEN;

        pframe_t *pf;
        mobj_find_pframe(o, cur / FRAME_LEN, &pf);
        if (pf == NULL) {
            break;
        }

        long n = FRAME_LEN - off;
        if (n > len - done) {
            n = len - done;
        }
        memcpy(pf->bytes + off, buf + done, n);
        done += n;
    }
    spinlock_unlock(&o->mo_mutex);
    return done;
}

/*
 * TODO: implement me!
 *
 * Hints:
 *   - make sure that disk is up to date in the places that we care about!
 */
void mobj_flush(mobj_t *o) {
    spinlock_lock(&o->mo_mutex);
    for (list_link_t *l = o->mo_pframes.head; l != NULL; l = l->next) {
        o->mo_ops.write_pframe(o, (pframe_t *) l->parent);
    }
    spinlock_unlock(&o->mo_mutex);
}
