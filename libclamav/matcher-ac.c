/*
 *  Copyright (C) 2007-2009 Sourcefire, Inc.
 *
 *  Authors: Tomasz Kojm
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>

#include <assert.h>
#ifdef	HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "clamav.h"
#include "others.h"
#include "matcher.h"
#include "matcher-ac.h"
#include "filetypes.h"
#include "cltypes.h"
#include "str.h"
#include "readdb.h"
#include "default.h"
#include "filtering.h"

#include "mpool.h"

#define AC_SPECIAL_ALT_CHAR	1
#define AC_SPECIAL_ALT_STR	2
#define AC_SPECIAL_LINE_MARKER	3
#define AC_SPECIAL_BOUNDARY	4
#define AC_SPECIAL_WORD_MARKER	5

#define AC_BOUNDARY_LEFT		0x0001
#define AC_BOUNDARY_LEFT_NEGATIVE	0x0002
#define AC_BOUNDARY_RIGHT		0x0004
#define AC_BOUNDARY_RIGHT_NEGATIVE	0x0008
#define AC_LINE_MARKER_LEFT		0x0010
#define AC_LINE_MARKER_LEFT_NEGATIVE	0x0020
#define AC_LINE_MARKER_RIGHT		0x0040
#define AC_LINE_MARKER_RIGHT_NEGATIVE	0x0080
#define AC_WORD_MARKER_LEFT		0x0100
#define AC_WORD_MARKER_LEFT_NEGATIVE	0x0200
#define AC_WORD_MARKER_RIGHT		0x0400
#define AC_WORD_MARKER_RIGHT_NEGATIVE	0x0800

static char boundary[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 2, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    3, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 3, 1, 3, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 2, 0, 
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static inline int insert_list(struct cli_matcher *root, struct cli_ac_patt *pattern, struct cli_ac_node *pt)
{
    struct cli_ac_list *ph, *new, *ph_prev, *ph_add_after;
    struct cli_ac_list **newtable;
    struct cli_ac_patt *php;
    struct cli_ac_special *a1, *a2;
    uint8_t i, match;

    new = (struct cli_ac_list *)mpool_calloc(root->mempool, 1, sizeof(struct cli_ac_list));
    if (!new) {
        cli_errmsg("cli_ac_addpatt: Can't allocate memory for list node\n");
        return CL_EMEM;
    }
    new->me = pattern;

    root->ac_lists++;
    newtable = mpool_realloc(root->mempool, root->ac_listtable, root->ac_lists * sizeof(struct cli_ac_list *));
    if(!newtable) {
        root->ac_lists--;
        cli_errmsg("cli_ac_addpatt: Can't realloc ac_listtable\n");
        mpool_free(root->mempool, new);
        return CL_EMEM;
    }

    root->ac_listtable = newtable;
    root->ac_listtable[root->ac_lists - 1] = new;

    ph = pt->list;
    ph_add_after = ph_prev = NULL;
    while(ph) {
        php = ph->me;
        if(!ph_add_after && php->partno <= pattern->partno && (!ph->next || ph->next->me->partno > pattern->partno))
            ph_add_after = ph;
        if((php->length == pattern->length) && (php->prefix_length == pattern->prefix_length) && (php->ch[0] == pattern->ch[0]) && (php->ch[1] == pattern->ch[1]) && (php->boundary == pattern->boundary)) {
            if(!memcmp(php->pattern, pattern->pattern, php->length * sizeof(uint16_t)) && !memcmp(php->prefix, pattern->prefix, php->prefix_length * sizeof(uint16_t))) {
                if(!php->special && !pattern->special) {
                    match = 1;
                } else if(php->special == pattern->special) {
                    match = 1;
                    for(i = 0; i < php->special; i++) {
                        a1 = php->special_table[i];
                        a2 = pattern->special_table[i];

                        if(a1->num != a2->num) {
                            match = 0;
                            break;
                        }

                        if(a1->negative != a2->negative) {
                            match = 0;
                            break;
                        }

                        if(a1->type != a2->type) {
                            match = 0;
                            break;
                        } else if(a1->type == AC_SPECIAL_ALT_CHAR) {
                            if(memcmp(a1->str, a2->str, a1->num)) {
                                match = 0;
                                break;
                            }
                        } else if(a1->type == AC_SPECIAL_ALT_STR) {
                            while(a1 && a2) {
                                if((a1->len != a2->len) || memcmp(a1->str, a2->str, a1->len))
                                    break;
                                a1 = a1->next;
                                a2 = a2->next;
                            }

                            if(a1 || a2) {
                                match = 0;
                                break;
                            }
                        }
                    }
                } else {
                    match = 0;
                }

                if(match) {
                    if(pattern->partno < php->partno) {
                        new->next_same = ph;
                        if(ph_prev)
                            ph_prev->next = ph->next;
                        else
                            pt->list = ph->next;

                        ph->next = NULL;
                        break;
                    } else {
                        while(ph->next_same && ph->next_same->me->partno < pattern->partno)
                            ph = ph->next_same;

                        new->next_same = ph->next_same;
                        ph->next_same = new;
                        return CL_SUCCESS;
                    }
                }
            }
        }

        ph_prev = ph;
        ph = ph->next;

    }

    if(ph_add_after) {
        new->next = ph_add_after->next;
        ph_add_after->next = new;
    } else {
        new->next = pt->list;
        pt->list = new;
    }

    return CL_SUCCESS;
}

static inline struct cli_ac_node *add_new_node(struct cli_matcher *root, uint16_t i, uint16_t len)
{
    struct cli_ac_node *new;
    struct cli_ac_node **newtable;

    new = (struct cli_ac_node *) mpool_calloc(root->mempool, 1, sizeof(struct cli_ac_node));
    if(!new) {
        cli_errmsg("cli_ac_addpatt: Can't allocate memory for AC node\n");
        return NULL;
    }

    if(i != len - 1) {
        new->trans = (struct cli_ac_node **) mpool_calloc(root->mempool, 256, sizeof(struct cli_ac_node *));
        if(!new->trans) {
            cli_errmsg("cli_ac_addpatt: Can't allocate memory for new->trans\n");
            mpool_free(root->mempool, new);
            return NULL;
        }
    }

    root->ac_nodes++;
    newtable = mpool_realloc(root->mempool, root->ac_nodetable, root->ac_nodes * sizeof(struct cli_ac_node *));
    if(!newtable) {
        root->ac_nodes--;
        cli_errmsg("cli_ac_addpatt: Can't realloc ac_nodetable\n");
        if(new->trans)
            mpool_free(root->mempool, new->trans);
        mpool_free(root->mempool, new);
        return NULL;
    }

    root->ac_nodetable = newtable;
    root->ac_nodetable[root->ac_nodes - 1] = new;

    return new;
}

static int cli_ac_addpatt_recursive(struct cli_matcher *root, struct cli_ac_patt *pattern, struct cli_ac_node *pt, uint16_t i, uint16_t len)
{
    struct cli_ac_node *next;
    int ret;

    /* last node, insert pattern here (base case)*/
    if(i >= len) {
        return insert_list(root, pattern, pt);
    }

    /* if current node has no trans table, generate one */
    if(!pt->trans) {
        pt->trans = (struct cli_ac_node **) mpool_calloc(root->mempool, 256, sizeof(struct cli_ac_node *));
        if(!pt->trans) {
            cli_errmsg("cli_ac_addpatt: Can't allocate memory for pt->trans\n");
            return CL_EMEM;
        }
    }

    /* if pattern is nocase, we need to enumerate all the combinations if applicable
     * it's why this function was re-written to be recursive
     */
    if((pattern->sigopts & ACPATT_OPTION_NOCASE) && isalpha(pattern->pattern[i] & 0xff)) {
        next = pt->trans[cli_nocasei((unsigned char) (pattern->pattern[i] & 0xff))];
        if(!next)
            next = add_new_node(root, i, len);
        if(!next)
            return CL_EMEM;
        else
            pt->trans[cli_nocasei((unsigned char) (pattern->pattern[i] & 0xff))] = next;

        if ((ret = cli_ac_addpatt_recursive(root, pattern, next, i+1, len)) != CL_SUCCESS)
            return ret;
    }

    /* normal transition, also enumerates the 'normal' nocase */
    next = pt->trans[(unsigned char) (pattern->pattern[i] & 0xff)];
    if(!next)
        next = add_new_node(root, i, len);
    if(!next)
        return CL_EMEM;
    else
        pt->trans[(unsigned char) (pattern->pattern[i] & 0xff)] = next;

    return cli_ac_addpatt_recursive(root, pattern, next, i+1, len);
}

int cli_ac_addpatt(struct cli_matcher *root, struct cli_ac_patt *pattern)
{
    struct cli_ac_node *pt;
    struct cli_ac_patt **newtable;
    uint16_t len = MIN(root->ac_maxdepth, pattern->length);
    uint8_t i;

    for(i = 0; i < len; i++) {
        if(pattern->pattern[i] & CLI_MATCH_WILDCARD) {
            len = i;
            break;
        }
    }

    if(len < root->ac_mindepth) {
        /* cli_errmsg("cli_ac_addpatt: Signature for %s is too short\n", pattern->virname); */
        return CL_EMALFDB;
    }

    /* pattern added to master list */
    root->ac_patterns++;
    newtable = mpool_realloc(root->mempool, root->ac_pattable, root->ac_patterns * sizeof(struct cli_ac_patt *));
    if(!newtable) {
        root->ac_patterns--;
        cli_errmsg("cli_ac_addpatt: Can't realloc ac_pattable\n");
        return CL_EMEM;
    }

    root->ac_pattable = newtable;
    root->ac_pattable[root->ac_patterns - 1] = pattern;

    pattern->depth = len;

    return cli_ac_addpatt_recursive(root, pattern, root->ac_root, 0, len);
}

struct bfs_list {
    struct cli_ac_node *node;
    struct bfs_list *next;
};

static int bfs_enqueue(struct bfs_list **bfs, struct bfs_list **last, struct cli_ac_node *n)
{
    struct bfs_list *new;

    new = (struct bfs_list *) cli_malloc(sizeof(struct bfs_list));
    if(!new) {
        cli_errmsg("bfs_enqueue: Can't allocate memory for bfs_list\n");
        return CL_EMEM;
    }

    new->next = NULL;
    new->node = n;

    if(*last) {
        (*last)->next = new;
        *last = new;
    } else {
        *bfs = *last = new;
    }

    return CL_SUCCESS;
}

static struct cli_ac_node *bfs_dequeue(struct bfs_list **bfs, struct bfs_list **last)
{
    struct bfs_list *lpt;
    struct cli_ac_node *pt;

    if(!(lpt = *bfs)) {
        return NULL;
    } else {
        *bfs = (*bfs)->next;
        pt = lpt->node;

        if(lpt == *last)
            *last = NULL;

        free(lpt);
        return pt;
    }
}

static int ac_maketrans(struct cli_matcher *root)
{
    struct bfs_list *bfs = NULL, *bfs_last = NULL;
    struct cli_ac_node *ac_root = root->ac_root, *child, *node, *fail;
    int i, ret;

    for(i = 0; i < 256; i++) {
        node = ac_root->trans[i];
        if(!node) {
            ac_root->trans[i] = ac_root;
        } else {
            node->fail = ac_root;
            if((ret = bfs_enqueue(&bfs, &bfs_last, node)))
                return ret;
        }
    }

    while((node = bfs_dequeue(&bfs, &bfs_last))) {
        if(IS_LEAF(node)) {
            struct cli_ac_node *failtarget = node->fail;

            while (NULL != failtarget && (IS_LEAF(failtarget) || !IS_FINAL(failtarget)))
                failtarget = failtarget->fail;

            if (NULL != failtarget)
                node->fail = failtarget;

            continue;
        }

        for(i = 0; i < 256; i++) {
            child = node->trans[i];
            if(child) {
                fail = node->fail;

                while(IS_LEAF(fail) || !fail->trans[i])
                    fail = fail->fail;

                child->fail = fail->trans[i];

                if((ret = bfs_enqueue(&bfs, &bfs_last, child)) != 0)
                    return ret;
            }
        }
    }

    bfs = bfs_last = NULL;
    for(i = 0; i < 256; i++) {
        node = ac_root->trans[i];
        if(node != ac_root) {
            if((ret = bfs_enqueue(&bfs, &bfs_last, node)))
            return ret;
        }
    }

    while((node = bfs_dequeue(&bfs, &bfs_last))) {
        if(IS_LEAF(node))
            continue;
        for(i = 0; i < 256; i++) {
            child = node->trans[i];
            if (!child || (!IS_FINAL(child) && IS_LEAF(child))) {
                struct cli_ac_node *failtarget = node->fail;

                while(IS_LEAF(failtarget) || !failtarget->trans[i])
                    failtarget = failtarget->fail;

                failtarget = failtarget->trans[i];
                node->trans[i] = failtarget;
            } else if (IS_FINAL(child) && IS_LEAF(child)) {
                struct cli_ac_list *list;

                list = child->list;
                if (list) {
                    while (list->next)
                        list = list->next;

                    list->next = child->fail->list;
                } else {
                    child->list = child->fail->list;
                }

                child->trans = child->fail->trans;
            } else {
                if((ret = bfs_enqueue(&bfs, &bfs_last, child)) != 0)
                    return ret;
            }
        }
    }

    return CL_SUCCESS;
}

int cli_ac_buildtrie(struct cli_matcher *root)
{
    if(!root)
        return CL_EMALFDB;

    if(!(root->ac_root)) {
        cli_dbgmsg("cli_ac_buildtrie: AC pattern matcher is not initialised\n");
        return CL_SUCCESS;
    }

    if (root->filter)
        cli_dbgmsg("Using filter for trie %d\n", root->type);

    return ac_maketrans(root);
}

int cli_ac_init(struct cli_matcher *root, uint8_t mindepth, uint8_t maxdepth, uint8_t dconf_prefiltering)
{
#ifdef USE_MPOOL
    assert(root->mempool && "mempool must be initialized");
#endif

    root->ac_root = (struct cli_ac_node *) mpool_calloc(root->mempool, 1, sizeof(struct cli_ac_node));
    if(!root->ac_root) {
        cli_errmsg("cli_ac_init: Can't allocate memory for ac_root\n");
        return CL_EMEM;
    }

    root->ac_root->trans = (struct cli_ac_node **) mpool_calloc(root->mempool, 256, sizeof(struct cli_ac_node *));
    if(!root->ac_root->trans) {
        cli_errmsg("cli_ac_init: Can't allocate memory for ac_root->trans\n");
        mpool_free(root->mempool, root->ac_root);
        return CL_EMEM;
    }

    root->ac_mindepth = mindepth;
    root->ac_maxdepth = maxdepth;

    if (cli_mtargets[root->type].enable_prefiltering && dconf_prefiltering) {
        root->filter = mpool_malloc(root->mempool, sizeof(*root->filter));
        if (!root->filter) {
            cli_errmsg("cli_ac_init: Can't allocate memory for ac_root->filter\n");
            mpool_free(root->mempool, root->ac_root->trans);
            mpool_free(root->mempool, root->ac_root);
            return CL_EMEM;
        }
        filter_init(root->filter);
    }

    return CL_SUCCESS;
}

#ifdef USE_MPOOL
#define mpool_ac_free_special(a, b) ac_free_special(a, b)
static void ac_free_special(mpool_t *mempool, struct cli_ac_patt *p)
#else
#define mpool_ac_free_special(a, b) ac_free_special(b)
static void ac_free_special(struct cli_ac_patt *p)
#endif
{
    unsigned int i;
    struct cli_ac_special *a1, *a2;


    if(!p->special)
        return;

    for(i = 0; i < p->special; i++) {
        a1 = p->special_table[i];
        while(a1) {
            a2 = a1;
            a1 = a1->next;

            if(a2->str)
                mpool_free(mempool, a2->str);
            mpool_free(mempool, a2);
        }
    }
    mpool_free(mempool, p->special_table);
}

void cli_ac_free(struct cli_matcher *root)
{
    uint32_t i;
    struct cli_ac_patt *patt;

    for(i = 0; i < root->ac_patterns; i++) {
        patt = root->ac_pattable[i];
        mpool_free(root->mempool, patt->prefix ? patt->prefix : patt->pattern);
        mpool_free(root->mempool, patt->virname);
        if(patt->special)
            mpool_ac_free_special(root->mempool, patt);
        mpool_free(root->mempool, patt);
    }

    if(root->ac_pattable)
        mpool_free(root->mempool, root->ac_pattable);

    if(root->ac_reloff)
        mpool_free(root->mempool, root->ac_reloff);

    /* Freeing trans nodes must be done before freeing table nodes! */
    for(i = 0; i < root->ac_nodes; i++) {
        if(!IS_LEAF(root->ac_nodetable[i]) &&
           root->ac_nodetable[i]->fail &&
           root->ac_nodetable[i]->trans != root->ac_nodetable[i]->fail->trans) {
            mpool_free(root->mempool, root->ac_nodetable[i]->trans);
        }
    }

    for(i = 0; i < root->ac_lists; i++)
        mpool_free(root->mempool, root->ac_listtable[i]);

    if(root->ac_listtable)
        mpool_free(root->mempool, root->ac_listtable);

    for(i = 0; i < root->ac_nodes; i++)
        mpool_free(root->mempool, root->ac_nodetable[i]);

    if(root->ac_nodetable)
        mpool_free(root->mempool, root->ac_nodetable);

    if(root->ac_root) {
        mpool_free(root->mempool, root->ac_root->trans);
        mpool_free(root->mempool, root->ac_root);
    }

    if (root->filter)
        mpool_free(root->mempool, root->filter);
}

/*
 * In parse_only mode this function returns -1 on error or the max subsig id
 */
int cli_ac_chklsig(const char *expr, const char *end, uint32_t *lsigcnt, unsigned int *cnt, uint64_t *ids, unsigned int parse_only)
{
    unsigned int i, len = end - expr, pth = 0, opoff = 0, op1off = 0, val;
    unsigned int blkend = 0, id, modval1, modval2 = 0, lcnt = 0, rcnt = 0, tcnt, modoff = 0;
    uint64_t lids = 0, rids = 0, tids;
    int ret, lval, rval;
    char op = 0, op1 = 0, mod = 0, blkmod = 0;
    const char *lstart = expr, *lend = NULL, *rstart = NULL, *rend = end, *pt;

    for(i = 0; i < len; i++) {
        switch(expr[i]) {
        case '(':
            pth++;
            break;

        case ')':
            if(!pth) {
                cli_errmsg("cli_ac_chklsig: Syntax error: Missing opening parenthesis\n");
                return -1;
            }
            pth--;

        case '>':
        case '<':
        case '=':
            mod = expr[i];
            modoff = i;
            break;

        default:
            if(strchr("&|", expr[i])) {
                if(!pth) {
                    op = expr[i];
                    opoff = i;
                } else if(pth == 1) {
                    op1 = expr[i];
                    op1off = i;
                }
            }
        }

        if(op)
            break;

        if(op1 && !pth) {
            blkend = i;
            if(expr[i + 1] == '>' || expr[i + 1] == '<' || expr[i + 1] == '=') {
                blkmod = expr[i + 1];

                ret = sscanf(&expr[i + 2], "%u,%u", &modval1, &modval2);
                if(ret != 2)
                    ret = sscanf(&expr[i + 2], "%u", &modval1);

                if(!ret || ret == EOF) {
                    cli_errmsg("chklexpr: Syntax error: Missing number after '%c'\n", expr[i + 1]);
                    return -1;
                }

                for(i += 2; i + 1 < len && (isdigit(expr[i + 1]) || expr[i + 1] == ','); i++)
                    ;
            }

            if(&expr[i + 1] == rend)
                break;
            else
                blkmod = 0;
        }
    }

    if(pth) {
        cli_errmsg("cli_ac_chklsig: Syntax error: Missing closing parenthesis\n");
        return -1;
    }

    if(!op && !op1) {
        if(expr[0] == '(')
            return cli_ac_chklsig(++expr, --end, lsigcnt, cnt, ids, parse_only);

        ret = sscanf(expr, "%u", &id);
        if(!ret || ret == EOF) {
            cli_errmsg("cli_ac_chklsig: Can't parse %s\n", expr);
            return -1;
        }

        if(parse_only)
            val = id;
        else
            val = lsigcnt[id];

        if(mod) {
            pt = expr + modoff + 1;
            ret = sscanf(pt, "%u", &modval1);
            if(!ret || ret == EOF) {
                cli_errmsg("chklexpr: Syntax error: Missing number after '%c'\n", mod);
                return -1;
            }

            if(!parse_only) {
                switch(mod) {
                case '=':
                    if(val != modval1)
                        return 0;
                    break;
                case '<':
                    if(val >= modval1)
                        return 0;
                    break;
                case '>':
                    if(val <= modval1)
                        return 0;
                    break;
                default:
                    return 0;
                }

                *cnt += val;
                *ids |= (uint64_t) 1 << id;
                return 1;
            }
        }

        if(parse_only) {
            return val;
        } else {
            if(val) {
                *cnt += val;
                *ids |= (uint64_t) 1 << id;
                return 1;
            } else {
                return 0;
            }
        }
    }

    if(!op) {
        op = op1;
        opoff = op1off;
        lstart++;
        rend = &expr[blkend];
    }

    if(!opoff) {
        cli_errmsg("cli_ac_chklsig: Syntax error: Missing left argument\n");
        return -1;
    }

    lend = &expr[opoff];
    if(opoff + 1 == len) {
        cli_errmsg("cli_ac_chklsig: Syntax error: Missing right argument\n");
        return -1;
    }

    rstart = &expr[opoff + 1];

    lval = cli_ac_chklsig(lstart, lend, lsigcnt, &lcnt, &lids, parse_only);
    if(lval == -1) {
        cli_errmsg("cli_ac_chklsig: Calculation of lval failed\n");
        return -1;
    }

    rval = cli_ac_chklsig(rstart, rend, lsigcnt, &rcnt, &rids, parse_only);
    if(rval == -1) {
        cli_errmsg("cli_ac_chklsig: Calculation of rval failed\n");
        return -1;
    }

    if(parse_only) {
        switch(op) {
        case '&':
        case '|':
            return MAX(lval, rval);
        default:
            cli_errmsg("cli_ac_chklsig: Incorrect operator type\n");
            return -1;
        }
    } else {
        switch(op) {
        case '&':
            ret = lval && rval;
            break;
        case '|':
            ret = lval || rval;
            break;
        default:
            cli_errmsg("cli_ac_chklsig: Incorrect operator type\n");
            return -1;
        }

        if(!blkmod) {
            if(ret) {
                *cnt += lcnt + rcnt;
                *ids |= lids | rids;
            }

            return ret;
        } else {
            if(ret) {
                tcnt = lcnt + rcnt;
                tids = lids | rids;
            } else {
                tcnt = 0;
                tids = 0;
            }

            switch(blkmod) {
            case '=':
                if(tcnt != modval1)
                    return 0;
                break;
            case '<':
                if(tcnt >= modval1)
                    return 0;
                break;
            case '>':
                if(tcnt <= modval1)
                    return 0;
                break;
            default:
                return 0;
            }

            if(modval2) {
                val = 0;
                while(tids) {
                    val += tids & (uint64_t) 1;
                    tids >>= 1;
                }

                if(val < modval2)
                    return 0;
            }

            *cnt += tcnt;
            return 1;
        }
    }
}

static int ac_findmatch_branch(const unsigned char *buffer, uint32_t offset, uint32_t length, uint32_t fileoffset, const struct cli_ac_patt *pattern, uint32_t pattoffset, uint16_t specialcnt, uint32_t *end);

/* special handler */
inline static int ac_findmatch_special(const unsigned char *buffer, uint32_t offset, uint32_t fileoffset, uint32_t length, const struct cli_ac_patt *pattern, uint32_t pattoffset, uint16_t specialcnt, uint32_t *end)
{
    int match;
    uint16_t j, b = buffer[offset];
    struct cli_ac_special *special = pattern->special_table[specialcnt];

    match = special->negative;

    switch(special->type) {
    case AC_SPECIAL_ALT_CHAR:
        for(j = 0; j < special->num; j++) {
			if(special->str[j] == b) {
			    match = !special->negative;
			    break;
			} else if(special->str[j] > b)
			    break;
        }
        break;

    case AC_SPECIAL_ALT_STR:
        /* branch for backtracking */
        while(special) {
            if(offset + special->len <= length) {
                if((!special->negative && !memcmp(&buffer[offset], special->str, special->len)) || special->negative) {
                    match = ac_findmatch_branch(buffer, offset+special->len, fileoffset, length, pattern, pattoffset+1, specialcnt+1, end);
                    if (match)
                        return -1; /* contrary to popular belief, this is good */
                }
            }
            special = special->next;
        }
        break;

    case AC_SPECIAL_LINE_MARKER:
        if(b == '\n')
            match = !special->negative;
        else if(b == '\r' && (offset + 1 < length && buffer[offset + 1] == '\n'))
            match = 2 * (!special->negative);
        break;

    case AC_SPECIAL_BOUNDARY:
        if(boundary[b])
            match = !special->negative;
        break;

    case AC_SPECIAL_WORD_MARKER:
        if(!isalnum(b))
            match = !special->negative;
        break;

    default:
        cli_errmsg("ac_findmatch: Unknown special\n");
        match = 0;
    }

    return match;
}

/* call only by ac_findmatch_branch! */
#define AC_MATCH_CHAR(p,b)								\
    switch(wc = p & CLI_MATCH_METADATA) {						\
    case CLI_MATCH_CHAR:								\
	if((unsigned char) p != b)							\
	    match = 0;									\
	break;										\
											\
    case CLI_MATCH_NOCASE:								\
	if((unsigned char)(p & 0xff) != cli_nocase(b))					\
	    match = 0;									\
	break;										\
											\
    case CLI_MATCH_IGNORE:								\
	break;										\
											\
    case CLI_MATCH_SPECIAL:								\
	/* >1 = movement, 0 = fail, <1 = resolved in branch */				\
	if ((match = ac_findmatch_special(buffer, bp, fileoffset, length, pattern, i,	\
	    specialcnt, end)) <= 0)							\
	    return match;								\
	bp += match;									\
	specialcnt++;									\
	continue; /* match value includes bp++ */					\
											\
    case CLI_MATCH_NIBBLE_HIGH:								\
	if((unsigned char) (p & 0x00f0) != (b & 0xf0))					\
	    match = 0;									\
	break;										\
											\
    case CLI_MATCH_NIBBLE_LOW:								\
	if((unsigned char) (p & 0x000f) != (b & 0x0f))					\
	    match = 0;									\
	break;										\
											\
    default:										\
	cli_errmsg("ac_findmatch: Unknown wildcard 0x%x\n", wc);			\
	match = 0;									\
    }


/* state should reset on call, recursion depth = number of alternate specials */
static int ac_findmatch_branch(const unsigned char *buffer, uint32_t offset, uint32_t fileoffset, uint32_t length, const struct cli_ac_patt *pattern, uint32_t pattoffset, uint16_t specialcnt, uint32_t *end)
{
    int match;
    uint32_t bp;
    uint16_t wc, i;

    bp = offset;

    match = 1;
    for(i = pattoffset; i < pattern->length && bp < length; i++) {
	AC_MATCH_CHAR(pattern->pattern[i],buffer[bp]);
	if (!match)
	    return 0;

        bp++;
    }
    *end = bp;

    /* special boundary checks */
    if(pattern->boundary & AC_BOUNDARY_LEFT) {
        match = !!(pattern->boundary & AC_BOUNDARY_LEFT_NEGATIVE);
        if(!fileoffset || (offset && (boundary[buffer[offset - 1]] == 1 || boundary[buffer[offset - 1]] == 3)))
            match = !match;

        if(!match)
            return 0;
    }

    if(pattern->boundary & AC_BOUNDARY_RIGHT) {
        match = !!(pattern->boundary & AC_BOUNDARY_RIGHT_NEGATIVE);
        if((length <= SCANBUFF) && (bp == length || boundary[buffer[bp]] >= 2))
            match = !match;

        if(!match)
            return 0;
    }

    if(pattern->boundary & AC_LINE_MARKER_LEFT) {
        match = !!(pattern->boundary & AC_LINE_MARKER_LEFT_NEGATIVE);
        if(!fileoffset || (offset && (buffer[offset - 1] == '\n')))
            match = !match;

        if(!match)
            return 0;
    }

    if(pattern->boundary & AC_LINE_MARKER_RIGHT) {
        match = !!(pattern->boundary & AC_LINE_MARKER_RIGHT_NEGATIVE);
        if((length <= SCANBUFF) && (bp == length || buffer[bp] == '\n' || (buffer[bp] == '\r' && bp + 1 < length && buffer[bp + 1] == '\n')))
            match = !match;

        if(!match)
            return 0;
    }

    if(pattern->boundary & AC_WORD_MARKER_LEFT) {
        match = !!(pattern->boundary & AC_WORD_MARKER_LEFT_NEGATIVE);
        /* absolute beginning of file */
        if(!fileoffset)
            match = !match;
        /* 'wide' characters need a 'wider' check */
        else if(pattern->sigopts & ACPATT_OPTION_WIDE) {
            /* beginning of file has only one preceding character */
            if(fileoffset-1 == 0)
                match = !match;
            if(offset - 1 && offset && !(isalnum(buffer[offset - 2]) && buffer[offset - 1] == '\0'))
                match = !match;
        }
        /* 'normal' characters */
        else if(offset && !isalnum(buffer[offset - 1]))
            match = !match;

        if(!match)
            return 0;
    }

    if(pattern->boundary & AC_WORD_MARKER_RIGHT) {
        match = !!(pattern->boundary & AC_WORD_MARKER_RIGHT_NEGATIVE);
        if(length <= SCANBUFF) {
            /* absolute end of file */
            if(bp == length)
                match = !match;
            /* 'wide' characters need a 'wider' check */
            else if(pattern->sigopts & ACPATT_OPTION_WIDE) {
                if(!(isalnum(buffer[bp]) && buffer[bp + 1] == '\0'))
                    match = !match;
            }
            /* 'normal' characters */
            else if(!isalnum(buffer[offset - 1]))
                match = !match;
        }

        if(!match)
            return 0;
    }

    /* single-byte anchors */
    if(!(pattern->ch[1] & CLI_MATCH_IGNORE)) {
        bp += pattern->ch_mindist[1];

        for(i = pattern->ch_mindist[1]; i <= pattern->ch_maxdist[1]; i++) {
            if(bp >= length)
                return 0;

            match = 1;
            AC_MATCH_CHAR(pattern->ch[1],buffer[bp]);
            if(match)
                break;

            bp++;
        }

        if(!match)
            return 0;
    }

    if(pattern->prefix) {
        specialcnt = 0;
        bp = offset - pattern->prefix_length;
        match = 1;

        for(i = 0; i < pattern->prefix_length; i++) {
            AC_MATCH_CHAR(pattern->prefix[i],buffer[bp]);
            if(!match)
                return 0;

            bp++;
        }
    }

    if(!(pattern->ch[0] & CLI_MATCH_IGNORE)) {
        bp = offset - pattern->prefix_length;
        if(pattern->ch_mindist[0] + (uint32_t) 1 > bp)
            return 0;

        bp -= pattern->ch_mindist[0] + 1;
        for(i = pattern->ch_mindist[0]; i <= pattern->ch_maxdist[0]; i++) {
            match = 1;
            AC_MATCH_CHAR(pattern->ch[0],buffer[bp]);
            if(match)
                break;

            if(!bp)
                return 0;
            else
                bp--;
        }
        if(!match)
            return 0;
    }

    return 1;
}

inline static int ac_findmatch(const unsigned char *buffer, uint32_t offset, uint32_t fileoffset, uint32_t length, const struct cli_ac_patt *pattern, uint32_t *end)
{
    int match;
    uint16_t specialcnt = pattern->special_pattern;

    if((offset + pattern->length > length) || (pattern->prefix_length > offset))
        return 0;

    match = ac_findmatch_branch(buffer, offset+pattern->depth, fileoffset, length, pattern, pattern->depth, specialcnt, end);
    if(match)
	return 1;
    return 0;
}

int cli_ac_initdata(struct cli_ac_data *data, uint32_t partsigs, uint32_t lsigs, uint32_t reloffsigs, uint8_t tracklen)
{
    unsigned int i, j;

    UNUSEDPARAM(tracklen);

    if(!data) {
        cli_errmsg("cli_ac_init: data == NULL\n");
        return CL_ENULLARG;
    }
    memset((void *)data, 0, sizeof(struct cli_ac_data));

    data->reloffsigs = reloffsigs;
    if(reloffsigs) {
        data->offset = (uint32_t *) cli_malloc(reloffsigs * 2 * sizeof(uint32_t));
        if(!data->offset) {
            cli_errmsg("cli_ac_init: Can't allocate memory for data->offset\n");
            return CL_EMEM;
        }
        for(i = 0; i < reloffsigs * 2; i += 2)
            data->offset[i] = CLI_OFF_NONE;
    }

    data->partsigs = partsigs;
    if(partsigs) {
        data->offmatrix = (int32_t ***) cli_calloc(partsigs, sizeof(int32_t **));
        if(!data->offmatrix) {
            cli_errmsg("cli_ac_init: Can't allocate memory for data->offmatrix\n");

            if(reloffsigs)
                free(data->offset);

            return CL_EMEM;
        }
    }
 
    data->lsigs = lsigs;
    if(lsigs) {
        data->lsigcnt = (uint32_t **) cli_malloc(lsigs * sizeof(uint32_t *));
        if(!data->lsigcnt) {
            if(partsigs)
                free(data->offmatrix);

            if(reloffsigs)
                free(data->offset);

            cli_errmsg("cli_ac_init: Can't allocate memory for data->lsigcnt\n");
            return CL_EMEM;
        }
        data->lsigcnt[0] = (uint32_t *) cli_calloc(lsigs * 64, sizeof(uint32_t));
        if(!data->lsigcnt[0]) {
            free(data->lsigcnt);
            if(partsigs)
                free(data->offmatrix);

            if(reloffsigs)
                free(data->offset);

            cli_errmsg("cli_ac_init: Can't allocate memory for data->lsigcnt[0]\n");
            return CL_EMEM;
        }
        for(i = 1; i < lsigs; i++)
            data->lsigcnt[i] = data->lsigcnt[0] + 64 * i;

        /* subsig offsets */
        data->lsig_matches = (struct cli_lsig_matches **) cli_calloc(lsigs * sizeof(struct cli_lsig_matches *), sizeof(struct cli_lsig_matches *));
        if(!data->lsig_matches) {
            free(data->lsigcnt[0]);
            free(data->lsigcnt);
            if(partsigs)
                free(data->offmatrix);

            if(reloffsigs)
                free(data->offset);

            cli_errmsg("cli_ac_init: Can't allocate memory for data->lsig_matches\n");
            return CL_EMEM;
        }
        data->lsigsuboff_last = (uint32_t **) cli_malloc(lsigs * sizeof(uint32_t *));
        data->lsigsuboff_first = (uint32_t **) cli_malloc(lsigs * sizeof(uint32_t *));
        if(!data->lsigsuboff_last || !data->lsigsuboff_first) {
            free(data->lsig_matches);
            free(data->lsigsuboff_last);
            free(data->lsigsuboff_first);
            free(data->lsigcnt[0]);
            free(data->lsigcnt);
            if(partsigs)
                free(data->offmatrix);

            if(reloffsigs)
                free(data->offset);

            cli_errmsg("cli_ac_init: Can't allocate memory for data->lsigsuboff_(last|first)\n");
            return CL_EMEM;
        }
        data->lsigsuboff_last[0] = (uint32_t *) cli_calloc(lsigs * 64, sizeof(uint32_t));
        data->lsigsuboff_first[0] = (uint32_t *) cli_calloc(lsigs * 64, sizeof(uint32_t));
        if(!data->lsigsuboff_last[0] || !data->lsigsuboff_first[0]) {
            free(data->lsig_matches);
            free(data->lsigsuboff_last[0]);
            free(data->lsigsuboff_first[0]);
            free(data->lsigsuboff_last);
            free(data->lsigsuboff_first);
            free(data->lsigcnt[0]);
            free(data->lsigcnt);
            if(partsigs)
                free(data->offmatrix);

            if(reloffsigs)
                free(data->offset);

            cli_errmsg("cli_ac_init: Can't allocate memory for data->lsigsuboff_(last|first)[0]\n");
            return CL_EMEM;
        }
        for(j = 0; j < 64; j++) {
            data->lsigsuboff_last[0][j] = CLI_OFF_NONE;
            data->lsigsuboff_first[0][j] = CLI_OFF_NONE;
        }
        for(i = 1; i < lsigs; i++) {
            data->lsigsuboff_last[i] = data->lsigsuboff_last[0] + 64 * i;
            data->lsigsuboff_first[i] = data->lsigsuboff_first[0] + 64 * i;
            for(j = 0; j < 64; j++) {
                data->lsigsuboff_last[i][j] = CLI_OFF_NONE;
                data->lsigsuboff_first[i][j] = CLI_OFF_NONE;
            }
        }
    }
    for (i=0;i<32;i++)
        data->macro_lastmatch[i] = CLI_OFF_NONE;

    data->min_partno = 1;

    return CL_SUCCESS;
}

int cli_ac_caloff(const struct cli_matcher *root, struct cli_ac_data *data, const struct cli_target_info *info)
{
    int ret;
    unsigned int i;
    struct cli_ac_patt *patt;

    if(info)
        data->vinfo = &info->exeinfo.vinfo;

    for(i = 0; i < root->ac_reloff_num; i++) {
        patt = root->ac_reloff[i];
        if(!info) {
            data->offset[patt->offset_min] = CLI_OFF_NONE;
        } else if((ret = cli_caloff(NULL, info, root->type, patt->offdata, &data->offset[patt->offset_min], &data->offset[patt->offset_max]))) {
            cli_errmsg("cli_ac_caloff: Can't calculate relative offset in signature for %s\n", patt->virname);
            return ret;
        } else if((data->offset[patt->offset_min] != CLI_OFF_NONE) && (data->offset[patt->offset_min] + patt->length > info->fsize)) {
            data->offset[patt->offset_min] = CLI_OFF_NONE;
        }
    }

    return CL_SUCCESS;
}

void cli_ac_freedata(struct cli_ac_data *data)
{
    uint32_t i;

    if (!data)
        return;

    if(data->partsigs) {
        for(i = 0; i < data->partsigs; i++) {
            if(data->offmatrix[i]) {
                free(data->offmatrix[i][0]);
                free(data->offmatrix[i]);
            }
        }
        free(data->offmatrix);
        data->offmatrix = NULL;
        data->partsigs = 0;
    }

    if(data->lsigs) {
        if (data->lsig_matches) {
            for (i = 0; i < data->lsigs; i++) {
                struct cli_lsig_matches * ls_matches;
                if ((ls_matches = data->lsig_matches[i])) {
                    uint32_t j;
                    for (j = 0; j < ls_matches->subsigs; j++) {
                        if (ls_matches->matches[j]) {
                            free(ls_matches->matches[j]);
                            ls_matches->matches[j] = 0;
                        }
                    }
                    free(data->lsig_matches[i]);
                    data->lsig_matches[i] = 0;
                }
            }
            free(data->lsig_matches);
            data->lsig_matches = 0;
        }
        free(data->lsigcnt[0]);
        free(data->lsigcnt);
        free(data->lsigsuboff_last[0]);
        free(data->lsigsuboff_last);
        free(data->lsigsuboff_first[0]);
        free(data->lsigsuboff_first);
        data->lsigs = 0;
    }

    if(data->reloffsigs) {
        free(data->offset);
        data->reloffsigs = 0;
    }
}

/* returns only CL_SUCCESS or CL_EMEM */
inline static int ac_addtype(struct cli_matched_type **list, cli_file_t type, off_t offset, const cli_ctx *ctx)
{
    struct cli_matched_type *tnode, *tnode_last;


    if(type == CL_TYPE_ZIPSFX) {
        if(*list && ctx && ctx->engine->maxfiles && (*list)->cnt > ctx->engine->maxfiles)
            return CL_SUCCESS;
    } else if(*list && (*list)->cnt >= MAX_EMBEDDED_OBJ) {
        return CL_SUCCESS;
    }

    if(!(tnode = cli_calloc(1, sizeof(struct cli_matched_type)))) {
        cli_errmsg("cli_ac_addtype: Can't allocate memory for new type node\n");
        return CL_EMEM;
    }

    tnode->type = type;
    tnode->offset = offset;

    tnode_last = *list;
    while(tnode_last && tnode_last->next)
        tnode_last = tnode_last->next;

    if(tnode_last)
        tnode_last->next = tnode;
    else
        *list = tnode;

    (*list)->cnt++;
    return CL_SUCCESS;
}

int lsig_sub_matched(const struct cli_matcher *root, struct cli_ac_data *mdata, uint32_t lsigid1, uint32_t lsigid2, uint32_t realoff, int partial)
{
    const struct cli_ac_lsig *ac_lsig = root->ac_lsigtable[lsigid1];
    const struct cli_lsig_tdb *tdb = &ac_lsig->tdb;

    if(realoff != CLI_OFF_NONE) {
        if(mdata->lsigsuboff_first[lsigid1][lsigid2] == CLI_OFF_NONE)
            mdata->lsigsuboff_first[lsigid1][lsigid2] = realoff;

        if(mdata->lsigsuboff_last[lsigid1][lsigid2] != CLI_OFF_NONE && ((!partial && realoff <= mdata->lsigsuboff_last[lsigid1][lsigid2]) || (partial && realoff < mdata->lsigsuboff_last[lsigid1][lsigid2])))
            return CL_SUCCESS;

        mdata->lsigcnt[lsigid1][lsigid2]++;
        if(mdata->lsigcnt[lsigid1][lsigid2] <= 1 || !tdb->macro_ptids || !tdb->macro_ptids[lsigid2])
            mdata->lsigsuboff_last[lsigid1][lsigid2] = realoff;
    }

    if (ac_lsig->type & CLI_YARA_OFFSET && realoff != CLI_OFF_NONE) {
        uint32_t * offs;
        struct cli_subsig_matches * ss_matches;
        struct cli_lsig_matches * ls_matches;
        cli_dbgmsg("lsig_sub_matched lsig %u:%u at %u\n", lsigid1, lsigid2, realoff);

        ls_matches = mdata->lsig_matches[lsigid1];
        if (ls_matches == NULL) { /* allocate cli_lsig_matches */
            ls_matches = mdata->lsig_matches[lsigid1] = (struct cli_lsig_matches *)cli_calloc(1, sizeof(struct cli_lsig_matches) +
                                                                                              (ac_lsig->tdb.subsigs - 1) * sizeof(struct cli_subsig_matches *));
            if (ls_matches == NULL) {
                cli_errmsg("lsig_sub_matched: cli_calloc failed for cli_lsig_matches\n");
                return CL_EMEM;
            }
            ls_matches->subsigs = ac_lsig->tdb.subsigs;
        }
        ss_matches = ls_matches->matches[lsigid2];
        if (ss_matches == NULL) { /*  allocate cli_subsig_matches */
            ss_matches = ls_matches->matches[lsigid2] = cli_malloc(sizeof(struct cli_subsig_matches));
            if (ss_matches == NULL) {
                cli_errmsg("lsig_sub_matched: cli_malloc failed for cli_subsig_matches struct\n");
                return CL_EMEM;
            }
            ss_matches->next = 0;
            ss_matches->last = sizeof(ss_matches->offsets) / sizeof(uint32_t) - 1; 
        }
        if (ss_matches->next > ss_matches->last) {  /* cli_matches out of space? realloc */
            ss_matches = ls_matches->matches[lsigid2] = cli_realloc(ss_matches, sizeof(struct cli_subsig_matches) + sizeof(uint32_t) * ss_matches->last * 2);
            if (ss_matches == NULL) {
                cli_errmsg("lsig_sub_matched: cli_realloc failed for cli_subsig_matches struct\n");
                return CL_EMEM;
            }
            ss_matches->last = sizeof(ss_matches->offsets)/ sizeof(uint32_t) + ss_matches->last * 2 - 1;
        }
        
        ss_matches->offsets[ss_matches->next] = realoff; /* finally, store the offset */
        ss_matches->next++;
    }

    if (mdata->lsigcnt[lsigid1][lsigid2] > 1) {
        /* Check that the previous match had a macro match following it at the 
         * correct distance. This check is only done after the 1st match.*/
        const struct cli_ac_patt *macropt;
        uint32_t id, last_macro_match, smin, smax, last_macroprev_match;

        if (!tdb->macro_ptids)
            return CL_SUCCESS;

        id = tdb->macro_ptids[lsigid2];
        if (!id)
            return CL_SUCCESS;

        macropt = root->ac_pattable[id];
        smin = macropt->ch_mindist[0];
        smax = macropt->ch_maxdist[0];
        /* start of last macro match */
        last_macro_match = mdata->macro_lastmatch[macropt->sigid];
        /* start of previous lsig subsig match */
        last_macroprev_match = mdata->lsigsuboff_last[lsigid1][lsigid2];
        if (last_macro_match != CLI_OFF_NONE)
            cli_dbgmsg("Checking macro match: %u + (%u - %u) == %u\n",
                   last_macroprev_match, smin, smax, last_macro_match);

        if (last_macro_match == CLI_OFF_NONE ||
            last_macroprev_match + smin > last_macro_match ||
            last_macroprev_match + smax < last_macro_match) {
            cli_dbgmsg("Canceled false lsig macro match\n");
            /* Previous match was false - cancel it */
            mdata->lsigcnt[lsigid1][lsigid2]--;
            mdata->lsigsuboff_last[lsigid1][lsigid2] = realoff;
        } else {
            /* mark the macro sig itself matched */
            mdata->lsigcnt[lsigid1][lsigid2+1]++;
            mdata->lsigsuboff_last[lsigid1][lsigid2+1] = last_macro_match;
        }
    }
    return CL_SUCCESS;
}

int cli_ac_chkmacro(struct cli_matcher *root, struct cli_ac_data *data, unsigned lsigid1)
{
    const struct cli_lsig_tdb *tdb = &root->ac_lsigtable[lsigid1]->tdb;
    unsigned i;
    int rc;

    /* Loop through all subsigs, and if they are tied to macros check that the
     * macro matched at a correct distance */
    for (i=0;i<tdb->subsigs;i++) {
        rc = lsig_sub_matched(root, data, lsigid1, i, CLI_OFF_NONE, 0);
        if (rc != CL_SUCCESS)
            return rc;
    }
    return CL_SUCCESS;
}


int cli_ac_scanbuff(const unsigned char *buffer, uint32_t length, const char **virname, void **customdata, struct cli_ac_result **res, const struct cli_matcher *root, struct cli_ac_data *mdata, uint32_t offset, cli_file_t ftype, struct cli_matched_type **ftoffset, unsigned int mode, cli_ctx *ctx)
{
    struct cli_ac_node *current;
    struct cli_ac_list *pattN, *ptN;
    struct cli_ac_patt *patt, *pt;
    uint32_t i, bp, realoff, matchend;
    uint16_t j;
    uint8_t found, viruses_found = 0;
    int32_t **offmatrix, swp;
    int type = CL_CLEAN;
    struct cli_ac_result *newres;
    int rc;

    if(!root->ac_root)
        return CL_CLEAN;

    if(!mdata && (root->ac_partsigs || root->ac_lsigs || root->ac_reloff_num)) {
        cli_errmsg("cli_ac_scanbuff: mdata == NULL\n");
        return CL_ENULLARG;
    }

    current = root->ac_root;

    for(i = 0; i < length; i++)  {
        current = current->trans[buffer[i]];

        if(UNLIKELY(IS_FINAL(current))) {
            struct cli_ac_list *faillist = current->fail->list;
            pattN = current->list;
            while(pattN) {
                patt = pattN->me;
                if(patt->partno > mdata->min_partno) {
                    pattN = faillist;
                    faillist = NULL;
                    continue;
                }
                bp = i + 1 - patt->depth;
                if(patt->offdata[0] != CLI_OFF_VERSION && patt->offdata[0] != CLI_OFF_MACRO && !pattN->next_same && (patt->offset_min != CLI_OFF_ANY) && (!patt->sigid || patt->partno == 1)) {
                    if(patt->offset_min == CLI_OFF_NONE) {
                        pattN = pattN->next;
                        continue;
                    }
                    realoff = offset + bp - patt->prefix_length;
                    if(patt->offdata[0] == CLI_OFF_ABSOLUTE) {
                        if(patt->offset_max < realoff || patt->offset_min > realoff) {
                            pattN = pattN->next;
                            continue;
                        }
                    } else {
                        if(mdata->offset[patt->offset_min] == CLI_OFF_NONE || mdata->offset[patt->offset_max] < realoff || mdata->offset[patt->offset_min] > realoff) {
                            pattN = pattN->next;
                            continue;
                        }
                    }
                }

                ptN = pattN;
                if(ac_findmatch(buffer, bp, offset + bp - patt->prefix_length, length, patt, &matchend)) {
                    while(ptN) {
                        pt = ptN->me;
                        if(pt->partno > mdata->min_partno)
                            break;

                        if((pt->type && !(mode & AC_SCAN_FT)) || (!pt->type && !(mode & AC_SCAN_VIR))) {
                            ptN = ptN->next_same;
                            continue;
                        }

                        realoff = offset + bp - pt->prefix_length;
                        if(pt->offdata[0] == CLI_OFF_VERSION) {
                            if(!cli_hashset_contains_maybe_noalloc(mdata->vinfo, realoff)) {
                                ptN = ptN->next_same;
                                continue;
                            }
                            cli_dbgmsg("cli_ac_scanbuff: VI match for offset %x\n", realoff);
                        } else if(pt->offdata[0] == CLI_OFF_MACRO) {
                            mdata->macro_lastmatch[patt->offdata[1]] = realoff;
                            ptN = ptN->next_same;
                            continue;
                        } else if(pt->offset_min != CLI_OFF_ANY && (!pt->sigid || pt->partno == 1)) {
                            if(pt->offset_min == CLI_OFF_NONE) {
                                ptN = ptN->next_same;
                                continue;
                            }
                            if(pt->offdata[0] == CLI_OFF_ABSOLUTE) {
                                if(pt->offset_max < realoff || pt->offset_min > realoff) {
                                    ptN = ptN->next_same;
                                    continue;
                                }
                            } else {
                                if(mdata->offset[pt->offset_min] == CLI_OFF_NONE || mdata->offset[pt->offset_max] < realoff || mdata->offset[pt->offset_min] > realoff) {
                                    ptN = ptN->next_same;
                                    continue;
                                }
                            }
                        }

                        if(pt->sigid) { /* it's a partial signature */

                            /* if 2nd or later part, confirm some prior part has matched */
                            if(pt->partno != 1 && (!mdata->offmatrix[pt->sigid - 1] || !mdata->offmatrix[pt->sigid - 1][pt->partno - 2][0])) {
                                ptN = ptN->next_same;
                                continue;
                            }

                            if(pt->partno + 1 > mdata->min_partno)
                                mdata->min_partno = pt->partno + 1;

                            /* sparsely populated matrix, so allocate and initialize if NULL */
                            if(!mdata->offmatrix[pt->sigid - 1]) {
                                mdata->offmatrix[pt->sigid - 1] = cli_malloc(pt->parts * sizeof(int32_t *));
                                if(!mdata->offmatrix[pt->sigid - 1]) {
                                    cli_errmsg("cli_ac_scanbuff: Can't allocate memory for mdata->offmatrix[%u]\n", pt->sigid - 1);
                                    return CL_EMEM;
                                }

                                mdata->offmatrix[pt->sigid - 1][0] = cli_malloc(pt->parts * (CLI_DEFAULT_AC_TRACKLEN + 2) * sizeof(int32_t));
                                if(!mdata->offmatrix[pt->sigid - 1][0]) {
                                    cli_errmsg("cli_ac_scanbuff: Can't allocate memory for mdata->offmatrix[%u][0]\n", pt->sigid - 1);
                                    free(mdata->offmatrix[pt->sigid - 1]);
                                    mdata->offmatrix[pt->sigid - 1] = NULL;
                                    return CL_EMEM;
                                }
                                memset(mdata->offmatrix[pt->sigid - 1][0], -1, pt->parts * (CLI_DEFAULT_AC_TRACKLEN + 2) * sizeof(int32_t));
                                mdata->offmatrix[pt->sigid - 1][0][0] = 0;
                                for(j = 1; j < pt->parts; j++) {
                                    mdata->offmatrix[pt->sigid - 1][j] = mdata->offmatrix[pt->sigid - 1][0] + j * (CLI_DEFAULT_AC_TRACKLEN + 2);
                                    mdata->offmatrix[pt->sigid - 1][j][0] = 0;
                                }
                            }
                            offmatrix = mdata->offmatrix[pt->sigid - 1];

                            found = 0;
                            if(pt->partno != 1) {
                                for(j = 1; j <= CLI_DEFAULT_AC_TRACKLEN + 1 && offmatrix[pt->partno - 2][j] != -1; j++) {
                                    found = j;
                                    if(pt->maxdist)
                                        if(realoff - offmatrix[pt->partno - 2][j] > pt->maxdist)
                                            found = 0;

                                    if(found && pt->mindist)
                                        if(realoff - offmatrix[pt->partno - 2][j] < pt->mindist)
                                            found = 0;

                                    if(found)
                                        break;
                                }
                            }

                            if(pt->partno == 2 && found > 1) {
                                swp = offmatrix[0][1];
                                offmatrix[0][1] = offmatrix[0][found];
                                offmatrix[0][found] = swp;

                                if(pt->type != CL_TYPE_MSEXE) {
                                    swp = offmatrix[pt->parts - 1][1];
                                    offmatrix[pt->parts - 1][1] = offmatrix[pt->parts - 1][found];
                                    offmatrix[pt->parts - 1][found] = swp;
                                }
                            }

                            if(pt->partno == 1 || (found && (pt->partno != pt->parts))) {
                                if(offmatrix[pt->partno - 1][0] == CLI_DEFAULT_AC_TRACKLEN + 1)
                                    offmatrix[pt->partno - 1][0] = 1; /* wrap, ends up at 2 */
                                offmatrix[pt->partno - 1][0]++;
                                offmatrix[pt->partno - 1][offmatrix[pt->partno - 1][0]] = offset + matchend;

                                if(pt->partno == 1) /* save realoff for the first part */
                                    offmatrix[pt->parts - 1][offmatrix[pt->partno - 1][0]] = realoff;
                            } else if(found && pt->partno == pt->parts) {
                                if(pt->type) {

                                    if(pt->type == CL_TYPE_IGNORED && (!pt->rtype || ftype == pt->rtype))
                                        return CL_TYPE_IGNORED;

                                    if((pt->type > type || pt->type >= CL_TYPE_SFX || pt->type == CL_TYPE_MSEXE) && (!pt->rtype || ftype == pt->rtype)) {
                                        cli_dbgmsg("Matched signature for file type %s\n", pt->virname);
                                        type = pt->type;
                                        if(ftoffset && (!*ftoffset || (*ftoffset)->cnt < MAX_EMBEDDED_OBJ || type == CL_TYPE_ZIPSFX) && (type >= CL_TYPE_SFX || ((ftype == CL_TYPE_MSEXE || ftype == CL_TYPE_ZIP || ftype == CL_TYPE_MSOLE2) && type == CL_TYPE_MSEXE)))  {
                                            /* FIXME: the first offset in the array is most likely the correct one but
                                             * it may happen it is not
                                             */
                                            for(j = 1; j <= CLI_DEFAULT_AC_TRACKLEN + 1 && offmatrix[0][j] != -1; j++)
                                                if(ac_addtype(ftoffset, type, offmatrix[pt->parts - 1][j], ctx))
                                                    return CL_EMEM;
                                        }

                                        memset(offmatrix[0], -1, pt->parts * (CLI_DEFAULT_AC_TRACKLEN + 2) * sizeof(int32_t));
                                        for(j = 0; j < pt->parts; j++)
                                            offmatrix[j][0] = 0;
                                    }

                                } else { /* !pt->type */
                                    if(pt->lsigid[0]) {
                                        rc = lsig_sub_matched(root, mdata, pt->lsigid[1], pt->lsigid[2], offmatrix[pt->parts - 1][1], 1);
                                        if (rc != CL_SUCCESS)
                                            return rc;
                                        ptN = ptN->next_same;
                                        continue;
                                    }

                                    if(res) {
                                        newres = (struct cli_ac_result *) malloc(sizeof(struct cli_ac_result));
                                        if(!newres) {
                                            cli_errmsg("cli_ac_scanbuff: Can't allocate memory for newres %lu\n", (unsigned long)sizeof(struct cli_ac_result));
                                            return CL_EMEM;
                                        }
                                        newres->virname = pt->virname;
                                        newres->customdata = pt->customdata;
                                        newres->next = *res;
                                        newres->offset = offmatrix[pt->parts - 1][1];
                                        *res = newres;

                                        ptN = ptN->next_same;
                                        continue;
                                    } else {
                                        if(ctx && SCAN_ALL) {
                                            cli_append_virus(ctx, (const char *)pt->virname);
                                            viruses_found = 1;
                                        }
                                        if (virname)
                                            *virname = pt->virname;
                                        if(customdata)
                                            *customdata = pt->customdata;
                                        if (!ctx || !SCAN_ALL)
                                            return CL_VIRUS;
                                        ptN = ptN->next_same;
                                        continue;
                                    }
                                }
                            }

                        } else { /* old type signature */
                            if(pt->type) {
                                if(pt->type == CL_TYPE_IGNORED && (!pt->rtype || ftype == pt->rtype))
                                    return CL_TYPE_IGNORED;

                                if((pt->type > type || pt->type >= CL_TYPE_SFX || pt->type == CL_TYPE_MSEXE) && (!pt->rtype || ftype == pt->rtype)) {

                                    cli_dbgmsg("Matched signature for file type %s at %u\n", pt->virname, realoff);
                                    type = pt->type;
                                    if(ftoffset && (!*ftoffset || (*ftoffset)->cnt < MAX_EMBEDDED_OBJ || type == CL_TYPE_ZIPSFX) && (type == CL_TYPE_MBR || type >= CL_TYPE_SFX || ((ftype == CL_TYPE_MSEXE || ftype == CL_TYPE_ZIP || ftype == CL_TYPE_MSOLE2) && type == CL_TYPE_MSEXE)))  {

                                        if(ac_addtype(ftoffset, type, realoff, ctx))
                                            return CL_EMEM;
                                    }
                                }
                            } else {
                                if(pt->lsigid[0]) {
                                    rc = lsig_sub_matched(root, mdata, pt->lsigid[1], pt->lsigid[2], realoff, 0);
                                    if (rc != CL_SUCCESS)
                                        return rc;
                                    ptN = ptN->next_same;
                                    continue;
                                }

                                if(res) {
                                    newres = (struct cli_ac_result *) malloc(sizeof(struct cli_ac_result));
                                    if(!newres) {
                                        cli_errmsg("cli_ac_scanbuff: Can't allocate memory for newres %lu\n", (unsigned long)sizeof(struct cli_ac_result));
                                        return CL_EMEM;
                                    }
                                    newres->virname = pt->virname;
                                    newres->customdata = pt->customdata;
                                    newres->offset = realoff;
                                    newres->next = *res;
                                    *res = newres;

                                    ptN = ptN->next_same;
                                    continue;
                                } else {
                                    if(ctx && SCAN_ALL) {
                                        cli_append_virus(ctx, (const char *)pt->virname);
                                        viruses_found = 1;
                                    }

                                    if (virname)
                                        *virname = pt->virname;

                                    if(customdata)
                                        *customdata = pt->customdata;

                                    if (!ctx || !SCAN_ALL)
                                        return CL_VIRUS;

                                    ptN = ptN->next_same;
                                    continue;
                                }
                            }
                        }
                        ptN = ptN->next_same;
                    }
                }
                pattN = pattN->next;
            }
        }
    }

    if (viruses_found)
        return CL_VIRUS;

    return (mode & AC_SCAN_FT) ? type : CL_CLEAN;
}

static int qcompare(const void *a, const void *b)
{
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

/* FIXME: clean up the code */
int cli_ac_addsig(struct cli_matcher *root, const char *virname, const char *hexsig, uint8_t sigopts, uint32_t sigid, uint16_t parts, uint16_t partno, uint16_t rtype, uint16_t type, uint32_t mindist, uint32_t maxdist, const char *offset, const uint32_t *lsigid, unsigned int options)
{
    struct cli_ac_patt *new;
    char *pt, *pt2, *hex = NULL, *hexcpy = NULL;
    uint16_t i, j, ppos = 0, pend, *dec, nzpos = 0;
    uint8_t wprefix = 0, zprefix = 1, plen = 0, nzplen = 0;
    struct cli_ac_special *newspecial, *specialpt, **newtable;
    int ret, error = CL_SUCCESS;


    if(!root) {
        cli_errmsg("cli_ac_addsig: root == NULL\n");
        return CL_ENULLARG;
    }

    if(strlen(hexsig) / 2 < root->ac_mindepth) {
        cli_errmsg("cli_ac_addsig: Signature for %s is too short\n", virname);
        return CL_EMALFDB;
    }

    if((new = (struct cli_ac_patt *) mpool_calloc(root->mempool, 1, sizeof(struct cli_ac_patt))) == NULL)
        return CL_EMEM;

    new->rtype = rtype;
    new->type = type;
    new->sigid = sigid;
    new->parts = parts;
    new->partno = partno;
    new->mindist = mindist;
    new->maxdist = maxdist;
    new->customdata = NULL;
    new->ch[0] |= CLI_MATCH_IGNORE;
    new->ch[1] |= CLI_MATCH_IGNORE;
    if(lsigid) {
        new->lsigid[0] = 1;
        memcpy(&new->lsigid[1], lsigid, 2 * sizeof(uint32_t));
    }

    if(strchr(hexsig, '[')) {
        if(!(hexcpy = cli_strdup(hexsig))) {
            mpool_free(root->mempool, new);
            return CL_EMEM;
        }

        hex = hexcpy;
        for(i = 0; i < 2; i++) {
            unsigned int n, n1, n2;

            if(!(pt = strchr(hex, '[')))
                break;

            *pt++ = 0;

            if(!(pt2 = strchr(pt, ']'))) {
                cli_dbgmsg("cli_ac_addsig: missing closing square bracket\n");
                error = CL_EMALFDB;
                break;
            }

            *pt2++ = 0;

            n = sscanf(pt, "%u-%u", &n1, &n2);
            if(n == 1) {
                n2 = n1;
            } else if(n != 2) {
                cli_dbgmsg("cli_ac_addsig: incorrect range inside square brackets\n");
                error = CL_EMALFDB;
                break;
            }

            if((n1 > n2) || (n2 > AC_CH_MAXDIST)) {
                cli_dbgmsg("cli_ac_addsig: incorrect range inside square brackets\n");
                error = CL_EMALFDB;
                break;
            }

            if(strlen(hex) == 2) {
                if(i) {
                    error = CL_EMALFDB;
                    break;
                }

                dec = cli_hex2ui(hex);
                if(!dec) {
                    error = CL_EMALFDB;
                    break;
                }

                if((sigopts & ACPATT_OPTION_NOCASE) && ((*dec & CLI_MATCH_METADATA) == CLI_MATCH_CHAR))
                    new->ch[i] = cli_nocase(*dec) | CLI_MATCH_NOCASE;
                else
                    new->ch[i] = *dec;
                free(dec);
                new->ch_mindist[i] = n1;
                new->ch_maxdist[i] = n2;
                hex = pt2;
            } else if(strlen(pt2) == 2) {
                i = 1;
                dec = cli_hex2ui(pt2);
                if(!dec) {
                    error = CL_EMALFDB;
                    break;
                }

                if((sigopts & ACPATT_OPTION_NOCASE) && ((*dec & CLI_MATCH_METADATA) == CLI_MATCH_CHAR))
                    new->ch[i] = cli_nocase(*dec) | CLI_MATCH_NOCASE;
                else
                    new->ch[i] = *dec;
                free(dec);
                new->ch_mindist[i] = n1;
                new->ch_maxdist[i] = n2;
            } else {
                error = CL_EMALFDB;
                break;
            }
        }

        if(error) {
            free(hexcpy);
            mpool_free(root->mempool, new);
            return error;
        }

        hex = cli_strdup(hex);
        free(hexcpy);
        if(!hex) {
            mpool_free(root->mempool, new);
            return CL_EMEM;
        }
    }

    if(strchr(hexsig, '(')) {
	    char *hexnew, *start, *h, *c;
        size_t hexnewsz;

	if(hex) {
	    hexcpy = hex;
	} else if(!(hexcpy = cli_strdup(hexsig))) {
	    mpool_free(root->mempool, new);
	    return CL_EMEM;
	}

    hexnewsz = strlen(hexsig) + 1;
	if(!(hexnew = (char *) cli_calloc(1, hexnewsz))) {
	    free(new);
	    free(hexcpy);
	    return CL_EMEM;
	}

	start = pt = hexcpy;
	while((pt = strchr(start, '('))) {
	    *pt++ = 0;

	    if(!start) {
		error = CL_EMALFDB;
		break;
	    }
	    newspecial = (struct cli_ac_special *) mpool_calloc(root->mempool, 1, sizeof(struct cli_ac_special));
	    if(!newspecial) {
		cli_errmsg("cli_ac_addsig: Can't allocate newspecial\n");
		error = CL_EMEM;
		break;
	    }
	    if(pt >= hexcpy + 2) {
		if(pt[-2] == '!') {
		    newspecial->negative = 1;
		    pt[-2] = 0;
		}
	    }
	    cli_strlcat(hexnew, start, hexnewsz);

	    if(!(start = strchr(pt, ')'))) {
		mpool_free(root->mempool, newspecial);
		error = CL_EMALFDB;
		break;
	    }
	    *start++ = 0;
	    if(!strlen(pt)) {
		cli_errmsg("cli_ac_addsig: Empty block\n");
		error = CL_EMALFDB;
		break;
	    }

	    if(!strcmp(pt, "B")) {
		if(!*start) {
		    new->boundary |= AC_BOUNDARY_RIGHT;
		    if(newspecial->negative)
			new->boundary |= AC_BOUNDARY_RIGHT_NEGATIVE;
		    mpool_free(root->mempool, newspecial);
		    continue;
		} else if(pt - 1 == hexcpy) {
		    new->boundary |= AC_BOUNDARY_LEFT;
		    if(newspecial->negative)
			new->boundary |= AC_BOUNDARY_LEFT_NEGATIVE;
		    mpool_free(root->mempool, newspecial);
		    continue;
		}
	    } else if(!strcmp(pt, "L")) {
		if(!*start) {
		    new->boundary |= AC_LINE_MARKER_RIGHT;
		    if(newspecial->negative)
			new->boundary |= AC_LINE_MARKER_RIGHT_NEGATIVE;
		    mpool_free(root->mempool, newspecial);
		    continue;
		} else if(pt - 1 == hexcpy) {
		    new->boundary |= AC_LINE_MARKER_LEFT;
		    if(newspecial->negative)
			new->boundary |= AC_LINE_MARKER_LEFT_NEGATIVE;
		    mpool_free(root->mempool, newspecial);
		    continue;
		}
	    } else if(!strcmp(pt, "W")) {
		if(!*start) {
		    new->boundary |= AC_WORD_MARKER_RIGHT;
		    if(newspecial->negative)
			new->boundary |= AC_WORD_MARKER_RIGHT_NEGATIVE;
		    mpool_free(root->mempool, newspecial);
		    continue;
		} else if(pt - 1 == hexcpy) {
		    new->boundary |= AC_WORD_MARKER_LEFT;
		    if(newspecial->negative)
			new->boundary |= AC_WORD_MARKER_LEFT_NEGATIVE;
		    mpool_free(root->mempool, newspecial);
		    continue;
		}
	    }
	    cli_strlcat(hexnew, "()", hexnewsz);
	    new->special++;
	    newtable = (struct cli_ac_special **) mpool_realloc(root->mempool, new->special_table, new->special * sizeof(struct cli_ac_special *));
	    if(!newtable) {
		new->special--;
		mpool_free(root->mempool, newspecial);
		cli_errmsg("cli_ac_addsig: Can't realloc new->special_table\n");
		error = CL_EMEM;
		break;
	    }
	    newtable[new->special - 1] = newspecial;
	    new->special_table = newtable;

	    if(!strcmp(pt, "B")) {
		newspecial->type = AC_SPECIAL_BOUNDARY;
	    } else if(!strcmp(pt, "L")) {
		newspecial->type = AC_SPECIAL_LINE_MARKER;
	    } else if(!strcmp(pt, "W")) {
		newspecial->type = AC_SPECIAL_WORD_MARKER;
	    } else {
		newspecial->num = 1;
		for(i = 0; i < strlen(pt); i++)
		    if(pt[i] == '|')
			newspecial->num++;

		if(3 * newspecial->num - 1 == (uint16_t) strlen(pt)) {
		    newspecial->type = AC_SPECIAL_ALT_CHAR;
		    newspecial->str = (unsigned char *) mpool_malloc(root->mempool, newspecial->num);
		    if(!newspecial->str) {
			cli_errmsg("cli_ac_addsig: Can't allocate newspecial->str\n");
			error = CL_EMEM;
			break;
		    }
		} else {
		    newspecial->type = AC_SPECIAL_ALT_STR;
		}

		for(i = 0; i < newspecial->num; i++) {
			unsigned int clen;

		    if(newspecial->num == 1) {
			c = (char *) cli_mpool_hex2str(root->mempool, pt);
			clen = strlen(pt) / 2;
		    } else {
			if(!(h = cli_strtok(pt, i, "|"))) {
			    error = CL_EMEM;
			    break;
			}
			c = (char *) cli_mpool_hex2str(root->mempool, h);
			clen = strlen(h) / 2;
			free(h);
		    }
		    if(!c) {
			error = CL_EMALFDB;
			break;
		    }

		    if(newspecial->type == AC_SPECIAL_ALT_CHAR) {
			newspecial->str[i] = *c;
			mpool_free(root->mempool, c);
		    } else {
			if(i) {
			    specialpt = newspecial;
			    while(specialpt->next)
				specialpt = specialpt->next;

			    specialpt->next = (struct cli_ac_special *) mpool_calloc(root->mempool, 1, sizeof(struct cli_ac_special));
			    if(!specialpt->next) {
				cli_errmsg("cli_ac_addsig: Can't allocate specialpt->next\n");
				error = CL_EMEM;
				free(c);
				break;
			    }
			    specialpt->next->str = (unsigned char *) c;
			    specialpt->next->len = clen;
			} else {
			    newspecial->str = (unsigned char *) c;
			    newspecial->len = clen;
			}
		    }
		}
		if(newspecial->num > 1 && newspecial->type == AC_SPECIAL_ALT_CHAR)
		    cli_qsort(newspecial->str, newspecial->num, sizeof(unsigned char), qcompare);

		if(error)
		    break;
	    }
	}

	if(start)
	    cli_strlcat(hexnew, start, hexnewsz);

	hex = hexnew;
	free(hexcpy);

	if(error) {
	    free(hex);
	    if(new->special) {
		mpool_ac_free_special(root->mempool, new);
	    }
	    mpool_free(root->mempool, new);
	    return error;
	}
    }

    new->pattern = cli_mpool_hex2ui(root->mempool, hex ? hex : hexsig);
    if(new->pattern == NULL) {
        if(new->special)
            mpool_ac_free_special(root->mempool, new);

        mpool_free(root->mempool, new);
        free(hex);
        return CL_EMALFDB;
    }

    new->length = strlen(hex ? hex : hexsig) / 2;
    free(hex);

    new->sigopts = sigopts;
    /* setting nocase match; TODO - move this to cli_realhex2ui and adjust for nocase */
    if (sigopts & ACPATT_OPTION_NOCASE) {
	for (i = 0; i < new->length; ++i)
	    if ((new->pattern[i] & CLI_MATCH_METADATA) == CLI_MATCH_CHAR) {
		new->pattern[i] = cli_nocase(new->pattern[i] & 0xff);
		new->pattern[i] += CLI_MATCH_NOCASE;
	    }
    }

    if (root->filter) { //TODO - fix filters for nocase state, also fix for sigtool as well
        /* so that we can show meaningful messages */
        new->virname = (char*)virname;
        if (filter_add_acpatt(root->filter, new) == -1) {
            cli_warnmsg("cli_ac_addpatt: cannot use filter for trie\n");
            mpool_free(root->mempool, root->filter);
            root->filter = NULL;
        }

        /* TODO: should this affect maxpatlen? */
    }

    for(i = 0; i < root->ac_maxdepth && i < new->length; i++) {
        if(new->pattern[i] & CLI_MATCH_WILDCARD) {
            wprefix = 1;
            break;
        }

        if(zprefix && new->pattern[i])
            zprefix = 0;
    }

    if(wprefix || zprefix) {
        pend = new->length - root->ac_mindepth + 1;
        for(i = 0; i < pend; i++) {
            for(j = i; j < i + root->ac_maxdepth && j < new->length; j++) {
                if(new->pattern[j] & CLI_MATCH_WILDCARD) {
                    break;
                } else {
                    if(j - i + 1 >= plen) {
                        plen = j - i + 1;
                        ppos = i;
                    }
                }

                if(new->pattern[ppos] || new->pattern[ppos + 1]) {
                    if(plen >= root->ac_maxdepth) {
                        break;
                    } else if(plen >= root->ac_mindepth && plen > nzplen) {
                        nzplen = plen;
                        nzpos = ppos;
                    }
                }
            }

            if(plen >= root->ac_maxdepth && (new->pattern[ppos] || new->pattern[ppos + 1]))
                break;
        }

        if(!new->pattern[ppos] && !new->pattern[ppos + 1] && nzplen) {
            plen = nzplen;
            ppos = nzpos;
        }

        if(plen < root->ac_mindepth) {
            cli_errmsg("cli_ac_addsig: Can't find a static subpattern of length %u\n", root->ac_mindepth);
            mpool_ac_free_special(root->mempool, new);
            mpool_free(root->mempool, new->pattern);
            mpool_free(root->mempool, new);
            return CL_EMALFDB;
        }

        new->prefix = new->pattern;
        new->prefix_length = ppos;
        new->pattern = &new->prefix[ppos];
        new->length -= ppos;

        for(i = 0; i < new->prefix_length; i++)
            if((new->prefix[i] & CLI_MATCH_WILDCARD) == CLI_MATCH_SPECIAL)
                new->special_pattern++;
    }

    if(new->length + new->prefix_length > root->maxpatlen)
        root->maxpatlen = new->length + new->prefix_length;

    new->virname = cli_mpool_virname(root->mempool, virname, options & CL_DB_OFFICIAL);
    if(!new->virname) {
        mpool_free(root->mempool, new->prefix ? new->prefix : new->pattern);
        mpool_ac_free_special(root->mempool, new);
        mpool_free(root->mempool, new);
        return CL_EMEM;
    }

    if(new->lsigid[0])
        root->ac_lsigtable[new->lsigid[1]]->virname = new->virname;

    ret = cli_caloff(offset, NULL, root->type, new->offdata, &new->offset_min, &new->offset_max);
    if(ret != CL_SUCCESS) {
        mpool_free(root->mempool, new->prefix ? new->prefix : new->pattern);
        mpool_ac_free_special(root->mempool, new);
        mpool_free(root->mempool, new->virname);
        mpool_free(root->mempool, new);
        return ret;
    }

    if((ret = cli_ac_addpatt(root, new))) {
        mpool_free(root->mempool, new->prefix ? new->prefix : new->pattern);
        mpool_free(root->mempool, new->virname);
        mpool_ac_free_special(root->mempool, new);
        mpool_free(root->mempool, new);
        return ret;
    }

    if(new->offdata[0] != CLI_OFF_ANY && new->offdata[0] != CLI_OFF_ABSOLUTE && new->offdata[0] != CLI_OFF_MACRO) {
        root->ac_reloff = (struct cli_ac_patt **) mpool_realloc2(root->mempool, root->ac_reloff, (root->ac_reloff_num + 1) * sizeof(struct cli_ac_patt *));
        if(!root->ac_reloff) {
            cli_errmsg("cli_ac_addsig: Can't allocate memory for root->ac_reloff\n");
            return CL_EMEM;
        }

        root->ac_reloff[root->ac_reloff_num] = new;
        new->offset_min = root->ac_reloff_num * 2;
        new->offset_max = new->offset_min + 1;
        root->ac_reloff_num++;
    }

    return CL_SUCCESS;
}
