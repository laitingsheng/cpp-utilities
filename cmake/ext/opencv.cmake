set(OPENCV_ENABLE_NONFREE ON)
set(OPENCV_FORCE_3RDPARTY_BUILD ON)
set(WITH_CUDA OFF)
set(WITH_FFMPEG OFF)
set(WITH_IPP OFF)
set(WITH_OPENMP ON)
set(WITH_PROTOBUF OFF)
set(BUILD_opencv_apps OFF)
set(BUILD_JAVA OFF)
set(BUILD_opencv_python2 OFF)
set(BUILD_opencv_python3 OFF)
set(CV_TRACE OFF)

add_subdirectory("${SUBMODULE}" EXCLUDE_FROM_ALL)
