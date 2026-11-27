#include "settings.hpp"

#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/setting_info.hpp"
#include "errors.hpp"
#include "node_hooks.hpp"

namespace qod_qdrant {
using namespace duckdb;

namespace {

struct SettingSpec {
	const char *name;
	const char *description;
	int64_t SearchLimits::*member;
	int64_t min;
	int64_t max;
};

const SettingSpec kSettings[] = {
    {"search_default_k", "k of qod_search when the call gives none", &SearchLimits::default_k, 1, 100000},
    {"search_max_k", "Largest k qod_search accepts", &SearchLimits::max_k, 1, 100000},
    {"search_max_request_bytes", "Largest remote request body, in bytes", &SearchLimits::max_request_bytes, 1024,
     INT64_C(67108864)},
    {"search_max_filter_nodes", "Most filter nodes after combining P and U", &SearchLimits::max_filter_nodes, 1,
     1000000},
    {"search_deadline_ms", "Deadline of one remote request, in milliseconds", &SearchLimits::deadline_ms, 1,
     INT64_C(3600000)},
    {"search_max_response_bytes", "Largest remote response body, in bytes", &SearchLimits::max_response_bytes, 1024,
     INT64_C(1073741824)},
    {"search_scan_budget_points", "Most points one plain scan may read", &SearchLimits::scan_budget_points, 1,
     INT64_C(1000000000000)},
    {"search_max_concurrent_requests", "Concurrent remote requests per node", &SearchLimits::max_concurrent_requests,
     1, 4096},
    {"search_max_query_text_bytes", "Largest keyword query text, in UTF-8 bytes after trimming",
     &SearchLimits::max_query_text_bytes, 1, 262144},
    {"search_max_prefetch_k", "Largest prefetch_k of a fused search", &SearchLimits::max_prefetch_k, 1, 100000},
    {"search_max_fusion_branches", "Most branches one fused search combines", &SearchLimits::max_fusion_branches, 2, 16},
    {"search_max_request_text_bytes", "Largest qod_search request text, in UTF-8 bytes",
     &SearchLimits::max_request_text_bytes, 2, INT64_C(16777216)},
    {"search_max_prefetch_depth", "Most nested prefetch levels of a raw request", &SearchLimits::max_prefetch_depth, 1, 8},
    {"search_max_hnsw_ef", "Largest params.hnsw_ef of a raw request", &SearchLimits::max_hnsw_ef, 1, 100000},
    {"search_max_request_ids", "Most point ids one raw request names", &SearchLimits::max_request_ids, 1, 4096},
    {"search_max_oversampling", "Largest params.quantization.oversampling of a raw request",
     &SearchLimits::max_oversampling, 1, 1000},
};

class TrustedOnly : public ExtensionOptionInfo {
public:
	explicit TrustedOnly(const SettingSpec &spec) : spec_(spec) {
	}
	void OnSet(ClientContext &context, SetScope, Value &parameter, bool is_reset) const override {
		if (IsTenantConnection(context)) {
			Raise(SearchCode::ConfigInvalid, "search settings are set only by the node's trusted init");
		}
		if (is_reset) {
			return;
		}
		if (parameter.IsNull()) {
			Raise(SearchCode::ConfigInvalid, std::string(spec_.name) + " must be an integer");
		}
		int64_t value = 0;
		try {
			value = parameter.GetValue<int64_t>();
		} catch (std::exception &) {
			Raise(SearchCode::ConfigInvalid, std::string(spec_.name) + " must be an integer");
		}
		if (value < spec_.min || value > spec_.max) {
			Raise(SearchCode::ConfigInvalid, std::string(spec_.name) + " must be in " + std::to_string(spec_.min) + ".." +
			                                     std::to_string(spec_.max));
		}
		parameter = Value::BIGINT(value);
	}

private:
	SettingSpec spec_;
};

} // namespace

bool IsTenantConnection(ClientContext &context) {
	return context.registered_state->Get<ClientContextState>(qod::kQuackSessionKey) != nullptr;
}

void RegisterSettings(DBConfig &config) {
	SearchLimits defaults;
	for (auto &spec : kSettings) {
		ExtensionOption option(spec.description, LogicalType::BIGINT, nullptr, Value::BIGINT(defaults.*(spec.member)),
		                       SetScope::GLOBAL, false, false);
		option.supports_session = false;
		option.info = make_shared_ptr<TrustedOnly>(spec);
		config.AddExtensionOption(Identifier(spec.name), std::move(option));
	}
}

SearchLimits ReadLimits(ClientContext &context) {
	SearchLimits limits;
	for (auto &spec : kSettings) {
		Value value;
		if (context.TryGetCurrentSetting(Identifier(spec.name), value) && !value.IsNull()) {
			limits.*(spec.member) = value.GetValue<int64_t>();
		}
	}
	return limits;
}

} // namespace qod_qdrant
