#pragma once

#include <compare>
#include <string>
#include <vector>

struct AcceptMime {
    std::string type;
    std::string subtype;
    float q_factor;
};

struct Mime {
    std::string type;
    std::string subtype;
};

std::strong_ordering operator<=>(const AcceptMime& a, const AcceptMime& b);

// Parses an Accept header
class AcceptValues {
public:
    AcceptValues(const std::string& raw);

    Mime highest_in(const std::vector<Mime>&);

private:
    std::vector<AcceptMime> m_values;
};
