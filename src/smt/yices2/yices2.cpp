/*
 * yices.cpp
 *
 *  Created on: Oct 24, 2014
 *      Author: dejan
 */

#include <gmp.h>
#include <yices.h>
#include <boost/unordered_map.hpp>

#include "expr/term.h"
#include "expr/term_manager.h"
#include "expr/rational.h"
#include "smt/yices2/yices2.h"

namespace sal2 {
namespace smt {

class yices2_internal {

  /** The term manager */
  expr::term_manager& d_tm;

  /** Options */
  const options& d_opts;

  /** Number of yices instances */
  static int s_instances;

  /** Yices boolean type */
  type_t d_bool_type;

  /** Yices integer type */
  type_t d_int_type;

  /** Yices real type */
  type_t d_real_type;

  /** The yices context */
  context_t *d_ctx;

  /** All assertions we have in context (strong) TODO: push/pop */
  std::vector<expr::term_ref_strong> d_assertions;

  typedef boost::unordered_map<expr::term_ref, term_t, expr::term_ref_hasher> term_cache;

  /** The map from terms to yices terms */
  term_cache d_term_cache;

  /** List of variables, for model construction */
  std::vector<expr::term_ref> d_variables;

  /** Last check return */
  smt_status_t d_last_check_status;

public:

  yices2_internal(expr::term_manager& tm, const options& opts);
  ~yices2_internal();

  term_t to_yices2_term(expr::term_ref ref);
  type_t to_yices2_type(expr::term_ref ref);

  term_t mk_yices2_term(expr::term_op op, size_t n, term_t* children);

  void add(expr::term_ref ref);
  solver::result check();
  void get_model(model& m);
  void push();
  void pop();
};

int yices2_internal::s_instances = 0;

yices2_internal::yices2_internal(expr::term_manager& tm, const options& opts)
: d_tm(tm)
, d_opts(opts)
, d_last_check_status(STATUS_UNKNOWN)
{
  // Initialize
  if (s_instances == 0) {
    yices_init();
  }
  s_instances ++;

  // The basic types
  d_bool_type = yices_bool_type();
  d_int_type = yices_int_type();
  d_real_type = yices_real_type();

  // The context
  d_ctx = yices_new_context(NULL);
  if (d_ctx == 0) {
    throw exception("Yices error (context creation).");
  }
}

yices2_internal::~yices2_internal() {

  // The context
  yices_free_context(d_ctx);

  // Cleanup if the last one
  s_instances--;
  if (s_instances == 0) {
    yices_exit();
  }
}

term_t yices2_internal::mk_yices2_term(expr::term_op op, size_t n, term_t* children) {
  term_t result = NULL_TERM;

  switch (op) {
  case expr::TERM_AND:
    result = yices_and(n, children);
    break;
  case expr::TERM_OR:
    result = yices_or(n, children);
    break;
  case expr::TERM_NOT:
    assert(n == 1);
    result = yices_not(children[0]);
    break;
  case expr::TERM_IMPLIES:
    assert(n == 2);
    result = yices_implies(children[0], children[1]);
    break;
  case expr::TERM_XOR:
    result = yices_xor(n, children);
    break;
  case expr::TERM_ADD:
    result = yices_sum(n, children);
    break;
  case expr::TERM_SUB:
    assert(n == 2 || n == 1);
    if (n == 1) {
      result = yices_neg(children[0]);
    } else {
      result = yices_sub(children[0], children[1]);
    }
    break;
  case expr::TERM_MUL:
    result = yices_product(n, children);
    break;
  case expr::TERM_DIV:
    result = yices_division(children[0], children[1]);
    break;
  case expr::TERM_LEQ:
    assert(n == 2);
    result = yices_arith_leq_atom(children[0], children[1]);
    break;
  case expr::TERM_LT:
    assert(n == 2);
    result = yices_arith_lt_atom(children[0], children[1]);
    break;
  case expr::TERM_GEQ:
    assert(n == 2);
    result = yices_arith_geq_atom(children[0], children[1]);
    break;
  case expr::TERM_GT:
    assert(n == 2);
    result = yices_arith_gt_atom(children[0], children[1]);
    break;
  case expr::TERM_EQ:
    assert(n == 2);
    result = yices_eq(children[0], children[1]);
    break;
  case expr::TERM_ITE:
    assert(n == 3);
    result = yices_ite(children[0], children[1], children[2]);
    break;
  default:
    assert(false);
  }

  return result;
}

type_t yices2_internal::to_yices2_type(expr::term_ref ref) {

  type_t result = NULL_TERM;

  switch (d_tm.term_of(ref).op()) {
  case expr::TYPE_BOOL:
    result = d_bool_type;
    break;
  case expr::TYPE_INTEGER:
    result = d_int_type;
    break;
  case expr::TYPE_REAL:
    result = d_real_type;
    break;
  default:
    assert(false);
  }

  return result;
}

term_t yices2_internal::to_yices2_term(expr::term_ref ref) {

  term_t result = NULL_TERM;

  // Check if in cache already
  term_cache::const_iterator find = d_term_cache.find(ref);
  if (find != d_term_cache.end()) {
    return find->second;
  }

  // The term
  const expr::term& t = d_tm.term_of(ref);
  expr::term_op t_op = t.op();

  switch (t_op) {
  case expr::VARIABLE:
    result = yices_new_uninterpreted_term(to_yices2_type(t[0]));
    yices_set_term_name(result, d_tm.get_variable_name(t).c_str());
    // Remember, for model construction
    d_variables.push_back(ref);
    break;
  case expr::CONST_BOOL:
    result = d_tm.get_boolean_constant(t) ? yices_true() : yices_false();
    break;
  case expr::CONST_RATIONAL:
    result = yices_mpq(d_tm.get_rational_constant(t).mpq().get_mpq_t());
    break;
  case expr::TERM_ITE:
  case expr::TERM_EQ:
  case expr::TERM_AND:
  case expr::TERM_OR:
  case expr::TERM_NOT:
  case expr::TERM_IMPLIES:
  case expr::TERM_XOR:
  case expr::TERM_ADD:
  case expr::TERM_SUB:
  case expr::TERM_MUL:
  case expr::TERM_DIV:
  case expr::TERM_LEQ:
  case expr::TERM_LT:
  case expr::TERM_GEQ:
  case expr::TERM_GT:
  {
    size_t size = t.size();
    term_t children[size];
    for (size_t i = 0; i < size; ++ i) {
      children[i] = to_yices2_term(t[i]);
    }
    result = mk_yices2_term(t.op(), size, children);
    break;
  }
  default:
    assert(false);
  }

  if (result < 0) {
    throw exception("Yices error (term creation)");
  }

  d_term_cache[ref] = result;

  return result;
}

void yices2_internal::add(expr::term_ref ref) {
  if (output::get_verbosity(std::cout) > 1) {
    std::cout << "yices2: adding " << ref << std::endl;
  }

  expr::term_ref_strong ref_strong(d_tm, ref);
  d_assertions.push_back(ref_strong);
  term_t yices_term = to_yices2_term(ref);
  int ret = yices_assert_formula(d_ctx, yices_term);
  if (ret < 0) {
    throw exception("Yices error (add).");
  }
}

solver::result yices2_internal::check() {
  d_last_check_status = yices_check_context(d_ctx, 0);
  if (d_last_check_status == STATUS_SAT) {
    return solver::SAT;
  } else if (d_last_check_status == STATUS_UNSAT) {
    return solver::UNSAT;
  } else if (d_last_check_status == STATUS_UNKNOWN) {
    return solver::UNKNOWN;
  } else {
    throw exception("Yices error (check).");
  }
}

void yices2_internal::get_model(model& m) {
  assert(d_last_check_status == STATUS_SAT);

  // Clear any data already there
  m.clear();

  // Get the model from yices
  model_t* yices_model = yices_get_model(d_ctx, true);

  for (size_t i = 0; i < d_variables.size(); ++ i) {
    expr::term_ref var = d_variables[i];
    term_t yices_var = d_term_cache[var];
    expr::term_ref var_type = d_tm.type_of(var);

    expr::term_ref var_value;
    switch (d_tm.term_of(var_type).op()) {
    case expr::TYPE_BOOL: {
      int32_t value;
      yices_get_bool_value(yices_model, yices_var, &value);
      var_value = d_tm.mk_boolean_constant(value);
      break;
    }
    case expr::TYPE_INTEGER: {
      // The integer mpz_t value
      mpz_t value;
      mpz_init(value);
      yices_get_mpz_value(yices_model, yices_var, value);
      // The rational mpq_t value
      mpq_t value_q;
      mpq_init(value_q);
      mpq_set_z(value_q, value);
      // Now, the rational
      expr::rational rational_value(value_q);
      var_value = d_tm.mk_rational_constant(rational_value);;
      // Clear the temps
      mpq_clear(value_q);
      mpz_clear(value);
      break;
    }
    case expr::TYPE_REAL: {
      // The integer mpz_t value
      mpq_t value;
      mpq_init(value);
      yices_get_mpq_value(yices_model, yices_var, value);
      // Now, the rational
      expr::rational rational_value(value);
      var_value = d_tm.mk_rational_constant(rational_value);;
      // Clear the temps
      mpq_clear(value);
      break;
    }
    default:
      assert(false);
    }

    // Add the association
    m.set_value(var, var_value);
  }

  // Free the yices model
  yices_free_model(yices_model);
}

void yices2_internal::push() {
  int ret = yices_push(d_ctx);
  if (ret < 0) {
    throw exception("Yices error (push).");
  }
}

void yices2_internal::pop() {
  int ret = yices_pop(d_ctx);
  if (ret < 0) {
    throw exception("Yices error (pop).");
  }
}

yices2::yices2(expr::term_manager& tm, const options& opts)
: solver("yices2", tm, opts)
{
  d_internal = new yices2_internal(tm, opts);
}

yices2::~yices2() {
  delete d_internal;
}

void yices2::add(expr::term_ref f) {
  d_internal->add(f);
}

solver::result yices2::check() {
  return d_internal->check();
}

void yices2::get_model(model& m) const {
  d_internal->get_model(m);
}

void yices2::push() {
  d_internal->push();
}

void yices2::pop() {
  d_internal->pop();
}


expr::term_ref yices2::generalize() {
  return expr::term_ref();
}

void yices2::interpolate(std::vector<expr::term_ref>& ) {

}

}
}

