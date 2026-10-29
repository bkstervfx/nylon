#include "progress.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <utility>

namespace nylon::progress
{

namespace
{

constexpr std::uint64_t detail_sample_mask = 127U;
constexpr auto refresh_interval = std::chrono::milliseconds(125);

std::string count_text(std::uint64_t value)
{
	std::string text = std::to_string(value);
	for (std::ptrdiff_t position = static_cast<std::ptrdiff_t>(text.size()) - 3;
		position > 0;
		position -= 3)
	{
		text.insert(static_cast<std::size_t>(position), 1U, ',');
	}
	return text;
}

std::size_t console_width()
{
	CONSOLE_SCREEN_BUFFER_INFO info{};
	const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
	if (output == INVALID_HANDLE_VALUE || output == nullptr || !GetConsoleScreenBufferInfo(output, &info))
	{
		return 80U;
	}

	const SHORT width = static_cast<SHORT>(info.srWindow.Right - info.srWindow.Left + 1);
	return width > 0 ? static_cast<std::size_t>(width) : 80U;
}

bool enable_interactive_output()
{
	const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
	if (output == INVALID_HANDLE_VALUE || output == nullptr) return false;

	DWORD mode = 0;
	if (!GetConsoleMode(output, &mode)) return false;
	if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) return true;
	return SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != FALSE;
}

std::string fit_detail(std::string detail, std::size_t width)
{
	if (detail.empty()) detail = "discovering registry hives";
	if (width <= 4U) return {};
	if (detail.size() <= width) return detail;
	return std::string("...") + detail.substr(detail.size() - (width - 3U));
}

std::string working_bar(std::size_t width, std::uint64_t frame)
{
	std::string bar(width, '-');
	if (width == 0U) return bar;

	const std::size_t segment = std::min<std::size_t>(5U, width);
	const std::size_t span = width - segment;
	std::size_t position = 0U;
	if (span != 0U)
	{
		const std::size_t cycle = span * 2U;
		position = static_cast<std::size_t>(frame % cycle);
		if (position > span) position = cycle - position;
	}
	std::fill_n(bar.begin() + static_cast<std::ptrdiff_t>(position), segment, '=');
	return bar;
}

}

void registry_state_t::reset()
{
	keys_.store(0U, std::memory_order_relaxed);
	values_.store(0U, std::memory_order_relaxed);
	hives_.store(0U, std::memory_order_relaxed);
	set_detail("discovering registry hives");
}

void registry_state_t::set_detail(std::string_view detail)
{
	std::lock_guard lock(detail_mutex_);
	detail_.assign(detail);
}

void registry_state_t::hive(std::string_view name)
{
	hives_.fetch_add(1U, std::memory_order_relaxed);
	set_detail(name);
}

void registry_state_t::key(std::string_view path)
{
	const std::uint64_t count = keys_.fetch_add(1U, std::memory_order_relaxed) + 1U;
	if (count <= 8U || (count & detail_sample_mask) == 0U)
	{
		set_detail(path);
	}
}

void registry_state_t::values(std::size_t count) noexcept
{
	values_.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
}

registry_snapshot_t registry_state_t::snapshot() const
{
	registry_snapshot_t result;
	result.keys = keys_.load(std::memory_order_relaxed);
	result.values = values_.load(std::memory_order_relaxed);
	result.hives = hives_.load(std::memory_order_relaxed);
	{
		std::lock_guard lock(detail_mutex_);
		result.detail = detail_;
	}
	return result;
}

registry_view_t::registry_view_t(registry_state_t& state) noexcept
	: state_(state)
{
}

registry_view_t::~registry_view_t()
{
	stop_thread();
}

void registry_view_t::start()
{
	state_.reset();
	interactive_ = enable_interactive_output();
	if (!interactive_)
	{
		std::cout << "registry...\n";
		return;
	}

	render(false, true, 0U);
	thread_ = std::jthread([this](std::stop_token stop)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		std::uint64_t frame = 1U;
		while (!stop.stop_requested())
		{
			std::this_thread::sleep_for(refresh_interval);
			if (stop.stop_requested()) break;
			render(false, true, frame++);
		}
	});
}

void registry_view_t::stop_thread() noexcept
{
	if (thread_.joinable())
	{
		thread_.request_stop();
		thread_.join();
	}
}

void registry_view_t::finish(bool success)
{
	stop_thread();
	if (interactive_)
	{
		render(true, success, 0U);
	}
}

void registry_view_t::render(bool final, bool success, std::uint64_t frame)
{
	const auto snapshot = state_.snapshot();
	const std::size_t columns = console_width();
	const std::size_t bar_width = std::clamp<std::size_t>(columns > 26U ? columns - 26U : 12U, 12U, 32U);
	const std::string bar = final
		? std::string(bar_width, success ? '=' : '!')
		: working_bar(bar_width, frame);

	std::ostringstream counters;
	counters
		<< count_text(snapshot.keys) << " keys | "
		<< count_text(snapshot.values) << " values | "
		<< count_text(snapshot.hives) << " hives";

	const std::size_t detail_width = columns > 12U ? columns - 12U : 20U;
	const std::string detail = fit_detail(snapshot.detail, detail_width);
	const char* state = final ? (success ? "done" : "failed") : "working";

	if (rendered_) std::cout << "\x1b[3A";
	std::cout
		<< "\r\x1b[2Kregistry  [" << bar << "] " << state << '\n'
		<< "\r\x1b[2K          " << counters.str() << '\n'
		<< "\r\x1b[2K          " << detail << '\n'
		<< std::flush;
	rendered_ = true;
}

}
