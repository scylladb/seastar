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
 * Copyright 2015 Cloudius Systems
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <string>
#include <vector>
#include <concepts>
#endif

#include <seastar/core/chunked_fifo.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/sstring.hh>
#include <seastar/json/formatter.hh>
#include <seastar/util/modules.hh>
#include <seastar/http/types.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/generator.hh>
#include <seastar/coroutine/as_future.hh>

namespace seastar {

namespace json {

SEASTAR_MODULE_EXPORT_BEGIN

/**
 * The base class for all json element.
 * Every json element has a name
 * An indication if it was set or not
 * And is this element is mandatory.
 * When a mandatory element is not set
 * this is not a valid object
 */
class json_base_element {
protected:
    /**
     * The constructors
     */
    json_base_element() noexcept
            : _mandatory(false), _set(false) {
    }

    json_base_element(const json_base_element& o) noexcept = default;
    json_base_element& operator=(const json_base_element& o) noexcept {
        // Names and mandatory are never changed after creation
        _set = o._set;
        return *this;
    }

    json_base_element(json_base_element&&) = delete;
    json_base_element& operator=(json_base_element&&) = delete;
public:
    virtual ~json_base_element() = default;
    /**
     * Check if it's a mandatory parameter
     * and if it's set.
     * @return true if this is not a mandatory parameter
     * or if it is and it's value is set
     */
    virtual bool is_verify() noexcept {
        return !(_mandatory && !_set);
    }

    /**
     * returns the internal value in a json format
     * Each inherit class must implement this method
     * @return formated internal value
     */
    virtual std::string to_string() = 0;

    virtual future<> write(output_stream<char>& s) const = 0;
    std::string _name;
    bool _mandatory;
    bool _set;
};

/**
 * Basic json element instantiate
 * the json_element template.
 * it adds a value to the base definition
 * and the to_string implementation using the formatter
 */
template<class T>
class json_element : public json_base_element {
public:

    /**
     * the assignment operator also set
     * the set value to true.
     * @param new_value the new value
     * @return the value itself
     */
    json_element &operator=(const T& new_value) {
        _value = new_value;
        _set = true;
        return *this;
    }
    /**
     * the assignment operator also set
     * the set value to true.
     * @param new_value the new value
     * @return the value itself
     */
    template<class C>
    json_element &operator=(const C& new_value) {
        _value = new_value;
        _set = true;
        return *this;
    }
    /**
     * The brackets operator
     * @return the value
     */
    const T& operator()() const noexcept {
        return _value;
    }

    /**
     * The to_string return the value
     * formated as a json value
     * @return the value formatted for json
     */
    virtual std::string to_string() override
    {
        return formatter::to_json(_value);
    }

    virtual future<> write(output_stream<char>& s) const override {
        return formatter::write(s, _value);
    }
private:
    T _value;
};

/**
 * json_list_template is an array type based on a
 * container type passed as a template parameter, as we want to
 * have flavors based on both vector and chunked_fifo.
 *
 * When values are added with push it is set the "set" flag to true
 * hence will be included in the parsed object
 */
template <class T, class Container>
class json_list_template : public json_base_element {
public:

    /**
     * Add an element to the list.
     * @param element a new element that will be added to the end of the list
     */
    void push(const T& element) {
        _set = true;
        _elements.push_back(element);
    }

    /**
     * Move an element into the list.
     * @param element a new element that will be added to the list using move-construction
     */
    void push(T&& element) {
        _set = true;
        _elements.push_back(std::move(element));
    }

    virtual std::string to_string() override
    {
        return formatter::to_json(_elements);
    }

    /**
     * Assignment can be done from any object that support const range
     * iteration and that it's elements can be assigned to the list elements
     */
    template<class C>
    json_list_template& operator=(const C& list) {
        _elements.clear();
        for  (auto i : list) {
            push(i);
        }
        return *this;
    }
    virtual future<> write(output_stream<char>& s) const override {
        return formatter::write(s, _elements);
    }

    Container _elements;
};

template <typename T>
using json_list = json_list_template<T, std::vector<T>>;

template <typename T>
using json_chunked_list = json_list_template<T, seastar::chunked_fifo<T>>;

class jsonable {
public:
    jsonable() = default;
    jsonable(const jsonable&) = default;
    jsonable& operator=(const jsonable&) = default;
    virtual ~jsonable() = default;
    /**
     * create a formatted string of the object.
     * @return the object formated.
     */
    virtual std::string to_json() const = 0;

    /*!
     * \brief write an object to the output stream
     *
     * The defult implementation uses the to_json
     * Object implementation override it.
     */
    virtual future<> write(output_stream<char>& s) const {
        return s.write(to_json());
    }
};

template <typename T>
concept Jsonable = requires (T j, output_stream<char>& s) {
    { j.to_json() } -> std::convertible_to<std::string_view>;
    { j.write(s) } -> std::same_as<future<>>;
};

/**
 * The base class for all json objects
 * It holds a list of all the element in it,
 * allowing it implement the to_json method.
 *
 * It also allows iterating over the element
 * in the object, even if not all the member
 * are known in advance and in practice mimic
 * reflection
 */
struct json_base : public jsonable {

    virtual ~json_base() = default;

    json_base() = default;

    json_base(const json_base&) = delete;

    json_base operator=(const json_base&) = delete;

    /**
     * create a formatted string of the object.
     * @return the object formatted.
     */
    virtual std::string to_json() const;

    /*!
     * \brief write to an output stream
     */
    virtual future<> write(output_stream<char>&) const;

    /**
     * Check that all mandatory elements are set
     * @return true if all mandatory parameters are set
     */
    virtual bool is_verify() const;

    /**
     * Register an element in an object
     * @param element the element to be added
     * @param name the element name
     * @param mandatory is this element mandatory.
     */
    virtual void add(json_base_element* element, std::string name,
            bool mandatory = false);

    std::vector<json_base_element*> _elements;
};

/**
 * There are cases where a json request needs to return a successful
 * empty reply.
 * The json_void class will be used to mark that the reply should be empty.
 *
 */
struct json_void : public jsonable{
    virtual std::string to_json() const {
        return "";
    }

    /*!
     * \brief write to an output stream
     */
    virtual future<> write(output_stream<char>& s) const {
        return s.close();
    }
};


/**
 * The json return type, is a helper class to return a json
 * formatted string.
 * It uses autoboxing in its constructor so when a function return
 * type is json_return_type, it could return a type that would be converted
 * ie.
 * json_return_type foo() {
 *     return "hello";
 * }
 *
 * would return a json formatted string: "hello" (rather then hello)
 */
struct json_return_type {
    sstring _res;
#if SEASTAR_API_LEVEL >= 8
    using body_writer_type = http::body_writer_type;
#else
    using body_writer_type = std::function<future<>(output_stream<char>&&)>;
#endif
    body_writer_type _body_writer;
    json_return_type(body_writer_type&& body_writer) : _body_writer(std::move(body_writer)) {
    }
    template<class T>
    json_return_type(const T& res) {
        _res = formatter::to_json(res);
    }

   json_return_type(json_return_type&& o) noexcept : _res(std::move(o._res)), _body_writer(std::move(o._body_writer)) {
   }
    json_return_type& operator=(json_return_type&& o) noexcept {
        if (this != &o) {
            _res = std::move(o._res);
            _body_writer = std::move(o._body_writer);
        }
        return *this;
    }

#if SEASTAR_API_LEVEL < 8
    json_return_type(const json_return_type&) = default;
    json_return_type& operator=(const json_return_type&) = default;
#endif
};

/*!
 * \brief capture a range and return a serialize function for it as a json array.
 *
 * To use it, pass a range and a mapping function.
 * For example, if res is a map:
 *
 * return make_ready_future<json::json_return_type>(stream_range_as_array(res, [](const auto&i) {return i.first}));
 */
template<typename Container, typename Func>
requires requires (Container c, Func aa, output_stream<char> s) { { formatter::write(s, aa(*c.begin())) } -> std::same_as<future<>>; }
json_return_type::body_writer_type stream_range_as_array(Container val, Func fun) {
    return [val = std::move(val), fun = std::move(fun)](output_stream<char>&& s) mutable {
        return do_with(output_stream<char>(std::move(s)), Container(std::move(val)), Func(std::move(fun)), true, [](output_stream<char>& s, const Container& val, const Func& f, bool& first){
            return s.write("[").then([&val, &s, &first, &f] () {
                return do_for_each(val, [&s, &first, &f](const typename Container::value_type& v){
                    auto fut = first ? make_ready_future<>() : s.write(", ");
                    first = false;
                    return fut.then([&s, &f, &v]() {
                        return formatter::write(s, f(v));
                    });
                });
            }).then([&s](){
                return s.write("]");
            }).finally([&s] {
                return s.close();
            });
        });
    };
}

/*!
 * \brief consume jsonable values from a coroutine generator \c gen and write them
 * onto the \c out output_stream as a json array.
 */
template<Jsonable T>
future<> generate_array_element(output_stream<char>& out, coroutine::experimental::generator<T>& gen) {
    bool first = true;
    co_await out.write("[");
    while (auto val = co_await gen()) {
        if (first) {
            first = false;
        } else {
            co_await out.write(", ");
        }
        co_await formatter::write(out, *val);
    }
    co_await out.write("]");
}

#if SEASTAR_API_LEVEL >= 8

/*!
 * \brief return a json body_writer function that consumes jsonable values from a coroutine generator \c gen
 * and writes them onto its \c output_stream parameter.
 *
 * To use it, pass a reference to coroutine::generator \c gen.
 * For example, if ``gen`` is a coroutine::generator<std::pair<K, V>> and you want to return the keys as json array
 *
 * return make_ready_future<json::json_return_type>(generate_array(gen, [](const auto&i) {return i.first}));
 *
 * Note that \c gen is passed by reference since we need to return a copyable function but generators cannot be copied.
 * So the caller is responsible for ensuring that the generator remains valid for the lifetime of the returned function.
 */
template<Jsonable T>
json_return_type::body_writer_type generate_array(coroutine::experimental::generator<T> gen) {
    return [gen_ = std::move(gen)] (output_stream<char>&& s) mutable -> future<> {
        auto gen = std::move(gen_);
        auto out = std::move(s);
        auto f = co_await coroutine::as_future(generate_array_element(out, gen));
        co_await out.close();
        f.get();
    };
}

#endif // SEASTAR_API_LEVEL >= 8

/*!
 * \brief capture an object and return a serialize function for it.
 *
 * To use it:
 * return make_ready_future<json::json_return_type>(stream_object(res));
 */
template<class T>
json_return_type::body_writer_type stream_object(T val) {
    return [val = std::move(val)](output_stream<char>&& s) mutable {
        return do_with(output_stream<char>(std::move(s)), T(std::move(val)), [](output_stream<char>& s, T& val){
            return formatter::write(s, std::move(val)).finally([&s] {
                return s.close();
            });
        });
    };
}

SEASTAR_MODULE_EXPORT_END
}

}
