/* bytecode.c */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "bytecode.h"

struct fh_bc {
  uint32_t *instr;
  uint32_t num_instr;
  uint32_t cap_instr;
  struct fh_stack funcs;
};

struct fh_bc *fh_new_bc(void)
{
  struct fh_bc *bc = malloc(sizeof(struct fh_bc));
  if (bc == NULL)
    return NULL;
  
  bc->instr = NULL;
  bc->num_instr = 0;
  bc->cap_instr = 0;

  fh_init_stack(&bc->funcs, sizeof(struct fh_bc_func));
  return bc;
}

static void free_bc_const(struct fh_bc_const *c)
{
  switch (c->type) {
  case FH_BC_CONST_NUMBER:
    return;

  case FH_BC_CONST_STRING:
    free(c->data.str);
    return;
  }

  fprintf(stderr, "ERROR: invalid constant type: %d\n", c->type);
}

static void free_bc_func(struct fh_bc_func *func)
{
  for (int i = 0; i < func->consts.num; i++) {
    struct fh_bc_const *c = fh_stack_item(&func->consts, i);
    free_bc_const(c);
  }
  fh_free_stack(&func->consts);
}

void fh_free_bc(struct fh_bc *bc)
{
  if (bc->instr)
    free(bc->instr);
  for (int i = 0; i < bc->funcs.num; i++) {
    struct fh_bc_func *f = fh_stack_item(&bc->funcs, i);
    free_bc_func(f);
  }
  fh_free_stack(&bc->funcs);
  free(bc);
}

uint32_t *fh_add_bc_instr(struct fh_bc *bc, struct fh_src_loc loc, uint32_t instr)
{
  if (bc->num_instr >= bc->cap_instr) {
    int32_t new_cap = (bc->cap_instr + 32 + 1) / 32 * 32;
    uint32_t *new_p = realloc(bc->instr, sizeof(uint32_t) * new_cap);
    if (! new_p)
      return NULL;
    bc->instr = new_p;
    bc->cap_instr = new_cap;
  }
  uint32_t pc = bc->num_instr;
  bc->instr[bc->num_instr++] = instr;
  return bc->instr + pc;
}

struct fh_bc_func *fh_add_bc_func(struct fh_bc *bc, struct fh_src_loc loc, int n_params, int n_regs)
{
  if (fh_push(&bc->funcs, NULL) < 0)
    return NULL;
  struct fh_bc_func *func = fh_stack_top(&bc->funcs);
  func->pc = bc->num_instr;
  func->n_params = n_params;
  func->n_regs = n_regs;
  fh_init_stack(&func->consts, sizeof(struct fh_bc_const));
  return func;
}

uint32_t *fh_get_bc_instructions(struct fh_bc *bc, uint32_t *num)
{
  *num = bc->num_instr;
  return bc->instr;
}

struct fh_bc_func *fh_get_bc_funcs(struct fh_bc *bc, uint32_t *num)
{
  *num = bc->funcs.num;
  return bc->funcs.data;
}

static struct fh_bc_const *add_const(struct fh_bc_func *func, int *k)
{
  if (fh_push(&func->consts, NULL) < 0)
    return NULL;
  *k = func->consts.num - 1;
  return fh_stack_top(&func->consts);
}

int fh_add_bc_const_number(struct fh_bc_func *func, double num)
{
  int k;
  struct fh_bc_const *c = add_const(func, &k);
  if (! c)
    return -1;
  c->type = FH_BC_CONST_NUMBER;
  c->data.num = num;
  return k;
}

int fh_add_bc_const_string(struct fh_bc_func *func, const uint8_t *str)
{
  int k;
  struct fh_bc_const *c = add_const(func, &k);
  if (! c)
    return -1;
  uint8_t *dup = malloc(strlen((char *) str));
  if (! dup) {
    fh_pop(&func->consts, NULL);
    return -1;
  }
  c->type = FH_BC_CONST_STRING;
  c->data.str = dup;
  return k;
}

struct fh_bc_const *fh_get_bc_func_consts(struct fh_bc_func *func, uint32_t *num)
{
  *num = func->consts.num;
  return func->consts.data;
}
