/*
 * Copyright (c) 2021 Icecream95
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
 *
 */

#ifndef __PAN_CORE_H__
#define __PAN_CORE_H__

#include <stdint.h>
#include "util/u_dynarray.h"

enum pan_core_cmd_type { PAN_CORE_INSTR = 0, PAN_CORE_SYM };

struct Dwelf_Strent;

struct pan_core_cmd {
        enum pan_core_cmd_type type;
        union {
                uint64_t instr;
                struct {
                        const char *sym;
                        struct Dwelf_Strent *str;
                };
        };
};

struct pan_core_cmdlist {
        struct util_dynarray cmds;

        unsigned num_instr;
        unsigned num_sym;
};

struct pan_core;

struct pan_core *panfrost_core_create(int fd);

/* Add a range to a core file. If ptr and/or label are not NULL, they must be
 * valid until panfrost_core_finish is called. flags should be set to 1 for
 * an executable BO, 0 otherwise. */
void panfrost_core_add(struct pan_core *core, uint64_t va, size_t size,
                       void *ptr, const char *label, uint32_t flags);

/* Add a command list to be interpreted by replay tools */
void panfrost_core_add_cmdlist(struct pan_core *core,
                               struct pan_core_cmdlist *c);

void panfrost_core_finish(struct pan_core *core);

#endif
