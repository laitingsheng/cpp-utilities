#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <opencv2/core.hpp>
#include <pylon/_BaslerUniversalCameraParams.h>
#include <pylon/BaslerUniversalInstantCamera.h>
#include <pylon/PylonIncludes.h>

#include "utilities/camera/base.hpp"
#include "utilities/camera/basler.hpp"

#include "./utils.hpp"

#include "../../CLoger.h"

namespace utilities::camera
{

basler::image_listener::image_listener(bool colour) :
	CBaslerUniversalImageEventHandler {},
	_converter {},
	_colour(colour),
	_rotation(base::rotation_direction::ORIGINAL),
	_lock {},
	_images {},
	_counter(0)
{
	_converter.Initialize(_colour ? Pylon::PixelType_BGR8packed : Pylon::PixelType_Mono8);
}

[[nodiscard]]
base::frame basler::image_listener::next(std::error_code& ec)
{
	auto guard = std::lock_guard { _lock };
	if (_images.empty())
		return {};

	base::frame ret { ++_counter, std::move(_images.front()) };
	_images.pop_front();
	return ret;
}

[[nodiscard]]
base::rotation_direction basler::image_listener::rotation() const
{
	return _rotation;
}

void basler::image_listener::rotation(base::rotation_direction direction)
{
	_rotation = direction;
}

void basler::image_listener::OnImageEventHandlerRegistered(Pylon::CBaslerUniversalInstantCamera& camera)
{
	auto guard = std::lock_guard { _lock };
	_images.clear();
	_counter = 0;
}

void basler::image_listener::OnImageGrabbed(
	Pylon::CBaslerUniversalInstantCamera& camera,
	const Pylon::CBaslerUniversalGrabResultPtr& grabResult
)
{
	Pylon::IImage& source_image = grabResult;
	cv::Mat image;
	if (_converter.ImageHasDestinationFormat(source_image))
	{
		size_t stride;
		if (!source_image.GetStride(stride))
			throw std::logic_error("failed to get the stride of BGR/Mono image");
		cv::Mat(
			source_image.GetHeight(),
			source_image.GetWidth(),
			_colour ? CV_8UC3 : CV_8UC1,
			source_image.GetBuffer(),
			stride
		).copyTo(image);
	}
	else
	{
		image.create(source_image.GetHeight(), source_image.GetWidth(), _colour ? CV_8UC3 : CV_8UC1);
		_converter.Convert(image.data, image.total() * image.channels(), source_image);
	}

	auto guard = std::lock_guard { _lock };
	_utils::rotate(image, _images.emplace_back(), _rotation);
}

const basler::initialiser basler::_global_guard;

basler::basler(const Pylon::CDeviceInfo& device_info, bool colour) :
	device {},
	_instance { Pylon::CTlFactory::GetInstance().CreateDevice(device_info), Pylon::Cleanup_Delete },
	_listener { colour }
{}

[[nodiscard]]
std::vector<std::unique_ptr<basler>> basler::find(
	const std::vector<std::string_view>& serials,
	transport_layer type,
	bool colour
)
{
	Pylon::CDeviceInfo reference;
	switch (type)
	{
		case transport_layer::USB:
			reference.SetTLType(Pylon::TLType::TLTypeUSB);
			break;
		case transport_layer::GIG_E:
			reference.SetTLType(Pylon::TLType::TLTypeGigE);
			break;
	}

	Pylon::DeviceInfoList filter;
	filter.assign(std::max(serials.size(), size_t(1)), reference);
	for (size_t i = 0; i < serials.size(); ++i)
	{
		const auto& serial = serials[i];
		filter[i].SetSerialNumber({ serial.data(), serial.size() });
	}

	Pylon::DeviceInfoList devices;
	Pylon::CTlFactory::GetInstance().EnumerateDevices(devices, filter, true);

	std::vector<std::unique_ptr<basler>> ret;
	ret.reserve(devices.size());
	for (auto& device_info : devices)
		ret.emplace_back(new basler(device_info, colour));
	return ret;
}

void basler::close()
{
	_instance.Close();
}

[[nodiscard]]
base::frame basler::next_image(std::error_code& ec)
{
	return _listener.next(ec);
}

void basler::open()
{
	_instance.Open();
}

[[nodiscard]]
base::rotation_direction basler::rotation() const
{
	return _listener.rotation();
}

void basler::rotation(base::rotation_direction direction)
{
	_listener.rotation(direction);
}

[[nodiscard]]
std::string basler::serial() const
{
	const auto& serial = _instance.GetDeviceInfo().GetSerialNumber();
	return { serial.c_str(), serial.size() };
}

void basler::start()
{
	_instance.StartGrabbing(Pylon::GrabStrategy_OneByOne, Pylon::GrabLoop_ProvidedByInstantCamera);
}

void basler::stop()
{
	_instance.StopGrabbing();
}

void basler::subscribe()
{
	_instance.RegisterImageEventHandler(
		&_listener,
		Pylon::RegistrationMode_ReplaceAll,
		Pylon::Cleanup_None
	);
}

void basler::unsubscribe()
{
	_instance.DeregisterImageEventHandler(&_listener);
}

[[nodiscard]]
bool basler::exposure_time_range(std::chrono::microseconds& min, std::chrono::microseconds& max)
{
	auto& value = _instance.ExposureTimeRaw;
	if (!value.IsValid() || !value.IsWritable())
		return false;
	min = std::chrono::microseconds { value.GetMin() };
	max = std::chrono::microseconds { value.GetMax() };
	return true;
}

[[nodiscard]]
bool basler::set_exposure_time(const std::chrono::microseconds& time)
{
	return _instance.ExposureAuto.TrySetValue(Basler_UniversalCameraParams::ExposureAuto_Off) &&
		_instance.ExposureMode.TrySetValue(Basler_UniversalCameraParams::ExposureMode_Timed) &&
		_instance.ExposureTimeRaw.TrySetValue(time.count());
}

[[nodiscard]]
bool basler::gain_range(int64_t& min, int64_t& max)
{
	auto& value = _instance.GainRaw;
	if (!value.IsValid() || !value.IsWritable())
		return false;
	min = value.GetMin();
	max = value.GetMax();
	return true;
}

[[nodiscard]]
bool basler::set_gain(int64_t gain)
{
	return _instance.GainAuto.TrySetValue(Basler_UniversalCameraParams::GainAuto_Off) &&
		_instance.GainRaw.TrySetValue(gain);
}

namespace
{

static constexpr auto _output_lines = std::array {
	Basler_UniversalCameraParams::LineSelector_Line1,
	Basler_UniversalCameraParams::LineSelector_Line2,
	Basler_UniversalCameraParams::LineSelector_Line3,
	Basler_UniversalCameraParams::LineSelector_Line4
};

}

[[nodiscard]]
bool basler::output_signal(size_t line, bool value)
{
	return line < _output_lines.size() &&
		_instance.LineSelector.TrySetValue(_output_lines[line]) &&
		_instance.LineMode.TrySetValue(Basler_UniversalCameraParams::LineMode_Output) &&
		_instance.LineSource.TrySetValue(Basler_UniversalCameraParams::LineSource_UserOutput1) &&
		_instance.UserOutputSelector.TrySetValue(Basler_UniversalCameraParams::UserOutputSelector_UserOutput1) &&
		_instance.UserOutputValue.TrySetValue(value);
}

namespace
{

static constexpr auto _trigger_lines = std::array {
	Basler_UniversalCameraParams::TriggerSource_Line1,
	Basler_UniversalCameraParams::TriggerSource_Line2,
	Basler_UniversalCameraParams::TriggerSource_Line3
};

}

[[nodiscard]]
bool basler::trigger_delay_range(std::chrono::duration<double, std::micro>& min, std::chrono::duration<double, std::micro>& max)
{
	auto& value = _instance.TriggerDelayAbs;
	if (!value.IsValid() || !value.IsWritable())
		return false;
	min = std::chrono::duration<double, std::micro> { value.GetMin() };
	max = std::chrono::duration<double, std::micro> { value.GetMax() };
	return true;
}

[[nodiscard]]
bool basler::set_manual_trigger_line_source(size_t line, const std::chrono::duration<double, std::micro>& delay)
{
	return line < _trigger_lines.size() &&
		_instance.TriggerSelector.TrySetValue(Basler_UniversalCameraParams::TriggerSelector_FrameStart) &&
		_instance.TriggerSource.TrySetValue(_trigger_lines[line]) &&
		_instance.TriggerActivation.TrySetValue(Basler_UniversalCameraParams::TriggerActivation_RisingEdge) &&
		_instance.TriggerDelayAbs.TrySetValue(delay.count()) &&
		_instance.TriggerMode.TrySetValue(Basler_UniversalCameraParams::TriggerMode_On);
}

}
