/**
 * @file types.hpp
 * @brief Self-contained vocabulary types for embsh: expected, function_ref.
 */

#ifndef EMBSH_TYPES_HPP_
#define EMBSH_TYPES_HPP_

#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace embsh {

// ============================================================================
// Error enumeration
// ============================================================================

enum class ShellError : uint8_t {
  kOk = 0,
  kRegistryFull,
  kDuplicateName,
  kAuthFailed,
  kPortInUse,
  kAlreadyRunning,
  kNotRunning,
  kDeviceOpenFailed,
  kInvalidArgument,
};

// ============================================================================
// expected<V, E>  --  Lightweight Result type (no exceptions)
// ============================================================================

template <typename V, typename E>
class expected {
 public:
  static expected success(const V& v) noexcept {
    expected e;
    e.has_val_ = true;
    new (&e.storage_) V(v);
    return e;
  }

  static expected error(const E& err) noexcept {
    expected e;
    e.has_val_ = false;
    new (&e.err_) E(err);
    return e;
  }

  bool has_value() const noexcept { return has_val_; }
  explicit operator bool() const noexcept { return has_val_; }

  V& value() noexcept { return *reinterpret_cast<V*>(&storage_); }
  const V& value() const noexcept { return *reinterpret_cast<const V*>(&storage_); }

  E& error_value() noexcept { return err_; }
  const E& error_value() const noexcept { return err_; }

 private:
  expected() = default;
  bool has_val_ = false;
  union {
    typename std::aligned_storage<sizeof(V), alignof(V)>::type storage_;
    E err_;
  };
};

/// @brief Specialization for void value type.
template <typename E>
class expected<void, E> {
 public:
  static expected success() noexcept {
    expected e;
    e.has_val_ = true;
    return e;
  }

  static expected error(const E& err) noexcept {
    expected e;
    e.has_val_ = false;
    e.err_ = err;
    return e;
  }

  bool has_value() const noexcept { return has_val_; }
  explicit operator bool() const noexcept { return has_val_; }

  E& error_value() noexcept { return err_; }
  const E& error_value() const noexcept { return err_; }

 private:
  expected() = default;
  bool has_val_ = false;
  E err_{};
};

// ============================================================================
// function_ref<R(Args...)>  --  Non-owning callable reference
// ============================================================================

template <typename Sig>
class function_ref;

template <typename R, typename... Args>
class function_ref<R(Args...)> {
 public:
  template <typename F, typename = std::enable_if_t<!std::is_same<std::decay_t<F>, function_ref>::value>>
  function_ref(F&& f) noexcept
      : obj_(const_cast<void*>(static_cast<const void*>(&f))), invoke_(&Invoke<std::decay_t<F>>) {}

  R operator()(Args... args) const { return invoke_(obj_, std::forward<Args>(args)...); }

 private:
  using InvokeFn = R (*)(void*, Args...);

  template <typename F>
  static R Invoke(void* obj, Args... args) {
    return (*static_cast<F*>(obj))(std::forward<Args>(args)...);
  }

  void* obj_ = nullptr;
  InvokeFn invoke_ = nullptr;
};

}  // namespace embsh

#endif  // EMBSH_TYPES_HPP_
