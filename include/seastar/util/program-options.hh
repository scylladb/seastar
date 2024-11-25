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
 * Copyright (C) 2017 ScyllaDB
 */

#pragma once

#include <seastar/core/format.hh>
#include <seastar/core/sstring.hh>
#include <seastar/util/modules.hh>

#ifndef SEASTAR_MODULE
#include <fmt/format.h>

#include <boost/any.hpp>
#include <boost/intrusive/list.hpp>

#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <optional>
#include <set>
#include <memory>
#endif

/// \defgroup program-options Program Options
///
/// \brief Infrastructure for configuring a seastar application
///
/// The program-options infrastructure allows configuring seastar both by C++
/// code and by command-line and/or config files. This is achieved by
/// providing a set of self-describing and self-validating value types as well
/// as option groups to allow grouping them into arbitrary tree structures.
/// Seastar modules expose statically declared option structs, which derive from
/// \ref option_group and contain various concrete \ref basic_value members
/// comprising the required configuration. These structs are self-describing, and
/// self-validating, the name of the option group as well as the list of its
/// \ref basic_value member can be queried run-time.

namespace seastar {

namespace program_options {

///
/// \brief Wrapper for command-line options with arbitrary string associations.
///
/// This type, to be used with Boost.Program_options, will result in an option that stores an arbitrary number of
/// string associations.
///
/// Values are specified in the form "key0=value0:[key1=value1:...]". Options of this type can be specified multiple
/// times, and the values will be merged (with the last-provided value for a key taking precedence).
///
/// \note We need a distinct type (rather than a simple type alias) for overload resolution in the implementation, but
/// advertizing our inheritance of \c std::unordered_map would introduce the possibility of memory leaks since STL
/// containers do not declare virtual destructors.
///
class string_map final : private std::unordered_map<sstring, sstring> {
private:
    using base = std::unordered_map<sstring, sstring>;
public:
    using base::value_type;
    using base::key_type;
    using base::mapped_type;

    using base::base;
    using base::at;
    using base::find;
    using base::count;
    using base::emplace;
    using base::clear;
    using base::operator[];
    using base::begin;
    using base::end;

    friend bool operator==(const string_map&, const string_map&);
    friend bool operator!=(const string_map&, const string_map&);
};

inline bool operator==(const string_map& lhs, const string_map& rhs) {
    return static_cast<const string_map::base&>(lhs) == static_cast<const string_map::base&>(rhs);
}

inline bool operator!=(const string_map& lhs, const string_map& rhs) {
    return !(lhs == rhs);
}

///
/// \brief Query the value of a key in a \c string_map, or a default value if the key doesn't exist.
///
sstring get_or_default(const string_map&, const sstring& key, const sstring& def = sstring());

std::istream& operator>>(std::istream& is, string_map&);
std::ostream& operator<<(std::ostream& os, const string_map&);

/// \cond internal

//
// Required implementation hook for Boost.Program_options.
//
void validate(boost::any& out, const std::vector<std::string>& in, string_map*, int);

using list_base_hook = boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;

} // namespace program_options

SEASTAR_MODULE_EXPORT_BEGIN
enum class log_level;
enum class logger_timestamp_style;
enum class logger_ostream_type;
namespace memory {
    enum class alloc_failure_kind;
}

namespace program_options {

/// \endcond

/// \addtogroup program-options
/// @{

class option_group;

/// Visitor interface for \ref option_group::describe().
///
/// See \ref option_group::describe() for more details on the visiting algorithm.
class options_descriptor {
public:
    /// Visit the start of the group.
    ///
    /// Called when entering a group. Groups can be nested, in which case there
    /// will be another call to this method, before the current groups is closed.
    /// \returns whether visitor is interested in the group: \p true - visit,
    /// \p false - skip.
    virtual bool visit_group_start(const std::string& name, bool used) = 0;
    /// Visit the end of the group.
    ///
    /// Called after all values and nested groups were visited in the current
    /// group.
    virtual void visit_group_end() = 0;

    /// Visit value metadata, common across all value types.
    ///
    /// Called at the start of visiting a value. After this, a call to the
    /// appropriate \ref visit_value() overload (or \ref visit_selection_value())
    /// follows.
    /// \returns whether visitor is interested in the value: \p true - visit,
    /// \p false - skip.
    virtual bool visit_value_metadata(const std::string& name, const std::string& description, bool used) = 0;

    /// Visit a switch (\ref value<std::monostate>).
    virtual void visit_value() = 0;
    /// Visit a value (\ref value), \p default_val is null when value has no default.
    virtual void visit_value(const bool* default_val) = 0;
    /// Visit a value (\ref value), \p default_val is null when value has no default.
    virtual void visit_value(const int* default_val) = 0;
    /// Visit a value (\ref value), \p default_val is null when value has no default.
    virtual void visit_value(const unsigned* default_val) = 0;
    /// Visit a value (\ref value), \p default_val is null when value has no default.
    virtual void visit_value(const float* default_val) = 0;
    /// Visit a value (\ref value), \p default_val is null when value has no default.
    virtual void visit_value(const double* default_val) = 0;
    /// Visit a value (\ref value), \p default_val is null when value has no default.
    virtual void visit_value(const std::string* default_val) = 0;
    /// Visit a value (\ref value), \p default_val is null when value has no default.
    virtual void visit_value(const std::set<unsigned>* default_val) = 0;
    /// Visit a value (\ref value), \p default_val is null when value has no default.
    virtual void visit_value(const log_level* default_val) = 0;
    /// Visit a value (\ref value), \p default_val is null when value has no default.
    virtual void visit_value(const logger_timestamp_style* default_val) = 0;
    /// Visit a value (\ref value), \p default_val is null when value has no default.
    virtual void visit_value(const logger_ostream_type* default_val) = 0;
    /// Visit a value (\ref value), \p default_val is null when value has no default.
    virtual void visit_value(const memory::alloc_failure_kind* default_val) = 0;
    /// Visit a value (\ref value), \p default_val is null when value has no default.
    virtual void visit_value(const std::unordered_map<sstring, log_level>* default_val) = 0;

    /// Visit a selection value (\ref selection_value), \p default_candidate is null when there is no default candidate.
    virtual void visit_selection_value(const std::vector<std::string>& candidate_names, const std::size_t* default_candidate) = 0;
};

/// Visitor interface \ref option_group::mutate().
///
/// See \ref option_group::mutate() for more details on the visiting algorithm.
class options_mutator {
public:
    /// Visit the start of the group.
    ///
    /// Called when entering a group. Groups can be nested, in which case there
    /// will be another call to this method, before the current groups is closed.
    /// \returns whether visitor is interested in the group: \p true - visit,
    /// \p false - skip.
    virtual bool visit_group_start(const std::string& name, bool used) = 0;
    /// Visit the end of the group.
    ///
    /// Called after all values and nested groups were visited in the current
    /// group.
    virtual void visit_group_end() = 0;

    /// Visit value metadata, common across all value types.
    ///
    /// Called at the start of visiting a value. After this, a call to the
    /// appropriate \ref visit_value() overload (or \ref visit_selection_value())
    /// follows.
    /// \returns whether visitor is interested in the value: \p true - visit,
    /// \p false - skip.
    virtual bool visit_value_metadata(const std::string& name, bool used) = 0;

    /// Visit a switch (\ref value<std::monostate>), switch is set to returned value.
    virtual bool visit_value() = 0;
    /// Visit and optionally mutate a value (\ref value), should return true if value was mutated.
    virtual bool visit_value(bool& val) = 0;
    /// Visit a value (\ref value), \p default_val is null when value has no default.
    virtual bool visit_value(int& val) = 0;
    /// Visit and optionally mutate a value (\ref value), should return true if value was mutated.
    virtual bool visit_value(unsigned& val) = 0;
    /// Visit and optionally mutate a value (\ref value), should return true if value was mutated.
    virtual bool visit_value(float& val) = 0;
    /// Visit and optionally mutate a value (\ref value), should return true if value was mutated.
    virtual bool visit_value(double& val) = 0;
    /// Visit and optionally mutate a value (\ref value), should return true if value was mutated.
    virtual bool visit_value(std::string& val) = 0;
    /// Visit and optionally mutate a value (\ref value), should return true if value was mutated.
    virtual bool visit_value(std::set<unsigned>& val) = 0;
    /// Visit and optionally mutate a value (\ref value), should return true if value was mutated.
    virtual bool visit_value(log_level& val) = 0;
    /// Visit and optionally mutate a value (\ref value), should return true if value was mutated.
    virtual bool visit_value(logger_timestamp_style& val) = 0;
    /// Visit and optionally mutate a value (\ref value), should return true if value was mutated.
    virtual bool visit_value(logger_ostream_type& val) = 0;
    /// Visit and optionally mutate a value (\ref value), should return true if value was mutated.
    virtual bool visit_value(memory::alloc_failure_kind& val) = 0;
    /// Visit and optionally mutate a value (\ref value), should return true if value was mutated.
    virtual bool visit_value(std::unordered_map<sstring, log_level>& val) = 0;

    /// Visit and optionally mutate a selection value (\ref selection_value), should return true if value was mutated.
    virtual bool visit_selection_value(const std::vector<std::string>& candidate_names, std::size_t& selected_candidate) = 0;
};

/// A tag type used to construct unused \ref option_group and \ref basic_value objects.
struct unused {};

class basic_value;

/// A group of options.
///
/// \ref option_group is the basis for organizing options. It can hold a number
/// of \ref basic_value objects. These are typically also its members:
///
///     struct my_option_group : public option_group {
///         value<> opt1;
///         value<bool> opt2;
///         ...
///
///         my_option_group()
///             : option_group(nullptr, "My option group")
///             , opt1(this, "opt1", ...
///             , opt2(this, "opt2", ...
///             , ...
///         { }
///     };
///
/// Option groups can also be nested and using this property one can build a
/// tree of option groups and values. This tree then can be visited using the
/// two visitor methods exposed by \ref option_group:
/// * \ref describe()
/// * \ref mutate()
///
/// Using these two visitors one can easily implement glue code to expose an
/// entire options tree to the command line. Use \ref describe() to build the
/// command-line level description of the objects (using e.g.
/// boost::program_options) and after parsing the provided command-line options
/// use \ref mutate() to propagate the extracted values back into the options
/// tree. How this is done is entirely up to the visitors, the above methods
/// only offer an API to visit each group and value in the tree, they don't make
/// any assumption about how the visitor works and what its purpose is.
class option_group : public list_base_hook {
    friend class basic_value;

public:
    using value_list_type = boost::intrusive::list<
            basic_value,
            boost::intrusive::base_hook<list_base_hook>,
            boost::intrusive::constant_time_size<false>>;

    using option_group_list_type = boost::intrusive::list<
            option_group,
            boost::intrusive::base_hook<list_base_hook>,
            boost::intrusive::constant_time_size<false>>;

private:
    option_group* _parent;
    bool _used = true;
    std::string _name;
    value_list_type _values;
    option_group_list_type _subgroups;

public:
    /// Construct an option group.
    ///
    /// \param parent - the parent option-group, this option group will become a
    /// sub option group of the parent group
    /// \param name - the name of the option group
    explicit option_group(option_group* parent, std::string name);
    /// Construct an unused option group.
    ///
    /// \param parent - the parent option-group, this option group will become a
    /// sub option group of the parent group
    /// \param name - the name of the option group
    explicit option_group(option_group* parent, std::string name, unused);
    option_group(option_group&&);
    option_group(const option_group&) = delete;
    virtual ~option_group() = default;

    option_group& operator=(option_group&&) = delete;
    option_group& operator=(const option_group&) = delete;

    /// Does the option group has any values contained in it?
    operator bool () const { return !_values.empty(); }
    bool used() const { return _used; }
    const std::string& name() const { return _name; }
    const value_list_type& values() const { return _values; }
    value_list_type& values() { return _values; }

    /// Describe the content of this option group to the visitor.
    ///
    /// The content is visited in a depth-first manner:
    /// * First the option groups itself is visited with
    ///   \ref options_descriptor::visit_group_start(). If this returns \p false
    ///   the entire content of the group, including all its subgroups and values
    ///   are skipped and \ref options_descriptor::visit_group_end() is called
    ///   immediately. Otherwise visiting the content of the group proceeds.
    /// * All the values contained therein are visited. For each value the
    ///   following happens:
    ///     - First \ref options_descriptor::visit_value_metadata() is called
    ///       with generic metadata that all values have. If this return
    ///       \p false the value is skipped, otherwise visiting the value
    ///       proceeds.
    ///     - Then the appropriate overload of
    ///       \ref options_descriptor::visit_value() is called, with a pointer
    ///       to the default value of the respective value. The pointer is null
    ///       if there is no default value.
    ///     - For \ref selection_value,
    ///       \ref options_descriptor::visit_selection_value() will be called
    ///       instead of \ref options_descriptor::visit_value(). After the value
    ///       is visited, the \ref option_group instance belonging to each
    ///       candidate (if set) will be visited.
    /// * All the nested \ref option_group instances in the current group are
    ///   visited.
    /// * Finally \ref options_descriptor::visit_group_end() is called.
    void describe(options_descriptor& descriptor) const;
    /// Mutate the content of this option group by the visitor.
    ///
    /// The visiting algorithm is identical to that of \ref describe(), with the
    /// following differences:
    /// * \ref options_mutator::visit_value() is allowed to mutate the value
    ///   through the passed-in reference. It should return \p true if it did so
    ///   and \p false otherwise.
    /// * When visiting a selection value, only the nested group belonging to
    ///   the selected value is visited afterwards.
    void mutate(options_mutator& mutator);
};

/// A basic configuration option value.
///
/// This serves as the common base-class of all the concrete value types.
class basic_value : public list_base_hook {
    friend class option_group;

public:
    option_group* _group;
    bool _used = true;
    std::string _name;
    std::string _description;

private:
    virtual void do_describe(options_descriptor& descriptor) const = 0;
    virtual void do_mutate(options_mutator& mutator) = 0;

public:
    basic_value(option_group& group, bool used, std::string name, std::string description);
    basic_value(basic_value&&);
    basic_value(const basic_value&) = delete;
    virtual ~basic_value() = default;

    basic_value& operator=(basic_value&&) = delete;
    basic_value& operator=(const basic_value&) = delete;

    bool used() const { return _used; }
    const std::string& name() const { return _name; }
    const std::string& description() const { return _description; }

    void describe(options_descriptor& descriptor) const;
    void mutate(options_mutator& mutator);
};

/// A configuration option value.
///
/// \tparam T the type of the contained value.
template <typename T = std::monostate>
class value : public basic_value {
    std::optional<T> _value;
    bool _defaulted = true;

private:
    virtual void do_describe(options_descriptor& descriptor) const override {
        auto* val = _value ? &*_value : nullptr;
        descriptor.visit_value(val);
    }
    virtual void do_mutate(options_mutator& mutator) override {
        T val;
        if (mutator.visit_value(val)) {
            _value = std::move(val);
            _defaulted = false;
        }
    }
    void do_set_value(T value, bool defaulted) {
        _value = std::move(value);
        _defaulted = defaulted;
    }

public:
    /// Construct a value.
    ///
    /// \param group - the group containing this value
    /// \param name - the name of this value
    /// \param default_value - the default value, can be unset
    /// \param description - the description of the value
    value(option_group& group, std::string name, std::optional<T> default_value, std::string description)
        : basic_value(group, true, std::move(name), std::move(description))
        , _value(std::move(default_value))
    { }
    /// Construct an unused value.
    value(option_group& group, std::string name, unused)
        : basic_value(group, false, std::move(name), {})
    { }
    value(value&&) = default;
    /// Is there a contained value?
    operator bool () const { return bool(_value); }
    /// Does this value still contain a default-value?
    bool defaulted() const { return _defaulted; }
    /// Return the contained value, assumes there is one, see \ref operator bool().
    const T& get_value() const { return _value.value(); }
    T& get_value() { return _value.value(); }
    void set_default_value(T value) { do_set_value(std::move(value), true); }
    void set_value(T value) { do_set_value(std::move(value), false); }
};

/// A switch-style configuration option value.
///
/// Contains no value, can be set or not.
template <>
class value<std::monostate> : public basic_value {
    std::optional<bool> _set;

private:
    virtual void do_describe(options_descriptor& descriptor) const override {
        descriptor.visit_value();
    }
    virtual void do_mutate(options_mutator& mutator) override {
        bool is_set = mutator.visit_value();
        if (_set.has_value()) {
            // override the value only if it is not preset
            if (is_set) {
                _set = true;
            }
        } else {
            _set = is_set;
        }
    }

public:
    /// Construct a value.
    ///
    /// \param group - the group containing this value
    /// \param name - the name of this value
    /// \param description - the description of the value
    value(option_group& group, std::string name, std::string description)
        : basic_value(group, true, std::move(name), std::move(description))
    { }
    /// Construct an unused value.
    value(option_group& group, std::string name, unused)
        : basic_value(group, false, std::move(name), {})
    { }
    /// Is the option set?
    operator bool () const { return _set ? _set.value() : false; }
    void set_value() { _set = true; }
    void unset_value() { _set = false; }
};

/// A selection value, allows selection from multiple candidates.
///
/// The candidates objects are of an opaque type which may not accessible to
/// whoever is choosing between the available candidates. This allows the user
/// selecting between seastar internal types without exposing them.
/// Each candidate has a name, which is what the users choose based on. Each
/// candidate can also have an associated \ref option_group containing related
/// candidate-specific configuration options, allowing further configuring the
/// selected candidate. The code exposing the candidates should document the
/// concrete types these can be down-casted to.
template <typename T = std::monostate>
class selection_value : public basic_value {
public:
    using deleter = std::function<void(T*)>;
    using value_handle = std::unique_ptr<T, deleter>;
    struct candidate {
        std::string name;
        value_handle value;
        std::unique_ptr<option_group> opts;
    };
    using candidates = std::vector<candidate>;

private:
    static constexpr size_t no_selected_candidate = -1;

private:
    candidates _candidates;
    size_t _selected_candidate = no_selected_candidate;
    bool _defaulted = true;

public:
    std::vector<std::string> get_candidate_names() const {
        std::vector<std::string> candidate_names;
        candidate_names.reserve(_candidates.size());
        for (const auto& c : _candidates) {
            candidate_names.push_back(c.name);
        }
        return candidate_names;
    }
private:
    virtual void do_describe(options_descriptor& descriptor) const override {
        descriptor.visit_selection_value(get_candidate_names(), _selected_candidate == no_selected_candidate ? nullptr : &_selected_candidate);
        for (auto& c : _candidates) {
            if (c.opts) {
                c.opts->describe(descriptor);
            }
        }
    }
    virtual void do_mutate(options_mutator& mutator) override {
        if (mutator.visit_selection_value(get_candidate_names(), _selected_candidate)) {
            _defaulted = false;
        }
        if (_selected_candidate != no_selected_candidate) {
            auto& c = _candidates.at(_selected_candidate);
            if (c.opts) {
                c.opts->mutate(mutator);
            }
        }
    }
    size_t find_candidate(const std::string& candidate_name) {
        auto it = find_if(_candidates.begin(), _candidates.end(), [&] (const auto& candidate) {
            return candidate.name == candidate_name;
        });
        if (it == _candidates.end()) {
            throw std::invalid_argument(fmt::format("find_candidate(): failed to find candidate {}", candidate_name));
        }
        return it - _candidates.begin();
    }

    option_group* do_select_candidate(std::string candidate_name, bool defaulted) {
        _selected_candidate = find_candidate(candidate_name);
        _defaulted = defaulted;
        return _candidates.at(_selected_candidate).opts.get();
    }

public:
    /// Construct a value.
    ///
    /// \param group - the group containing this value
    /// \param name - the name of this value
    /// \param candidates - the available candidates
    /// \param default_candidates - the name of the default candidate
    /// \param description - the description of the value
    selection_value(option_group& group, std::string name, candidates candidates, std::string default_candidate, std::string description)
        : basic_value(group, true, std::move(name), std::move(description))
        , _candidates(std::move(candidates))
        , _selected_candidate(find_candidate(default_candidate))
    { }
    selection_value(option_group& group, std::string name, candidates candidates, std::string description)
        : basic_value(group, true, std::move(name), std::move(description))
        , _candidates(std::move(candidates))
    { }
    /// Construct an unused value.
    selection_value(option_group& group, std::string name, unused)
        : basic_value(group, false, std::move(name), {})
    { }
    /// Was there a candidate selected (default also counts)?
    operator bool () const { return _selected_candidate != no_selected_candidate; }
    /// Is the currently selected candidate the default one?
    bool defaulted() const { return _defaulted; }
    /// Get the name of the currently selected candidate (assumes there is one selected, see \operator bool()).
    const std::string& get_selected_candidate_name() const { return _candidates.at(_selected_candidate).name; }
    /// Get the options of the currently selected candidate (assumes there is one selected, see \operator bool()).
    const option_group* get_selected_candidate_opts() const { return _candidates.at(_selected_candidate).opts.get(); }
    /// Get the options of the currently selected candidate (assumes there is one selected, see \operator bool()).
    option_group* get_selected_candidate_opts() { return _candidates.at(_selected_candidate).opts.get(); }
    T& get_selected_candidate() const { return *_candidates.at(_selected_candidate).value; }
    /// Select a candidate.
    ///
    /// \param candidate_name - the name of the to-be-selected candidate.
    option_group* select_candidate(std::string candidate_name) { return do_select_candidate(candidate_name, false); }
    /// Select a candidate and make it the default.
    ///
    /// \param candidate_name - the name of the to-be-selected candidate.
    option_group* select_default_candidate(std::string candidate_name) { return do_select_candidate(candidate_name, true); }
};

/// @}

}
SEASTAR_MODULE_EXPORT_END

}
