#include "stdafx.h"
#include "state_arguments_functions.h"

namespace award_system
{
	//definitions of static members of functions_cf

	template <> __attribute__((used)) ge_function<float> functions_cf<float>::ge_function = {};
	template <> __attribute__((used)) le_function<float> functions_cf<float>::le_function = {};

	template <> __attribute__((used)) ge_function<u32> functions_cf<u32>::ge_function = {};
	template <> __attribute__((used)) le_function<u32> functions_cf<u32>::le_function = {};
} //namespace award_system
