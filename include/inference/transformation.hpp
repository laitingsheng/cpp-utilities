#ifndef __INFERENCE_TRANSFORMATION_HPP__
#define __INFERENCE_TRANSFORMATION_HPP__

#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <type_traits>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace inference
{

struct transformation final
{
	const double ratio;
	const int64_t left, right, top, bottom;

	[[nodiscard]]
	static transformation letterbox(
		cv::Mat & image,
		const cv::Size & new_size,
		bool scale_up,
		const cv::Scalar & padded_colour
	)
	{
		auto image_size = image.size();
		if (image_size == new_size)
			return {};

		auto ratio = std::min(double(new_size.width) / image_size.width, double(new_size.height) / image_size.height);
		if (ratio < 1.0 || (ratio > 1.0 && scale_up))
		{
			image_size.height *= ratio;
			image_size.width *= ratio;
			image_size.height = std::clamp(image_size.height, 0, new_size.height);
			image_size.width = std::clamp(image_size.width, 0, new_size.width);

			cv::resize(image, image, image_size, ratio, ratio, cv::InterpolationFlags::INTER_LINEAR);
		}
		else
			ratio = 1.0;

		size_t width_pad = new_size.width - image_size.width, height_pad = new_size.height - image_size.height;
		size_t left = width_pad >> 1, right = width_pad - left;
		size_t top = height_pad >> 1, bottom = height_pad - top;

		cv::copyMakeBorder(image, image, top, bottom, left, right, cv::BorderTypes::BORDER_CONSTANT, padded_colour);

		return { ratio, left, right, top, bottom };
	}

	[[nodiscard]]
	static transformation scale(cv::Mat & image, size_t side_length, bool side_length_as_max)
	{
		auto image_size = image.size();
		double ratio;
		if ((image_size.height > image_size.width) == side_length_as_max)
		{
			ratio = double(side_length) / image_size.height;
			image_size.height = side_length;
			image_size.width *= ratio;
		}
		else
		{
			ratio = double(side_length) / image_size.width;
			image_size.width = side_length;
			image_size.height *= ratio;
		}
		cv::resize(image, image, image_size, ratio, ratio, cv::InterpolationFlags::INTER_LINEAR);
		return { ratio };
	}

	~transformation() noexcept = default;

	transformation(const transformation &) noexcept = default;
	transformation(transformation &&) = delete;

	transformation & operator=(const transformation &) = delete;
	transformation & operator=(transformation &&) = delete;

	template<typename Tensor>
		requires std::is_object_v<Tensor>
	void rescale(Tensor & boxes, const cv::Size & size) const;
private:
	inline transformation(
		double ratio = 0.0,
		int64_t left = 0,
		int64_t right = 0,
		int64_t top = 0,
		int64_t bottom = 0
	) noexcept : ratio(ratio), left(left), right(right), top(top), bottom(bottom) {}
};

}

#endif
