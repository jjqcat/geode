// Python needs numeric_limits

#include <geode/utility/Object.h>
#include <geode/utility/format.h>
#include <limits>
namespace geode {

using std::numeric_limits;

namespace {
template<class T> struct Limits : public Object {
  GEODE_NEW_FRIEND
  static const T min, max, epsilon, round_error, infinity, quiet_NaN, signaling_NaN, denorm_min;
  static const int digits = numeric_limits<T>::digits;
  static const int digits10 = numeric_limits<T>::digits10;
  static const int min_exponent = numeric_limits<T>::min_exponent;
  static const int min_exponent10 = numeric_limits<T>::min_exponent10;
  static const int max_exponent = numeric_limits<T>::max_exponent;
  static const int max_exponent10 = numeric_limits<T>::max_exponent10;
  string repr() const {
    // Use separate format calls since Windows lacks variadic templates
    return format("numeric_limits<%s>:\n  min = %g\n  max = %g\n  epsilon = %g\n  round_error = %g\n  quiet_NaN = %g\n",
                  is_same<T,float>::value?"float":"double",
                  min,max,epsilon,round_error,quiet_NaN)
         + format("  signaling_NaN = %g\n  denorm_min = %g\n",
                  signaling_NaN,denorm_min)
         + format("  digits = %d\n  digits10 = %d\n  min_exponent = %d\n  min_exponent10 = %d\n  max_exponent = %d\n  max_exponent10 = %d",
                  digits,digits10,min_exponent,min_exponent10,max_exponent,max_exponent10);
  }
};
#define VALUE(name) template<class T> const T Limits<T>::name = numeric_limits<T>::name();
VALUE(min)
VALUE(max)
VALUE(epsilon)
VALUE(round_error)
VALUE(infinity)
VALUE(quiet_NaN)
VALUE(signaling_NaN)
VALUE(denorm_min)
#undef VALUE
#define COUNT(name) template<class T> const int Limits<T>::name;
COUNT(digits)
COUNT(digits10)
COUNT(min_exponent)
COUNT(min_exponent10)
COUNT(max_exponent)
COUNT(max_exponent10)
#undef COUNT

#if 0 // Value python support
#define INSTANTIATE(T) template<> GEODE_DEFINE_TYPE(Limits<T>)
INSTANTIATE(int32_t)
INSTANTIATE(int64_t)
INSTANTIATE(uint32_t)
INSTANTIATE(uint64_t)
INSTANTIATE(float)
INSTANTIATE(double)
#endif
}

#if 0 // Value python support
PyObject* build_limits(PyObject* object) {
  PyArray_Descr* dtype;
  if (!PyArray_DescrConverter(object,&dtype))
    return 0;
  const Ref<> save = steal_ref(*(PyObject*)dtype);
  const int type = dtype->type_num;
  switch (type) {
    #define CASE(T) case NumpyScalar<T>::value: return to_python(new_<Limits<T>>());
    CASE(int32_t)
    CASE(int64_t)
    CASE(uint32_t)
    CASE(uint64_t)
    CASE(float)
    CASE(double)
    default:
      Ref<PyObject> s = steal_ref_check(PyObject_Str((PyObject*)dtype));
      throw TypeError(format("numeric_limits unimplemented for type %s",from_python<const char*>(s)));
  }
}
#endif

}
