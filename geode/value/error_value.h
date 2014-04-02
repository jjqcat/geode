//#####################################################################
// Function error_value
//#####################################################################
#pragma once

#include <geode/value/Value.h>
namespace geode {

template<class T> class ErrorValue : public Value<T> {
public:
  GEODE_NEW_FRIEND
  typedef Value<T> Base;
private:
  const Ref<const SavedException> error;

  ErrorValue(const exception& error)
    : error(save(error)) {}

  void update() const {
    error->throw_();
  }

  void dump(const int indent) const {
    printf("%*sErrorValue<%s>\n",2*indent,"",typeid(T).name());
  }
};

// Compute a value that always throws the given exception
template<class T> static inline ValueRef<T> error_value(const exception& error) {
  return ValueRef<T>(new_<ErrorValue<T> >(error));
}

}
