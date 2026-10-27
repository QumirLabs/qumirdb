#include "config.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace NQdb::NUtils {

namespace {

std::string Join(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) {
        if (!out.empty()) {
            out += ", ";
        }
        out += item;
    }
    return out;
}

std::string_view Trim(std::string_view text) {
    constexpr std::string_view blanks = " \t\r\n";
    const size_t begin = text.find_first_not_of(blanks);
    if (begin == std::string_view::npos) {
        return {};
    }
    const size_t end = text.find_last_not_of(blanks);
    return text.substr(begin, end - begin + 1);
}

std::string MapKey(std::string_view section, std::string_view key) {
    return std::format("{}:{}", section, key);
}

std::vector<std::string> SectionNames(const TConfigSchema& schema) {
    std::vector<std::string> names;
    for (const auto& section : schema.Sections()) {
        names.push_back(section.Name);
    }
    return names;
}

std::vector<std::string> KeyNames(const TConfigSection& section) {
    std::vector<std::string> names;
    for (const auto& key : section.Keys) {
        names.push_back(key.Name);
    }
    return names;
}

std::string Describe(const TConfigKey& key) {
    switch (key.Type) {
    case EConfigType::Bool:
        return "true or false";
    case EConfigType::Int:
        if (key.Max == std::numeric_limits<int64_t>::max()) {
            return std::format("an integer >= {}", key.Min);
        }
        return std::format("an integer in {}..{}", key.Min, key.Max);
    case EConfigType::Double:
        if (key.MinValue != std::numeric_limits<double>::lowest() &&
            key.MaxValue != std::numeric_limits<double>::max())
        {
            return std::format("a number in {}..{}", key.MinValue, key.MaxValue);
        }
        if (key.MinValue != std::numeric_limits<double>::lowest()) {
            return std::format("a number >= {}", key.MinValue);
        }
        if (key.MaxValue != std::numeric_limits<double>::max()) {
            return std::format("a number <= {}", key.MaxValue);
        }
        return "a number";
    case EConfigType::String:
        return "a string";
    case EConfigType::Enum:
        return std::format("one of: {}", Join(key.Values));
    }
    return {};
}

using TParsed = std::variant<bool, int64_t, double, std::string>;

// Checks a value against the key's type and bounds. The error says what was
// expected, and the caller adds where the value came from.
std::expected<TParsed, std::string> ParseValue(const TConfigKey& key, std::string_view text) {
    auto bad = [&]() {
        return std::unexpected(std::format(
            "invalid value '{}' for '{}': expected {}", text, key.Name, Describe(key)));
    };
    switch (key.Type) {
    case EConfigType::Bool:
        if (text == "true") {
            return TParsed(true);
        }
        if (text == "false") {
            return TParsed(false);
        }
        return bad();
    case EConfigType::Int: {
        int64_t value = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || ec != std::errc() || end != text.data() + text.size()) {
            return bad();
        }
        if (value < key.Min || value > key.Max) {
            return bad();
        }
        return TParsed(value);
    }
    case EConfigType::Double: {
        const std::string copy(text);
        char* end = nullptr;
        errno = 0;
        const double value = std::strtod(copy.c_str(), &end);
        if (copy.empty() || end != copy.c_str() + copy.size() || errno != 0 || !std::isfinite(value)) {
            return bad();
        }
        if (value < key.MinValue || value > key.MaxValue) {
            return bad();
        }
        return TParsed(value);
    }
    case EConfigType::String:
        return TParsed(std::string(text));
    case EConfigType::Enum:
        for (const auto& accepted : key.Values) {
            if (text == accepted) {
                return TParsed(std::string(text));
            }
        }
        return bad();
    }
    return bad();
}

std::string Format(const TParsed& value) {
    return std::visit([](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return v;
        } else {
            return std::format("{}", v);
        }
    }, value);
}

std::string TypeName(EConfigType type) {
    switch (type) {
    case EConfigType::Bool:
        return "bool";
    case EConfigType::Int:
        return "int";
    case EConfigType::Double:
        return "double";
    case EConfigType::String:
        return "string";
    case EConfigType::Enum:
        return "enum";
    }
    return {};
}

} // namespace

TConfigSchema& TConfigSchema::Section(std::string name) {
    for (size_t i = 0; i < Sections_.size(); ++i) {
        if (Sections_[i].Name == name) {
            Current_ = i;
            return *this;
        }
    }
    Sections_.push_back({ .Name = std::move(name) });
    Current_ = Sections_.size() - 1;
    return *this;
}

TConfigSchema& TConfigSchema::Add(TConfigKey key) {
    if (Sections_.empty()) {
        throw std::logic_error("config key '" + key.Name + "' is declared before any section");
    }
    auto& section = Sections_[Current_];
    for (const auto& existing : section.Keys) {
        if (existing.Name == key.Name) {
            throw std::logic_error("config key '" + section.Name + ":" + key.Name + "' is declared twice");
        }
    }
    if (auto parsed = ParseValue(key, key.Default); !parsed) {
        throw std::logic_error("default of '" + section.Name + ":" + key.Name + "': " + parsed.error());
    }
    section.Keys.push_back(std::move(key));
    return *this;
}

TConfigSchema& TConfigSchema::Bool(std::string name, bool def, std::string description) {
    return Add({
        .Name = std::move(name),
        .Type = EConfigType::Bool,
        .Default = def ? "true" : "false",
        .Description = std::move(description),
    });
}

TConfigSchema& TConfigSchema::Int(
    std::string name,
    int64_t def,
    std::string description,
    int64_t min,
    int64_t max)
{
    return Add({
        .Name = std::move(name),
        .Type = EConfigType::Int,
        .Default = std::to_string(def),
        .Description = std::move(description),
        .Min = min,
        .Max = max,
    });
}

TConfigSchema& TConfigSchema::Double(
    std::string name,
    double def,
    std::string description,
    double min,
    double max)
{
    return Add({
        .Name = std::move(name),
        .Type = EConfigType::Double,
        .Default = std::format("{}", def),
        .Description = std::move(description),
        .MinValue = min,
        .MaxValue = max,
    });
}

TConfigSchema& TConfigSchema::String(std::string name, std::string def, std::string description) {
    return Add({
        .Name = std::move(name),
        .Type = EConfigType::String,
        .Default = std::move(def),
        .Description = std::move(description),
    });
}

TConfigSchema& TConfigSchema::Enum(
    std::string name,
    std::vector<std::string> values,
    std::string def,
    std::string description)
{
    return Add({
        .Name = std::move(name),
        .Type = EConfigType::Enum,
        .Default = std::move(def),
        .Description = std::move(description),
        .Values = std::move(values),
    });
}

const TConfigSection* TConfigSchema::FindSection(std::string_view section) const {
    for (const auto& s : Sections_) {
        if (s.Name == section) {
            return &s;
        }
    }
    return nullptr;
}

const TConfigKey* TConfigSchema::FindKey(std::string_view section, std::string_view key) const {
    const auto* s = FindSection(section);
    if (!s) {
        return nullptr;
    }
    for (const auto& k : s->Keys) {
        if (k.Name == key) {
            return &k;
        }
    }
    return nullptr;
}

std::string TConfigSchema::Help() const {
    std::string out;
    for (const auto& section : Sections_) {
        out += std::format("[{}]\n", section.Name);
        for (const auto& key : section.Keys) {
            std::string type;
            switch (key.Type) {
            case EConfigType::Bool:
                type = "true|false";
                break;
            case EConfigType::Int:
                type = (key.Max == std::numeric_limits<int64_t>::max())
                    ? std::format("int, min {}", key.Min)
                    : std::format("int, {}..{}", key.Min, key.Max);
                break;
            case EConfigType::Double:
                type = "number";
                if (key.MinValue != std::numeric_limits<double>::lowest()) {
                    type += std::format(", min {}", key.MinValue);
                }
                if (key.MaxValue != std::numeric_limits<double>::max()) {
                    type += std::format(", max {}", key.MaxValue);
                }
                break;
            case EConfigType::String:
                type = "string";
                break;
            case EConfigType::Enum:
                for (const auto& value : key.Values) {
                    type += (type.empty() ? "" : "|") + value;
                }
                break;
            }
            out += std::format(
                "  --{}:{}=<{}>\n      {} (default: {})\n",
                section.Name,
                key.Name,
                type,
                key.Description,
                key.Default.empty() ? "empty" : key.Default);
        }
    }
    return out;
}

TConfig::TConfig(const TConfigSchema& schema)
    : Schema_(&schema)
{
    for (const auto& section : schema.Sections()) {
        for (const auto& key : section.Keys) {
            Values_[MapKey(section.Name, key.Name)] = *ParseValue(key, key.Default);
        }
    }
}

TConfig::TResult TConfig::LoadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format("cannot open config file '{}'", path));
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) {
        return std::unexpected(std::format("cannot read config file '{}'", path));
    }
    return LoadString(text.str(), path);
}

TConfig::TResult TConfig::LoadString(std::string_view text, std::string_view origin) {
    std::map<std::string, TValue> parsed;
    const TConfigSection* section = nullptr;
    size_t lineNo = 0;

    while (!text.empty()) {
        const size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = (eol == std::string_view::npos) ? std::string_view() : text.substr(eol + 1);
        ++lineNo;

        auto fail = [&](const std::string& message) {
            return std::unexpected(std::format("{}:{}: {}", origin, lineNo, message));
        };

        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        if (line[0] == '[') {
            if (line.back() != ']') {
                return fail(std::format("malformed section header '{}': expected [name]", line));
            }
            const std::string_view name = Trim(line.substr(1, line.size() - 2));
            section = Schema_->FindSection(name);
            if (!section) {
                return fail(std::format(
                    "unknown section '{}'; accepted sections: {}",
                    name,
                    Join(SectionNames(*Schema_))));
            }
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            return fail(std::format("malformed line '{}': expected 'key = value'", line));
        }
        const std::string_view name = Trim(line.substr(0, eq));
        std::string_view value = Trim(line.substr(eq + 1));
        if (name.empty()) {
            return fail(std::format("malformed line '{}': the key is empty", line));
        }
        if (!section) {
            return fail(std::format("key '{}' is outside any section", name));
        }

        // A comment may follow the value after a blank; '#' inside a value stays.
        for (size_t i = 1; i < value.size(); ++i) {
            if ((value[i] == '#' || value[i] == ';') && (value[i - 1] == ' ' || value[i - 1] == '\t')) {
                value = Trim(value.substr(0, i));
                break;
            }
        }

        const TConfigKey* key = nullptr;
        for (const auto& candidate : section->Keys) {
            if (candidate.Name == name) {
                key = &candidate;
                break;
            }
        }
        if (!key) {
            return fail(std::format(
                "unknown key '{}' in [{}]; accepted keys: {}",
                name,
                section->Name,
                Join(KeyNames(*section))));
        }

        auto mapKey = MapKey(section->Name, key->Name);
        if (parsed.contains(mapKey)) {
            return fail(std::format("duplicate key '{}' in [{}]", name, section->Name));
        }
        auto result = ParseValue(*key, value);
        if (!result) {
            return fail(std::format("[{}] {}", section->Name, result.error()));
        }
        parsed.emplace(std::move(mapKey), std::move(*result));
    }

    for (auto& [mapKey, value] : parsed) {
        Set_.insert(mapKey);
        Values_[mapKey] = std::move(value);
    }
    return {};
}

TConfig::TResult TConfig::Override(std::string_view spec) {
    auto fail = [&](const std::string& message) {
        return std::unexpected(std::format("override '{}': {}", spec, message));
    };
    const size_t colon = spec.find(':');
    const size_t eq = spec.find('=');
    if (colon == std::string_view::npos || eq == std::string_view::npos || colon > eq) {
        return fail("expected 'section:key=value'");
    }
    const std::string_view sectionName = spec.substr(0, colon);
    const std::string_view keyName = spec.substr(colon + 1, eq - colon - 1);
    const std::string_view value = spec.substr(eq + 1);

    const auto* section = Schema_->FindSection(sectionName);
    if (!section) {
        return fail(std::format(
            "unknown section '{}'; accepted sections: {}",
            sectionName,
            Join(SectionNames(*Schema_))));
    }
    const auto* key = Schema_->FindKey(sectionName, keyName);
    if (!key) {
        return fail(std::format(
            "unknown key '{}' in [{}]; accepted keys: {}",
            keyName,
            sectionName,
            Join(KeyNames(*section))));
    }
    auto result = ParseValue(*key, value);
    if (!result) {
        return fail(result.error());
    }
    Values_[MapKey(sectionName, keyName)] = std::move(*result);
    Set_.insert(MapKey(sectionName, keyName));
    return {};
}

const TConfig::TValue& TConfig::Get(std::string_view section, std::string_view key, EConfigType type) const {
    const auto* schemaKey = Schema_->FindKey(section, key);
    if (!schemaKey) {
        throw std::logic_error(std::format("unknown config key '{}:{}'", section, key));
    }
    const bool sameType = (schemaKey->Type == type) ||
        (type == EConfigType::String && schemaKey->Type == EConfigType::Enum);
    if (!sameType) {
        throw std::logic_error(std::format(
            "config key '{}:{}' is {}, not {}", section, key, TypeName(schemaKey->Type), TypeName(type)));
    }
    return Values_.at(MapKey(section, key));
}

bool TConfig::GetBool(std::string_view section, std::string_view key) const {
    return std::get<bool>(Get(section, key, EConfigType::Bool));
}

int64_t TConfig::GetInt(std::string_view section, std::string_view key) const {
    return std::get<int64_t>(Get(section, key, EConfigType::Int));
}

double TConfig::GetDouble(std::string_view section, std::string_view key) const {
    return std::get<double>(Get(section, key, EConfigType::Double));
}

const std::string& TConfig::GetString(std::string_view section, std::string_view key) const {
    return std::get<std::string>(Get(section, key, EConfigType::String));
}

bool TConfig::IsSet(std::string_view section, std::string_view key) const {
    return Set_.contains(MapKey(section, key));
}

std::string TConfig::Dump() const {
    std::string out;
    for (const auto& section : Schema_->Sections()) {
        if (!out.empty()) {
            out += "\n";
        }
        out += std::format("[{}]\n", section.Name);
        for (const auto& key : section.Keys) {
            out += std::format("{} = {}\n", key.Name, Format(Values_.at(MapKey(section.Name, key.Name))));
        }
    }
    return out;
}

} // namespace NQdb::NUtils
