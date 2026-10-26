#pragma once
#include "content_search.h"
#include "wsl_protocol.h"

namespace poppy::wsl {
Message contentSearchMessage(const ContentSearchOptions& options);
ContentSearchOptions contentSearchOptions(const Message& message, std::string_view exclusions);
Message contentMatchMessage(const ContentMatch& match);
ContentMatch contentMatchFromMessage(const Message& message);
Message contentSummaryMessage(const ContentSearchSummary& summary);
ContentSearchSummary contentSummaryFromMessage(const Message& message);
bool validSearchRelative(std::string_view relative);
}
