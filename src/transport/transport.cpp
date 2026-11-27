#include "transport/transport.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace qod_qdrant {

const char *FailureName(Failure failure) {
	switch (failure) {
	case Failure::None:
		return "none";
	case Failure::Connect:
		return "connect";
	case Failure::Timeout:
		return "timeout";
	case Failure::Cancelled:
		return "cancelled";
	case Failure::TooLarge:
		return "too_large";
	case Failure::Redirect:
		return "redirect";
	case Failure::Tls:
		return "tls";
	case Failure::Other:
		return "other";
	}
	return "other";
}

#ifdef QOD_QDRANT_TESTING
namespace {
std::mutex &FactoryMutex() {
	static std::mutex mutex;
	return mutex;
}
TransportFactory &Factory() {
	static TransportFactory factory;
	return factory;
}
} // namespace

void SetTransportFactoryForTesting(TransportFactory factory) {
	std::lock_guard<std::mutex> lock(FactoryMutex());
	Factory() = std::move(factory);
}

void AppendRequestLog(const std::string &line) {
	auto path = std::getenv("QOD_QDRANT_TEST_REQUEST_LOG");
	if (!path || !*path) {
		return;
	}
#ifdef _WIN32
	// The request log is a test hook of the POSIX test builds only.
	(void)line;
	return;
#else
	// Opened per line in append mode and written with one write(2), so lines from concurrent requests and
	// from several nodes sharing the path never interleave.
	auto fd = ::open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) {
		return;
	}
	auto text = line + "\n";
	ssize_t n;
	do {
		n = ::write(fd, text.data(), text.size());
	} while (n < 0 && errno == EINTR);
	::close(fd);
#endif
}
#endif

std::unique_ptr<Transport> MakeTransport(const std::string &endpoint, const std::string &api_key) {
#ifdef QOD_QDRANT_TESTING
	{
		std::lock_guard<std::mutex> lock(FactoryMutex());
		if (Factory()) {
			return Factory()(endpoint, api_key);
		}
	}
#endif
	auto ca = std::getenv("QOD_QDRANT_CA_FILE");
	return MakeHttpsTransport(endpoint, api_key, ca ? ca : "");
}

} // namespace qod_qdrant
