/*
 * Copyright (C) 2020 Collabora, Ltd.
 * Copyright (C) 2018-2019 Alyssa Rosenzweig <alyssa@rosenzweig.io>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "compiler.h"
#include "nodearray.h"
#include "util/u_memory.h"
#include "util/list.h"
#include "util/set.h"

/* Liveness analysis is a backwards-may dataflow analysis pass. Within a block,
 * we compute live_out from live_in. The intrablock pass is linear-time. It
 * returns whether progress was made. */

void
bi_liveness_ins_update(nodearray *live, bi_instr *ins, unsigned max)
{
        /* live_in[s] = GEN[s] + (live_out[s] - KILL[s]) */

        bi_foreach_dest(ins, d) {
                unsigned node = bi_get_node(ins->dest[d]);

                if (node < max)
                        nodearray_bic(live, node, bi_writemask(ins, d));
        }

        bi_foreach_src(ins, src) {
                unsigned count = bi_count_read_registers(ins, src);
                unsigned rmask = BITFIELD_MASK(count);
                uint8_t mask = (rmask << ins->src[src].offset);

                unsigned node = bi_get_node(ins->src[src]);
                if (node < max)
                        nodearray_orr(live, node, mask, ~0, ~0);
        }
}

static bool
liveness_block_update(bi_block *blk, unsigned temp_count)
{
        /* live_out[s] = sum { p in succ[s] } ( live_in[p] ) */
        bi_foreach_successor(blk, succ)
                nodearray_orr_array(&blk->live_out, &succ->live_in, ~0, ~0);

        nodearray live;
        nodearray_clone(&live, &blk->live_out);

        bi_foreach_instr_in_block_rev(blk, ins)
                bi_liveness_ins_update(&live, ins, temp_count);

        /* To figure out progress, diff live_in */
        bool progress = !nodearray_equal(&live, &blk->live_in);

        nodearray_reset(&blk->live_in);
        blk->live_in = live;

        return progress;
}

/* Globally, liveness analysis uses a fixed-point algorithm based on a
 * worklist. We initialize a work list with the exit block. We iterate the work
 * list to compute live_in from live_out for each block on the work list,
 * adding the predecessors of the block to the work list if we made progress.
 */

void
bi_compute_liveness(bi_context *ctx)
{
        if (ctx->has_liveness)
                return;

        unsigned temp_count = bi_max_temp(ctx);

        /* Set of bi_block */
        struct set *work_list = _mesa_set_create(NULL,
                        _mesa_hash_pointer,
                        _mesa_key_pointer_equal);

        struct set *visited = _mesa_set_create(NULL,
                        _mesa_hash_pointer,
                        _mesa_key_pointer_equal);

        bi_foreach_block(ctx, block) {
                nodearray_reset(&block->live_in);
                nodearray_reset(&block->live_out);
        }

        /* Initialize the work list with the exit block */
        struct set_entry *cur;

        cur = _mesa_set_add(work_list, bi_exit_block(&ctx->blocks));

        /* Iterate the work list */

        do {
                /* Pop off a block */
                bi_block *blk = (struct bi_block *) cur->key;
                _mesa_set_remove(work_list, cur);

                /* Update its liveness information */
                bool progress = liveness_block_update(blk, temp_count);

                /* If we made progress, we need to process the predecessors */

                if (progress || !_mesa_set_search(visited, blk)) {
                        bi_foreach_predecessor(blk, pred)
                                _mesa_set_add(work_list, *pred);
                }

                _mesa_set_add(visited, blk);
        } while((cur = _mesa_set_next_entry(work_list, NULL)) != NULL);

        _mesa_set_destroy(visited, NULL);
        _mesa_set_destroy(work_list, NULL);

        ctx->has_liveness = true;
}

/* Once liveness data is no longer valid, call this */

void
bi_invalidate_liveness(bi_context *ctx)
{
        ctx->has_liveness = false;
}
