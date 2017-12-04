# required for libdvd

set(PATCH_ARCHIVE_NAME "patch-2.5.9-7-bin-3")
set(PATCH_PATH ${CMAKE_SOURCE_DIR}/project/BuildDependencies/downloads/tools/${PATCH_ARCHIVE_NAME})
find_program(PATCH_EXE NAMES patch patch.exe PATHS "${PATCH_PATH}" PATH_SUFFIXES bin)
if(NOT PATCH_EXE)
  if(NOT DEFINED KODI_MIRROR)
    set(KODI_MIRROR "http://mirrors.kodi.tv")
  endif()
  set(PATCH_ARCHIVE "${PATCH_ARCHIVE_NAME}.zip")
  set(PATCH_URL "${KODI_MIRROR}/build-deps/win32/${PATCH_ARCHIVE}")
  set(PATCH_DOWNLOAD ${CMAKE_SOURCE_DIR}/project/BuildDependencies/downloads/tools/${PATCH_ARCHIVE})

  # download the archive containing patch.exe
  message(STATUS "Downloading patch utility from ${PATCH_URL}...")
  file(DOWNLOAD "${PATCH_URL}" "${PATCH_DOWNLOAD}" STATUS PATCH_DL_STATUS LOG PATCH_LOG SHOW_PROGRESS)
  list(GET PATCH_DL_STATUS 0 PATCH_RETCODE)
  if(NOT PATCH_RETCODE EQUAL 0)
    message(FATAL_ERROR "ERROR downloading ${PATCH_URL} - status: ${PATCH_DL_STATUS} log: ${PATCH_LOG}")
  endif()

  # extract the archive containing patch.exe
  execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzvf ${PATCH_DOWNLOAD}
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/project/BuildDependencies/downloads/tools
                  OUTPUT_QUIET)

  # make sure the extraction worked and that patch.exe is there
  set(PATCH_BINARY_PATH ${PATCH_PATH}/bin/patch.exe)
  if(NOT EXISTS ${PATCH_PATH} OR NOT EXISTS ${PATCH_BINARY_PATH})
    message(FATAL_ERROR "ERROR extracting patch utility from ${PATCH_PATH}")
  endif()

  # make sure that cmake can find patch.exe
  find_program(PATCH_EXE NAMES patch patch.exe PATHS "${PATCH_PATH}" PATH_SUFFIXES bin)
  if(NOT PATCH_EXE)
    message(FATAL_ERROR "ERROR unable to find patch utility")
  endif()
endif()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PATCH_EXE REQUIRED_VARS PATCH_EXE)
