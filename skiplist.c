/* 
 * Copyright (c) 2011 Scott Vokes <vokes.s@gmail.com>
 *  
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *  
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include "skiplist_config.h"
#include "skiplist.h"
#include "skiplist_macros_internal.h"

typedef struct skiplist {
    skiplist_count_t count;
    struct skiplist_node *head;
    
#ifndef SKIPLIST_CMP_CB
    skiplist_cmp_cb *cmp;
#endif

#if SKIPLIST_USE_LOCK
    /* Lock. Only needed when changing count or head. Right? */
    pthread_mutex_t lock;
#endif

} skiplist;

typedef struct skiplist_node {
    int h;                  /* node height */
    void *k;                /* key */
    void *v;                /* value */
    
#if SKIPLIST_USE_LOCK
    pthread_mutex_t lock;
#endif
    
    /* Forward pointers.
     * allocated with (h)*sizeof(N*) extra bytes. */
    struct skiplist_node *next[];
} N;

/* Sentinel. */
static struct skiplist_node SENTINEL = { 0, NULL, NULL };
#define IS_SENTINEL(n) (n == &SENTINEL)

static N *N_alloc(int height, void *key, void *value);

T *skiplist_new(SKIPLIST_NEW_ARGS) {
    T *sl = SKIPLIST_MALLOC(sizeof(*sl));
    if (sl == NULL) { LOG1("alloc fail\n"); R NULL; }
    sl->count = 0;
    
    N *head = N_alloc(1, &SENTINEL, &SENTINEL);
    if (head == NULL) { SKIPLIST_FREE(sl, sizeof(*sl)); R NULL; }
    sl->head = head;
    
    SKIPLIST_CMP_INIT();    /* set *cmp, if not hardcoded */
    SKIPLIST_LOCK_INIT();   /* init lock, if used */
    R sl;
}

/* Allocate a node. The forward pointers are initialized to &SENTINEL.
 * Returns NULL on failure. */
static N *N_alloc(int height, void *key, void *value) {
    A(height > 0);
    A(height <= SKIPLIST_MAX_HEIGHT);
    N *n = SKIPLIST_MALLOC(sizeof(*n) + height*sizeof(N *));
    if (n == NULL) { fprintf(stderr, "alloc fail\n"); R NULL; }
    n->h = height; n->k = key; n->v = value;
    LOG2("allocated %d-level node at %p\n", height, n);
    DO(height, n->next[i] = &SENTINEL);
    R n;
}

/* Free a node. If necessary, everything it references should be
 * freed by the calling function. */
static void N_free(N *n) {
    SKIPLIST_FREE(n, sizeof(*n) + n->h*sizeof(N *));
}

/* Set the random seed used when randomly constructing skiplists. */
void skiplist_set_seed(unsigned seed) { srandom(seed); }

#ifndef SKIPLIST_GEN_HEIGHT
unsigned int SKIPLIST_GEN_HEIGHT() {
    int h = 1;
    long r = random();
    /* According to the random(3) manpages on OpenBSD and OS X,
     * "[a]ll of the bits generated by random() are usable", so
     * it should be adequate to only call random() once if the
     * default probability of 50% for each additional level
     * increase is used. */
    for (int bit=0; r & (2 << bit); bit++) h++;
    R h > SKIPLIST_MAX_HEIGHT ? SKIPLIST_MAX_HEIGHT : h;
}
#endif

/* Get pointers to the HEIGHT nodes that precede the position
 * for key. Used by add/set/delete/delete_all. */
static void init_prevs(T *sl, void *key, N *head, int height, N **prevs) {
    A(sl);
    A(head);
    N *cur = NULL, *next = NULL;
    int lvl = height - 1, res = 0;
    
    cur = head;
    LOG2("sentinel is %p\n", &SENTINEL);
    LOG2("head is %p\n", head);

    do {
        A(lvl < cur->h);
        A(cur->h <= SKIPLIST_MAX_HEIGHT);
        next = cur->next[lvl];
        LOG2("next is %p, level is %d\n", next, lvl);
        res = IS_SENTINEL(next) ? 1 : SKIPLIST_CMP(next->k, key);
        LOG2("res is %d\n", res);
        if (res < 0) {              /* < - advance. */
            cur = next;
        } else /*if (res >= 0)*/ {  /* >= - overshot, descend. */
            prevs[lvl] = cur;
            lvl--;
        }
    } while (lvl >= 0);
}

static int grow_head(T *sl, N *nn) {
    N *old_head = sl->head;
    LOG2("growing head from %d to %d\n", old_head->h, nn->h);
    N *new_head = N_alloc(nn->h, &SENTINEL, &SENTINEL);
    if (new_head == NULL) {
        fprintf(stderr, "alloc fail\n");
        R -1;
    }
    DO(old_head->h, new_head->next[i] = old_head->next[i]);
    for (int i=old_head->h; i<new_head->h; i++) {
        /* The actual next[i] will be set later. */
        new_head->next[i] = nn;
    }
    sl->head = new_head;
    N_free(old_head);
    R 0;
}

static int add_or_set(T *sl, int try_replace,
                      void *key, void *value, void **old) {
    A(sl);
    N *head = sl->head;
    A(head);
    int cur_height = head->h;
    N *prevs[cur_height];

    init_prevs(sl, key, head, cur_height, prevs);

    if (try_replace) {
        N *next = prevs[0]->next[0];
        if (!IS_SENTINEL(next)) {
            int res = SKIPLIST_CMP(next->k, key);
            if (res == 0) { /* key exists, replace value */
                if (old) *old = next->v;
                next->v = value;
                R 0;
            } else {        /* not found */
                if (old) *old = NULL;
            }
        }
    }
    
    int new_height = SKIPLIST_GEN_HEIGHT();
    N *nn = N_alloc(new_height, key, value); /* new node */
    if (nn == NULL) R -1;
    
    if (new_height > cur_height) {
        if (grow_head(sl, nn) < 0) R -1;
        DO(cur_height, if (prevs[i] == /* old */ head)
                           prevs[i] = sl->head);
        head = sl->head;
    }

    /* Insert n between prev[lvl] and prevs->next[lvl] */
    int minH = nn->h < cur_height ? nn->h : cur_height;
    for (int i=0; i<minH; i++) {
        A(i < prevs[i]->h);
        nn->next[i] = prevs[i]->next[i];
        A(prevs[i]->h <= SKIPLIST_MAX_HEIGHT);
        prevs[i]->next[i] = nn;
    }
    sl->count++;
    R 0;
}

int skiplist_add(T *sl, void *key, void *value) {
    R add_or_set(sl, 0, key, value, NULL);
}

int skiplist_set(T *sl, void *key, void *value, void **old) {
    R add_or_set(sl, 1, key, value, old);
}

void *delete_one_or_all(T *sl, void *key,
                        void *udata, skiplist_free_cb *cb) {
    A(sl);
    N *head = sl->head;
    int cur_height = head->h;
    N *prevs[cur_height];
    init_prevs(sl, key, head, cur_height, prevs);
    
    N *doomed = prevs[0]->next[0];
    if (IS_SENTINEL(doomed) || 0 != SKIPLIST_CMP(doomed->k, key))
        R NULL;                 /* not found */
    
    if (cb == NULL) {           /* delete one w/ key */
        DO(doomed->h, prevs[i]->next[i]=doomed->next[i]);
        void *res = doomed->v;
        N_free(doomed);
        sl->count--;
        R res;
    } else {                    /* delete all w/ key */
        int res = 0;
        int tdh = 0;            /* tallest doomed height */
        N *nexts[cur_height];

        DO(cur_height, nexts[i] = &SENTINEL);

        LOG2("head is %p, sentinel is %p\n", head, &SENTINEL);
        if (SKIPLIST_LOG_LEVEL > 0)
            DO(cur_height, LOG2("prevs[%i]: %p\n", i, prevs[i]));

        /* Take the prevs, make another array of the first
         * point beyond the deleted cells at each level, and
         * link from prev to post. */
        do {
            LOG2("doomed is %p\n", doomed);
            N *next = doomed->next[0];
            A(next);
            LOG2("cur tdh: %d, next->h: %d, new tdh: %d\n",
                tdh, doomed->h, tdh > doomed->h ? tdh : doomed->h);
            tdh = tdh > doomed->h ? tdh : doomed->h;
            
            /* Maintain proper forward references.
             * This does some redundant work, and could instead
             * update to doomed's nexts' that have a greater
             * key. The added CMPs could be slower, though.*/
            DO(doomed->h,
                LOG2("nexts[%d] = doomed->next[%d] (%p)\n",
                    i, i, doomed->next[i]);
                nexts[i] = doomed->next[i]);
            if (SKIPLIST_LOG_LEVEL > 1)
                DO(tdh, fprintf(stderr, "nexts[%d] = %p\n", i, nexts[i]));
            
            cb(key, doomed->v, udata);
            sl->count--;
            N_free(doomed);
            res = IS_SENTINEL(next)
              ? -1 : SKIPLIST_CMP(next->k, key);
            doomed = next;
        } while (res == 0);

        LOG2("tdh is %d\n", tdh);
        DO(tdh,
            LOG2("setting prevs[%d]->next[%d] to %p\n", i, i, nexts[i]);
            prevs[i]->next[i] = nexts[i]);
        R NULL;
    }
}

void *skiplist_delete(T *sl, void *key) {
    return delete_one_or_all(sl, key, NULL, NULL);
}

void skiplist_delete_all(T *sl, void *key,
                         void *udata, skiplist_free_cb *cb) {
    A(cb);
    (void) delete_one_or_all(sl, key, udata, cb);
}

static N *get_first_eq_node(T *sl, void *key) {
    A(sl);
    N *head = sl->head;
    int height = head->h;
    int lvl = height - 1;
    N *cur = head, *next = NULL;
    
    do {
        A(cur->h > lvl);
        next = cur->next[lvl];

        A(next->h <= SKIPLIST_MAX_HEIGHT);
        int res = IS_SENTINEL(next) ? 1 : SKIPLIST_CMP(next->k, key);
        if (res < 0) {  /* next->key < key, advance */
            cur = next;
        } else if (res >= 0) { /* next->key >= key, descend */
            /* Descend when == to make sure it's the FIRST match. */
            if (lvl == 0)
                {
                    if (res == 0) R next; /* found */
                    R NULL;               /* not found */
                }
            lvl--;
        }
    } while (lvl >= 0);
    
    R NULL;                 /* not found */
}

void *skiplist_get(T *sl, void *key) {
    N *n = get_first_eq_node(sl, key);
    R n ? n->v : NULL;
}

int skiplist_member(T *sl, void *key) {
    void *v = skiplist_get(sl, key);
    R v != NULL;
}

int skiplist_first(T *sl, void **key, void **value) {
    A(sl);
    N *first = sl->head->next[0];
    if (IS_SENTINEL(first)) R -1; /* empty */
    if (key) *key = first->k;
    if (value) *value = first->v;
    R 0;
}

int skiplist_last(T *sl, void **key, void **value) {
    A(sl);
    N *head = sl->head;
    int lvl = head->h - 1;
    N *cur = head->next[lvl];
    if (IS_SENTINEL(cur)) R -1; /* empty */
    do {
        N *next = cur->next[lvl];
        if (IS_SENTINEL(next)) {
            lvl--;
        } else {
            cur = next;
        }
    } while (lvl >= 0);
    
    A(!IS_SENTINEL(cur));
    A(IS_SENTINEL(cur->next[0]));
    if (key) *key = cur->k;
    if (value) *value = cur->v;
    R 0;
}

int skiplist_pop_first(T *sl, void **key, void **value) {
    int height = 0;
    A(sl);
    N *head = sl->head;
    N *first = head->next[0];
    A(first);
    height = first->h;
    if (IS_SENTINEL(first)) R -1; /* empty */
    if (key) *key = first->k;
    if (value) *value = first->v;
    sl->count--;

    DO(height, head->next[i] = first->next[i]);
    N_free(first);
    R 0;
}

int skiplist_pop_last(T *sl, void **key, void **value) {
    A(sl);
    N *head = sl->head;
    N *prevs[head->h];
    int lvl = head->h - 1;
    N *cur = head;
    if (sl->count == 0) R -1; /* empty */
    
    /* Get all the nodes that are (node -> last -> &SENTINEL) so
     * node can skip directly to the sentinel. */
    do {
        if (IS_SENTINEL(cur->next[lvl])) {
            prevs[lvl--] = cur;
        } else {
            N *next = cur->next[lvl]->next[lvl];
            if (IS_SENTINEL(next)) {
                prevs[lvl--] = cur;
            } else {
                cur = cur->next[lvl];
            }
        }
    } while (lvl >= 0);
    
    cur = cur->next[0];
    A(!IS_SENTINEL(cur));
    A(IS_SENTINEL(cur->next[0]));

    /* skip over the last non-SENTINEL nodes. */
    DO(cur->h, A(prevs[i]->next[i] == cur));
    DO(cur->h, prevs[i]->next[i] = &SENTINEL);
    
    if (key) *key = cur->k;
    if (value) *value = cur->v;
    sl->count--;

    A(!IS_SENTINEL(cur));
    N_free(cur);
    R 0;
}

skiplist_count_t skiplist_count(T *sl) { A(sl); R sl->count; }

int skiplist_empty(T *sl) { R (skiplist_count(sl) == 0); }

static void walk_and_apply(N *cur, void *udata, skiplist_iter_cb *cb) {
    while (!IS_SENTINEL(cur)) {
        if (cb(cur->k, cur->v, udata) != 0) return;
        cur = cur->next[0];
    }
}

void skiplist_iter(T *sl, void *udata, skiplist_iter_cb *cb) {
    A(sl); A(cb);
    walk_and_apply(sl->head->next[0], udata, cb);
}

int skiplist_iter_from(T *sl, void *key,
                       void *udata, skiplist_iter_cb *cb) {
    A(sl); A(cb);
    N *cur = get_first_eq_node(sl, key);
    LOG2("first node is %p\n", cur);
    if (cur == NULL) R -1;
    walk_and_apply(cur, udata, cb);
    R 0;
}

skiplist_count_t skiplist_clear(T *sl, void *udata, skiplist_free_cb *cb) {
    A(sl);
    N *cur = sl->head->next[0];
    skiplist_count_t ct = 0;
    while (!IS_SENTINEL(cur)) {
        N *doomed = cur;
        if (cb) cb(doomed->k, doomed->v, udata);
        cur = doomed->next[0];
        N_free(doomed);
        ct++;
    }
    DO(sl->head->h, sl->head->next[i] = &SENTINEL);
    R ct;
}

skiplist_count_t skiplist_free(T *sl, void *udata, skiplist_free_cb *cb) {
    A(sl);
    skiplist_count_t ct = skiplist_clear(sl, udata, cb);
    N_free(sl->head);
    SKIPLIST_FREE(sl, sizeof(*sl));
    R ct;
}

void skiplist_debug(T *sl, FILE *f,
                    void *udata, skiplist_fprintf_kv_cb *cb) {
    A(sl);
    int max_lvl = sl->head->h;
    int counts[max_lvl];
    DO(max_lvl, counts[i] = 0);
    if (f) fprintf(f, "max level is %d\n", max_lvl);
#ifndef SKIPLIST_CMP_CB
    if (f) fprintf(f, "cmp cb is %p\n", sl->cmp);
#endif
    N *head = sl->head;
    A(head);
    if (f) fprintf(f, "head is %p\nsentinel is %p\n",
        head, &SENTINEL);
    N *n = NULL;

    int ct = 0, prev_ct = 0;
    for (int i=max_lvl - 1; i>=0; i--) {
        if (f) fprintf(f, "-- L %d:", i);
        for (n = head->next[i]; n != &SENTINEL; n = n->next[i]) {
            if (f) {
                fprintf(f, " -> %p(%d%s", n, n->h, cb == NULL ? "" : ":");
                if (cb) cb(f, n->k, n->v, udata);
                fprintf(f, ")");
            }
            
            if (f && n->h > max_lvl)
                fprintf(stderr, "\nERROR: node %p's ->h > head->h (%d, %d)\n",
                    n, n->h, max_lvl);
            A(n->h <= max_lvl);
            ct++;
        }
        if (prev_ct != 0) A(ct >= prev_ct);
        prev_ct = ct;
        counts[i] = ct;
        ct = 0;
        if (f) fprintf(f, " -> &SENTINEL(%p)\n", &SENTINEL);
    }
    
    if (f) DO(max_lvl,
        if (counts[i] > 0)
            fprintf(f, "-- Count @ %d: %d\n", i, counts[i]));
}
