# This file is part of AyuGram Desktop,
# the unofficial fork of Telegram Desktop with a built-in download API.
#
# For license and copyright information please follow this link:
# https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL

add_library(td_download_api OBJECT)
init_non_host_target(td_download_api)
add_library(tdesktop::td_download_api ALIAS td_download_api)

target_precompile_headers(td_download_api PRIVATE
    ${src_loc}/download_api/download_api_pch.h
)
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