#pragma once

#include <string>

namespace qod_qdrant {

//! The stable codes of contract section 3. Every connector error's raw message starts with the code and ": ".
enum class SearchCode {
	PolicyUnsupported,
	FieldDenied,
	QueryUnsupported,
	SchemaMismatch,
	BudgetExceeded,
	SourceUnavailable,
	DeadlineExceeded,
	PolicyChanged,
	ConfigInvalid
};

const char *CodeName(SearchCode code);

//! Throws the DuckDB exception for `code` with message "<code>: <tenant_text>". `tenant_text` is fixed
//! wording: never policy SQL, literals, backend text, credentials, the endpoint or a collection name.
[[noreturn]] void Raise(SearchCode code, const std::string &tenant_text);

//! `text` with every occurrence of `secret` (when not empty) replaced by "[redacted]".
std::string Redact(std::string text, const std::string &secret);

//! The privileged log line: "[qod_qdrant] source=<id> code=<code> detail=<detail>", the secret redacted
//! and every control byte (U+0000..U+001F, DEL) replaced by a space, so it is always one line.
std::string DetailLine(const std::string &source_id, SearchCode code, const std::string &detail,
                       const std::string &secret);

//! Writes DetailLine to stderr, which the node forwards to the manager's privileged log.
void LogDetail(const std::string &source_id, SearchCode code, const std::string &detail, const std::string &secret);

//! LogDetail, then Raise with the tenant text.
[[noreturn]] void Fail(const std::string &source_id, SearchCode code, const std::string &tenant_text,
                       const std::string &detail, const std::string &secret);

} // namespace qod_qdrant
