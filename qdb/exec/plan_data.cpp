#include <qdb/exec/plan_data.h>

#include <qdb/plan/types/type_text.h>

namespace NQdb {

std::string TypeToText(const NQumir::NAst::TTypePtr& type)
{
    return PrintTypeText(type);
}

NQumir::NAst::TTypePtr TypeFromText(std::string_view text)
{
    return ParseTypeText(text);
}

} // namespace NQdb
