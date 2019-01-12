set(SOURCES BaseFrameAllocator.cpp
            GeneralAllocator.cpp
            MfxDecoderUtils.cpp
            SysMemFrameAllocator.cpp)

set(HEADERS BaseFrameAllocator.h
            GeneralAllocator.h
            MfxDecoderUtils.h
            SysMemFrameAllocator.h)

if(CORE_SYSTEM_NAME MATCHES windows)
  list(APPEND SOURCES D3D11FrameAllocator.cpp)
  list(APPEND HEADERS D3D11FrameAllocator.h)
endif()

core_add_library(dvdvideocodecsmfx)
