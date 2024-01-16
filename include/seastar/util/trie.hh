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

#pragma once

#include <seastar/core/sstring.hh>
#include <vector>

namespace seastar {

/**
* Trie is used to check the existence of an associated instance based on
* a given string. In very rare scenarios, using trie for lookups has the
* following significant advantages over using hashmap: If all keys in the
* stored key-value pairs are short, then for a query with an extremely long
* key, hashmap will need to traverse the entire key to calculate the hash.
* However, the overhead of trie is only determined by the length of the
* longest key among the stored key-value pairs.
*/
template<typename V>
class trie {
public:
    class node {
        std::vector<node*> _children;
        char _key;
        V _value;
        bool _hasVal;
    private:
        explicit node(): _key(0), _hasVal(false) {}
        ~node() {
            for (auto& child : _children) {
                delete child;
            }
        }

        node* putChild(char key) {
            int size = _children.size();
            int i = 0;
            for ( ; i < size; i++) {
                if (_children[i]->_key > key) {
                    break;
                } else if (_children[i]->_key == key) {
                    return _children[i];
                }
            }
            node* child = new node();
            child->_key = key;
            _children.insert(_children.begin() + i, child);
            return child;
        }

        node* getChild(char key) {
            int size = _children.size();
            int i = 0;
            for ( ; i < size; i++) {
                if (_children[i]->_key > key) {
                    return nullptr;
                } else if (_children[i]->_key == key) {
                    return _children[i];
                }
            }
            return nullptr;
        }

        friend class trie;
    public:
        /**
         * Retrieve the value stored in the node.
         * @return value stored in the node
         */
        V& getValue() {
            return _value;
        }
    };
private:
    node* root;
public:
    explicit trie(): root(new node()) {}
    ~trie() {
        delete root;
    }

    /**
     * Add a key-value pair to this trie.
     * @param str starting pointer of the string as the key
     * @param length length of the string as the key
     * @param value value associated with the string.
     */
    void put(const char* str, unsigned int length, V&& value) {
        node* node = root;
        for (unsigned int i = 0; i < length; i++) {
            node = node->putChild(str[i]);
        }
        node->_value = std::move(value);
        node->_hasVal = true;
    }

    /**
     * Add a key-value pair to this trie.
     * @param str string as the key
     * @param value value associated with the string.
     */
    void put(const sstring& str, V&& value) {
        put(str.data(), str.length(), std::move(value));
    }

    /**
     * Retrieve the node storing the value. When the ultimately found node
     * is not a storage node, we will return null. We can check if the returned
     * node pointer is null to determine whether the string is valid.
     * @param str starting pointer of the string as the key
     * @param length length of the string as the key
     * @return pointer of node storing the value
     */
    node* get(const char* str, unsigned int length) {
        node* node = root;
        for (unsigned int i = 0; i < length; i++) {
            node = node->getChild(str[i]);
            if (node == nullptr) {
                break;
            }
        }
        if (node != nullptr && node->_hasVal) {
            return node;
        } else {
            return nullptr;
        }
    }

    /**
     * Retrieve the node storing the value. When the ultimately found node
     * is not a storage node, we will return null. We can check if the returned
     * node pointer is null to determine whether the string is valid.
     * @param str string as the key
     * @return pointer of node storing the value
     */
    node* get(const sstring& str) {
        return get(str.data(), str.length());
    }
};

}