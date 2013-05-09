// General purpose black box simulation of simplicity

#include <other/core/exact/perturb.h>
#include <other/core/exact/math.h>
#include <other/core/exact/vandermonde-generated.h> // Pull in autogenerated tables
#include <other/core/array/alloca.h>
#include <other/core/array/Array2d.h>
#include <other/core/python/wrap.h>
#include <other/core/random/counter.h>
#include <other/core/structure/Hashtable.h>
#include <other/core/utility/move.h>
#include <other/core/vector/Matrix.h>
namespace other {

// Our function is defined by
//
//   perturbed_sign(f,_,x) = lim_{ek -> 0} (f(x + sum_{k>0} ek yk) > 0)
//
// where yk are fixed pseudorandom vectors and ei >> ej for i > j in the limit.  Almost all of the time,
// the first e1 y1 term is sufficient to reach nondegeneracy, so the practical complexity is O(predicate-cost*degree).
// Our scheme is a combination of the fully general scheme of Yap and the randomized linear scheme of Seidel:
//
//   Yap 1990, "Symbolic treatment of geometric degeneracies".
//   Seidel 1998, "The nature and meaning of perturbations in geometric computing".
//
// To recover the expanded predicate at each level of the interpolation, we use the divided difference algorithm of
//
//   Neidinger 2010, "Multivariable interpolating polynomials in Newton forms".
//
// In their terminology, we evaluate polynomials on "easy corners" where x_i(j) = j.  In the univariate case we
// precompute the LU decomposition of the Vandermonde matrix, invert each part, and clear fractions to avoid the need
// for rational arithmetic.  The structure of the LU decomposition of the Vandermonde matrix is given in
//
//   Oliver 2009, "On multivariate interpolation".

using std::cout;
using std::endl;
using exact::Exact;
using exact::init_set_steal;

// Compile time debugging support
static const bool check = false;
static const bool verbose = false;

// Our fixed deterministic pseudorandom perturbation sequence
template<int m> static inline Vector<exact::Int,m> perturbation(const int level, const int i) {
  BOOST_STATIC_ASSERT(m<=4);
  BOOST_STATIC_ASSERT(exact::log_bound+1<=32);
  const uint128_t bits = threefry(level,i);
  Vector<exact::Int,m> result;
  const auto limit = 1<<exact::log_bound;
  for (int a=0;a<m;a++)
    result[a] = (cast_uint128<exact::Int>(bits>>32*a)&(2*limit-1))-limit;
  return result;
}

// List all n-variate monomials of degree <= d, ordered by ascending total degree and then arbitrarily.
// Warning: This is the order needed for divided difference interpolation, but is *not* the correct infinitesimal size order.
static Array<const uint8_t,2> monomials(const int degree, const int variables) {
  // Count monomials: choose(degree+variables,degree)
  uint64_t num = 1, den = 1;
  for (int k=1;k<=degree;k++) {
    num *= k+variables;
    den *= k;
  }
  num /= den;
  OTHER_ASSERT(num <= (1<<20));
  Array<uint8_t,2> results(num,variables);

  // We simulate a stack manually to avoid recursion
  if (variables) {
    int next = 0;
    const auto alpha = OTHER_RAW_ALLOCA(variables,uint8_t);
    alpha.fill(0);
    for (int d=0;d<=degree;d++) {
      int i = 0;
      int left = d;
      alpha[0] = 0;
      for (;;) {
        if (i<variables-1) {
          // Traverse down
          i++;
        } else {
          // Add our complete monomial to the list
          alpha[i] = left;
          results[next++] = alpha;
          // Traverse up until we can increment alpha[i]
          for (;;) {
            if (!i--)
              goto end;
            if (!left) {
              left += alpha[i];
              alpha[i] = 0;
            } else {
              left--;
              alpha[i]++;
              break;
            }
          }
        }
      }
      end:;
    }
    assert(next==results.m);
  }
  return results;
}

static string show_monomial(RawArray<const uint8_t> alpha) {
  string s(alpha.size(),'0');
  for (int i=0;i<alpha.size();i++)
    s[i] = '0'+alpha[i];
  return s;
}

// The relative size ordering on infinitesimals
static inline bool monomial_less(RawArray<const uint8_t> a, RawArray<const uint8_t> b) {
  assert(a.size()==b.size());
  for (int i=a.size()-1;i>=0;i--)
    if (a[i] != b[i])
      return a[i]>b[i];
  return false;
}

// Faster division of rationals by ints
static inline void mpq_div_si(mpq_t x, long n) {
  // Make n relatively prime to numerator(x)
  const auto gcd = mpz_gcd_ui(0,mpq_numref(x),abs(n));
  n /= gcd;
  mpz_divexact_ui(mpq_numref(x),mpq_numref(x),gcd);
  // Perform the division
  mpz_mul_si(mpq_denref(x),mpq_denref(x),n);
}

// Given the values of a polynomial at every point in the standard "easy corner", solve for the monomial coefficients using divided differences
// lambda and A are as in Neidinger.  We assume lambda is partially sorted by total degree.
static void in_place_interpolating_polynomial(const int degree, const RawArray<const uint8_t,2> lambda, RawArray<__mpq_struct> A) {
  // For now we are lazy, and index using a rectangular helper array mapping multi-indices to flat indices
  const int n = lambda.n;
  Array<int> powers(n+1,false);
  powers[0] = 1;
  for (int i=0;i<n;i++)
    powers[i+1] = powers[i]*(degree+1);
  Array<uint16_t> to_flat(powers.back(),false);
  Array<int> from_flat(lambda.m,false);
  to_flat.fill(-1);
  for (int k=0;k<lambda.m;k++) {
    int f = 0;
    for (int i=0;i<n;i++)
      f += powers[i]*lambda(k,i);
    from_flat[k] = f;
    to_flat[f] = k;
  }

  // Bookkeeping information for the divided difference algorithm
  Array<Vector<int,2>> info(lambda.m,false); // m,alpha[m] for each tick
  for (int k=0;k<lambda.m;k++)
    info[k].set(0,lambda(k,0));
  // In self check mode, keep track of the entire alpha
  Array<uint8_t,2> alpha;
  if (check)
    alpha = lambda.copy();

  // Iterate divided differences for degree = max |lambda| passes.
  for (int pass=1;pass<=degree;pass++) {
    for (int k=lambda.m-1;k>=0;k--) {
      // Decrement alpha
      auto& I = info[k];
      while (!I.y) {
        if (++I.x==n) // Quit if we're done with this degree, since lambda[k] for smaller k will also be finished.
          goto next_pass;
        I.y = lambda(k,I.x);
      }
      I.y--;
      // Compute divided difference
      const int child = to_flat[from_flat[k]-powers[I.x]];
      mpq_sub(&A[k],&A[k],&A[child]); // A[k] -= A[child]
      mpq_div_si(&A[k],lambda(k,I.x)-I.y); // A[k] /= lambda(k,I.x)-I.y
      // In self check mode, verify that the necessary f[alpha,beta] values were available
      if (check) {
        alpha(k,I.x)--;
        OTHER_ASSERT(alpha[k]==alpha[child]);
      }
    }
    next_pass:;
  }

  // At this point A contains the coefficients of the interpolating polynomial in the Newton basis, and we are halfway there.  Next, we must
  // expand the Newton basis out into the monomial basis.  We start with A(beta) = g(beta), the coefficient of the Newton basis polynomial q_beta.
  // We seek h(alpha) for alpha <= beta, the coefficients of the monomial basis polynomials x^alpha.  This gives rise to a special upper triangular
  // matrix M = M_{alpha,beta}:
  //
  //   h = M g
  //   M_ab = 0 unless a <= b componentwise: M is upper triangular w.r.t. the partial order on ticks
  //   M_bb = 1: M is special upper triangular, since Newton basis polynomials are monic
  //
  // We define the signed elementary symmetric polynomials tau_r(x_1,...,x_k) as
  //
  //   tau_r(x_1, ..., x_k) = (-1)^r sum_{1 <= i_1 < ... < i_r <= k} x_{i_1} ... x_{i_r}
  //   tau_r(k) = tau_r(0, ..., k-1)
  //
  // Then we have
  //
  //   M_ab = prod_m tau_{b_m - a_m}(x_m(<b_m)) = prod_m tau_{b_m - a_m}(b_m)
  //
  // where the last equality uses our particular choice of evaluation points x_m(i) = i.
  // To evaluate these entries, we use the following recurrences for tau:
  //
  //   tau_0(k) = 1
  //   tau_{r+1}(r+1) = -r tau_r(r)
  //   tau_{r+1}(k+1) = tau_{r+1}(k) - k tau_r(k)
  //
  // In fact, we have tau_r(k) = s(k+r,k), where s(n,k) are the signed Stirling numbers of the first kind, though we won't make use of this.
  // Instead of storing tau directly, we use the above precomputed table of
  //
  //   sigma(n,k) = tau_{n-k}(n)
  //
  // With sigma in hand, we convert from the Newton basis to the monomial basis.
  if (check)
    alpha.copy(lambda);
  mpq_t tmp;
  mpq_init(tmp);
  for (int k=0;k<lambda.m;k++) {
    const auto beta = lambda[k];
    // For all gamma <= beta, do A[gamma] += taus(gamma,beta)*A[beta]
    for (int kk=0;kk<k;kk++) {
      const auto gamma = lambda[kk];
      int32_t taus = 1;
      for (int i=0;i<n;i++) {
        if (gamma[i]>beta[i])
          goto skip;
        if (gamma[i]<beta[i])
          taus *= sigma(beta[i],gamma[i]);
      }
      // A[kk] += taus*A[k]
      mpq_set_si(tmp,taus,1);
      mpq_mul(tmp,tmp,&A[k]);
      mpq_add(&A[kk],&A[kk],tmp);
      skip:;
    }
  }
  mpq_clear(tmp);
}

static inline void mpz_addmul_si(mpz_t rop, mpz_t op1, long op2) {
  if (op2>=0)
    mpz_addmul_ui(rop,op1,op2);
  else
    mpz_submul_ui(rop,op1,-op2);
}

// A specialized version of in_place_interpolating_polynomial for the univariate case.  The constant term is assumed to be zero.
// The result is scaled by degree! to avoid the need for rational arithmetic.
static void scaled_univariate_in_place_interpolating_polynomial(const int degree, RawArray<__mpz_struct> A) {
  assert(degree==A.size());
  // Multiply by the inverse of the lower triangular part.
  int factor = 1; // Row k of L is stored multiplied by k!, so when using row k we must multiply by factor = degree!/k!
  for (int k=degree-1;k>=0;k--) {
    for (int i=0;i<k;i++)
      mpz_addmul_si(&A[k],&A[i],lower_triangle(k+1,i+1)); // A[k] += lower_triangle(k+1,i+1)*A[i];
    mpz_mul_ui(&A[k],&A[k],factor); // A[k] *= factor
    factor *= k+1;
  }
  // Multiply by the inverse of the special upper triangular part.  This part is naturally integral; no extra factors are necessary.
  for (int k=0;k<degree;k++)
    for (int i=0;i<k;i++)
      mpz_addmul_si(&A[i],&A[k],sigma(k+1,i+1)); // A[i] += sigma(k+1,i+1)*A[k];
}

template<int m> bool perturbed_sign(Exact<>(*const predicate)(RawArray<const Vector<exact::Int,m>>), const int degree, RawArray<const Tuple<int,Vector<exact::Int,m>>> X) {
  OTHER_ASSERT(degree<=max_degree);
  const int n = X.size();
  if (verbose)
    cout << "perturbed_sign:\n  degree = "<<degree<<"\n  X = "<<X<<endl;

  // In debug mode, check that point indices are all unique
  OTHER_DEBUG_ONLY( {
    Hashtable<int> indices;
    for (int i=0;i<n;i++)
      OTHER_ASSERT(indices.set(X[i].x));
  } )

  // If desired, verify that predicate(X) == 0
  const auto Z = OTHER_RAW_ALLOCA(n,Vector<exact::Int,m>);
  if (check) {
    for (int i=0;i<n;i++)
      Z[i] = X[i].y;
    OTHER_ASSERT(!sign(predicate(Z)));
  }

  // Check the first perturbation variable with specialized code
  vector<Vector<exact::Int,m>> Y(n); // perturbations
  {
    // Compute the first level of perturbations
    for (int i=0;i<n;i++)
      Y[i] = perturbation<m>(1,X[i].x);
    if (verbose)
      cout << "  Y = "<<Y<<endl;

    // Evaluate polynomial at epsilon = 1, ..., degree
    const auto values = OTHER_RAW_ALLOCA(degree,__mpz_struct);
    for (int j=0;j<degree;j++) {
      for (int i=0;i<n;i++)
        Z[i] = X[i].y+(j+1)*Y[i];
      init_set_steal(&values[j],predicate(Z).n);
      if (verbose)
        cout << "  predicate("<<Z<<") = "<<values[j]<<endl;
    }

    // Find an interpolating polynomial, overriding the input with the result.
    scaled_univariate_in_place_interpolating_polynomial(degree,values);
    if (verbose)
      cout << "  coefs = "<<values<<endl;

    // Compute sign
    int sign = 0;
    for (int j=0;j<degree;j++)
      if (const int s = mpz_sgn(&values[j])) {
        sign = s;
        break;
      }

    // Free mpz_t memory
    for (auto& v : values)
      mpz_clear(&v);

    if (sign)
      return sign>0;
  }

  {
    // Add one perturbation variable after another until we hit a nonzero polynomial.  Our current implementation duplicates
    // work from one iteration to the next for simplicity, which is fine since the first interation suffices almost always.
    Array<__mpq_struct> values;
    for (int d=1;;d++) {
      // Compute the next level of perturbations
      Y.resize(d*n);
      for (int i=0;i<n;i++)
        Y[(d-1)*n+i] = perturbation<m>(d,X[i].x);

      // Evaluate polynomial at every point in an "easy corner"
      const auto lambda = monomials(degree,d);
      values.resize(lambda.m,false,false);
      for (int j=0;j<lambda.m;j++) {
        for (int i=0;i<n;i++)
          Z[i] = X[i].y+lambda(j,0)*Y[i];
        for (int v=1;v<d;v++)
          for (int i=0;i<n;i++)
            Z[i] += lambda(j,v)*Y[v*n+i];
        init_set_steal(mpq_numref(&values[j]),predicate(Z).n);
        mpz_init_set_ui(mpq_denref(&values[j]),1);
      }

      // Find an interpolating polynomial, overriding the input with the result.
      in_place_interpolating_polynomial(degree,lambda,values);

      // Compute sign
      int sign = 0;
      int sign_j = -1;
      for (int j=0;j<lambda.m;j++)
        if (const int s = mpq_sgn(&values[j])) {
          if (check) // Verify that a term which used to be zero doesn't become nonzero
            OTHER_ASSERT(lambda(j,d-1));
          if (!sign || monomial_less(lambda[sign_j],lambda[j])) {
            sign = s;
            sign_j = j;
          }
        }

      // Free mpq_t memory
      for (auto& v : values)
        mpq_clear(&v);

      // If we find a nonzero sign, we're done!
      if (sign)
        return sign>0;
    }
  }
}

// Everything that follows is for testing purposes

static int32_t evaluate(RawArray<const uint8_t,2> lambda, RawArray<const int32_t> coefs, RawArray<const uint8_t> inputs) {
  OTHER_ASSERT(lambda.sizes()==vec(coefs.size(),inputs.size()));
  int32_t sum = 0;
  for (int k=0;k<lambda.m;k++) {
    int32_t v = coefs[k];
    for (int i=0;i<lambda.n;i++)
      for (int j=0;j<lambda(k,i);j++)
        v *= inputs[i];
    sum += v;
  }
  return sum;
}

static void in_place_interpolating_polynomial_test(const int degree, RawArray<const uint8_t,2> lambda, RawArray<const int32_t> coefs, const bool verbose) {
  OTHER_ASSERT(degree<=max_degree);
  Array<__mpz_struct> values_z(lambda.m,false);
  Array<__mpq_struct> values_q(lambda.m,false);
  for (int k=0;k<lambda.m;k++) {
    init_set_steal(&values_z[k],evaluate(lambda,coefs,lambda[k]));
    mpq_init(&values_q[k]);
    mpz_set(mpq_numref(&values_q[k]),&values_z[k]);
  }
  if (verbose) {
    cout << "\ndegree = "<<degree<<"\nlambda =";
    for (int k=0;k<lambda.m;k++)
      cout << ' ' << show_monomial(lambda[k]);
    cout << "\ncoefs = "<<coefs<<"\nvalues = "<<values_z<<endl;
  }
  in_place_interpolating_polynomial(degree,lambda,values_q);
  if (verbose)
    cout << "result = "<<values_q<<endl;
  for (int k=0;k<lambda.m;k++)
    OTHER_ASSERT(!mpq_cmp_si(&values_q[k],coefs[k],1));

  // If we're univariate, compare against the specialized routine
  if (degree+1==lambda.m) {
    for (int j=1;j<=degree;j++)
      mpz_sub(&values_z[j],&values_z[j],&values_z[0]); // values_z[j] -= values_z[0];
    scaled_univariate_in_place_interpolating_polynomial(degree,values_z.slice(1,degree+1));
    int scale = 1;
    for (int k=1;k<=degree;k++)
      scale *= k;
    mpz_mul_ui(&values_z[0],&values_z[0],scale); // values_z[0] *= scale;
    if (verbose)
      cout << "scale = "<<scale<<", univariate = "<<values_z<<endl;
    for (int k=0;k<lambda.m;k++) {
      // Compare scale*values_q[k] with values_z[k]
      __mpq_struct* vq = &values_q[k];
      mpz_mul_ui(mpq_numref(vq),mpq_numref(vq),scale);
      mpq_canonicalize(vq);
      OTHER_ASSERT(!mpz_cmp(mpq_numref(vq),&values_z[k]) && !mpz_cmp_ui(mpq_denref(vq),1));
    }
  }

  // Free memory
  for (auto& v : values_z)
    mpz_clear(&v);
  for (auto& v : values_q)
    mpq_clear(&v);
}

// Test against malicious predicates that are zero along 0, 1, or 2 perturbation levels.

static int nasty_index, nasty_degree;

static Exact<> nasty_pow(Exact<>&& x) {
  switch (nasty_degree) {
    case 1: return other::move(x);
    case 2: return sqr(other::move(x));
    case 3: return cube(other::move(x));
    default: OTHER_FATAL_ERROR();
  };
}

template<class In> static Exact<> nasty_predicate(RawArray<const Vector<In,1>> X) {
  return nasty_pow(Exact<>(X[0].x));
}

template<class In> static Exact<> nasty_predicate(RawArray<const Vector<In,2>> X) {
  typedef Vector<Exact<>,2> EV;
  return nasty_pow(edet(EV(X[0]),
                        EV(perturbation<2>(1,nasty_index))));
}

template<class In> static Exact<> nasty_predicate(RawArray<const Vector<In,3>> X) {
  typedef Vector<Exact<>,3> EV;
  return nasty_pow(edet(EV(X[0]),
                        EV(perturbation<3>(1,nasty_index)),
                        EV(perturbation<3>(2,nasty_index))));
}

template<int m> static void perturbed_sign_test() {
  for (const int degree : vec(1,2,3))
    for (const int index : range(20)) {
      // Evaluate perturbed sign using our fancy routine
      nasty_degree = degree;
      nasty_index = index;
      Array<Tuple<int,Vector<exact::Int,m>>> fX(1);
      fX[0].x = index;
      const bool fast = perturbed_sign<m>(nasty_predicate<exact::Int>,degree,fX);
      OTHER_ASSERT((degree&1) || fast);
      // Evaluate the series out to several terms using brute force
      Vector<Exact<>,m> sX;
      Array<int> powers(m+1); // Choose powers of 2 to approximate nested infinitesimals
      for (int i=0;i<m;i++)
        powers[i+1] = (degree+1)*powers[i]+128;
      mpz_t yp;
      mpz_init(yp);
      for (int i=0;i<=m+1;i++) {
        if (i) {
          const auto y = perturbation<m>(i,index);
          for (int j=0;j<m;j++) {
            auto& x = sX[j];
            mpz_set_si(yp,y[j]);
            mpz_mul_2exp(yp,yp,powers.back()-powers[i-1]); // yp = y[j]<<(powers[-1]-powers[i-1])
            mpz_add(x.n,x.n,yp); // x += yp
          }
        }
        // We should be initially zero, and then match the correct sign once nonzero
        const int slow = sign(nasty_predicate<Exact<>>(RawArray<const Vector<Exact<>,m>>(1,&sX)));
        if (0) {
          cout << "m "<<m<<", degree "<<degree<<", index "<<index<<", i "<<i<<", fast "<<2*fast-1<<", slow "<<slow<<endl;
          cout << "  fX = "<<fX[0]<<", sX = "<<sX<<endl;
        }
        OTHER_ASSERT(slow==(i<m?0:2*fast-1));
      }
      mpz_clear(yp);
    }
}

#define INSTANTIATE(m) \
  template bool perturbed_sign(Exact<>(*const)(RawArray<const Vector<exact::Int,m>>), const int, RawArray<const Tuple<int,Vector<exact::Int,m>>>);
INSTANTIATE(1)
INSTANTIATE(2)
INSTANTIATE(3)

}
using namespace other;

void wrap_perturb() {
  OTHER_FUNCTION_2(perturb_monomials,monomials)
  OTHER_FUNCTION_2(perturbed_sign_test_1,perturbed_sign_test<1>)
  OTHER_FUNCTION_2(perturbed_sign_test_2,perturbed_sign_test<2>)
  OTHER_FUNCTION_2(perturbed_sign_test_3,perturbed_sign_test<3>)
  OTHER_FUNCTION(in_place_interpolating_polynomial_test)
}
