/* vm.c */

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>

#include "vm.h"
#include "program.h"
#include "bytecode.h"

void fh_init_vm(struct fh_vm *vm, struct fh_program *prog)
{
  vm->prog = prog;
  vm->stack = NULL;
  vm->stack_size = 0;
  call_frame_stack_init(&vm->call_stack);
}

void fh_destroy_vm(struct fh_vm *vm)
{
  if (vm->stack)
    free(vm->stack);
  call_frame_stack_free(&vm->call_stack);
}

static int vm_error(struct fh_vm *vm, char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fh_set_error(vm->prog, fmt, ap);
  va_end(ap);
  return -1;
}

const char *fh_get_vm_error(struct fh_vm *vm)
{
  return vm->last_err_msg;
}

static int ensure_stack_size(struct fh_vm *vm, int size)
{
  if (vm->stack_size >= size)
    return 0;
  int new_size = (size + 1024 + 1) / 1024 * 1024;
  void *new_stack = realloc(vm->stack, new_size * sizeof(struct fh_value));
  if (! new_stack)
    return vm_error(vm, "out of memory");
  vm->stack = new_stack;
  vm->stack_size = new_size;
  return 0;
}

static struct fh_vm_call_frame *prepare_call(struct fh_vm *vm, struct fh_func *func, int ret_reg, int n_args)
{
  if (ensure_stack_size(vm, ret_reg + 1 + func->n_regs) < 0)
    return NULL;
  if (n_args < func->n_params)
    memset(vm->stack + ret_reg + 1 + n_args, 0, (func->n_params - n_args) * sizeof(struct fh_value));

  /*
   * Clear uninitialized registers. See comment [XXX]
   */
  memset(vm->stack + ret_reg + 1 + func->n_params, 0, (func->n_regs - func->n_params) * sizeof(struct fh_value));
  
  struct fh_vm_call_frame *frame = call_frame_stack_push(&vm->call_stack, NULL);
  if (! frame) {
    vm_error(vm, "out of memory");
    return NULL;
  }
  frame->func = func;
  frame->base = ret_reg + 1;
  frame->ret_addr = NULL;
  return frame;
}

static struct fh_vm_call_frame *prepare_c_call(struct fh_vm *vm, int ret_reg, int n_args)
{
  if (ensure_stack_size(vm, ret_reg + 1 + n_args) < 0)
    return NULL;
  
  struct fh_vm_call_frame *frame = call_frame_stack_push(&vm->call_stack, NULL);
  if (! frame) {
    vm_error(vm, "out of memory");
    return NULL;
  }
  frame->func = NULL;
  frame->base = ret_reg + 1;
  frame->ret_addr = NULL;
  return frame;
}

static void dump_val(char *label, struct fh_value *val)
{
  printf("%s", label);
  fh_dump_value(val);
  printf("\n");
}

static void dump_regs(struct fh_vm *vm)
{
  struct fh_vm_call_frame *frame = call_frame_stack_top(&vm->call_stack);
  struct fh_value *reg_base = vm->stack + frame->base;
  printf("--- base=%d, n_regs=%d\n", frame->base, frame->func->n_regs);
  for (int i = 0; i < frame->func->n_regs; i++) {
    printf("[%-3d] r%-2d = ", i+frame->base, i);
    dump_val("", &reg_base[i]);
  }
  printf("----------------------------\n");
}

int fh_call_vm_function(struct fh_vm *vm, const char *name, struct fh_value *args, int n_args, struct fh_value *ret)
{
  struct fh_func *func = fh_get_bc_func_by_name(vm->prog, name);
  if (! func)
    return vm_error(vm, "function '%s' doesn't exist", name);
  
  if (n_args > func->n_params)
    n_args = func->n_params;
  
  struct fh_vm_call_frame *prev_frame = call_frame_stack_top(&vm->call_stack);
  int ret_reg = (prev_frame) ? prev_frame->base + prev_frame->func->n_regs : 0;
  if (ensure_stack_size(vm, ret_reg + n_args + 1) < 0)
    return -1;
  memset(&vm->stack[ret_reg], 0, sizeof(struct fh_value));
  if (args)
    memcpy(&vm->stack[ret_reg+1], args, n_args*sizeof(struct fh_value));

  /*
   * [XXX] Clear uninitialized registers.
   *
   * This is not strictly necessary (since well-behaved bytecode will
   * never use a register before writing to it), but when debugging
   * with dump_regs() we get complaints from valgrind if we don't do
   * it:
   */
  if (n_args < func->n_regs)
    memset(&vm->stack[ret_reg+1+n_args], 0, (func->n_regs-n_args)*sizeof(struct fh_value));

  if (! prepare_call(vm, func, ret_reg, n_args))
    return -1;
  vm->pc = func->code;
  if (fh_run_vm(vm) < 0)
    return -1;
  if (ret)
    *ret = vm->stack[ret_reg];
  return 0;
}

static int call_c_func(struct fh_vm *vm, fh_c_func func, struct fh_value *ret, struct fh_value *args, int n_args)
{
  int num_c_vals = value_stack_size(&vm->prog->c_vals);
  
  int r = func(vm->prog, ret, args, n_args);

  value_stack_set_size(&vm->prog->c_vals, num_c_vals);  // release any objects created by the C function
  return r;
}

static int is_true(struct fh_value *val)
{
  switch (val->type) {
  case FH_VAL_NULL:   return 0;
  case FH_VAL_NUMBER: return val->data.num != 0.0;
  case FH_VAL_STRING: return fh_get_string(val)[0] != '\0';
  case FH_VAL_ARRAY:  return 1;
  case FH_VAL_FUNC:   return 1;
  case FH_VAL_C_FUNC: return 1;
  }
  return 0;
}

static int vals_equal(struct fh_value *v1, struct fh_value *v2)
{
  if (v1->type != v2->type)
    return 0;
  switch (v1->type) {
  case FH_VAL_NULL:   return 1;
  case FH_VAL_NUMBER: return v1->data.num == v2->data.num;
  case FH_VAL_C_FUNC: return v1->data.c_func == v2->data.c_func;
  case FH_VAL_STRING: return strcmp(fh_get_string(v1), fh_get_string(v2)) == 0;
  case FH_VAL_ARRAY:  return v1->data.obj == v2->data.obj;
  case FH_VAL_FUNC:   return v1->data.obj == v2->data.obj;
  }
  return 0;
}

#define handle_op(op) case op:
#define LOAD_REG_OR_CONST(index)  (((index) <= MAX_FUNC_REGS) ? &reg_base[index] : &const_base[(index)-MAX_FUNC_REGS-1])

int fh_run_vm(struct fh_vm *vm)
{
  struct fh_value *const_base;
  struct fh_value *reg_base;

  uint32_t *pc = vm->pc;
  
 changed_stack_frame:
  {
    struct fh_vm_call_frame *frame = call_frame_stack_top(&vm->call_stack);
    const_base = frame->func->consts;
    reg_base = vm->stack + frame->base;
  }
  while (1) {
    //dump_regs(vm);
    //fh_dump_bc_instr(vm->prog, NULL, -1, *pc);
    
    uint32_t instr = *pc++;
    struct fh_value *ra = &reg_base[GET_INSTR_RA(instr)];
    switch (GET_INSTR_OP(instr)) {
      handle_op(OPC_LDC) {
        *ra = const_base[GET_INSTR_RU(instr)];
        break;
      }

      handle_op(OPC_LDNULL) {
        ra->type = FH_VAL_NULL;
        break;
      }

      handle_op(OPC_MOV) {
        *ra = reg_base[GET_INSTR_RB(instr)];
        break;
      }
      
      handle_op(OPC_RET) {
        struct fh_vm_call_frame *frame = call_frame_stack_top(&vm->call_stack);
        int has_val = GET_INSTR_RB(instr);
        if (has_val)
          vm->stack[frame->base-1] = *ra;
        else
          vm->stack[frame->base-1].type = FH_VAL_NULL;
        uint32_t *ret_addr = frame->ret_addr;
        call_frame_stack_pop(&vm->call_stack, NULL);
        if (call_frame_stack_size(&vm->call_stack) == 0 || ! ret_addr) {
          vm->pc = pc;
          return 0;
        }
        pc = ret_addr;
        goto changed_stack_frame;
      }

      handle_op(OPC_GETEL) {
        struct fh_value *rb = LOAD_REG_OR_CONST(GET_INSTR_RB(instr));
        struct fh_value *rc = LOAD_REG_OR_CONST(GET_INSTR_RC(instr));
        if (rb->type == FH_VAL_ARRAY) {
          if (rc->type != FH_VAL_NUMBER) {
            vm_error(vm, "invalid array access (non-numeric index)D");
            goto err;
          }
          struct fh_value *val = fh_get_array_item(rb, (int) rc->data.num);
          if (! val) {
            vm_error(vm, "invalid array index");
            goto err;
          }
          *ra = *val;
          break;
        }
        vm_error(vm, "invalid element access (non-container object)");
        goto err;
      }

      handle_op(OPC_SETEL) {
        struct fh_value *rb = LOAD_REG_OR_CONST(GET_INSTR_RB(instr));
        struct fh_value *rc = LOAD_REG_OR_CONST(GET_INSTR_RC(instr));
        if (ra->type == FH_VAL_ARRAY) {
          if (rb->type != FH_VAL_NUMBER) {
            vm_error(vm, "invalid array access (non-numeric index)");
            goto err;
          }
          struct fh_value *val = fh_get_array_item(ra, (int) rb->data.num);
          if (! val) {
            vm_error(vm, "invalid array index");
            goto err;
          }
          *val = *rc;
          break;
        }
        vm_error(vm, "invalid element access (non-container object)");
        goto err;
      }

      handle_op(OPC_ADD) {
        struct fh_value *rb = LOAD_REG_OR_CONST(GET_INSTR_RB(instr));
        struct fh_value *rc = LOAD_REG_OR_CONST(GET_INSTR_RC(instr));
        if (rb->type != FH_VAL_NUMBER || rc->type != FH_VAL_NUMBER) {
          vm_error(vm, "arithmetic on non-numeric values");
          goto err;
        }
        ra->type = FH_VAL_NUMBER;
        ra->data.num = rb->data.num + rc->data.num;
        break;
      }
      
      handle_op(OPC_SUB) {
        struct fh_value *rb = LOAD_REG_OR_CONST(GET_INSTR_RB(instr));
        struct fh_value *rc = LOAD_REG_OR_CONST(GET_INSTR_RC(instr));
        if (rb->type != FH_VAL_NUMBER || rc->type != FH_VAL_NUMBER) {
          vm_error(vm, "arithmetic on non-numeric values");
          goto err;
        }
        ra->type = FH_VAL_NUMBER;
        ra->data.num = rb->data.num - rc->data.num;
        break;
      }

      handle_op(OPC_MUL) {
        struct fh_value *rb = LOAD_REG_OR_CONST(GET_INSTR_RB(instr));
        struct fh_value *rc = LOAD_REG_OR_CONST(GET_INSTR_RC(instr));
        if (rb->type != FH_VAL_NUMBER || rc->type != FH_VAL_NUMBER) {
          vm_error(vm, "arithmetic on non-numeric values");
          goto err;
        }
        ra->type = FH_VAL_NUMBER;
        ra->data.num = rb->data.num * rc->data.num;
        break;
      }

      handle_op(OPC_DIV) {
        struct fh_value *rb = LOAD_REG_OR_CONST(GET_INSTR_RB(instr));
        struct fh_value *rc = LOAD_REG_OR_CONST(GET_INSTR_RC(instr));
        if (rb->type != FH_VAL_NUMBER || rc->type != FH_VAL_NUMBER) {
          vm_error(vm, "arithmetic on non-numeric values");
          goto err;
        }
        ra->type = FH_VAL_NUMBER;
        ra->data.num = rb->data.num / rc->data.num;
        break;
      }

      handle_op(OPC_MOD) {
        struct fh_value *rb = LOAD_REG_OR_CONST(GET_INSTR_RB(instr));
        struct fh_value *rc = LOAD_REG_OR_CONST(GET_INSTR_RC(instr));
        if (rb->type != FH_VAL_NUMBER || rc->type != FH_VAL_NUMBER) {
          vm_error(vm, "arithmetic on non-numeric values");
          goto err;
        }
        ra->type = FH_VAL_NUMBER;
        ra->data.num = fmod(rb->data.num, rc->data.num);
        break;
      }

      handle_op(OPC_NEG) {
        struct fh_value *rb = LOAD_REG_OR_CONST(GET_INSTR_RB(instr));
        if (rb->type != FH_VAL_NUMBER) {
          vm_error(vm, "arithmetic on non-numeric value");
          goto err;
        }
        ra->type = FH_VAL_NUMBER;
        ra->data.num = -rb->data.num;
        break;
      }

      handle_op(OPC_NOT) {
        struct fh_value *rb = LOAD_REG_OR_CONST(GET_INSTR_RB(instr));
        ra->type = FH_VAL_NUMBER;
        ra->data.num = (is_true(rb)) ? 0.0 : 1.0;
        break;
      }
      
      handle_op(OPC_CALL) {
        //dump_regs(vm);
        struct fh_vm_call_frame *frame = call_frame_stack_top(&vm->call_stack);
        if (ra->type == FH_VAL_FUNC) {
          uint32_t *func_addr = GET_OBJ_FUNC(ra->data.obj)->code;
          
          /*
           * WARNING: prepare_call() may move the stack, so don't trust reg_base
           * or ra after calling it -- jumping to changed_stack_frame fixes it.
           */
          struct fh_vm_call_frame *new_frame = prepare_call(vm, GET_OBJ_FUNC(ra->data.obj), frame->base + GET_INSTR_RA(instr), GET_INSTR_RB(instr));
          if (! new_frame)
            goto err;
          new_frame->ret_addr = pc;
          pc = func_addr;
          goto changed_stack_frame;
        }
        if (ra->type == FH_VAL_C_FUNC) {
          fh_c_func c_func = ra->data.c_func;
          
          /*
           * WARNING: above warning about prepare_call() also applies to prepare_c_call()
           */
          struct fh_vm_call_frame *new_frame = prepare_c_call(vm, frame->base + GET_INSTR_RA(instr), GET_INSTR_RB(instr));
          if (! new_frame)
            goto err;
          int ret = call_c_func(vm, c_func, vm->stack + new_frame->base - 1, vm->stack + new_frame->base, GET_INSTR_RB(instr));
          call_frame_stack_pop(&vm->call_stack, NULL);
          if (ret < 0)
            goto c_func_err;
          goto changed_stack_frame;
        }
        vm_error(vm, "call to non-function value");
        goto err;
      }
      
      handle_op(OPC_JMP) {
        pc += GET_INSTR_RS(instr);
        break;
      }
      
      handle_op(OPC_TEST) {
        int b = GET_INSTR_RB(instr);
        int test = is_true(ra) ^ b;
        if (test) {
          pc++;
          break;
        }
        pc += GET_INSTR_RS(*pc) + 1;
        break;
      }

      handle_op(OPC_CMP_EQ) {
        int inv = GET_INSTR_RA(instr);
        struct fh_value *rb = LOAD_REG_OR_CONST(GET_INSTR_RB(instr));
        struct fh_value *rc = LOAD_REG_OR_CONST(GET_INSTR_RC(instr));
        int test = vals_equal(rb, rc) ^ inv;
        if (test) {
          pc++;
          break;
        }
        pc += GET_INSTR_RS(*pc) + 1;
        break;
      }

      handle_op(OPC_CMP_LT) {
        int inv = GET_INSTR_RA(instr);
        struct fh_value *rb = LOAD_REG_OR_CONST(GET_INSTR_RB(instr));
        struct fh_value *rc = LOAD_REG_OR_CONST(GET_INSTR_RC(instr));
        if (rb->type != FH_VAL_NUMBER || rc->type != FH_VAL_NUMBER) {
          vm_error(vm, "using < with non-numeric values");
          goto err;
        }
        int test = (rb->data.num < rc->data.num) ^ inv;
        //printf("(%f < %f) ^ %d ==> %d\n", ra->data.num, rb->data.num, c, test);
        if (test) {
          pc++;
          break;
        }
        pc += GET_INSTR_RS(*pc) + 1;
        break;
      }

      handle_op(OPC_CMP_LE) {
        int inv = GET_INSTR_RA(instr);
        struct fh_value *rb = LOAD_REG_OR_CONST(GET_INSTR_RB(instr));
        struct fh_value *rc = LOAD_REG_OR_CONST(GET_INSTR_RC(instr));
        if (rb->type != FH_VAL_NUMBER || rc->type != FH_VAL_NUMBER) {
          vm_error(vm, "using < with non-numeric values");
          goto err;
        }
        int test = (rb->data.num <= rc->data.num) ^ inv;
        //printf("(%f <= %f) ^ %d ==> %d\n", ra->data.num, rb->data.num, c, test);
        if (test) {
          pc++;
          break;
        }
        pc += GET_INSTR_RS(*pc) + 1;
        break;
      }

    default:
      vm_error(vm, "unhandled opcode");
      goto err;
    }
  }

 err:
  printf("\n");
  printf("****************************\n");
  printf("***** HALTING ON ERROR *****\n");
  printf("****************************\n");
  printf("** current stack frame:\n");
  dump_regs(vm);
  printf("** instruction that caused error:\n");
  struct fh_vm_call_frame *frame = call_frame_stack_top(&vm->call_stack);
  int addr = (frame) ? pc - 1 - frame->func->code : -1;
  fh_dump_bc_instr(vm->prog, NULL, addr, pc[-1]);
  printf("----------------------------\n");

 c_func_err:
  vm->pc = pc;
  return -1;
}
