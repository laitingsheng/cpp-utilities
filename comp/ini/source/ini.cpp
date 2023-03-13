#define MINI_CASE_SENSITIVE

#include <filesystem>
#include <utility>

#include <mini/ini.h>

#include "utilities/preprocessor.hpp"
#include "utilities/ini.hpp"

namespace utilities
{

ini::ini() noexcept : _path {}, _data {}, _modified(false) {}

[[nodiscard]]
bool ini::load(std::filesystem::path path)
{
	if (!std::filesystem::exists(path) || !(mINI::INIReader { path.string() } >> _data))
		return false;

	_path = std::move(path);
	_modified = false;
	return true;
}

[[nodiscard]]
bool ini::loaded() const noexcept
{
	return !_path.empty();
}

[[nodiscard]]
bool ini::modified() const noexcept
{
	return _modified;
}

[[nodiscard]]
bool ini::read(std::string section, std::string key, std::string& value) const
{
	auto fetched = _data.get(std::move(section)).get(std::move(key));
	if (fetched.empty())
		return false;
	value = std::move(fetched);
	return true;
}

[[nodiscard]]
bool ini::save()
{
	if (_path.empty() || !(mINI::INIWriter { _path.string() } << _data))
		return false;

	_modified = false;
	return true;
}

[[nodiscard]]
bool ini::write(std::string section, std::string key, std::string value)
{
	_data[std::move(section)].set(std::move(key), std::move(value));
	_modified = true;
	return true;
}

}