#pragma once

#include <memory>

struct bad_function_call : std::exception {
  const char* what() const noexcept {
    return "Function is not definition";
  }
};

namespace details {

  using storage_t = std::aligned_storage_t<sizeof(void*), alignof(void*)>;

  template <typename T>
  constexpr inline bool fits_small =
      sizeof(T) < sizeof(storage_t) && std::is_nothrow_move_constructible_v<T> &&
      alignof(storage_t) % alignof(T) == 0 &&
      std::is_nothrow_move_assignable_v<T>;

  template <typename T>
  static T const* get(storage_t const& data) {
    if constexpr (fits_small<T>) {
      return reinterpret_cast<T const*>(&data);
    } else {
      return *reinterpret_cast<T* const*>(&data);
    }
  }

  template <typename T>
  static T* get(storage_t& data) {
    if constexpr (fits_small<T>) {
      return reinterpret_cast<T*>(&data);
    } else {
      return *reinterpret_cast<T**>(&data);
    }
  }

} // namespace details

template <typename R, typename... Args>
struct type_descriptor {

  void (*copy)(details::storage_t const& src,
               details::storage_t& dst); // pointer to func
  void (*move)(details::storage_t& src, details::storage_t& dst);
  void (*destroy)(details::storage_t& src);
  R (*invoke)(details::storage_t const& src, Args&&... args);

  template <typename T>
  static void move_val_to_storage(details::storage_t& storage, T& val) {
    if constexpr (details::fits_small<T>) {
      new (&storage) T(std::move(val));
    } else {
      auto ptr = new T(std::move(val));
      new (&storage) T*(ptr);
    }
  }

  static type_descriptor<R, Args...> const*
  get_empty_func_descriptor() noexcept {
    constexpr static type_descriptor<R, Args...> result = {
        [](details::storage_t const&, details::storage_t&) {},
        [](details::storage_t&, details::storage_t&) {},
        [](details::storage_t&) {},
        [](details::storage_t const&, Args&&...) -> R {
          throw bad_function_call{};
        }
    };

    return &result;
  }

  template <typename T>
  static type_descriptor<R, Args...> const* get_descriptor() noexcept {

    constexpr static type_descriptor<R, Args...> result = {
        [](details::storage_t const& src, details::storage_t& dst) {
          // copy T from src to dst
          if constexpr (details::fits_small<T>) {
            new (&dst) T(*details::get<T>(src));
          } else {
            new (&dst)(void*)(static_cast<void*>(new T(*details::get<T>(src))));
          }
        },
        [](details::storage_t& src, details::storage_t& dst) {
          // move T from src to dst
          if constexpr (details::fits_small<T>) {
            new (&dst) T(std::move(*details::get<T>(src)));
            details::get<T>(src)->~T();
          } else {
            new (&dst)(void*)(static_cast<void*>(details::get<T>(src)));
          }
        },
        [](details::storage_t& src) {
          // destroy T from src
          if constexpr (details::fits_small<T>) {
            details::get<T>(src)->~T();
          } else {
            delete details::get<T>(src);
          }
        },
        [](details::storage_t const& src, Args&&... args) -> R {
          // invoke T with args
          return (*details::get<T>(src))(std::forward<Args>(args)...);
        }};

    return &result;
  }

private:
};

template <typename F>
struct function;

template <typename R, typename... Args>
struct function<R(Args...)> {
  function() noexcept
      : desc(type_descriptor<R, Args...>::get_empty_func_descriptor()) {}

  function(function const& other) : desc(other.desc) {
    other.desc->copy(other.storage, this->storage);
  }

  function(function&& other) noexcept : desc(other.desc) {
    other.desc->move(other.storage, this->storage);
    other.desc = type_descriptor<R, Args...>::get_empty_func_descriptor();
  }

  template <typename T>
  function(T val) : desc(type_descriptor<R, Args...>::template get_descriptor<T>()) {
    desc->move_val_to_storage(storage, val);
  }

  function& operator=(function const& rhs) {
    // strong guarantee
    if (this != &rhs) {
      function(rhs).swap(*this);
    }
    return *this;
  }

  void swap(function& other) {
    using std::swap;
    details::storage_t tmp;
    other.desc->move(other.storage, tmp);
    desc->move(storage, other.storage);
    other.desc->move(tmp, storage);
    swap(desc, other.desc);
  }

  function& operator=(function&& rhs) noexcept {
    if (this != &rhs) {
      function(std::move(rhs)).swap(*this);
    }
    return *this;
  }

  ~function() {
    desc->destroy(storage);
  }

  explicit operator bool() const noexcept {
    return type_descriptor<R, Args...>::get_empty_func_descriptor() != desc;
  }

  R operator()(Args... args) const {
    return desc->invoke(storage, std::forward<Args>(args)...);
  }

  template <typename T>
  T* target() noexcept {
    return const_cast<T*>(get_target<T>());
  }

  template <typename T>
  T const* target() const noexcept {
    return get_target<T>();
  }

private:
  details::storage_t storage; // 8 bytes, char[8]
  const type_descriptor<R, Args...>* desc;

  template <typename T>
  T const* get_target() const noexcept {
    if (!*this || desc != type_descriptor<R, Args...>::template get_descriptor<T>()) {
      return nullptr;
    }
    return details::get<T>(storage);
  }

};
