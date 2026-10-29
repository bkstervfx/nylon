#ifndef NYLON_PROGRESS_PROGRESS_HPP
#define NYLON_PROGRESS_PROGRESS_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace nylon::progress
{

struct registry_snapshot_t
{
	std::uint64_t keys = 0;
	std::uint64_t values = 0;
	std::uint64_t hives = 0;
	std::string detail;
};

class registry_state_t
{
public:
	void reset();
	void hive(std::string_view name);
	void batch(
		std::uint64_t keys,
		std::uint64_t values,
		std::string_view detail = {});
	[[nodiscard]] registry_snapshot_t snapshot() const;

private:
	void set_detail(std::string_view detail);

	std::atomic<std::uint64_t> keys_{ 0 };
	std::atomic<std::uint64_t> values_{ 0 };
	std::atomic<std::uint64_t> hives_{ 0 };
	mutable std::mutex detail_mutex_;
	std::string detail_;
};

class registry_view_t
{
public:
	explicit registry_view_t(registry_state_t& state) noexcept;
	~registry_view_t();

	registry_view_t(const registry_view_t&) = delete;
	registry_view_t& operator=(const registry_view_t&) = delete;

	void start();
	void finish(bool success);

private:
	void render(bool final, bool success, std::uint64_t frame);
	void stop_thread() noexcept;

	registry_state_t& state_;
	std::jthread thread_;
	bool interactive_ = false;
	bool rendered_ = false;
};

}

#endif
