#pragma once

#include <cstddef>   
#include <cstdint>  

constexpr std::size_t PAGE_SIZE = 4096;

using page_id_t = std::int32_t;
constexpr page_id_t INVALID_PAGE_ID = -1;

using frame_id_t = std::size_t;
constexpr frame_id_t INVALID_FRAME_ID = static_cast<frame_id_t>(-1);