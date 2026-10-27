/* Copyright (c) 2010, 2022 Alexey Ozeritsky
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Based on Config and ConfigSkeleton from https://github.com/resetius/fdm,
// src/config.{h,cpp}, commit 7917bfeef459b5bc67e6569ed5b2f0e8925f2100, by
// Alexey Ozeritsky. The original is BSD-3-Clause; its notice is kept above.

#pragma once

#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace NQdb::NUtils {

enum class EConfigType {
    Bool,
    Int,
    Double,
    String,
    Enum,
};

struct TConfigKey {
    std::string Name;
    EConfigType Type = EConfigType::String;
    // The default in its textual form; it must satisfy the key's own rules.
    std::string Default;
    std::string Description;
    // Inclusive bounds of an Int key.
    int64_t Min = 0;
    int64_t Max = std::numeric_limits<int64_t>::max();
    // Inclusive bounds of a Double key.
    double MinValue = std::numeric_limits<double>::lowest();
    double MaxValue = std::numeric_limits<double>::max();
    // The accepted spellings of an Enum key.
    std::vector<std::string> Values;
};

struct TConfigSection {
    std::string Name;
    std::vector<TConfigKey> Keys;
};

// The table of known sections and keys. The reader, the validation and the
// generated help all work from it, so a setting is described in one place.
// Declaration order is kept for help and for the effective-settings dump.
class TConfigSchema {
public:
    // Starts a section, or continues one that exists; the keys added next go there.
    TConfigSchema& Section(std::string name);

    TConfigSchema& Bool(std::string name, bool def, std::string description);
    TConfigSchema& Int(
        std::string name,
        int64_t def,
        std::string description,
        int64_t min = 0,
        int64_t max = std::numeric_limits<int64_t>::max());
    TConfigSchema& Double(
        std::string name,
        double def,
        std::string description,
        double min = std::numeric_limits<double>::lowest(),
        double max = std::numeric_limits<double>::max());
    TConfigSchema& String(std::string name, std::string def, std::string description);
    TConfigSchema& Enum(
        std::string name,
        std::vector<std::string> values,
        std::string def,
        std::string description);

    const std::vector<TConfigSection>& Sections() const {
        return Sections_;
    }

    const TConfigSection* FindSection(std::string_view section) const;
    const TConfigKey* FindKey(std::string_view section, std::string_view key) const;

    // One entry per key: the override spelling, the accepted values, the default
    // and the description.
    std::string Help() const;

private:
    TConfigSchema& Add(TConfigKey key);

    std::vector<TConfigSection> Sections_;
    // Index of the section that receives the next key.
    size_t Current_ = 0;
};

// Settings checked against a schema. Each source is applied in the order of the
// calls, so a later call overrides an earlier one: defaults, then the files,
// then the command-line overrides. A failed call changes nothing.
class TConfig {
public:
    using TResult = std::expected<void, std::string>;

    // The schema must outlive the config. A default that breaks its own rules is
    // a programming error and throws std::logic_error.
    explicit TConfig(const TConfigSchema& schema);

    TResult LoadFile(const std::string& path);
    // The origin names the text in errors, as a file name does.
    TResult LoadString(std::string_view text, std::string_view origin = "<string>");
    // A command-line override: `section:key=value`.
    TResult Override(std::string_view spec);

    bool GetBool(std::string_view section, std::string_view key) const;
    int64_t GetInt(std::string_view section, std::string_view key) const;
    double GetDouble(std::string_view section, std::string_view key) const;
    // Also the accessor of Enum keys.
    const std::string& GetString(std::string_view section, std::string_view key) const;

    // Whether a file or an override set the key, rather than its default.
    bool IsSet(std::string_view section, std::string_view key) const;

    // Every key with its effective value, as ini text that loads back.
    std::string Dump() const;

private:
    using TValue = std::variant<bool, int64_t, double, std::string>;

    const TValue& Get(std::string_view section, std::string_view key, EConfigType type) const;

    const TConfigSchema* Schema_;
    // Keyed by `section:key`.
    std::map<std::string, TValue> Values_;
    // The keys a file or an override has set, for callers that apply only those.
    std::set<std::string> Set_;
};

} // namespace NQdb::NUtils
