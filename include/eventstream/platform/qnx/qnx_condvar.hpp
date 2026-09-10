#pragma once

#include <eventstream/platform/platform_contract.hpp>
#include <eventstream/platform/qnx/qnx_mutex.hpp>

#include <chrono>
#include <pthread.h>

namespace eventstream::platform {

class QnxCondvarBackend {
public:
	using NativeHandle = pthread_cond_t*;

	QnxCondvarBackend();
	~QnxCondvarBackend() noexcept;

	QnxCondvarBackend(const QnxCondvarBackend&) = delete;
	QnxCondvarBackend& operator=(const QnxCondvarBackend&) = delete;
	QnxCondvarBackend(QnxCondvarBackend&&) = delete;
	QnxCondvarBackend& operator=(QnxCondvarBackend&&) = delete;

	void wait(QnxMutexBackend& mutex);
	bool wait_for(
		QnxMutexBackend& mutex,
		std::chrono::nanoseconds timeout);

	void notify_one() noexcept;
	void notify_all() noexcept;

	NativeHandle native_handle() noexcept;

private:
	pthread_cond_t cond_{};
	bool initialized_{false};
};

} // namespace eventstream::platform