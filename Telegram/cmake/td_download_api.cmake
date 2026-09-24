# This file is part of AyuGram Desktop,
# the unofficial fork of Telegram Desktop with a built-in download API.
#
# For license and copyright information please follow this link:
# https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL

add_library(td_download_api OBJECT)
init_non_host_target(td_download_api)
add_library(tdesktop::td_download_api ALIAS td_download_api)

nice_target_sources(td_download_api ${src_loc}
PRIVATE
    download_api/download_api.cpp
    download_api/download_api.h
    download_api/download_api_http.cpp
    download_api/download_api_http.h
    download_api/download_api_job.cpp
    download_api/download_api_job.h
    download_api/download_api_json.cpp
    download_api/download_api_json.h
    download_api/download_api_pch.h
)

target_include_directories(td_download_api
PUBLIC
    ${src_loc}
)

target_link_libraries(td_download_api
PUBLIC
    desktop-app::lib_base
    tdesktop::td_scheme
    tdesktop::td_lang
    tdesktop::td_export
)