#pragma once

#include <seastar/core/shared_ptr.hh>
#include <stdexcept>

namespace seastar {

//! Safe pointer that keeps track of whether the pointed object is still alive
template <typename T>
class safe_ptr {
    T* raw_ptr_ {nullptr};
    lw_shared_ptr<bool> pointee_is_alive_;

  public:
    safe_ptr() = default;
    explicit safe_ptr(T* ptr, lw_shared_ptr<bool> sp) : raw_ptr_(ptr), pointee_is_alive_(std::move(sp))
    {
    }

    //! Dereference the pointer. Throws std::runtime_error if the pointee is dead.
    T& operator*() const
    {
        if (*pointee_is_alive_) {
            return *raw_ptr_;
        }
        else {
            throw std::runtime_error("tried to dereference pointer to dead object");
        }
    }

    //! Gets a pointer p to the object. Returns nullptr if the pointee is dead.
    T* get() const noexcept
    {
        if (*pointee_is_alive_) {
            return raw_ptr_;
        }
        else {
            return nullptr;
        }
    }

    //! Check whether the pointee is still alive
    bool valid() const noexcept
    {
        return *pointee_is_alive_;
    }

    //! 'operator bool'-overload to check whether the pointee is still alive
    explicit operator bool() const noexcept
    {
        return valid();
    }
};

//! Mechanism for interacting with a possibly destructed object
/*!
 * Sometimes some object A needs to interact with another object B, e.g., A calls one of B's methods.
 * If B happens to be already destructed (and freed), this results in a use-after-free bug.
 * One way to avoid this problem is for A to manage the lifetime of B via a unique_ptr or shared_ptr.
 * Sometimes, however, it is undesirable to have such ownership relation, and instead A would like to 
 * just skip the interaction in case B has already died (in the above example, A would simply not call
 * the method on B).
 *
 * This class provides a solution; B is given a safe_ptr_factory as class member:
 *
 * \code{.cpp}
 * class B {
 * public:
 *      safe_ptr_factory<B> sp_factory;
 *      B() : sp_factory(this) {}
 * };
 *
 * safe_ptr<B> sp;
 * {
 *      B b;
 *      sp = b.sp_factory.get_safe_ptr();
 * } // b destructs here
 *
 * *sp; // throws exception
 * \endcode
 */ 
template <typename T>
class safe_ptr_factory {
    T* ptr_ {nullptr};
    lw_shared_ptr<bool> alive_;

  public:
    explicit safe_ptr_factory(T* ptr) : ptr_(ptr), alive_(make_lw_shared<bool>(true))
    {
    }
    safe_ptr_factory(safe_ptr_factory const&) = default;
    safe_ptr_factory(safe_ptr_factory&&) noexcept = default;
    safe_ptr_factory& operator=(safe_ptr_factory const&) = default;
    safe_ptr_factory& operator=(safe_ptr_factory&&) noexcept = default;
    ~safe_ptr_factory() noexcept
    {
        // notify destruction to shared_state
        *alive_ = false;
    }

    //! Get a safe pointer to the pointed object
    safe_ptr<T> get_safe_ptr()
    {
        return safe_ptr<T>{ptr_, alive_};
    }
};

} // namespace seastar
