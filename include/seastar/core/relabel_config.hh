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
 * Copyright 2022 ScyllaDB
 */
#include <regex>

namespace seastar {
namespace metrics {

/*!
 * \brief a wrapper class around regex with the original expr
 *
 * regex does not contain the original expression, this wrapper class
 * acts both as a string and as a regex.
 */
class relabel_config_regex {
    std::string _regex_str;
    std::regex _regex;
public:
    relabel_config_regex() = default;
    relabel_config_regex(const std::string& expr) : _regex_str(expr), _regex(std::regex(expr)) {}
    relabel_config_regex(const char* expr) : _regex_str(expr), _regex(std::regex(expr)) {}
    const std::string& str() const noexcept {
        return _regex_str;
    }
    const std::regex& regex() const noexcept {
        return _regex;
    }

    relabel_config_regex& operator=(const char* expr) {
        std::string str(expr);
        return operator=(str);
    }

    relabel_config_regex& operator=(const std::string& expr) {
        _regex_str = expr;
        _regex = std::regex(_regex_str);
        return *this;
    }
};

/*!
 * \brief a relabel_config allows changing metrics labels dynamically
 *
 * The logic is similar to Prometheus configuration
 * This is how Prometheus entry looks like:
 *  - source_labels: [version]
      regex: '([0-9]+\.[0-9]+)(\.?[0-9]*).*'
      replacement: '$1$2'
      target_label: svr
 * relabel_action values:
 *   skip_when_empty - when set supported metrics (histogram, summary and counters)
 *                     will not be reported if they were never used.
 *   report_when_empty - revert the skip_when_empty flag
 *   replace - replace the value of the target_label
 *   keep - enable the metrics
 *   drop - disable the metrics
 *   drop_label  - remove the target label
 *
 * source_labels - a list of source labels, the labels are concatenated
 *                 with the separator and and the combine value is match to the regex.
 * target_label  - the labels to perform the action on when replacing a value or when dropping a label.
 * replacement   - the string to use when replacing a label value, regex group can be used.
 * expr          - a regular expression in a string format. Action would be taken if the regex
 *                 match the concatenated labels.
 * action        - The action to perform when there is a match.
 * separator     - separator to use when concatenating the labels.
 *
 */
struct relabel_config {
    enum class relabel_action {skip_when_empty, report_when_empty, replace, keep, drop, drop_label};
    std::vector<std::string> source_labels;
    std::string target_label;
    std::string replacement = "${1}";
    relabel_config_regex expr = "(.*)";
    relabel_action action = relabel_action::replace;
    std::string separator = ";";
};

/*!
 * \brief a helper function to translate a string to relabel_config::relabel_action enum values
 */
relabel_config::relabel_action relabel_config_action(const std::string& action);
}
}
