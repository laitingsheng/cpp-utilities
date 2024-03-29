#include <cstddef>

#include <mutex>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <MvCameraControl.h>

#include "utilities/camera/base.hpp"
#include "utilities/camera/hikvision.hpp"

#include "./utils.hpp"

namespace utilities::camera
{

namespace
{

#define _EC_NAME "HikVision MV Camera"

struct mvs_error_category final : public std::error_category
{
	[[nodiscard]]
	virtual const char *name() const noexcept override
	{
		return _EC_NAME;
	}

	[[nodiscard]]
	virtual std::string message(int condition) const override
	{
		const unsigned int actual_code = condition;
		static constexpr size_t code_width = sizeof(unsigned int) * 2;
		return _UTILITIES_FORMAT_STRING(_EC_NAME" error (code 0x{1:0{0}X})", code_width, actual_code);
	}
};

static const mvs_error_category _mvs_error_category;

_UTILITIES_FUNCTION_TEMPLATE(F, Args, int)
inline void _wrap_mvs(std::error_code& ec, F&& f, Args&&... args)
{
	if (auto ret = f(std::forward<Args>(args)...); ret != MV_OK)
		ec.assign(ret, _mvs_error_category);
	else
		ec.clear();
}

_UTILITIES_FUNCTION_TEMPLATE(F, Args, int)
inline void _wrap_mvs(F&& f, Args&&... args)
{
	std::error_code ec;
	_wrap_mvs(ec, std::forward<F>(f), std::forward<Args>(args)...);
	if (ec)
		throw std::system_error(ec);
}

}

void hikvision::_callback(unsigned char *data, ::MV_FRAME_OUT_INFO_EX *info, void *user)
{
	auto self = reinterpret_cast<hikvision *>(user);
	const auto required_pixel_type = self->_colour ?
		::MvGvspPixelType::PixelType_Gvsp_BGR8_Packed :
		::MvGvspPixelType::PixelType_Gvsp_Mono8;

	cv::Mat image;
	if (info->enPixelType == required_pixel_type)
		cv::Mat(
			info->nHeight,
			info->nWidth,
			self->_colour ? CV_8UC3 : CV_8UC1,
			data
		).copyTo(image);
	else
	{
		image.create(info->nHeight, info->nWidth, self->_colour ? CV_8UC3 : CV_8UC1);

		::MV_CC_PIXEL_CONVERT_PARAM param {};
		param.nWidth = info->nWidth;
		param.nHeight = info->nHeight;
		param.enSrcPixelType = info->enPixelType;
		param.pSrcData = data;
		param.nSrcDataLen = info->nFrameLen;
		param.enDstPixelType = required_pixel_type;
		param.pDstBuffer = image.data;
		param.nDstBufferSize = param.nDstLen = info->nHeight * info->nWidth * image.channels();

		_wrap_mvs(::MV_CC_ConvertPixelType, self->_handle, &param);
	}

	auto guard = std::lock_guard { self->_lock };
	_utils::rotate(image, self->_images.emplace_back(), self->_rotation);
}

hikvision::hikvision(const ::MV_CC_DEVICE_INFO *device_info, bool colour) :
	device {},
	_handle(nullptr),
	_colour(colour),
	_rotation(base::rotation_direction::ORIGINAL),
	_lock {},
	_images {},
	_counter(0)
{
	_wrap_mvs(::MV_CC_CreateHandleWithoutLog, &_handle, device_info);
}

namespace
{

[[nodiscard]]
std::unordered_map<std::string, ::MV_CC_DEVICE_INFO *> _process_gig_e(::MV_CC_DEVICE_INFO_LIST& device_info_list)
{
	std::unordered_map<std::string, ::MV_CC_DEVICE_INFO *> ret;
	for (size_t i = 0; i < device_info_list.nDeviceNum; ++i)
	{
		auto device_info = device_info_list.pDeviceInfo[i];
		ret.emplace(reinterpret_cast<char *>(device_info->SpecialInfo.stGigEInfo.chSerialNumber), device_info);
	}
	return ret;
}

[[nodiscard]]
std::unordered_map<std::string, ::MV_CC_DEVICE_INFO *> _process_usb(::MV_CC_DEVICE_INFO_LIST& device_info_list)
{
	std::unordered_map<std::string, ::MV_CC_DEVICE_INFO *> ret;
	for (size_t i = 0; i < device_info_list.nDeviceNum; ++i)
	{
		auto device_info = device_info_list.pDeviceInfo[i];
		ret.emplace(reinterpret_cast<char *>(device_info->SpecialInfo.stUsb3VInfo.chSerialNumber), device_info);
	}
	return ret;
}

}

[[nodiscard]]
std::vector<std::unique_ptr<hikvision>> hikvision::find(
	const std::vector<std::string>& serials,
	transport_layer type,
	bool colour
)
{
	::MV_CC_DEVICE_INFO_LIST list {};
	switch (type)
	{
		case transport_layer::USB:
			_wrap_mvs(::MV_CC_EnumDevices, MV_USB_DEVICE, &list);
			break;
		case transport_layer::GIG_E:
			_wrap_mvs(::MV_CC_EnumDevices, MV_GIGE_DEVICE, &list);
			break;
		default:
			return {};
	}

	std::vector<std::unique_ptr<hikvision>> ret;
	if (serials.empty())
	{
		ret.reserve(list.nDeviceNum);
		for (size_t i = 0; i < list.nDeviceNum; ++i)
			ret.emplace_back(new hikvision(list.pDeviceInfo[i], colour));
	}
	else
	{
		std::unordered_map<std::string, ::MV_CC_DEVICE_INFO *> mapping;
		switch (type)
		{
			case transport_layer::USB:
				mapping = _process_usb(list);
				break;
			case transport_layer::GIG_E:
				mapping = _process_gig_e(list);
				break;
			default:
				return {};
		}

		ret.reserve(serials.size());
		for (const auto& serial : serials)
			if (auto node = mapping.extract(serial))
				ret.emplace_back(new hikvision(node.mapped(), colour));
	}
	return ret;
}

hikvision::~hikvision() noexcept
{
	_wrap_mvs(::MV_CC_DestroyHandle, _handle);
	_handle = nullptr;
}

void hikvision::close()
{
	_wrap_mvs(::MV_CC_CloseDevice, _handle);
}

[[nodiscard]]
base::frame hikvision::next_image(std::error_code& ec)
{
	auto guard = std::lock_guard { _lock };
	if (_images.empty())
		return {};

	base::frame ret { ++_counter, std::move(_images.front()) };
	_images.pop_front();
	return ret;
}

void hikvision::open()
{
	_wrap_mvs(::MV_CC_OpenDevice, _handle, MV_ACCESS_Control, 0);
}

[[nodiscard]]
base::rotation_direction hikvision::rotation() const
{
	return _rotation;
}

void hikvision::rotation(base::rotation_direction rotation)
{
	_rotation = rotation;
}

[[nodiscard]]
std::string hikvision::serial() const
{
	::MV_CC_DEVICE_INFO device_info {};
	_wrap_mvs(::MV_CC_GetDeviceInfo, _handle, &device_info);
	switch (device_info.nTLayerType)
	{
		case MV_GIGE_DEVICE:
			return reinterpret_cast<char *>(device_info.SpecialInfo.stGigEInfo.chSerialNumber);
		case MV_USB_DEVICE:
			return reinterpret_cast<char *>(device_info.SpecialInfo.stUsb3VInfo.chSerialNumber);
		default:
			throw std::logic_error("unexpected device transport layer type");
	}
}

void hikvision::start()
{
	_wrap_mvs(::MV_CC_StartGrabbing, _handle);
}

void hikvision::stop()
{
	_wrap_mvs(::MV_CC_StopGrabbing, _handle);
}

void hikvision::subscribe()
{
	_wrap_mvs(::MV_CC_RegisterImageCallBackEx, _handle, _callback, this);

	auto guard = std::lock_guard { _lock };
	_images.clear();
	_counter = 0;
}

void hikvision::unsubscribe()
{
	_wrap_mvs(::MV_CC_RegisterImageCallBackEx, _handle, nullptr, nullptr);
}

namespace
{

inline bool _mvs_execute(void *handle, const char *name)
{
	std::error_code ec;
	_wrap_mvs(ec, ::MV_CC_SetCommandValue, handle, name);
	return !ec;
}

inline bool _mvs_set(void *handle, const char *name, bool value)
{
	std::error_code ec;
	_wrap_mvs(ec, ::MV_CC_SetBoolValue, handle, name, value);
	return !ec;
}

#ifdef __cpp_concepts
#define PARAMETER_CONSTRAINT(N, C) \
template<typename N> \
	requires C<N>
#else
#define PARAMETER_CONSTRAINT(N, C) \
template< \
	typename N, \
	const std::enable_if_t<C<N>, N> * = nullptr \
>
#endif

PARAMETER_CONSTRAINT(T, std::is_integral_v)
[[nodiscard]]
inline bool _mvs_set(void *handle, const char *name, T value)
{
	std::error_code ec;
	_wrap_mvs(ec, ::MV_CC_SetIntValueEx, handle, name, value);
	return !ec;
}

PARAMETER_CONSTRAINT(T, std::is_floating_point_v)
[[nodiscard]]
inline bool _mvs_set(void *handle, const char *name, T value)
{
	std::error_code ec;
	_wrap_mvs(ec, ::MV_CC_SetFloatValue, handle, name, value);
	return !ec;
}

template<bool string>
[[nodiscard]]
inline bool _mvs_set(void *handle, const char *name, const char *value)
{
	std::error_code ec;
	if constexpr (string)
		_wrap_mvs(ec, ::MV_CC_SetStringValue, handle, name, value);
	else
		_wrap_mvs(ec, ::MV_CC_SetEnumValueByString, handle, name, value);
	return !ec;
}

}

[[nodiscard]]
bool hikvision::set_exposure_time(const std::chrono::duration<double, std::micro>& time)
{
	return _mvs_set<false>(_handle, "ExposureAuto", "Off") &&
		_mvs_set<false>(_handle, "ExposureMode", "Timed") &&
		_mvs_set(_handle, "ExposureTime", time.count());
}

[[nodiscard]]
bool hikvision::set_gain(double gain)
{
	return _mvs_set<false>(_handle, "GainAuto", "Off") &&
		_mvs_set(_handle, "Gain", gain);
}

namespace
{

static constexpr auto _universal_lines = std::array {
	"Line0",
	"Line1",
	"Line2"
};

}

[[nodiscard]]
bool hikvision::set_line_debouncer_time(size_t line, const std::chrono::microseconds& time)
{
	return line < _universal_lines.size() &&
		_mvs_set<false>(_handle, "LineSelector", _universal_lines[line]) &&
		_mvs_set(_handle, "LineDebouncerTime", time.count());
}

[[nodiscard]]
bool hikvision::output_signal(size_t line, const std::chrono::microseconds& duration)
{
	return line < _universal_lines.size() &&
		_mvs_set<false>(_handle, "LineSelector", _universal_lines[line]) &&
		_mvs_set<false>(_handle, "LineMode", "Strobe") &&
		_mvs_set<false>(_handle, "LineSource", "SoftTriggerActive") &&
		_mvs_set(_handle, "StrobeEnable", true) &&
		_mvs_set(_handle, "StrobeLinePreDelay", 0) &&
		_mvs_set(_handle, "StrobeLineDelay", 0) &&
		_mvs_set(_handle, "StrobeLineDuration", duration.count()) &&
		_mvs_execute(_handle, "LineTriggerSoftware");
}

[[nodiscard]]
bool hikvision::set_manual_trigger_line_source(size_t line, const std::chrono::duration<double, std::micro>& delay)
{
	return line < _universal_lines.size() &&
		_mvs_set<false>(_handle, "TriggerSource", _universal_lines[line]) &&
		_mvs_set<false>(_handle, "TriggerActivation", "RisingEdge") &&
		_mvs_set(_handle, "TriggerDelay", delay.count()) &&
		_mvs_set<false>(_handle, "TriggerMode", "On");
}

}
