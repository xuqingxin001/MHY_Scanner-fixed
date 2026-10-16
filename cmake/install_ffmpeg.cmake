# FFmpeg 依赖: 固定 BtbN autobuild tag(asset 永久存在), 避免 latest 重建窗口下载失败。
# 历史事故: latest tag 在 autobuild 重建期间指向不稳定资源, file(DOWNLOAD) 静默失败,
# 头文件缺失直到编译阶段才报 C1083(libavcodec/avcodec.h 找不到)。
# 现加 STATUS 检查 + 最多3次重试 + 失败 FATAL_ERROR 明确报错。
set(FFMPEG_URL "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-16-13-04/ffmpeg-N-127579-g5cb9fcd211-win64-gpl-shared.zip")
set(FFMPEG_ARCHIVE "${CMAKE_BINARY_DIR}/ffmpeg.zip")
set(FFMPEG_DIR "${CMAKE_BINARY_DIR}/ffmpeg-N-127579-g5cb9fcd211-win64-gpl-shared")

if(NOT EXISTS ${FFMPEG_DIR})
	message(STATUS "Downloading FFmpeg... ${FFMPEG_URL}")
	set(download_ok FALSE)
	foreach(attempt RANGE 1 3)
		file(DOWNLOAD ${FFMPEG_URL} ${FFMPEG_ARCHIVE} SHOW_PROGRESS STATUS dl_status TIMEOUT 600)
		list(GET dl_status 0 dl_code)
		if(dl_code EQUAL 0 AND EXISTS ${FFMPEG_ARCHIVE})
			set(download_ok TRUE)
			break()
		endif()
		message(WARNING "FFmpeg 下载失败(第${attempt}次): ${dl_status}, 稍后重试...")
		file(REMOVE ${FFMPEG_ARCHIVE})
	endforeach()
	if(NOT download_ok)
		message(FATAL_ERROR "FFmpeg 下载失败: ${FFMPEG_URL} (${dl_status})")
	endif()
	file(ARCHIVE_EXTRACT INPUT ${FFMPEG_ARCHIVE} DESTINATION "${CMAKE_BINARY_DIR}")
endif()

set(FFMPEG_LIBS
	avcodec
	avdevice
	avfilter
	avformat
	avutil
	swscale
	swresample
)

set(FFMPEG_BIN_DIR ${FFMPEG_DIR}/bin)
set(FFMPEG_LIB_DIR ${FFMPEG_DIR}/lib)
set(FFMPEG_INCLUDE_DIR ${FFMPEG_DIR}/include)

link_directories(${FFMPEG_LIB_DIR})
include_directories(${FFMPEG_INCLUDE_DIR})
