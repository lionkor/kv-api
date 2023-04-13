#include "Accept.h"

#include <algorithm>
#include <boost/fusion/sequence/intrinsic_fwd.hpp>
#include <boost/phoenix.hpp>
#include <boost/spirit/home/qi/directive/lexeme.hpp>
#include <boost/spirit/home/qi/parse.hpp>
#include <boost/spirit/home/support/char_class.hpp>
#include <boost/spirit/home/support/common_terminals.hpp>
#include <boost/spirit/include/qi.hpp>
#include <compare>
#include <doctest/doctest.h>
#include <fmt/core.h>
#include <optional>
#include <spdlog/spdlog.h>

AcceptValues::AcceptValues(const std::string& raw) {
    using namespace boost::spirit;
    using namespace boost;
    using qi::_1;

    using Accept = boost::fusion::vector<std::string, std::string, std::optional<float>>;

    qi::rule<std::string::const_iterator, Accept> single;

    single %= (+(qi::char_("a-zA-Z\\-\\+")) | qi::char_('*')) >> '/'
        >> (+(qi::char_("a-zA-Z\\-\\+")) | qi::char_("*"))
        >> -(";q=" >> qi::float_);

    qi::rule<std::string::const_iterator, std::vector<Accept>> multiple;

    multiple %= single % ',';

    std::vector<Accept> res;

    qi::phrase_parse(raw.begin(), raw.end(),
        multiple[phoenix::ref(res) = _1],
        ascii::space);

    for (const auto& entry : res) {
        AcceptMime mime;
        mime.type = boost::fusion::at_c<0>(entry);
        mime.subtype = boost::fusion::at_c<1>(entry);
        mime.q_factor = boost::fusion::at_c<2>(entry).value_or(1.0f);
        m_values.push_back(mime);
    }
    std::sort(m_values.begin(), m_values.end());
}

TEST_CASE("AcceptValues") {
    AcceptValues a("text/html,text/*,application/json;q=0.3,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");

    SUBCASE("simple") {
        auto result = a.highest_in({ { "text", "html" } });
        CHECK_EQ(result.type, "text");
        CHECK_EQ(result.subtype, "html");
    }

    SUBCASE("multiple options") {
        auto result = a.highest_in({ { "text", "html" }, { "application", "xml" } });
        CHECK_EQ(result.type, "text");
        CHECK_EQ(result.subtype, "html");
    }

    SUBCASE("specificity") {
        auto result = a.highest_in({ { "text", "html" }, { "text", "*" } });
        CHECK_EQ(result.type, "text");
        CHECK_EQ(result.subtype, "html");
    }

    SUBCASE("specificity 2") {
        auto result = a.highest_in({ { "*", "*" }, { "text", "*" } });
        CHECK_EQ(result.type, "text");
        CHECK_EQ(result.subtype, "*");
    }

    SUBCASE("q factors") {
        auto result = a.highest_in({ { "application", "xml" }, { "application", "json" } });
        CHECK_EQ(result.type, "application");
        CHECK_EQ(result.subtype, "xml");
    }
}

std::strong_ordering operator<=>(const AcceptMime& a, const AcceptMime& b) {
    if (a.q_factor > b.q_factor) {
        return std::strong_ordering::less;
    } else {
        if (a.type == "*" && b.type != "*") {
            return std::strong_ordering::greater;
        } else if (b.type == "*" && a.type != "*") {
            return std::strong_ordering::less;
        } else if (a.subtype == "*" && b.subtype != "*") {
            return std::strong_ordering::greater;
        } else if (b.subtype == "*" && a.subtype != "*") {
            return std::strong_ordering::less;
        } else {
            return std::strong_ordering::equal;
        }
    }
}

Mime AcceptValues::highest_in(const std::vector<Mime>& options) {
    std::vector<AcceptMime> matches;
    for (const auto& val : m_values) {
        auto iter = std::find_if(options.begin(), options.end(),
            [&val](const auto& opt) -> bool {
                return val.type == opt.type && val.subtype == opt.subtype;
            });
        if (iter != options.end()) {
            matches.push_back(val);
        }
    }

    if (matches.empty()) {
        return Mime { .type = "*", .subtype = "*" };
    } else {
        std::sort(matches.begin(), matches.end());
        auto match = *matches.begin();
        return Mime { .type = match.type, .subtype = match.subtype };
    }
}
