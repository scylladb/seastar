/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2016 Cloudius Systems, Ltd.
 */
#pragma once

#include <ostream>

/// \addtogroup logging
/// @{

namespace seastar {

/// \brief This class is a wrapper for a lazy evaluation of a value.
///
/// The value is evaluated by a functor that gets no parameters, which is
/// provided to a lazy_value constructor.
///
/// The instance may be created only using seastar::value_of helper function.
///
/// The evaluation is triggered by operator().
template<typename Func>
class lazy_eval {
private:
    Func _func;

private:
    lazy_eval(Func&& f) : _func(std::forward<Func>(f)) {}

public:
    /// \brief Evaluate a value.
    ///
    /// \return the evaluated value
    auto operator()() {
        return _func();
    }

    /// \brief Evaluate a value (const version).
    ///
    /// \return the evaluated value
    auto operator()() const {
        return _func();
    }

    template <typename F>
    friend lazy_eval<F> value_of(F&& func);
};


/// Create a seastar::lazy_eval object that will use a given functor for
/// evaluating a value when the evaluation is triggered.
///
/// The actual evaluation is triggered by applying a () operator on a
/// returned object.
///
/// \param Func a type of a func
/// \param func func a functor to evaluate the value
///
/// \return a lazy_eval object that may be used for evaluating a value
template <typename Func>
inline lazy_eval<Func> value_of(Func&& func) {
    return lazy_eval<Func>(std::forward<Func>(func));
}
}

namespace std {
/// Output operator for a seastar::lazy_eval<Func>
/// This would allow printing a seastar::lazy_eval<Func> as if it's a regular
/// value.
///
/// For example:
///
/// `logger.debug("heavy eval result:{}", seastar::value_of([&] { return <heavy evaluation>; }));`
///
/// (If a logging level is lower than "debug" the evaluation will not take place.)
///
/// \tparam Func a functor type
/// \param os ostream to print to
/// \param lf a reference to a lazy_eval<Func> to be printed
///
/// \return os
template <typename Func>
ostream& operator<<(ostream& os, const seastar::lazy_eval<Func>& lf) {
    return os << lf();
}

template <typename Func>
ostream& operator<<(ostream& os, seastar::lazy_eval<Func>& lf) {
    return os << lf();
}

template <typename Func>
ostream& operator<<(ostream& os, seastar::lazy_eval<Func>&& lf) {
    return os << lf();
}
}
/// @}
