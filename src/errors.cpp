#include "errors.hpp"

#include "duckdb/common/exception.hpp"

#include <cerrno>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace qod_qdrant {

const char *CodeName(SearchCode code) {
	switch (code) {
	case SearchCode::PolicyUnsupported:
		return "search_policy_unsupported";
	case SearchCode::FieldDenied:
		return "search_field_denied";
	case SearchCode::QueryUnsupported:
		return "search_query_unsupported";
	case SearchCode::SchemaMismatch:
		return "search_schema_mismatch";
	case SearchCode::BudgetExceeded:
		return "search_budget_exceeded";
	case SearchCode::SourceUnavailable:
		return "search_source_unavailable";
	case SearchCode::DeadlineExceeded:
		return "search_deadline_exceeded";
	case SearchCode::PolicyChanged:
		return "search_policy_changed";
	case SearchCode::ConfigInvalid:
		return "search_config_invalid";
	}
	return "search_query_unsupported";
}

void Raise(SearchCode code, const std::string &tenant_text) {
	auto message = std::string(CodeName(code)) + ": " + tenant_text;
	switch (code) {
	case SearchCode::PolicyUnsupported:
	case SearchCode::FieldDenied:
	case SearchCode::PolicyChanged:
		throw duckdb::PermissionException(message);
	case SearchCode::SourceUnavailable:
	case SearchCode::DeadlineExceeded:
		throw duckdb::IOException(message);
	default:
		throw duckdb::InvalidInputException(message);
	}
}

std::string Redact(std::string text, const std::string &secret) {
	if (secret.empty()) {
		return text;
	}
	size_t at = 0;
	while ((at = text.find(secret, at)) != std::string::npos) {
		text.replace(at, secret.size(), "[redacted]");
		at += 10;
	}
	return text;
}

std::string DetailLine(const std::string &source_id, SearchCode code, const std::string &detail,
                       const std::string &secret) {
	auto one_line = Redact(detail, secret);
	for (auto &c : one_line) {
		// Every control byte (U+0000..U+001F, DEL), not only newlines: config strings and backend text may
		// hold them, and the log is line oriented.
		if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
			c = ' ';
		}
	}
	return "[qod_qdrant] source=" + source_id + " code=" + CodeName(code) + " detail=" + one_line;
}

void LogDetail(const std::string &source_id, SearchCode code, const std::string &detail, const std::string &secret) {
	// One write(2) of the whole line: lines from concurrent requests never interleave.
	auto line = DetailLine(source_id, code, detail, secret) + "\n";
	size_t written = 0;
	while (written < line.size()) {
#ifdef _WIN32
		auto n = ::_write(2, line.data() + written, static_cast<unsigned int>(line.size() - written));
#else
		auto n = ::write(STDERR_FILENO, line.data() + written, line.size() - written);
#endif
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return;
		}
		written += size_t(n);
	}
}

void Fail(const std::string &source_id, SearchCode code, const std::string &tenant_text, const std::string &detail,
          const std::string &secret) {
	LogDetail(source_id, code, detail, secret);
	Raise(code, tenant_text);
}

} // namespace qod_qdrant
