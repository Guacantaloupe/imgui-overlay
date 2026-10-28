// 统一按固定顺序包含所有 kiero2 后端头文件。
// kiero2 用后端的包含顺序生成 Implementation_* 索引，
// 所有使用 kiero 的翻译单元（包括各 kiero_*.cpp）都必须通过本文件引入，
// 否则 locate<N> 特化在不同 obj 间索引不一致，导致链接冲突。
#pragma once
#ifndef KIERO_BACKENDS_HPP
#define KIERO_BACKENDS_HPP

#include "kiero.hpp"
#include "kiero_d3d11.hpp"
#include "kiero_d3d12.hpp"
#include "kiero_opengl.hpp"
#include "kiero_vulkan.hpp"

#endif // KIERO_BACKENDS_HPP
